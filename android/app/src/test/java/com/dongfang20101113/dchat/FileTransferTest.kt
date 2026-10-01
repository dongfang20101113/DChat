package com.dongfang20101113.dchat

import com.dongfang20101113.dchat.protocol.FILE_CHUNK_BYTES
import com.dongfang20101113.dchat.protocol.MAX_FILE_NAME_BYTES
import com.dongfang20101113.dchat.protocol.MAX_LINE_BYTES
import com.dongfang20101113.dchat.protocol.base64Decode
import com.dongfang20101113.dchat.protocol.base64Encode
import com.dongfang20101113.dchat.protocol.chunkFile
import com.dongfang20101113.dchat.protocol.formatBytes
import com.dongfang20101113.dchat.protocol.makeUniqueName
import com.dongfang20101113.dchat.protocol.sanitizeFileName
import com.dongfang20101113.dchat.protocol.utf8ByteLength
import com.dongfang20101113.dchat.protocol.worstCaseFileDataLineBytes
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 文件传输公共部分测试（对应桌面端 `test_file_transfer`）。
 *
 * 文件名清理是**安全相关**的：收到的名字会被直接拿去存盘，
 * 所以路径穿越、非法字符、结尾点和空格这些用例必须钉死。
 */
class FileTransferTest {

    // ------------------------------------------------------------------
    // Base64
    // ------------------------------------------------------------------

    @Test
    fun `Base64 往返_覆盖所有字节值`() {
        val data = ByteArray(256) { it.toByte() }
        val encoded = base64Encode(data)
        val decoded = base64Decode(encoded)
        assertArrayEquals(data, decoded)
    }

    @Test
    fun `Base64 往返_各种长度`() {
        for (size in 0..40) {
            val data = ByteArray(size) { (it * 7).toByte() }
            assertArrayEquals("长度 $size 往返失败", data, base64Decode(base64Encode(data)))
        }
    }

    @Test
    fun `Base64 使用标准字母表_和 RFC 4648 一致`() {
        // "Man" -> "TWFu" 是 RFC 4648 的标准测试向量
        assertEquals("TWFu", base64Encode("Man".toByteArray(Charsets.UTF_8)))
        assertEquals("TWE=", base64Encode("Ma".toByteArray(Charsets.UTF_8)))
        assertEquals("TQ==", base64Encode("M".toByteArray(Charsets.UTF_8)))
    }

    @Test
    fun `Base64 非法输入返回 null`() {
        assertNull("长度不是 4 的倍数", base64Decode("abc"))
        assertNull("含非法字符", base64Decode("ab!d"))
        assertNull("填充位置错误", base64Decode("ab=c"))
    }

    @Test
    fun `空输入解码成空数组`() {
        assertArrayEquals(ByteArray(0), base64Decode(""))
        assertEquals("", base64Encode(ByteArray(0)))
    }

    @Test
    fun `中文文件名往返编码`() {
        val name = "季度报告（终版）.pdf"
        val encoded = base64Encode(name.toByteArray(Charsets.UTF_8))
        val decoded = String(base64Decode(encoded)!!, Charsets.UTF_8)
        assertEquals(name, decoded)
    }

    // ------------------------------------------------------------------
    // 文件名清理（安全相关）
    // ------------------------------------------------------------------

    @Test
    fun `路径穿越被挡掉`() {
        assertEquals("evil.exe", sanitizeFileName("../../evil.exe"))
        assertEquals("evil.exe", sanitizeFileName("..\\..\\evil.exe"))
        assertEquals("passwd", sanitizeFileName("/etc/passwd"))
        assertEquals("x.exe", sanitizeFileName("C:\\Windows\\System32\\x.exe"))
        assertEquals("d.txt", sanitizeFileName("a/b/c/d.txt"))
    }

    @Test
    fun `非法字符被替换成下划线`() {
        assertEquals("a_b_c.txt", sanitizeFileName("a<b>c.txt"))
        assertEquals("a_b.txt", sanitizeFileName("a:b.txt"))
        assertEquals("a_b.txt", sanitizeFileName("a|b.txt"))
        assertEquals("a_b.txt", sanitizeFileName("a?b.txt"))
        assertEquals("a_b.txt", sanitizeFileName("a*b.txt"))
        assertEquals("a_b.txt", sanitizeFileName("a\"b.txt"))
    }

    @Test
    fun `控制字符被替换`() {
        assertEquals("a_b", sanitizeFileName("a\u0000b"))
        assertEquals("a_b", sanitizeFileName("a\u0007b"))
        assertEquals("a_b", sanitizeFileName("a\u007Fb"))
    }

    @Test
    fun `结尾的空格和点被去掉`() {
        assertEquals("name", sanitizeFileName("name..."))
        assertEquals("name", sanitizeFileName("name   "))
        assertEquals("name.txt", sanitizeFileName("name.txt..."))
    }

