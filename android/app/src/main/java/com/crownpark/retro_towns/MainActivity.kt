package com.crownpark.retro_towns

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import org.libsdl.app.SDLActivity
import java.io.File

class MainActivity : SDLActivity() {

    override fun getLibraries(): Array<String> =
        arrayOf("SDL3", "retrotowns")

    override fun getMainSharedObject(): String =
        "${applicationInfo.nativeLibraryDir}/libretrotowns.so"

    override fun getMainFunction(): String = "SDL_main"

    override fun getArguments(): Array<String> = arrayOf()

    companion object {
        private const val REQ_PICK_FOLDER = 0x5A70
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        SafBridge.activity = this
        writeCandidateRoots()
        super.onCreate(savedInstanceState)
    }

    override fun onDestroy() {
        SafBridge.activity = null
        super.onDestroy()
    }

    fun launchFolderPicker() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or
                     Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                     Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(i, REQ_PICK_FOLDER)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQ_PICK_FOLDER) {
            if (resultCode == Activity.RESULT_OK) data?.data?.let { SafBridge.onPicked(it) }
            return
        }
        super.onActivityResult(requestCode, resultCode, data)
    }

    private fun writeCandidateRoots() {
        try {
            val roots = getExternalFilesDirs(null)
                .filterNotNull()
                .map { File(it, "FM Towns") }
            for (r in roots) r.mkdirs()
            File(filesDir, "roots.txt")
                .writeText(roots.joinToString("\n") { it.absolutePath })
        } catch (_: Exception) {
        }
    }
}
