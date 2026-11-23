package org.vpinball.app.vr

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.AlertDialog as M3AlertDialog
import androidx.compose.material3.AlertDialogDefaults
import androidx.compose.material3.BottomSheetDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet as M3ModalBottomSheet
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.material3.SheetState
import androidx.compose.material3.Surface
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntRect
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog as PlatformDialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupPositionProvider
import androidx.compose.ui.window.PopupProperties

/*
 * Dialogs that also work in the VR menu.
 *
 * The VR menu runs on a virtual display behind a Context whose WindowManager is wrapped (see
 * TrackingContext in VrShell.kt) so popup windows can be tracked and given input. A platform
 * android.app.Dialog can't be created from such a Context: Window.setWindowManager() casts the
 * WindowManager to the framework's WindowManagerImpl and throws a ClassCastException. Compose's
 * Dialog, AlertDialog and ModalBottomSheet are all platform dialogs underneath.
 *
 * The functions below have the same names and parameters as the Compose ones, so a screen only has
 * to import them from here. On the flat UI they call straight through to Compose. In the VR menu
 * they draw the same content in a full screen Popup, which is a window type the VR shell supports.
 */

/** True for UI composed inside the VR menu (the VR shell's virtual display). */
val LocalVrShell = staticCompositionLocalOf { false }

private val ScrimColor = Color.Black.copy(alpha = 0.5f)

private object FullScreenPosition : PopupPositionProvider {
    override fun calculatePosition(
        anchorBounds: IntRect,
        windowSize: IntSize,
        layoutDirection: LayoutDirection,
        popupContentSize: IntSize,
    ): IntOffset = IntOffset.Zero
}

/** Full screen popup with a scrim behind [content]. [onScrimClick] null: touches outside the content do nothing. */
@Composable
private fun VrOverlay(
    onDismissRequest: () -> Unit,
    dismissOnBackPress: Boolean,
    scrim: Color,
    onScrimClick: (() -> Unit)?,
    contentAlignment: Alignment,
    content: @Composable () -> Unit,
) {
    val currentOnScrimClick = rememberUpdatedState(onScrimClick)
    Popup(
        popupPositionProvider = FullScreenPosition,
        onDismissRequest = onDismissRequest,
        // Focusable so text fields work and the back key reaches this window; outside touches are handled by the scrim.
        properties = PopupProperties(focusable = true, dismissOnBackPress = dismissOnBackPress, dismissOnClickOutside = false),
    ) {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = contentAlignment) {
            Box(modifier = Modifier.fillMaxSize().background(scrim).pointerInput(Unit) { detectTapGestures { currentOnScrimClick.value?.invoke() } })
            content()
        }
    }
}

@Composable
fun Dialog(onDismissRequest: () -> Unit, properties: DialogProperties = DialogProperties(), content: @Composable () -> Unit) {
    if (!LocalVrShell.current) {
        PlatformDialog(onDismissRequest = onDismissRequest, properties = properties, content = content)
        return
    }
    VrOverlay(
        onDismissRequest = onDismissRequest,
        dismissOnBackPress = properties.dismissOnBackPress,
        scrim = Color.Transparent,
        onScrimClick = if (properties.dismissOnClickOutside) onDismissRequest else null,
        contentAlignment = Alignment.Center,
        content = content,
    )
}

@Composable
fun AlertDialog(
    onDismissRequest: () -> Unit,
    confirmButton: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    dismissButton: @Composable (() -> Unit)? = null,
    title: @Composable (() -> Unit)? = null,
    text: @Composable (() -> Unit)? = null,
) {
    if (!LocalVrShell.current) {
        M3AlertDialog(
            onDismissRequest = onDismissRequest,
            confirmButton = confirmButton,
            modifier = modifier,
            dismissButton = dismissButton,
            title = title,
            text = text,
        )
        return
    }
    VrOverlay(
        onDismissRequest = onDismissRequest,
        dismissOnBackPress = true,
        scrim = ScrimColor,
        onScrimClick = onDismissRequest,
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = modifier.padding(24.dp).widthIn(min = 280.dp, max = 560.dp),
            shape = AlertDialogDefaults.shape,
            color = AlertDialogDefaults.containerColor,
            tonalElevation = AlertDialogDefaults.TonalElevation,
        ) {
            Column(modifier = Modifier.padding(24.dp)) {
                if (title != null) {
                    CompositionLocalProvider(LocalContentColor provides AlertDialogDefaults.titleContentColor) {
                        ProvideTextStyle(MaterialTheme.typography.headlineSmall) { Box(modifier = Modifier.padding(bottom = 16.dp)) { title() } }
                    }
                }
                if (text != null) {
                    CompositionLocalProvider(LocalContentColor provides AlertDialogDefaults.textContentColor) {
                        ProvideTextStyle(MaterialTheme.typography.bodyMedium) {
                            Box(modifier = Modifier.weight(1f, fill = false).padding(bottom = 24.dp)) { text() }
                        }
                    }
                }
                Row(modifier = Modifier.align(Alignment.End), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ProvideTextStyle(MaterialTheme.typography.labelLarge) {
                        dismissButton?.invoke()
                        confirmButton()
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModalBottomSheet(
    onDismissRequest: () -> Unit,
    sheetState: SheetState = rememberModalBottomSheetState(),
    containerColor: Color = BottomSheetDefaults.ContainerColor,
    content: @Composable ColumnScope.() -> Unit,
) {
    if (!LocalVrShell.current) {
        M3ModalBottomSheet(onDismissRequest = onDismissRequest, sheetState = sheetState, containerColor = containerColor, content = content)
        return
    }
    // The sheet is shown fully expanded and is not draggable in the VR menu, so sheetState is not used.
    VrOverlay(
        onDismissRequest = onDismissRequest,
        dismissOnBackPress = true,
        scrim = ScrimColor,
        onScrimClick = onDismissRequest,
        contentAlignment = Alignment.BottomCenter,
    ) {
        Surface(modifier = Modifier.widthIn(max = 640.dp).fillMaxWidth(), shape = BottomSheetDefaults.ExpandedShape, color = containerColor) {
            Column {
                Box(modifier = Modifier.align(Alignment.CenterHorizontally)) { BottomSheetDefaults.DragHandle() }
                content()
            }
        }
    }
}
