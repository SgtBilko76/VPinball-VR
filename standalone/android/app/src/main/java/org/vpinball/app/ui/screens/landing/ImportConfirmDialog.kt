package org.vpinball.app.ui.screens.landing

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.vpinball.app.ui.screens.common.RoundedCard
import org.vpinball.app.ui.theme.VpxRed

/**
 * Table import confirmation prompt.
 *
 * Rendered as an inline overlay inside the menu composition, NOT a
 * [android.app.Dialog]: the VR shell tracks popup windows by wrapping the
 * context's WindowManager (see VrShell.TrackingContext), and the framework's
 * Dialog constructor (Window.setWindowManager) hard-casts that object to the
 * final WindowManagerImpl class, so a Dialog built from the shell context
 * crashes with a ClassCastException. An inline overlay is part of the shell's
 * own window, so input routes to it without any window tracking.
 */
@Composable
fun ImportConfirmDialog(filename: String?, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    Box(modifier = Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.5f))) {
        Column(
            modifier = Modifier.align(Alignment.Center).padding(horizontal = 48.dp).widthIn(max = 480.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            RoundedCard {
                Text(text = "Confirm Import Table", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Spacer(modifier = Modifier.padding(vertical = 12.dp))
                Text(text = "Import \"$filename\"?")
            }
            Spacer(modifier = Modifier.padding(vertical = 16.dp))
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onDismiss) {
                    Text(
                        text = "Cancel",
                        color = Color.VpxRed,
                        fontSize = MaterialTheme.typography.titleMedium.fontSize,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(modifier = Modifier.padding(horizontal = 12.dp))
                TextButton(onClick = onConfirm) {
                    Text(text = "OK", color = Color.VpxRed, fontSize = MaterialTheme.typography.titleMedium.fontSize, fontWeight = FontWeight.SemiBold)
                }
            }
        }
    }
}
