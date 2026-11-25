package org.vpinball.app.vr

import android.annotation.SuppressLint
import android.app.Presentation
import android.content.Context
import android.content.ContextWrapper
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.AttributeSet
import android.util.Log
import android.view.Display
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.compose.LocalOnBackPressedDispatcherOwner
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.platform.ComposeView
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import java.util.concurrent.atomic.AtomicLong

/**
 * Shows the app UI on a curved screen in an empty, black VR space (Meta Quest and PICO).
 *
 * The native side (libvrshell.so) runs an OpenXR session (standard OpenXR only, through the Khronos
 * loader) that composites one cylinder layer (a flat quad where cylinders are missing). The UI is drawn
 * into a SurfaceTexture owned by the native GL context: its Surface backs a private VirtualDisplay
 * hosting a Presentation with the normal Compose UI, and every new frame is copied into the layer's
 * swapchain together with the pointer cursor. Controller input arrives as (u, v) hits on the screen and
 * is turned into touch, hover, scroll and back key events.
 */
@SuppressLint("StaticFieldLeak")
object VrShell {
    private const val TAG = "VPX-VRShell"
    const val WIDTH = 1920
    const val HEIGHT = 1080
    private const val DPI = 240

    @JvmStatic private external fun nativeStart(activity: Any, width: Int, height: Int, gen: Int, mixedReality: Boolean)

    private const val PREFS = "vr_shell"
    private const val KEY_MIXED_REALITY = "mixed_reality"

    @JvmStatic private external fun nativeStop()

    private val main = Handler(Looper.getMainLooper())
    private var activity: ComponentActivity? = null
    private var content: (@Composable () -> Unit)? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var presentation: MenuPresentation? = null
    private var panelSurface: Surface? = null
    private var onFailed: (() -> Unit)? = null
    private val frames = AtomicLong()
    private var started = false
    private var gen = 0

    // Windows added on top of the presentation (dialogs, menus), bottom to top.
    private val windows = mutableListOf<View>()

    // Pointer state
    private var pressed = false
    private var downTime = 0L
    private var touchTarget: View? = null
    private var hoverTarget: View? = null

