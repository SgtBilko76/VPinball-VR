package org.vpinball.app.ui.screens.landing

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import java.net.URLEncoder
import java.util.concurrent.TimeUnit
import javax.xml.parsers.DocumentBuilderFactory
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.vpinball.app.TableManager
import org.vpinball.app.VPinballManager
import org.vpinball.app.jni.VPinballLogLevel
import org.vpinball.app.jni.VPinballStatus

/** One table folder of the Visual Pinball Collection on archive.org. */
data class ArchiveTableEntry(
    val name: String,
    val vpxPath: String,
    val vpxSize: Long,
    val b2sPath: String?,
    val b2sSize: Long,
) {
    val displayName: String
        get() = name

    val totalSize: Long
        get() = vpxSize + b2sSize
}

class TableDownloaderViewModel(private val context: Context) : ViewModel() {
    companion object {
        const val ITEM = "Visual-Pinball-Collection-2025-12-29"
        const val BASE_URL = "https://archive.org/download/$ITEM"
        private const val FILES_XML = "$BASE_URL/${ITEM}_files.xml"
    }

    private val _entries = MutableStateFlow<List<ArchiveTableEntry>>(emptyList())
    val entries: StateFlow<List<ArchiveTableEntry>> = _entries.asStateFlow()

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()

    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status.asStateFlow()

    /** Download progress 0..1, or null when no download is running. */
    private val _progress = MutableStateFlow<Float?>(null)
    val progress: StateFlow<Float?> = _progress.asStateFlow()

    private val _downloading = MutableStateFlow<String?>(null)
    val downloading: StateFlow<String?> = _downloading.asStateFlow()

    // ROM and NVRAM files in the collection, keyed by lower-case ROM name.
    private val roms = mutableMapOf<String, String>()
    private val nvrams = mutableMapOf<String, String>()

    private var loaded = false
    private var downloadJob: Job? = null

    private val client =
        OkHttpClient.Builder().followRedirects(true).connectTimeout(20, TimeUnit.SECONDS).readTimeout(60, TimeUnit.SECONDS).build()

    fun loadIndex() {
        if (loaded || _isLoading.value) return
        viewModelScope.launch {
            _isLoading.value = true
            _status.value = "Fetching table list..."
            try {
                val list = withContext(Dispatchers.IO) { fetchIndex() }
                _entries.value = list
                loaded = true
                _status.value = "${list.size} tables. Select one to download it."
            } catch (e: Exception) {
                VPinballManager.log(VPinballLogLevel.ERROR, "Table list failed: $e")
                _status.value = "Could not load the table list: ${describe(e)}"
            } finally {
                _isLoading.value = false
            }
        }
    }

    private fun fetchIndex(): List<ArchiveTableEntry> {
        val request = Request.Builder().url(FILES_XML).build()
        val doc =
            client.newCall(request).execute().use { response ->
                if (!response.isSuccessful) throw Exception("HTTP ${response.code}")
                val body = response.body ?: throw Exception("Empty response")
                DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(body.byteStream())
            }

        data class Folder(var vpx: Pair<String, Long>? = null, var b2s: Pair<String, Long>? = null)
        val folders = sortedMapOf<String, Folder>(String.CASE_INSENSITIVE_ORDER)
        roms.clear()
        nvrams.clear()

        val nodes = doc.getElementsByTagName("file")
        for (i in 0 until nodes.length) {
            val el = nodes.item(i) as org.w3c.dom.Element
            val name = el.getAttribute("name")
            val size = el.getElementsByTagName("size").item(0)?.textContent?.trim()?.toLongOrNull() ?: 0L
            val parts = name.split('/')
            when {
                parts.size == 2 && parts[0] == "roms" && name.endsWith(".zip", true) -> roms[parts[1].dropLast(4).lowercase()] = name
                parts.size == 2 && parts[0] == "nvram" && name.endsWith(".nv", true) -> nvrams[parts[1].dropLast(3).lowercase()] = name
                parts.size == 3 && parts[0] == "tables" -> {
                    val folder = folders.getOrPut(parts[1]) { Folder() }
                    if (name.endsWith(".vpx", true) && folder.vpx == null) folder.vpx = name to size
                    if (name.endsWith(".directb2s", true) && folder.b2s == null) folder.b2s = name to size
                }
            }
        }

        return folders.mapNotNull { (name, f) ->
            val vpx = f.vpx ?: return@mapNotNull null
            ArchiveTableEntry(name = name, vpxPath = vpx.first, vpxSize = vpx.second, b2sPath = f.b2s?.first, b2sSize = f.b2s?.second ?: 0L)
        }
    }

