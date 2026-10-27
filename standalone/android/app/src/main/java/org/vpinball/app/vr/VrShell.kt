package org.vpinball.app.vr

import android.annotation.SuppressLint
import android.app.Presentation
import android.content.Context
import android.content.ContextWrapper
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Display
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.WindowMetrics
import androidx.activity.ComponentActivity
import androidx.annotation.RequiresApi
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

    internal fun baseToken() = baseWindow()?.windowToken

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

/** Context whose WindowManager records popup and dialog windows so input can be routed to them. */
private class TrackingContext(base: Context, override val hostActivity: ComponentActivity) : ContextWrapper(base), ActivityHost {
    private val wm by lazy { TrackingWindowManager(base.getSystemService(WINDOW_SERVICE) as WindowManager) }

    override fun getSystemService(name: String): Any? = if (name == WINDOW_SERVICE) wm else super.getSystemService(name)
}

private class TrackingWindowManager(private val base: WindowManager) : WindowManager by base {
    override fun addView(view: View, params: ViewGroup.LayoutParams) {
        val lp = params as? WindowManager.LayoutParams
        if (lp != null && lp.type in WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW..WindowManager.LayoutParams.LAST_APPLICATION_WINDOW) {
            // Dialogs: attach them to the presentation window so they get a valid token on the virtual display.
            lp.type = WindowManager.LayoutParams.TYPE_APPLICATION_ATTACHED_DIALOG
            if (lp.token == null) lp.token = VrShell.baseToken()
        }
        base.addView(view, params)
        VrShell.onWindowAdded(view)
    }

    override fun removeView(view: View) {
        VrShell.onWindowRemoved(view)
        base.removeView(view)
    }

    override fun removeViewImmediate(view: View) {
        VrShell.onWindowRemoved(view)
        base.removeViewImmediate(view)
    }

    @RequiresApi(Build.VERSION_CODES.R)
    override fun getCurrentWindowMetrics(): WindowMetrics = base.currentWindowMetrics

    @RequiresApi(Build.VERSION_CODES.R)
    override fun getMaximumWindowMetrics(): WindowMetrics = base.maximumWindowMetrics
}