    /** [onFailed] runs on the main thread when the VR menu can't run on this headset: show the flat UI instead. */
    fun attach(activity: ComponentActivity, onFailed: () -> Unit, content: @Composable () -> Unit) {
        try {
            System.loadLibrary("vrshell")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "VR shell library not available, using the flat menu: ${e.message}")
            onFailed()
            return
        }
        this.activity = activity
        this.content = content
        this.onFailed = onFailed
    }

    fun resume() {
        val act = activity ?: return
        if (started) return
        started = true
        gen++
        val mixedReality = act.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_MIXED_REALITY, false)
        nativeStart(act, WIDTH, HEIGHT, gen, mixedReality)
    }

    fun pause() {
        if (!started) return
        started = false
        gen++
        endTouch()
        virtualDisplay?.surface = null
        panelSurface?.release()
        panelSurface = null
        nativeStop() // releases the SurfaceTexture on the GL thread
    }

    fun detach() {
        pause()
        presentation?.dismiss()
        presentation = null
        virtualDisplay?.release()
        virtualDisplay = null
        windows.clear()
        activity = null
        content = null
        onFailed = null
    }

    // ---- Callbacks from the native OpenXR thread ----

    /** Called on the GL thread with its context current: the UI frames land in [texId] (GL_TEXTURE_EXTERNAL_OES). */
    @JvmStatic
    fun createPanelTexture(texId: Int, width: Int, height: Int): SurfaceTexture {
        frames.set(0)
        return SurfaceTexture(texId).apply {
            setDefaultBufferSize(width, height)
            setOnFrameAvailableListener({ frames.incrementAndGet() }, main)
        }
    }

    /** GL thread, every frame: latches the newest UI frame and returns a counter that changes with each new frame. */
    @JvmStatic
    fun latchPanelTexture(st: SurfaceTexture, matrix: FloatArray): Long {
        val n = frames.get()
        return try {
            st.updateTexImage()
            st.getTransformMatrix(matrix)
            n
        } catch (e: RuntimeException) {
            -1L
        }
    }

    @JvmStatic
    fun releasePanelTexture(st: SurfaceTexture) {
        st.setOnFrameAvailableListener(null)
        st.release()
    }

    @JvmStatic
    fun onSurfaces(g: Int, panel: SurfaceTexture) {
        main.post {
            if (g != gen || !started) return@post
            panelSurface?.release()
            val surface = Surface(panel)
            panelSurface = surface
            showPanel(surface)
        }
    }

    /** The OpenXR runtime can't run the VR menu: fall back to the flat UI. */
    @JvmStatic
    fun onFailed(g: Int) {
        main.post {
            if (g != gen || !started) return@post
            Log.e(TAG, "VR menu failed to start, using the flat menu")
            val callback = onFailed
            detach()
            callback?.invoke()
        }
    }

    @JvmStatic
    fun onPointer(g: Int, hand: Int, valid: Boolean, u: Float, v: Float, isPressed: Boolean, scroll: Float) {
        main.post { if (g == gen && started) handlePointer(valid, u * WIDTH, v * HEIGHT, isPressed, scroll) }
    }

    /** Left stick click switched between mixed reality and the black VR space; remember it for next time. */
    @JvmStatic
    fun onModeChanged(g: Int, mixedReality: Boolean) {
        main.post {
            activity?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putBoolean(KEY_MIXED_REALITY, mixedReality)?.apply()
        }
    }

    @JvmStatic
    fun onBack(g: Int) {
        main.post { if (g == gen && started) handleBack() }
    }

    // ---- Display ----

    private fun showPanel(surface: Surface) {
        val act = activity ?: return
        val vd = virtualDisplay
        if (vd != null) {
            vd.surface = surface
            return
        }
        val dm = act.getSystemService(DisplayManager::class.java)
        val display =
            dm.createVirtualDisplay(
                "VPX-Menu",
                WIDTH,
                HEIGHT,
                DPI,
                surface,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION or DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY,
            )
        virtualDisplay = display
        presentation = MenuPresentation(act, display.display).also { it.show() }
        Log.i(TAG, "Menu shown on virtual display ${display.display.displayId}")
    }

    // ---- Input ----

    private fun baseWindow(): View? = presentation?.window?.decorView

    /** The menu window's token: a dialog anchored to it renders on the same (virtual) display. */
    internal fun menuToken(): IBinder? = presentation?.window?.decorView?.windowToken

    private fun windowFlags(view: View): Int = (view.layoutParams as? WindowManager.LayoutParams)?.flags ?: 0

    private fun containsPoint(view: View, x: Float, y: Float): Boolean {
        val loc = IntArray(2)
        view.getLocationOnScreen(loc)
        return x >= loc[0] && y >= loc[1] && x < loc[0] + view.width && y < loc[1] + view.height
    }

    /** Topmost window that should receive a touch at (x, y), mimicking the window manager rules. */
    private fun targetAt(x: Float, y: Float, sendOutside: Boolean): View? {
        for (i in windows.indices.reversed()) {
            val w = windows[i]
            if (!w.isAttachedToWindow) continue
            val flags = windowFlags(w)
            if (flags and WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE != 0) continue
            if (containsPoint(w, x, y)) return w
            if (flags and WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL == 0) return w // modal: takes all touches
            if (sendOutside && flags and WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH != 0) {
                dispatchTouch(w, MotionEvent.ACTION_OUTSIDE, x, y, SystemClock.uptimeMillis())
            }
        }
        return baseWindow()
    }

    private fun dispatchTouch(target: View, action: Int, x: Float, y: Float, time: Long) {
        val loc = IntArray(2)
        target.getLocationOnScreen(loc)
        val ev = MotionEvent.obtain(downTime, time, action, x - loc[0], y - loc[1], 0)
        ev.source = InputDevice.SOURCE_TOUCHSCREEN
        target.dispatchTouchEvent(ev)
        ev.recycle()
    }

    private fun dispatchMouse(target: View, action: Int, x: Float, y: Float, scroll: Float = 0f) {
        val loc = IntArray(2)
        target.getLocationOnScreen(loc)
        val props = MotionEvent.PointerProperties().apply {
            id = 0
            toolType = MotionEvent.TOOL_TYPE_MOUSE
        }
        val coords = MotionEvent.PointerCoords().apply {
            this.x = x - loc[0]
            this.y = y - loc[1]
            if (scroll != 0f) setAxisValue(MotionEvent.AXIS_VSCROLL, scroll)
        }
        val now = SystemClock.uptimeMillis()
        val ev = MotionEvent.obtain(now, now, action, 1, arrayOf(props), arrayOf(coords), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0)
        target.dispatchGenericMotionEvent(ev)
        ev.recycle()
    }

    private fun handlePointer(valid: Boolean, x: Float, y: Float, isPressed: Boolean, scroll: Float) {
        val now = SystemClock.uptimeMillis()

        if (pressed) {
            val target = touchTarget
            if (target != null) {
                if (!isPressed) {
                    dispatchTouch(target, MotionEvent.ACTION_UP, x, y, now)
                    touchTarget = null
                    pressed = false
                } else if (!valid) {
                    dispatchTouch(target, MotionEvent.ACTION_CANCEL, x, y, now)
                    touchTarget = null
                    pressed = false
                } else {
                    dispatchTouch(target, MotionEvent.ACTION_MOVE, x, y, now)
                }
            } else if (!isPressed) {
                pressed = false
            }
            return
        }

        if (!valid) {
            hoverTarget?.let { dispatchMouse(it, MotionEvent.ACTION_HOVER_EXIT, x, y) }
            hoverTarget = null
            return
        }

        if (isPressed) {
            hoverTarget?.let { dispatchMouse(it, MotionEvent.ACTION_HOVER_EXIT, x, y) }
            hoverTarget = null
            val target = targetAt(x, y, sendOutside = true) ?: return
            pressed = true
            downTime = now
            touchTarget = target
            dispatchTouch(target, MotionEvent.ACTION_DOWN, x, y, now)
            return
        }

        val target = targetAt(x, y, sendOutside = false) ?: return
        if (target !== hoverTarget) {
            hoverTarget?.let { dispatchMouse(it, MotionEvent.ACTION_HOVER_EXIT, x, y) }
            dispatchMouse(target, MotionEvent.ACTION_HOVER_ENTER, x, y)
            hoverTarget = target
        }
        dispatchMouse(target, MotionEvent.ACTION_HOVER_MOVE, x, y)
        if (scroll != 0f) dispatchMouse(target, MotionEvent.ACTION_SCROLL, x, y, scroll * 0.25f)
    }

    private fun endTouch() {
        touchTarget?.let { dispatchTouch(it, MotionEvent.ACTION_CANCEL, 0f, 0f, SystemClock.uptimeMillis()) }
        touchTarget = null
        hoverTarget = null
        pressed = false
    }

    private fun handleBack() {
        val top = windows.lastOrNull { it.isAttachedToWindow }
        if (top != null) {
            val now = SystemClock.uptimeMillis()
            top.dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0))
            top.dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0))
            return
        }
        val dispatcher = activity?.onBackPressedDispatcher ?: return
        // Never leave the app from the main menu by accident; only go back inside screens.
        if (dispatcher.hasEnabledCallbacks()) dispatcher.onBackPressed()
    }

    // ---- Window tracking ----

    internal fun onWindowAdded(view: View) {
        windows.remove(view)
        windows.add(view)
    }

    internal fun onWindowRemoved(view: View) {
        windows.remove(view)
        if (touchTarget === view) touchTarget = null
        if (hoverTarget === view) hoverTarget = null
    }

    @Composable
    internal fun Content() {
        val act = activity ?: return
        CompositionLocalProvider(LocalActivityResultRegistryOwner provides act, LocalOnBackPressedDispatcherOwner provides act) {
            content?.invoke()
        }
    }
}

