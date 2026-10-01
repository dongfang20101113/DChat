package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.COPY_BUFFER_BYTES
import com.dongfang20101113.dchat.protocol.copyWithLimit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream

/**
 * 「有上限的流式复制」测试——**这是一个闪退事故的回归测试**。
 *
 * ## 事故经过
 *
 * 用户点「📎」选文件。如果 provider 不返回文件大小（`OpenableColumns.SIZE` 为 -1，
 * 网盘、部分相册、下载管理器都这样），旧实现走的是：
 *
 * ```kotlin
 * val bytes = stream.readBytes()   // ← 把整个文件读进内存
 * ```
 *
 * 选一个大视频就是一次 OutOfMemoryError 闪退。
 *
 * ## 现在的不变量
 *
 * 1. 内存占用与数据量**无关**，堆上始终只有一个缓冲区
 * 2. 超过上限**立刻停**，不会把剩下的读完
 * 3. 返回 -1 表示超限，调用方据此清理临时文件并提示用户
 */
class BoundedCopyTest {

    /**
     * 一个**假装很大**的流：只在内存里存一个块，反复吐 [totalBytes] 次。
     *
     * 关键在于它自己只占一个块的内存——如果被测代码试图全部读进内存，
     * 测试就会 OOM 而不是"碰巧通过"。
     */
    private class RepeatingStream(
        private val block: ByteArray,
        private val totalBytes: Long,
    ) : InputStream() {
        private var produced = 0L
        private var pos = 0
        var maxSingleRead: Int = 0
            private set

        override fun read(): Int {
            if (produced >= totalBytes) return -1
            val b = block[pos].toInt() and 0xFF
            pos = (pos + 1) % block.size
            produced++
            return b
        }

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (produced >= totalBytes) return -1
            val remaining = totalBytes - produced
            val want = minOf(len.toLong(), remaining, block.size.toLong()).toInt()
            if (want <= 0) return -1
            System.arraycopy(block, 0, b, off, want)
            produced += want
            maxSingleRead = maxOf(maxSingleRead, want)
            return want
        }
    }

    /** 只数数、不保存内容的输出流，避免测试自己吃掉内存。 */
    private class CountingSink : OutputStream() {
        var count = 0L
            private set

        override fun write(b: Int) { count++ }

        override fun write(b: ByteArray, off: Int, len: Int) { count += len }
    }

    // ------------------------------------------------------------------

    @Test
    fun `正常复制_返回真实字节数`() {
        val data = ByteArray(300_000) { (it % 251).toByte() }
        val out = ByteArrayOutputStream()
        val copied = copyWithLimit(ByteArrayInputStream(data), out, limit = 1_000_000)

        assertEquals(300_000L, copied)
        assertTrue("内容必须完整写出去", data.contentEquals(out.toByteArray()))
    }

    @Test
    fun `刚好等于上限_应当成功`() {
        val data = ByteArray(1024)
        val out = CountingSink()
        assertEquals(1024L, copyWithLimit(ByteArrayInputStream(data), out, limit = 1024))
        assertEquals(1024L, out.count)
    }

    @Test
    fun `超过上限一个字节_返回负一并且立刻停`() {
        val out = CountingSink()
        val copied = copyWithLimit(ByteArrayInputStream(ByteArray(1025)), out, limit = 1024)

        assertEquals("超限必须返回 -1", -1L, copied)
        // 允许写出去一个缓冲区那么多（发现超限时已经写了一批），但不该写完全部
        assertTrue("超限后不该继续写完", out.count <= 1024L + COPY_BUFFER_BYTES)
    }

    /**
     * 核心回归测试：**一个 4 GB 的流，内存占用必须是常数**。
     *
     * 4 GB 远超任何 JVM 测试堆——如果实现试图把它读进内存，这条测试必然 OOM 失败。
     */
    @Test
    fun `四个 G 的流不会把内存撑爆_而是到上限就停`() {
        val fourGb = 4L * 1024 * 1024 * 1024
        val stream = RepeatingStream(ByteArray(64 * 1024) { 7 }, fourGb)
        val sink = CountingSink()

        val limit = 8L * 1024 * 1024   // 8 MB 上限
        val copied = copyWithLimit(stream, sink, limit)

        assertEquals("应当因为超限返回 -1", -1L, copied)
        assertTrue("最多只该读到「上限 + 一个缓冲区」", sink.count <= limit + COPY_BUFFER_BYTES)
        assertTrue("单次读取不该超过缓冲区大小", stream.maxSingleRead <= COPY_BUFFER_BYTES)
    }

    @Test
    fun `上限为零时任何非空输入都超限`() {
        assertEquals(-1L, copyWithLimit(ByteArrayInputStream(ByteArray(1)), CountingSink(), limit = 0))
        assertEquals(0L, copyWithLimit(ByteArrayInputStream(ByteArray(0)), CountingSink(), limit = 0))
    }

    @Test
    fun `空输入返回零`() {
        val out = CountingSink()
        assertEquals(0L, copyWithLimit(ByteArrayInputStream(ByteArray(0)), out, limit = 100))
        assertEquals(0L, out.count)
    }

    @Test
    fun `负数上限直接判超限_不做任何写入`() {
        val out = CountingSink()
        assertEquals(-1L, copyWithLimit(ByteArrayInputStream(ByteArray(10)), out, limit = -5))
        assertEquals(0L, out.count)
    }

    @Test
    fun `缓冲区大小可以调小_行为不变`() {
        val data = ByteArray(5000) { 3 }
        val out = ByteArrayOutputStream()
        assertEquals(5000L, copyWithLimit(ByteArrayInputStream(data), out, limit = 10_000, bufferBytes = 128))
        assertTrue(data.contentEquals(out.toByteArray()))
    }

    @Test(expected = IllegalArgumentException::class)
    fun `缓冲区大小为零应该立刻报错_而不是死循环`() {
        copyWithLimit(ByteArrayInputStream(ByteArray(10)), CountingSink(), limit = 100, bufferBytes = 0)
    }

    @Test
    fun `默认缓冲区是 64 KB`() {
        // 这个常量决定了发大文件时的堆占用，钉住它避免被无意改大
        assertEquals(64 * 1024, COPY_BUFFER_BYTES)
    }
}
