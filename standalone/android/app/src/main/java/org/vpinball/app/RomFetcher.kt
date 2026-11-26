package org.vpinball.app

import java.io.File
import java.net.URLEncoder
import java.util.concurrent.TimeUnit
import javax.xml.parsers.DocumentBuilderFactory
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.vpinball.app.jni.VPinballStatus

/**
 * Looks up and downloads a table's ROM (and NVRAM) from the Visual Pinball Collection on archive.org.
 *
 * Shared by the "Download from Archive.org" flow and the manual "Files" import so both attach the
 * same ROM. The collection's ROM/NVRAM index is fetched once and cached for the app's lifetime.
 */
object RomFetcher {
    const val ITEM = "Visual-Pinball-Collection-2025-12-29"
    const val BASE_URL = "https://archive.org/download/$ITEM"
    private const val FILES_XML = "$BASE_URL/${ITEM}_files.xml"

    private val client =
        OkHttpClient.Builder()
            .followRedirects(true)
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .build()

    // ROM and NVRAM files in the collection, keyed by lower-case ROM name.
    private val roms = mutableMapOf<String, String>()
    private val nvrams = mutableMapOf<String, String>()
    private val loadMutex = Mutex()
    @Volatile private var loaded = false

    /** Fetch (once) the collection's ROM/NVRAM index. Idempotent and safe to call from any flow. */
    suspend fun loadCollection() {
        if (loaded) return
        loadMutex.withLock {
            if (loaded) return@withLock
            withContext(Dispatchers.IO) {
                val request = Request.Builder().url(FILES_XML).build()
                client.newCall(request).execute().use { response ->
                    if (!response.isSuccessful) throw Exception("HTTP ${response.code}")
                    val body = response.body ?: throw Exception("Empty response")
                    val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(body.byteStream())
                    val nodes = doc.getElementsByTagName("file")
                    for (i in 0 until nodes.length) {
                        val el = nodes.item(i) as org.w3c.dom.Element
                        val name = el.getAttribute("name")
                        val parts = name.split('/')
                        when {
                            parts.size == 2 && parts[0] == "roms" && name.endsWith(".zip", true) ->
                                roms[parts[1].dropLast(4).lowercase()] = name

                            parts.size == 2 && parts[0] == "nvram" && name.endsWith(".nv", true) ->
                                nvrams[parts[1].dropLast(3).lowercase()] = name
                        }
                    }
                }
            }
            loaded = true
        }
    }

    /** Reads the ROM name from the table script (cGameName = "..."), preferring names present in the collection. */
    fun findRomName(vpx: File): String? {
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
        Regex("""\"([A-Za-z0-9_]{3,16})""").findAll(text).map { it.groupValues[1] }.firstOrNull { roms.containsKey(it.lowercase()) }?.let { return it }
        return patterns.firstNotNullOfOrNull { p -> p.find(text)?.groupValues?.get(1) }
    }

    /** Collection path of the ROM (e.g. "roms/indys.zip"), or null when it is not in the collection. */
    fun romPath(romName: String): String? = roms[romName.lowercase()]

    /** Collection path of the NVRAM (e.g. "nvram/indys.nv"), or null when it is not in the collection. */
    fun nvramPath(romName: String): String? = nvrams[romName.lowercase()]

    /** Downloads [path] from the collection into [dest]. Retries up to 3 times on temporary 5xx errors. */
    suspend fun download(path: String, dest: File) {
        var attempt = 1
        while (true) {
            try {
                downloadOnce(path, dest)
                return
            } catch (e: CancellationException) {
                throw e
            } catch (e: ServerBusyException) {
                if (attempt >= 3) throw Exception("HTTP ${e.code} for ${path.substringAfterLast('/')} (archive.org is busy, try again later)")
                delay(3000L * attempt)
                attempt++
            }
        }
    }

    private class ServerBusyException(val code: Int) : Exception("HTTP $code")

    private suspend fun downloadOnce(path: String, dest: File) =
        withContext(Dispatchers.IO) {
            val request = Request.Builder().url(urlFor(path)).build()
            ensureActive()
            client.newCall(request).execute().use { response ->
                if (response.code >= 500) throw ServerBusyException(response.code)
                if (!response.isSuccessful) throw Exception("HTTP ${response.code} for ${path.substringAfterLast('/')}")
                val body = response.body ?: throw Exception("Empty response")
                body.byteStream().use { input ->
                    dest.parentFile?.mkdirs()
                    dest.outputStream().use { output ->
                        val buffer = ByteArray(256 * 1024)
                        while (true) {
                            ensureActive()
                            val n = input.read(buffer)
                            if (n < 0) break
                            output.write(buffer, 0, n)
                        }
                    }
                }
            }
        }

    fun urlFor(path: String): String =
        BASE_URL + "/" + path.split('/').joinToString("/") { URLEncoder.encode(it, "UTF-8").replace("+", "%20") }
}
