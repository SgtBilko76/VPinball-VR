package org.vpinball.app.ui.util

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.FileProvider
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.vpinball.app.VPinballManager
import org.vpinball.app.jni.VPinballLogLevel
import org.vpinball.app.vr.LocalVrShell

/**
 * Returns a function that hands a file (log, ini, exported table) to the user.
 *
 * On the flat UI this is the Android share sheet. The VR menu can't use it: its UI context is not an activity, so starting the chooser from it
 * throws, and the headsets have next to no share targets anyway. There the file is saved with the system "save as" file picker instead, which is
 * started through the activity like the other pickers of the app.
 */
@Composable
fun rememberShareFile(): (File) -> Unit {
    val context = LocalContext.current

    if (!LocalVrShell.current) {
        return { file ->
            val fileUri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
            val shareIntent =
                Intent(Intent.ACTION_SEND).apply {
                    type = "application/octet-stream"
                    putExtra(Intent.EXTRA_STREAM, fileUri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
            context.startActivity(Intent.createChooser(shareIntent, "Share File: ${file.name}"))
        }
    }

    val coroutineScope = rememberCoroutineScope()
    var pendingFile by remember { mutableStateOf<File?>(null) }
    val saveLauncher =
        rememberLauncherForActivityResult(contract = ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
            val file = pendingFile
            pendingFile = null
            if (uri != null && file != null) {
                val resolver = context.applicationContext.contentResolver
                coroutineScope.launch(Dispatchers.IO) {
                    try {
                        val output = resolver.openOutputStream(uri) ?: error("Can't open the destination")
                        output.use { out -> file.inputStream().use { input -> input.copyTo(out) } }
                    } catch (e: Exception) {
                        VPinballManager.log(VPinballLogLevel.ERROR, "Failed to save ${file.name}: ${e.message}")
                    }
                }
            }
        }

    return { file ->
        pendingFile = file
        saveLauncher.launch(file.name)
    }
}