    @Test
    fun `全是点的名字兜底成 file`() {
        assertEquals("file", sanitizeFileName("."))
        assertEquals("file", sanitizeFileName(".."))
        assertEquals("file", sanitizeFileName("..."))
        assertEquals("file", sanitizeFileName(""))
        assertEquals("file", sanitizeFileName("   "))
    }

    @Test
    fun `中文和 emoji 文件名原样保留`() {
        assertEquals("报告.pdf", sanitizeFileName("报告.pdf"))
        assertEquals("照片😀.jpg", sanitizeFileName("照片😀.jpg"))
    }

    @Test
    fun `超长文件名按 UTF-8 字节截断_并保住扩展名`() {
        val long = "a".repeat(300) + ".txt"
        val cleaned = sanitizeFileName(long)
        assertTrue("不应超过 $MAX_FILE_NAME_BYTES 字节，实际 ${utf8ByteLength(cleaned)}",
            utf8ByteLength(cleaned) <= MAX_FILE_NAME_BYTES)
        assertTrue("扩展名要保住: $cleaned", cleaned.endsWith(".txt"))
    }

    @Test
    fun `超长中文文件名不会把字符切一半`() {
        val long = "测试文件名".repeat(40) + ".dat"
        val cleaned = sanitizeFileName(long)
        assertTrue(utf8ByteLength(cleaned) <= MAX_FILE_NAME_BYTES)
        // 截断后必须是合法 UTF-8（重新编码再解码应该完全一致）
        assertEquals(cleaned, String(cleaned.toByteArray(Charsets.UTF_8), Charsets.UTF_8))
        assertTrue(cleaned.endsWith(".dat"))
    }

    @Test
    fun `扩展名超长时不保留扩展名_直接截断`() {
        val weird = "a".repeat(200) + "." + "b".repeat(30)
        val cleaned = sanitizeFileName(weird)
        assertTrue(utf8ByteLength(cleaned) <= MAX_FILE_NAME_BYTES)
    }

    // ------------------------------------------------------------------
    // 重名处理
    // ------------------------------------------------------------------

    @Test
    fun `不一致重名时原样返回`() {
        assertEquals("a.txt", makeUniqueName("a.txt") { false })
    }

    @Test
    fun `重名时加序号`() {
        val taken = setOf("a.txt", "a (2).txt", "a (3).txt")
        assertEquals("a (4).txt", makeUniqueName("a.txt") { it in taken })
    }

    @Test
    fun `没有扩展名的文件也能加序号`() {
        assertEquals("README (2)", makeUniqueName("README") { it == "README" })
    }

    @Test
    fun `最多试到 100`() {
        val all = (2..99).map { "a ($it).txt" }.toSet() + "a.txt"
        assertEquals("a (100).txt", makeUniqueName("a.txt") { it in all })
    }

    // ------------------------------------------------------------------
    // 字节数格式化
    // ------------------------------------------------------------------

    @Test
    fun `字节数格式化`() {
        assertEquals("0 B", formatBytes(0))
        assertEquals("1023 B", formatBytes(1023))
        assertEquals("1.0 KB", formatBytes(1024))
        assertEquals("1.5 KB", formatBytes(1536))
        assertEquals("1.0 MB", formatBytes(1024L * 1024))
        assertEquals("1.0 GB", formatBytes(1024L * 1024 * 1024))
    }

    @Test
    fun `格式化不使用本地化小数点_避免变成逗号`() {
        // 固定 Locale.US，任何系统语言下都该是点号
        assertTrue(formatBytes(1536).contains('.'))
    }

    // ------------------------------------------------------------------
    // 分块
    // ------------------------------------------------------------------

    @Test
    fun `分块大小与块数正确`() {
        val data = ByteArray(5000)
        val chunks = chunkFile(data)
        assertEquals(3, chunks.size)
        assertEquals(FILE_CHUNK_BYTES, chunks[0].size)
        assertEquals(FILE_CHUNK_BYTES, chunks[1].size)
        assertEquals(5000 - FILE_CHUNK_BYTES * 2, chunks[2].size)
        assertEquals(5000, chunks.sumOf { it.size })
    }

    @Test
    fun `整除时不会多出空块`() {
        val chunks = chunkFile(ByteArray(FILE_CHUNK_BYTES * 2))
        assertEquals(2, chunks.size)
        assertTrue(chunks.none { it.isEmpty() })
    }

    @Test
    fun `空文件不分块`() {
        assertTrue(chunkFile(ByteArray(0)).isEmpty())
    }

    @Test
    fun `最坏情况下 FILE_DATA 一行不会超协议上限`() {
        // 传输 ID 最长 64 字符（服务端的 IsValidTransferId 限制）
        val worst = worstCaseFileDataLineBytes(transferIdLength = 64)
        assertTrue("最坏一行 $worst 字节，超过了 $MAX_LINE_BYTES", worst <= MAX_LINE_BYTES)
    }
}