    fun downloadEntry(entry: ArchiveTableEntry) {
        if (downloadJob?.isActive == true) {
            _status.value = "Please wait, ${_downloading.value} is still downloading."
            return
        }
        downloadJob =
            viewModelScope.launch {
                _downloading.value = entry.name
                _progress.value = 0f
                val work = File(context.cacheDir, "archive_download").apply {
                    deleteRecursively()
                    mkdirs()
                }
                try {
                    val extras = mutableMapOf<String, File>()
                    val totalBytes = entry.totalSize.coerceAtLeast(1)
                    var doneBytes = 0L

                    _status.value = "Downloading ${entry.name} (${mb(entry.vpxSize)})..."
                    val vpx = File(work, entry.vpxPath.substringAfterLast('/'))
                    download(entry.vpxPath, vpx) { bytes -> _progress.value = (doneBytes + bytes).toFloat() / totalBytes }
                    doneBytes += entry.vpxSize

                    entry.b2sPath?.let { path ->
                        _status.value = "Downloading backglass (${mb(entry.b2sSize)})..."
                        val b2s = File(work, path.substringAfterLast('/'))
                        download(path, b2s) { bytes -> _progress.value = (doneBytes + bytes).toFloat() / totalBytes }
                        extras[b2s.name] = b2s
                    }

                    _status.value = "Looking for the ROM..."
                    val romName = withContext(Dispatchers.IO) { findRomName(vpx) }
                    var romNote = "no ROM needed"
                    if (romName != null) {
                        val romPath = roms[romName.lowercase()]
                        if (romPath != null) {
                            _status.value = "Downloading ROM $romName..."
                            val rom = File(work, romPath.substringAfterLast('/'))
                            download(romPath, rom) {}
                            extras["pinmame/roms/${rom.name}"] = rom
                            nvrams[romName.lowercase()]?.let { nvPath ->
                                val nv = File(work, nvPath.substringAfterLast('/'))
                                runCatching { download(nvPath, nv) {} }.onSuccess { extras["pinmame/nvram/${nv.name}"] = nv }
                            }
                            romNote = "ROM $romName"
                        } else {
                            romNote = "ROM $romName is not in the collection"
                        }
                    }

                    _status.value = "Importing ${entry.name}..."
                    _progress.value = null
                    val table = TableManager.getInstance().importDownloadedTable(vpx, extras)
                    _status.value = if (table != null) "${entry.name} is ready ($romNote)." else "Import of ${entry.name} failed."
                } catch (e: Exception) {
                    VPinballManager.log(VPinballLogLevel.ERROR, "Download of ${entry.name} failed: $e")
                    _status.value = if (!isActive) "Download cancelled." else "Download failed: ${describe(e)}"
                } finally {
                    withContext(Dispatchers.IO) { work.deleteRecursively() }
                    _progress.value = null
                    _downloading.value = null
                }
            }
    }

    fun cancelDownload() {
        downloadJob?.cancel()
    }

    /** Downloads with up to 3 attempts: archive.org sometimes answers with a temporary 5xx error. */
    private suspend fun download(path: String, dest: File, onBytes: (Long) -> Unit) {
        var attempt = 1
        while (true) {
            try {
                return downloadOnce(path, dest, onBytes)
            } catch (e: ServerBusyException) {
                if (attempt >= 3) throw Exception("HTTP ${e.code} for ${path.substringAfterLast('/')} (archive.org is busy, try again later)")
                _status.value = "archive.org is busy, retrying (${attempt + 1}/3)..."
                kotlinx.coroutines.delay(3000L * attempt)
                attempt++
            }
        }
    }

    private class ServerBusyException(val code: Int) : Exception("HTTP $code")

    private suspend fun downloadOnce(path: String, dest: File, onBytes: (Long) -> Unit) =
        withContext(Dispatchers.IO) {
            val request = Request.Builder().url(urlFor(path)).build()
            val call = client.newCall(request)
            coroutineContext[Job]?.invokeOnCompletion { if (it != null) call.cancel() }
            call.execute().use { response ->
                if (response.code >= 500) throw ServerBusyException(response.code)
                if (!response.isSuccessful) throw Exception("HTTP ${response.code} for ${path.substringAfterLast('/')}")
                val body = response.body ?: throw Exception("Empty response")
                body.byteStream().use { input ->
                    dest.outputStream().use { output ->
                        val buffer = ByteArray(256 * 1024)
                        var total = 0L
                        var lastReport = 0L
                        while (true) {
                            val n = input.read(buffer)
                            if (n < 0) break
                            output.write(buffer, 0, n)
                            total += n
                            if (total - lastReport > 512 * 1024) {
                                lastReport = total
                                withContext(Dispatchers.Main) { onBytes(total) }
                            }
                        }
                        withContext(Dispatchers.Main) { onBytes(total) }
                    }
                }
            }
        }

    /** Reads the ROM name from the table script (cGameName = "..."). */
    private fun findRomName(vpx: File): String? {
        if (VPinballManager.vpinballJNI.VPinballExtractTableScript(vpx.absolutePath) != VPinballStatus.SUCCESS.value) return null
        val script = File(vpx.parentFile, vpx.nameWithoutExtension + ".vbs")
        if (!script.exists()) return null
        val text = script.readText(Charsets.ISO_8859_1)
        script.delete()
        val patterns =
            listOf(
                Regex("""(?im)^\s*(?:Const\s+|Dim\s+)?cGameName\s*=\s*"([^"]+)""""),
                Regex("""(?im)\.GameName\s*=\s*"([^"]+)""""),
                Regex("""(?im)^\s*(?:Const\s+)?GameName\s*=\s*"([^"]+)""""),
            )
        for (p in patterns) {
            p.findAll(text).map { it.groupValues[1] }.firstOrNull { roms.containsKey(it.lowercase()) }?.let { return it }
        }
        // Fall back to the first quoted string that matches a ROM in the collection.
        Regex(""""([A-Za-z0-9_]{3,16})"""").findAll(text).map { it.groupValues[1] }.firstOrNull { roms.containsKey(it.lowercase()) }?.let { return it }
        return patterns.firstNotNullOfOrNull { p -> p.find(text)?.groupValues?.get(1) }
    }

    private fun urlFor(path: String): String =
        BASE_URL + "/" + path.split('/').joinToString("/") { URLEncoder.encode(it, "UTF-8").replace("+", "%20") }

    private fun mb(bytes: Long) = if (bytes >= 1024 * 1024) "${bytes / (1024 * 1024)} MB" else "${(bytes / 1024).coerceAtLeast(1)} KB"

    private fun describe(e: Exception): String =
        when (e) {
            is java.net.UnknownHostException -> "no internet connection"
            is java.net.SocketTimeoutException -> "the server did not answer in time"
            else -> e.message ?: e.javaClass.simpleName
        }
}