/** Presentation hosting the Compose UI on the VR shell's virtual display. */
private class MenuPresentation(private val activity: ComponentActivity, display: Display) : Presentation(activity, display) {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val view = ComposeView(TrackingContext(context, activity)).apply { setContent { VrShell.Content() } }
        view.setViewTreeLifecycleOwner(activity)
        view.setViewTreeViewModelStoreOwner(activity)
        view.setViewTreeSavedStateRegistryOwner(activity)
        setContentView(view, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        window?.decorView?.let {
            it.setViewTreeLifecycleOwner(activity)
            it.setViewTreeViewModelStoreOwner(activity)
            it.setViewTreeSavedStateRegistryOwner(activity)
        }
        setCancelable(false)
    }

    @Deprecated("Back is routed by VrShell")
    override fun onBackPressed() {
        // Ignore: the presentation must stay up for the whole life of the menu.
    }
}

/** Lets UI code find the activity when it runs on the VR shell's virtual display. */
interface ActivityHost {
    val hostActivity: ComponentActivity
}

/**
 * Context for the VR shell's UI.
 *
 * Wraps a display context for the virtual display rather than the Presentation's own context: that
 * context is a window context (the Quest's presentation window type), and WindowManagerImpl refuses
 * to add any window whose type differs from the context's (WindowManagerImpl asserts the match).
 * A dialog asks for a different type, so it crashed with "Window type mismatch". A display context
 * (window type 0) accepts every window type, and its windows are still placed on the virtual display.
 * Dialogs opened from this context are tracked for controller input via [WindowTrackingFactory]:
 * a view that ends up under a ViewRootImpl is the root of a new top-level window, which the shell
 * then routes input to.
 */
