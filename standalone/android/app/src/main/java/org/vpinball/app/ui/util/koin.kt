package org.vpinball.app.ui.util

import android.content.Context
import android.content.ContextWrapper
import androidx.activity.ComponentActivity
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.ViewModel
import org.koin.androidx.compose.koinViewModel
import org.vpinball.app.vr.ActivityHost

/** The activity behind a UI context, also when the UI is hosted on the VR shell's virtual display. */
fun Context.findComponentActivity(): ComponentActivity? {
    var c: Context? = this
    while (c != null) {
        if (c is ComponentActivity) return c
        if (c is ActivityHost) return c.hostActivity
        c = (c as? ContextWrapper)?.baseContext
    }
    return null
}

@Composable
inline fun <reified T : ViewModel> koinActivityViewModel() =
    koinViewModel<T>(viewModelStoreOwner = LocalContext.current.findComponentActivity() ?: error("No activity for this UI context"))
