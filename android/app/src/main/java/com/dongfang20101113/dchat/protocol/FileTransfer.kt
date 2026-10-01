package com.dongfang20101113.dchat.protocol

import java.util.Base64

/**
 * 文件传输的公共部分：Base64、文件名清理、分块大小、字节数格式化。
 *
 * C++ 端 `file_transfer.cpp` 的等价移植。因为要挡路径穿越和 Windows 非法字符，
 * 这里的清理规则**故意比 Android/Linux 更严**——收到的文件名会被交给系统存盘，
 * 严格一点没有坏处。
 */

/** 每个 `FILE_CHUNK` 携带的原始字节数。
 *
 * 协议是"一行一条"、单行上限 4096 字节，Base64 还会把体积撑大约 1/3，
 * 所以这里不能大：2048 字节 → 2732 个 Base64 字符，加上命令名/传输 ID 仍在安全范围内。
 */
const val FILE_CHUNK_BYTES: Int = 2048

/** 单个文件大小上限（服务器也可以用 documentsize 规则改小）。 */
const val MAX_FILE_BYTES: Long = 64L * 1024 * 1024

/** 文件名长度上限（清理后按 UTF-8 字节算）。 */
const val MAX_FILE_NAME_BYTES: Int = 120

/** 缩略图字节上限（与服务器一致）。 */
const val MAX_THUMB_BYTES: Int = 128 * 1024

/** 标准 Base64 编码（带 `=` 填充）。空输入返回空串。 */
fun base64Encode(data: ByteArray): String {
    if (data.isEmpty()) return ""
    return Base64.getEncoder().encodeToString(data)
}

/** 标准 Base64 解码；非法输入（含非法字符、长度不对、填充位置错误）返回 `null`。 */
fun base64Decode(text: String): ByteArray? {
    if (text.isEmpty()) return ByteArray(0)
    // C++ 端严格要求长度是 4 的倍数
    if (text.length % 4 != 0) return null
    return try {
        Base64.getDecoder().decode(text)
    } catch (_: IllegalArgumentException) {
        null
    }
}

/**
 * 清理收到的文件名：
 *  1. 只取最后一段（挡掉 `../../evil.exe`、`C:\Windows\x.exe` 这类路径穿越）
 *  2. 替换非法字符和控制字符为 `_`
 *  3. 去掉结尾的空格和点
 *  4. 全是点（`.` / `..`）视为空
 *  5. 太长就截断，但尽量保住扩展名
 *
 * 结果保证非空（兜底 `"file"`）。
 */
fun sanitizeFileName(name: String): String {
    // 1) 只取最后一段
    var begin = 0
    for (i in name.indices) {
        if (name[i] == '/' || name[i] == '\\') begin = i + 1
    }
    val rest = name.substring(begin)

    // 2) 替换非法字符和控制字符
    val cleaned = buildString(rest.length) {
        for (c in rest) {
            if (c.code < 0x20 || c.code == 0x7F) { append('_'); continue }
            when (c) {
                '<', '>', ':', '"', '/', '\\', '|', '?', '*' -> append('_')
                else -> append(c)
            }
        }
    }.let { text ->
        // 3) 去掉结尾的空格和点；顺便去掉开头的空格
        var s = text
        while (s.isNotEmpty() && (s.last() == ' ' || s.last() == '.')) s = s.dropLast(1)
        s.trimStart(' ')
    }

    // 4) 全是点也算空
    if (cleaned.isNotEmpty() && cleaned.all { it == '.' }) return "file"
    if (cleaned.isEmpty()) return "file"

    // 5) 太长就截断，尽量保住扩展名
    if (utf8ByteLength(cleaned) > MAX_FILE_NAME_BYTES) {
        val dot = cleaned.lastIndexOf('.')
        val hasUsableExt = dot > 0 && utf8ByteLength(cleaned.substring(dot)) <= 16
        return if (hasUsableExt) {
            val ext = cleaned.substring(dot)
            val stem = utf8TruncateBytes(cleaned.substring(0, dot), MAX_FILE_NAME_BYTES - utf8ByteLength(ext))
            stem + ext
        } else {
            utf8TruncateBytes(cleaned, MAX_FILE_NAME_BYTES)
        }
    }
    return cleaned
}

/**
 * 目录里已有同名文件时换成 `名字 (2).扩展名`，最多到 `(100)`。
 *
 * [exists] 由调用方注入（Android 上查真实文件系统），这样这个函数保持纯逻辑、可单测。
 */
fun makeUniqueName(fileName: String, exists: (String) -> Boolean): String {
    if (!exists(fileName)) return fileName

    val dot = fileName.lastIndexOf('.')
    val hasExt = dot > 0
    val stem = if (hasExt) fileName.substring(0, dot) else fileName
    val ext = if (hasExt) fileName.substring(dot) else ""

    for (index in 2..99) {
        val candidate = "$stem ($index)$ext"
        if (!exists(candidate)) return candidate
    }
    return "$stem (100)$ext"
}

/** `1234567` → `"1.2 MB"`；`1024` → `"1.0 KB"`。 */
fun formatBytes(bytes: Long): String = when {
    bytes < 1024 -> "$bytes B"
    // 固定用 Locale.US：跟系统语言走的话，某些地区会把小数点变成逗号，协议/日志里很难看
    bytes < 1024L * 1024 -> String.format(java.util.Locale.US, "%.1f KB", bytes / 1024.0)
    bytes < 1024L * 1024 * 1024 -> String.format(java.util.Locale.US, "%.1f MB", bytes / (1024.0 * 1024.0))
    else -> String.format(java.util.Locale.US, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0))
}

/**
 * 把文件切成协议的块序列。
 *
 * 抽成纯函数是为了能单测"最坏情况下 `FILE_DATA` 一行不超协议上限"——
 * 桌面端有同样的测试项（`test_file_transfer` 里那条）。
 */
fun chunkFile(data: ByteArray, chunkBytes: Int = FILE_CHUNK_BYTES): List<ByteArray> {
    if (data.isEmpty()) return emptyList()
    val out = ArrayList<ByteArray>((data.size + chunkBytes - 1) / chunkBytes)
    var offset = 0
    while (offset < data.size) {
        val end = minOf(offset + chunkBytes, data.size)
        out.add(data.copyOfRange(offset, end))
        offset = end
    }
    return out
}

/**
 * 最坏情况下单行的字节长度估算：命令名 + 传输 ID + Base64 后的块大小 + 空格。
 *
 * 用来在单测里断言不会越过 [MAX_LINE_BYTES]。
 */
fun worstCaseFileDataLineBytes(transferIdLength: Int, chunkBytes: Int = FILE_CHUNK_BYTES): Int {
    val base64Length = ((chunkBytes + 2) / 3) * 4
    // "FILE_DATA" (9) + 空格 + id + 空格 + base64
    return 9 + 1 + transferIdLength + 1 + base64Length
}
