package com.dongfang20101113.dchat.protocol

/**
 * 把 TCP 字节流切成一行一行。
 *
 * 负责三件在真实网络里必然遇到的事：
 *  1. **半包**：一次 recv 只拿到半行 → 留在缓冲里等下次；
 *  2. **粘包**：一次 recv 拿到多行 → 循环取出；
 *  3. **超长行**：迟迟没有换行且超过 [MAX_LINE_BYTES] → 判定为异常数据，调用方应断开连接。
 *
 * 与 C++ 的 `LineBuffer` 行为一致：兼容 `\r\n`，超限置位 [bad]。
 *
 * 这一层操作的是**原始字节**，不是字符串——网络边界上按字节处理才不会在多字节字符处出错。
 */
class LineBuffer {
    private var raw = ByteArray(0)
    private var length = 0

    var bad: Boolean = false
        private set

    /** 追加一段收到的字节。 */
    fun append(data: ByteArray, offset: Int = 0, count: Int = data.size) {
        if (bad) return
        ensureCapacity(length + count)
        System.arraycopy(data, offset, raw, length, count)
        length += count

        // 一整段里没有换行、且已经超过单行上限：立刻判异常，避免缓冲区无界增长
        if (!containsNewline() && length > MAX_LINE_BYTES) bad = true
    }

    /**
     * 取出一行（已去掉结尾的 `\r\n` 或 `\n`）。没有完整行时返回 `null`。
     *
     * 返回的是 UTF-8 解码后的字符串；非法字节序列用替换字符兜底而不是抛异常——
     * 网络上收到脏数据不应该让客户端崩溃。
     */
    fun popLine(): String? {
        if (bad) return null
        val pos = indexOfNewline()
        if (pos < 0) {
            if (length > MAX_LINE_BYTES) bad = true
            return null
        }

        var lineEnd = pos
        if (lineEnd > 0 && raw[lineEnd - 1] == '\r'.code.toByte()) lineEnd--

        val lineBytes = raw.copyOfRange(0, lineEnd)
        // 把这一行连同换行符一起移除
        val restCount = length - (pos + 1)
        System.arraycopy(raw, pos + 1, raw, 0, restCount)
        length = restCount

        if (lineBytes.size > MAX_LINE_BYTES) {
            bad = true
            return null
        }
        return String(lineBytes, Charsets.UTF_8)
    }

    fun reset() {
        length = 0
        bad = false
    }

    private fun ensureCapacity(need: Int) {
        if (raw.size >= need) return
        var cap = if (raw.isEmpty()) 1024 else raw.size * 2
        while (cap < need) cap *= 2
        raw = raw.copyOf(cap)
    }

    private fun containsNewline(): Boolean {
        for (i in 0 until length) if (raw[i] == '\n'.code.toByte()) return true
        return false
    }

    private fun indexOfNewline(): Int {
        for (i in 0 until length) if (raw[i] == '\n'.code.toByte()) return i
        return -1
    }
}
