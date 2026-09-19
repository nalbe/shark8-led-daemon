package com.bastet.ledgui

import java.util.concurrent.TimeUnit

/**
 * Root shell wrapper. Runs a command through the first working su binary
 * (KernelSU first, then the usual system paths). Every shell command here
 * is short-lived; stdout is captured, stderr is discarded.
 */
object Su {

    private val suCandidates = listOf(
        "/system/bin/su",       // KernelSU installs its su here (verified on Shark8)
        "su",                   // whatever is in the app PATH
        "/data/adb/ksu/bin/su", // KernelSU alternate location
        "/sbin/su",
        "/system/xbin/su"
    )

    data class Result(val out: String, val code: Int) {
        val ok: Boolean get() = code == 0
        /** Process was killed on timeout -> kernel is waiting for a decision. */
        val pending: Boolean get() = code == -1
    }

    /** True when some su binary answers with uid=0. */
    val isRootAvailable: Boolean
        get() = run("id").out.contains("uid=0")

    fun run(cmd: String, timeoutSec: Long = 10): Result {
        // Fast path: one persistent root shell handles every command on a
        // stdin/stdout pipe - no per-call su fork, no fresh supervisor
        // session, no SELinux audit noise from the render path.
        return try {
            val out = SuShell.exec(cmd, timeoutSec * 1000)
            Result(out, 0)
        } catch (_: IllegalStateException) {
            legacyRun(cmd, timeoutSec)
        }
    }

    /** One-shot root probe over every su candidate (no persistent shell).
     *  Used by the UI's "Request root": legacy pending (code -1) means su
     *  spawned and is stuck waiting on the manager's dialog. */
    fun probeRootAny(cmd: String = "id", timeoutSec: Long = 8): Result =
        legacyRun(cmd, timeoutSec)

    private fun legacyRun(cmd: String, timeoutSec: Long): Result {
        for (su in suCandidates) {
            val proc = try {
                Runtime.getRuntime().exec(arrayOf(su, "-c", cmd))
            } catch (_: Exception) {
                continue
            }
            proc.errorStream.close()

            val out = try {
                proc.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            } catch (_: Exception) {
                ""
            }

            val finished = try {
                proc.waitFor(timeoutSec, TimeUnit.SECONDS)
            } catch (_: Exception) {
                false
            }
            if (!finished) {
                proc.destroyForcibly()
                return Result(out.trim(), -1)   // still waiting on a root dialog
            }
            val code = proc.exitValue()
            if (code == 127) continue           // su started but cmd missing: try next
            return Result(out.trim(), code)     // 0 = ok, anything else = denied/error
        }
        return Result("", 255)
    }

    /** Atomically replace [path] with [content] (tmp + rename on the device). */
    fun writeFile(path: String, content: String, timeoutSec: Long = 30): Result {
        val tmp = "$path.tmp"
        var last = Result("", 255)
        for (su in suCandidates) {
            val proc = try {
                Runtime.getRuntime().exec(
                    arrayOf(su, "-c", "cat > \"$tmp\" && mv -f \"$tmp\" \"$path\"")
                )
            } catch (_: Exception) {
                continue
            }
            proc.errorStream.close()
            try {
                proc.outputStream.bufferedWriter(Charsets.UTF_8).use { it.write(content) }
            } catch (_: Exception) {
            }
            val out = try {
                proc.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            } catch (_: Exception) {
                ""
            }
            val finished = try {
                proc.waitFor(timeoutSec, TimeUnit.SECONDS)
            } catch (_: Exception) {
                false
            }
            if (!finished) {
                proc.destroyForcibly()
                continue
            }
            last = Result(out.trim(), proc.exitValue())
            if (last.code == 0) break
        }
        return last
    }
}