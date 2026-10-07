package com.crownpark.retro_towns

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File

object SafBridge {
    private const val PREFS = "retro_towns_saf"
    private const val KEY = "trees"

    @Volatile @JvmStatic var activity: MainActivity? = null

    private fun ctx(): Context? = activity?.applicationContext

    @JvmStatic
    fun pick() {
        val a = activity ?: return
        a.runOnUiThread { a.launchFolderPicker() }
    }

    fun onPicked(uri: Uri) {
        val c = ctx() ?: return
        try {
            c.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        } catch (_: Exception) {}
        val s = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val have = s.getStringSet(KEY, emptySet())!!.toMutableSet()
        have.add(uri.toString())
        s.edit().putStringSet(KEY, have).apply()
        ensureLayout(uri.toString())
    }

    @JvmStatic
    fun trees(): String {
        val c = ctx() ?: return ""
        val s = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val saved = s.getStringSet(KEY, emptySet())!!
        val live = c.contentResolver.persistedUriPermissions.map { it.uri.toString() }.toSet()
        val out = StringBuilder()
        val keep = mutableSetOf<String>()
        for (u in saved) {
            val uri = Uri.parse(u)
            val doc = try { DocumentFile.fromTreeUri(c, uri) } catch (_: Exception) { null }
            if (doc == null || !doc.canRead()) continue
            if (live.isNotEmpty() && u !in live && !doc.canRead()) continue
            keep.add(u)
            out.append(u).append('\t').append(doc.name ?: u).append('\n')
        }
        if (keep != saved) s.edit().putStringSet(KEY, keep).apply()
        return out.toString()
    }

    @JvmStatic
    fun forget(treeUri: String) {
        val c = ctx() ?: return
        val s = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val have = s.getStringSet(KEY, emptySet())!!.toMutableSet()
        have.remove(treeUri)
        s.edit().putStringSet(KEY, have).apply()
        try {
            c.contentResolver.releasePersistableUriPermission(
                Uri.parse(treeUri),
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        } catch (_: Exception) {}
    }

    private val ALIASES = mapOf(
        "bios"  to listOf("bios", "bios roms", "system"),
        "cd"    to listOf("cd", "cds", "games", "roms", "discs", "disks", "iso", "isos"),
        "zip"   to listOf("zip", "zips", "compressed"),
    )

    private fun findFolder(root: DocumentFile, kind: String): DocumentFile? {
        val want = ALIASES[kind] ?: listOf(kind)
        val children = try { root.listFiles() } catch (_: Exception) { return null }
        var firstExisting: DocumentFile? = null
        for (alias in want) {
            for (f in children) {
                if (!f.isDirectory) continue
                if (f.name?.equals(alias, ignoreCase = true) != true) continue
                if (firstExisting == null) firstExisting = f
                val inside = try { f.listFiles() } catch (_: Exception) { emptyArray() }
                if (inside.isNotEmpty()) return f
            }
        }
        return firstExisting
    }

    @JvmStatic
    fun ensureLayout(treeUri: String): Boolean {
        val c = ctx() ?: return false
        val root = try { DocumentFile.fromTreeUri(c, Uri.parse(treeUri)) } catch (_: Exception) { null }
            ?: return false
        if (!root.canWrite()) return false
        for (kind in ALIASES.keys) {
            if (findFolder(root, kind) == null) root.createDirectory(kind)
        }
        return true
    }

    @JvmStatic
    fun folderName(treeUri: String, kind: String): String {
        val c = ctx() ?: return kind
        val root = try { DocumentFile.fromTreeUri(c, Uri.parse(treeUri)) } catch (_: Exception) { null }
            ?: return kind
        return findFolder(root, kind)?.name ?: kind
    }

    @JvmStatic
    fun list(treeUri: String, sub: String): String {
        val c = ctx() ?: return ""
        var dir = try { DocumentFile.fromTreeUri(c, Uri.parse(treeUri)) } catch (_: Exception) { null }
            ?: return ""
        if (sub.isNotEmpty()) dir = findFolder(dir, sub) ?: return ""
        val out = StringBuilder()
        for (f in dir.listFiles()) {
            if (f.isDirectory) continue
            out.append(f.name ?: continue).append('\t').append(f.length()).append('\n')
        }
        return out.toString()
    }

    @Volatile private var progress: Int = -1
    @JvmStatic fun stageProgress(): Int = progress

    @JvmStatic
    fun stage(treeUri: String, sub: String, name: String, destDir: String): String {
        val c = ctx() ?: return ""
        var dir = try { DocumentFile.fromTreeUri(c, Uri.parse(treeUri)) } catch (_: Exception) { null }
            ?: return ""
        if (sub.isNotEmpty()) dir = findFolder(dir, sub) ?: return ""
        val src = dir.findFile(name) ?: return ""
        val want = src.length()

        val dst = File(destDir, name)
        if (dst.exists() && dst.length() == want) return dst.absolutePath
        File(destDir).mkdirs()

        progress = 0
        try {
            c.contentResolver.openInputStream(src.uri).use { input ->
                if (input == null) return ""
                val tmp = File(destDir, "$name.part")
                tmp.outputStream().use { out ->
                    val buf = ByteArray(1 shl 20)
                    var done = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (want > 0) progress = ((done * 100) / want).toInt()
                    }
                }
                if (!tmp.renameTo(dst)) { tmp.delete(); return "" }
            }
        } catch (_: Exception) {
            return ""
        } finally {
            progress = -1
        }
        return dst.absolutePath
    }

    @JvmStatic
    fun realPath(treeUri: String): String {
        val c = ctx() ?: return ""
        val id = try {
            DocumentsContract.getTreeDocumentId(Uri.parse(treeUri))
        } catch (_: Exception) { return "" }
        val colon = id.indexOf(':')
        if (colon < 0) return ""
        val volume = id.substring(0, colon)
        val rel = id.substring(colon + 1)

        val direct = if (volume == "primary") {
            File(Environment.getExternalStorageDirectory(), rel)
        } else {
            File("/storage/$volume", rel)
        }
        if (direct.isDirectory && direct.canRead()) return direct.absolutePath

        for (base in c.getExternalFilesDirs(null).filterNotNull()) {
            val root = base.parentFile?.parentFile?.parentFile?.parentFile ?: continue
            val isPrimary = root.absolutePath.startsWith("/storage/emulated")
            if ((volume == "primary") != isPrimary) continue

            val candidate = File(root, rel)
            if (candidate.isDirectory && candidate.canRead()) return candidate.absolutePath
        }
        return ""
    }
}
