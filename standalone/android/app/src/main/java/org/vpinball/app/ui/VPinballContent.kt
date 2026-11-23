package org.vpinball.app.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import java.io.File
import org.vpinball.app.CodeLanguage
import org.vpinball.app.VPinballModel
import org.vpinball.app.ui.screens.common.CodeWebViewDialog
import org.vpinball.app.ui.screens.landing.LandingScreen
import org.vpinball.app.ui.screens.landing.TableDownloaderScreen
import org.vpinball.app.ui.screens.loading.LoadingScreen
import org.vpinball.app.ui.screens.splash.SplashScreen
import org.vpinball.app.ui.theme.VPinballTheme
import org.vpinball.app.ui.util.koinActivityViewModel
import org.vpinball.app.ui.util.rememberShareFile

@Composable
fun VPinballContent(viewModel: VPinballModel = koinActivityViewModel()) {
    val shareFile = rememberShareFile()
    var codeFile by remember { mutableStateOf<File?>(null) }
    var showDownloader by remember { mutableStateOf(false) }

    VPinballTheme {
        if (viewModel.showSplash) {
            SplashScreen()
        } else {
            Box(modifier = Modifier.fillMaxSize()) {
                LandingScreen(
                    onViewFile = { file -> codeFile = file },
                    onOpenDownloader = { showDownloader = true }
                )
                if (showDownloader) {
                    TableDownloaderScreen(onClose = { showDownloader = false })
                }
                if (viewModel.showHUD) {
                    viewModel.activeTable?.let { table -> LoadingScreen(table, viewModel.hudProgress, viewModel.hudStatus) }
                }
            }

            codeFile?.let {
                CodeWebViewDialog(
                    file = it,
                    canClear = CodeLanguage.fromFile(it) == CodeLanguage.LOG,
                    onDismissRequest = { codeFile = null },
                    onShare = { shareFile(it) },
                )
            }
        }
    }
}
