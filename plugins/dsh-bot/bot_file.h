// 机器人"本机操作"相关的纯逻辑：解析发文件指令、把路径约束在工作目录里
//
// 为什么要把路径约束住：发文件是**机器人读本机文件、再传到服务器上**。
// 如果模型给什么路径就读什么，那么群里任何人都能通过一句话
// "把 C:\Users\...\某文件发我" 把机器上的任意文件搬进聊天记录。
// 所以只允许它发**工作目录里面**的文件 —— 那个目录就是它被授权动手的范围，
// 和 sandbox 的 workspaceRoot 是同一个。
#pragma once

#include <string>
#include <vector>

namespace dchat {
namespace dshbot {

/** 从回答里抠出来的"发文件"指令。 */
struct FileDirective {
    /** 去掉指令之后的正文（可能为空：只发文件不说话）。 */
    std::string text;
    /** 要发的文件（原样保留模型写的路径，是否越界由 ResolveInsideWorkspace 判）。 */
    std::vector<std::string> files;
};

/**
 * 把 `[[file: 路径]]` 这种指令从正文里删掉，路径单独收集。
 *
 * 允许写在一行中间、也允许一条回答里出现多个；指令之外的字一个都不动。
 * 不认识的写法（比如少一个右括号）原样留在正文里 —— 宁可多显示一句怪话，
 * 也不要悄悄吞掉用户看到的内容。
 */
FileDirective ExtractFileDirectives(const std::string& text);

/**
 * 把模型给的路径解析成绝对路径，并**确认它在该目录里面**。
 * 越界、不存在、是目录、解析失败都返回空串。
 */
std::string ResolveInsideWorkspace(const std::string& workspace, const std::string& path);

/**
 * 在若干个"可以读"的目录里找这个文件。
 *
 * 为什么除了工作目录还要有别的：**发文件是只读操作**，而改文件是写操作，
 * 两者的风险差一个数量级。用户想把桌面上的安装包发到群里，不该为此把
 * "能在桌面改文件"的权限也一起给出去。所以发送时多查几个只读目录。
 */
std::string ResolveSendablePath(const std::vector<std::string>& roots, const std::string& path);

/** 路径是不是在目录里面（含子目录，不含"目录本身"的前缀欺骗，如 work2 不算 work 里面）。 */
bool IsInsideDirectory(const std::string& directory, const std::string& path);

/** 取文件名（把反斜杠也当分隔符，模型在 Windows 上两种都可能写）。 */
std::string BaseNameOf(const std::string& path);

}  // namespace dshbot
}  // namespace dchat