private class TrackingContext(base: Context, override val hostActivity: ComponentActivity) :
    ContextWrapper(base), ActivityHost {

    /**
     * Context windows (dialogs) are added from. A plain display context (window type 0) is accepted
     * for any window type; it has no window token though, and app-type windows (dialogs) require one
     * — so it gets the menu window's token: a window anchored to the menu window renders on the same
     * (virtual) display, like a status-bar panel is anchored to the status bar.
     */
    private val dialogContext: Context by lazy {
        val dc = base.createDisplayContext(base.display)
        val token = VrShell.menuToken() ?: windowTokenOf(base) ?: hostActivity.window.decorView.windowToken
        if (token != null) setWindowToken(dc, token)
        dc
    }

    override fun getSystemService(name: String): Any? = when (name) {
        WINDOW_SERVICE -> trackedWindowManager
        LAYOUT_INFLATER_SERVICE -> {
            val inflater = dialogContext.getSystemService(name) as LayoutInflater
            inflater.factory2 = WindowTrackingFactory(inflater)
            inflater
        }
        else -> super.getSystemService(name)
    }

    /**
     * Every window added through this context (Compose popups like the import dropdown, dialogs,
     * pickers) is registered with [VrShell] so controller input is routed to it. Without this a
     * popup window renders on the virtual display but is invisible to the input router, and taps on
     * it fall straight through to the menu behind.
     */
    private val trackedWindowManager: WindowManager by lazy {
        val wm = dialogContext.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        object : WindowManager by wm {
            override fun addView(view: View, params: ViewGroup.LayoutParams) {
                Log.i("VPX-VRShell", "Window added: ${view.javaClass.name}")
                wm.addView(view, params)
                VrShell.onWindowAdded(view)
            }
            override fun removeView(view: View) {
                wm.removeView(view)
                VrShell.onWindowRemoved(view)
            }
            override fun removeViewImmediate(view: View) {
                wm.removeViewImmediate(view)
                VrShell.onWindowRemoved(view)
            }
        }
    }

    /** Window token stored in the first underlying ContextImpl (hidden class, read reflectively). */
    private fun windowTokenOf(context: Context): IBinder? {
        var ctx: Context? = context
        while (ctx != null) {
            try {
                val f = ctx.javaClass.getDeclaredField("mWindowToken")
                f.isAccessible = true
                val v = f.get(ctx)
                if (v is IBinder) return v
            } catch (t: Throwable) {
                // Not a ContextImpl (or the field moved) — keep unwrapping.
            }
            ctx = (ctx as? ContextWrapper)?.baseContext
        }
        return null
    }

    /** Sets the hidden ContextImpl.mWindowToken via its setter (or the field directly). */
    private fun setWindowToken(context: Context, token: IBinder) {
        try {
            val m = context.javaClass.getDeclaredMethod("setWindowToken", IBinder::class.java)
            m.isAccessible = true
            m.invoke(context, token)
        } catch (t: Throwable) {
            try {
                val f = context.javaClass.getDeclaredField("mWindowToken")
                f.isAccessible = true
                f.set(context, token)
            } catch (t2: Throwable) {
                Log.w("VPX-VRShell", "Could not set dialog window token", t2)
            }
        }
    }
}

/** Adds [WINDOW_TRACKER] to every view created for [TrackingContext]. */
private class WindowTrackingFactory(private val inflater: LayoutInflater) : LayoutInflater.Factory2 {
    override fun onCreateView(name: String, context: Context, attrs: AttributeSet): View? = create(name, context, attrs)

    override fun onCreateView(parent: View?, name: String, context: Context, attrs: AttributeSet): View? =
        create(name, context, attrs)

    private fun create(name: String, context: Context, attrs: AttributeSet): View? = try {
        // The inflater's own default view creation (framework switch or reflection), plus the tracker.
        val view = inflater.createView(context, name, null, attrs)
        view?.addOnAttachStateChangeListener(WINDOW_TRACKER)
        view
    } catch (t: Throwable) {
        null // let the inflater fall back to its default creation path
    }
}

/**
 * Registers views that become the root of a new top-level window (dialog, popup) so input can be
 * routed to them. The parent class is compared by name: ViewRootImpl is not part of the SDK stubs.
 */
private val WINDOW_TRACKER = object : View.OnAttachStateChangeListener {
    override fun onViewAttachedToWindow(v: View) {
        if (v.parent?.javaClass?.name == "android.view.ViewRootImpl") VrShell.onWindowAdded(v)
    }

    override fun onViewDetachedFromWindow(v: View) {
        if (v.parent?.javaClass?.name == "android.view.ViewRootImpl") VrShell.onWindowRemoved(v)
    }
}
