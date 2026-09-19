package com.bastet.ledgui

import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.nio.charset.StandardCharsets

/**
 * Persistent root shell: ONE `su -c "exec sh"` process lives as long as
 * the app, commands are fed through stdin and answers are read back with
 * unique markers.
 *
 * The old Su.run spawned a fresh su process per command (6-7 per status
 * poll, on top of untrusted-app sysfs reads that tripped SELinux audit).
 * A shell that stays alive removes those syscalls from the render path
 * and makes frequent live reads cheap.
 */
class RootShell {

    private var proc: Process? = null
    private var writer: OutputStream? = null
    private val buffered = StringBuilder()
    private val markerLock = Object()
    private var markerSeq = 0

    /** Spawn the shell once; returns false when su is unavailable. */
    private fun ensure(): Boolean = synchronized(this) {
        val p = proc
        if (p != null && p.isAlive) return true
        buffered.setLength(0)
        val np = try {
            Runtime.getRuntime().exec(arrayOf("/system/bin/su", "-c", "exec sh"))
        } catch (_: Exception) {
            return false
        }
        proc = np
        writer = np.outputStream
        val rd = Thread {
            val br = BufferedReader(
                InputStreamReader(np.inputStream, StandardCharsets.UTF_8), 64 * 1024
            )
            try {
                while (true) {
                    val line = br.readLine() ?: break
                    synchronized(markerLock) {
                        buffered.append(line).append('\n')
                        markerLock.notifyAll()
                    }
                }
            } catch (_: Exception) {
            }
        }
        rd.isDaemon = true
        rd.start()
        true
    }

    /**
     * Run [cmd] and return everything until its end marker.
     * Returns "" on timeout (root dialog still open, slow device).
     * Throws IllegalStateException when the shell is gone without root.
     *
     * Synchronized so concurrent callers (status poll + config reload on
     * the SAME shared main shell) can't interleave commands and garble the
     * response markers - that used to surface as a bogus "no root yet".
     */
    @Synchronized
    fun exec(cmd: String, timeoutMs: Long = 4000): String {
        if (!ensure()) throw IllegalStateException("no root shell")
        val marker = "__SUDONE${markerSeq++}__"
        val w = writer ?: throw IllegalStateException("no shell writer")

        synchronized(markerLock) {
            try {
                w.write("$cmd\necho \"$marker\"\n".toByteArray(StandardCharsets.UTF_8))
                w.flush()
            } catch (e: Exception) {
                proc?.destroy()
                proc = null
                throw IllegalStateException(e)
            }
            val start = buffered.length
            val deadline = System.currentTimeMillis() + timeoutMs
            while (System.currentTimeMillis() < deadline) {
                val idx = buffered.indexOf(marker, start)
                if (idx >= 0) {
                    val end = idx + marker.length + 1
                    val resp = buffered.substring(start, idx)
                    buffered.delete(start, end)
                    return resp
                }
                markerLock.wait(50)
            }
            // timeout: drop leftovers, caller retries on the next tick
            buffered.delete(0, buffered.length)
            return ""
        }
    }
}

object SuShell {

    /**
     * Two independent persistent root shells:
     *  - main: status polls, hooks, config load/save. Must stay responsive.
     *  - live: cheap frequent brightness reads. While the daemon blasts the
     *    sysfs rgb files (rainbow/charge animations) a read can stall for
     *    a while; keeping it on its own shell keeps the status ticking.
     */
    val main = RootShell()
    val live = RootShell()

    /** Kept for callers wired to the shared main shell only. */
    fun exec(cmd: String, timeoutMs: Long = 4000): String = main.exec(cmd, timeoutMs)
}