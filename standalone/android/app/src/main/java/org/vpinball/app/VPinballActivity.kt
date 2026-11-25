package org.vpinball.app

import android.content.ComponentName
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import org.koin.androidx.viewmodel.ext.android.viewModel
import org.vpinball.app.ui.VPinballContent
import org.vpinball.app.ui.screens.landing.LandingScreenViewModel
import org.vpinball.app.vr.VrShell

class VPinballActivity : ComponentActivity() {
    private companion object {
        const val TAG = "VPinballActivity"
    }

    val viewModel: VPinballModel by viewModel()

    // On the headsets (Meta Quest and PICO) the menu is shown on a curved screen in VR instead of a flat 2D window.
    // Falls back to the flat window when the headset's OpenXR runtime can't run the VR menu.
    private var useVrShell = BuildConfig.IS_VR

    // One restart per process, so a genuinely broken runtime can't loop it.
    private var relaunchAttempted = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        window.addFlags(FLAG_KEEP_SCREEN_ON)

        VPinballManager.onActivityReady(this)

        VPinballManager.whenReady { TableManager.initialize(this) }

        handleIntent(intent)

        if (useVrShell) {
            VrShell.attach(this, onFailed = { handleShellFailure() }) { AppContent() }
        } else {
            setContent { AppContent() }
        }
    }

    /**
     * The VR shell could not start. The usual cause on a headset: the pinball engine of a previous
     * game session still holds the process's OpenXR instance, and the runtime only allows one per
     * process — so the menu's own instance can never be created in this process. Relaunching in a
     * fresh process releases it; if the shell still fails there, fall back to the flat menu.
     */
    private fun handleShellFailure() {
        if (!relaunchAttempted) {
            relaunchAttempted = true
            Log.w(TAG, "VR shell failed to start; relaunching the app in a fresh process")
            try {
                // Safety net in case the restart never takes: the flat menu still works.
                useVrShell = false
                setContent { AppContent() }
                startActivity(Intent.makeRestartActivityTask(ComponentName(this, VPinballActivity::class.java)))
                Handler(Looper.getMainLooper()).postDelayed({ Process.killProcess(Process.myPid()) }, 500)
                return
            } catch (t: Throwable) {
                Log.e(TAG, "Could not relaunch the app; staying on the flat menu", t)
            }
        }
        useVrShell = false
        setContent { AppContent() }
    }

    @Composable
    private fun AppContent() {
        LaunchedEffect(Unit) { viewModel.startSplashTimer() }

        LaunchedEffect(viewModel.showHUD, viewModel.activeTable) {
            if (viewModel.showHUD && viewModel.activeTable != null) {
                val table = viewModel.activeTable!!

                val success = VPinballManager.load(table) { progress, status -> lifecycleScope.launch { viewModel.updateHUD(progress, status) } }

                if (success) {
                    // The player runs its own OpenXR session, so the menu's VR session must be gone first.
                    if (useVrShell) VrShell.pause()
                    val intent = Intent(this@VPinballActivity, VPinballPlayerActivity::class.java)
                    startActivity(intent)
                    @Suppress("DEPRECATION") overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out)
                }
            }
        }

        VPinballContent(viewModel = viewModel)
    }

    override fun onResume() {
        super.onResume()
        if (useVrShell) VrShell.resume()
    }

    override fun onPause() {
        if (useVrShell) VrShell.pause()
        super.onPause()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onDestroy() {
        if (useVrShell) VrShell.detach()
        VPinballManager.setMainActivity(null)
        super.onDestroy()
    }

    private fun handleIntent(intent: Intent?) {
        intent?.getStringExtra("autoplay")?.let { name ->
            intent.removeExtra("autoplay")
            VPinballManager.whenReady {
                lifecycleScope.launch {
                    val table = TableManager.getInstance().tables.map { list -> list.firstOrNull { it.name == name } }.filterNotNull().first()
                    viewModel.activeTable = table
                    viewModel.showHUD(table.name, "Launching")
                }
            }
        }
        val uri = intent?.data ?: return
        LandingScreenViewModel.triggerOpenUri(uri)
    }
}
