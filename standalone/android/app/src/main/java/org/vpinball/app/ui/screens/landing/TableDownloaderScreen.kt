package org.vpinball.app.ui.screens.landing

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.koin.androidx.compose.koinViewModel
import org.vpinball.app.ui.theme.VPinballTheme

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TableDownloaderScreen(onClose: () -> Unit, viewModel: TableDownloaderViewModel = koinViewModel()) {
    BackHandler(onBack = onClose)
    LaunchedEffect(Unit) { viewModel.loadIndex() }
    val entries by viewModel.entries.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val status by viewModel.status.collectAsState()
    val progress by viewModel.progress.collectAsState()
    val downloading by viewModel.downloading.collectAsState()

    VPinballTheme {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { Text("Table Downloader") },
                    navigationIcon = { IconButton(onClick = onClose) { Text("X") } },
                )
            }
        ) { padding ->
            Column(modifier = Modifier.fillMaxSize().padding(padding).padding(16.dp)) {
                Text("Visual Pinball Collection 2025-12-29", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (isLoading) {
                        CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(12.dp))
                    }
                    Text(status, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                    if (downloading != null) {
                        TextButton(onClick = { viewModel.cancelDownload() }) { Text("Cancel") }
                    }
                }
                if (downloading != null) {
                    Spacer(Modifier.height(8.dp))
                    val p = progress
                    if (p != null) LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth())
                    else LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
                Spacer(Modifier.height(12.dp))
                LazyVerticalGrid(columns = GridCells.Adaptive(minSize = 220.dp)) {
                    items(entries, key = { it.name }) { entry ->
                        Card(modifier = Modifier.padding(4.dp), enabled = downloading == null, onClick = { viewModel.downloadEntry(entry) }) {
                            Column(Modifier.padding(12.dp)) {
                                Text(entry.displayName, maxLines = 2, overflow = TextOverflow.Ellipsis, minLines = 2)
                                Spacer(Modifier.height(4.dp))
                                val sizeMb = (entry.totalSize / (1024 * 1024)).coerceAtLeast(1)
                                val extra = if (entry.b2sPath != null) " + backglass" else ""
                                Text("Download  $sizeMb MB$extra", style = MaterialTheme.typography.labelSmall)
                            }
                        }
                    }
                }
            }
        }
    }
}
