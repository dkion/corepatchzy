package org.lsposed.corepatch.zygisk

import java.io.File

/**
 * Reads CorePatch's toggles from the module's config file
 * (moduleDir/config/corepatch.conf, seeded by customize.sh, one `key=0|1` per
 * line). Replaces LSPosed's remote preferences: there is no companion app UI,
 * so the user edits the file directly (or flips defaults in customize.sh).
 */
object ConfigStore {

    private const val CONFIG_FILE = "config/corepatch.conf"

    private val cache = HashMap<String, Boolean>()
    private var lastLoadMs = 0L
    private val lock = Any()

    private fun configFile(): File? {
        return if (Runtime.moduleDir.isNotEmpty()) {
            File(Runtime.moduleDir, CONFIG_FILE)
        } else {
            null
        }
    }

    private fun reloadIfNeeded() {
        val f = configFile() ?: return
        val mtime = f.lastModified()
        if (mtime == lastLoadMs && cache.isNotEmpty()) return
        lastLoadMs = mtime
        cache.clear()
        runCatching {
            f.forEachLine { line ->
                val t = line.trim()
                if (t.isEmpty() || t.startsWith("#")) return@forEachLine
                val idx = t.indexOf('=')
                if (idx > 0) {
                    val key = t.substring(0, idx).trim()
                    val value = t.substring(idx + 1).trim()
                    if (key.isNotEmpty()) {
                        cache[key] = (value == "1" || value.equals("true", true))
                    }
                }
            }
        }
    }

    /** Match the `SharedPreferences.getBoolean(key, def)` contract CorePatch uses. */
    fun getBoolean(key: String, def: Boolean): Boolean {
        synchronized(lock) {
            reloadIfNeeded()
            return cache[key] ?: def
        }
    }

    /** Write a toggle back to the config file (used by Config.setConfig). */
    fun setBoolean(key: String, value: Boolean) {
        synchronized(lock) {
            reloadIfNeeded()
            cache[key] = value
            val f = configFile() ?: return
            runCatching {
                f.parentFile?.mkdirs()
                val body = cache.entries.sortedBy { it.key }
                    .joinToString("\n") { "${it.key}=${if (it.value) 1 else 0}" } + "\n"
                f.writeText(body)
                lastLoadMs = f.lastModified()
            }
        }
    }
}
