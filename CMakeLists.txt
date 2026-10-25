cmake_minimum_required(VERSION 3.20)
project(dchat LANGUAGES CXX)

if(WIN32)
    enable_language(RC)   # 用于把应用图标编译进 exe
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# 静态链接 MinGW 运行时（libgcc / libstdc++ / winpthread）：这样发布出去的 exe
# 不依赖 libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll，
# 别人下载下来直接双击就能运行，不用先装编译器
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# 协议层、显示规则、输入历史、服务端指令解析：与网络、界面都无关，可单独做单元测试
# 密码学后端按平台选：Windows 走 CNG（系统自带），Linux 走 OpenSSL（libssl-dev）。
# 上层的 crypto.cpp / auth.cpp 只认 crypto_backend.h 里的接口，两端共用同一份源码。
if(WIN32)
    set(DCHAT_CRYPTO_BACKEND src/crypto_backend_win.cpp)
else()
    set(DCHAT_CRYPTO_BACKEND src/crypto_backend_openssl.cpp)
endif()

add_library(dchat_protocol STATIC src/socket_util.cpp src/protocol.cpp src/render.cpp src/input_history.cpp src/server_command.cpp src/auth.cpp src/file_transfer.cpp src/server_rules.cpp src/register_guard.cpp src/history_store.cpp src/room.cpp src/room_registry.cpp src/crypto.cpp ${DCHAT_CRYPTO_BACKEND})
target_include_directories(dchat_protocol PUBLIC src)
# 密码学后端各自要链的系统库：
#   Windows 用系统自带的 CNG（bcrypt.dll）
#   Linux 用 OpenSSL（libssl-dev 提供，find_package 找不到就直接报错，
#   比"编到一半才发现缺库"友好）
if(WIN32)
    target_link_libraries(dchat_protocol PUBLIC bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(dchat_protocol PUBLIC OpenSSL::Crypto)
endif()

# 服务器：控制台程序，支持多客户端 + 广播
add_executable(dchat_server src/server.cpp)
# ws2_32 只在 Windows 上存在；Linux 的 socket 在 libc 里（不需要额外链）
if(WIN32)
    target_link_libraries(dchat_server PRIVATE dchat_protocol ws2_32 Threads::Threads)
else()
    target_link_libraries(dchat_server PRIVATE dchat_protocol Threads::Threads)
endif()

# 界面绘制库：圆角控件（GDI+ 抗锯齿）+ 气泡布局，独立成库以便做像素级/几何测试
# 预览用 Windows 自带的缩略图提供者（shell32/ole32），所以放在 dchat_ui 里
# 语音：录音（waveIn/winmm）、压缩（ACM/msacm32）、播放（PlaySound/winmm）。
# 放在 dchat_ui 里是因为它和预览一样属于"系统能力"，不属于聊天协议。
# ---------------------------------------------------------------------------
# 界面层（GDI+ / Win32 / 系统缩略图）—— **只在 Windows 上构建**。
# 协议、加密、账号、规则这些是跨平台的，Linux 端只用 dchat_protocol。
# ---------------------------------------------------------------------------
if(WIN32)
add_library(dchat_ui STATIC src/rounded.cpp src/bubble.cpp src/image_preview.cpp src/voice_notes.cpp src/ui_layout.cpp src/chat_color.cpp src/color_picker.cpp src/picker_dialog.cpp)
target_include_directories(dchat_ui PUBLIC src)
target_link_libraries(dchat_ui PUBLIC gdiplus gdi32 user32 shell32 ole32 winmm)

# 客户端：Win32 图形界面（微信式气泡 + 圆角控件）
add_executable(dchat_client WIN32 src/client.cpp)
# dwmapi/uxtheme：让滚动条与窗口标题栏跟随深色模式
# comdlg32：选文件的对话框；shell32：窗口接受拖放文件
# crypt32：自动登录用 DPAPI 加密保存密码（绝不明文落盘）
target_link_libraries(dchat_client PRIVATE dchat_ui dchat_protocol ws2_32 user32 gdi32 dwmapi uxtheme comdlg32 shell32 crypt32 Threads::Threads)
target_include_directories(dchat_client PRIVATE resources)

# 应用图标（resources/dchat.ico 由 tools/make_dchat_icon.ps1 生成）
if(WIN32)
    configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    target_sources(dchat_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc resources/resource.h)
endif()

# 协议单元测试
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE dchat_protocol)

# 彩色聊天规则单元测试
add_executable(test_render tests/test_render.cpp)
target_link_libraries(test_render PRIVATE dchat_protocol)

# 圆角控件的像素级测试（离屏渲染 + 读像素）
add_executable(test_rounded tests/test_rounded.cpp)
target_link_libraries(test_rounded PRIVATE dchat_ui)

# 气泡布局测试（左右对齐、宽度上限、换行测量）
add_executable(test_bubble tests/test_bubble.cpp)
target_link_libraries(test_bubble PRIVATE dchat_ui)

# 图片/视频预览的类型判断测试
add_executable(test_image_preview tests/test_image_preview.cpp)
target_link_libraries(test_image_preview PRIVATE dchat_ui)

# 语音消息测试（判定纯函数 + WAV 拼装 + FILE_SEND 的字段数；不碰麦克风）
# 也要链 dchat_protocol：FILE_SEND 的字段拼装在那里（服务器按位置解析，错了整条被拒）
add_executable(test_voice_notes tests/test_voice_notes.cpp)
target_link_libraries(test_voice_notes PRIVATE dchat_ui dchat_protocol)

# 主窗口外壳的布局测试（菜单栏 / 状态条 / 输入行 / 加号菜单 / 设置窗口的几何）
add_executable(test_ui_layout tests/test_ui_layout.cpp)
target_link_libraries(test_ui_layout PRIVATE dchat_ui)

# 彩色文字代码：解析与排版是纯计算，单独测
add_executable(test_chat_color tests/test_chat_color.cpp)
target_link_libraries(test_chat_color PRIVATE dchat_ui)

# 取色盘：HSV 换算、几何与命中的纯计算
add_executable(test_color_picker tests/test_color_picker.cpp)
target_link_libraries(test_color_picker PRIVATE dchat_ui)

# 界面改版效果图：把提议的新布局离屏画成 PNG（复用真界面的配色/字号/圆角绘制）
# 不是测试目标，只在改版讨论时用：build\ui_mockup.exe [输出目录]
add_executable(picker_mockup tools/picker_mockup.cpp)
target_link_libraries(picker_mockup PRIVATE dchat_ui gdiplus gdi32 user32)

add_executable(icon_probe tools/icon_probe.cpp)
target_link_libraries(icon_probe PRIVATE dchat_ui gdiplus gdi32 user32)

add_executable(ui_mockup tools/ui_mockup.cpp)
target_link_libraries(ui_mockup PRIVATE dchat_ui dchat_protocol gdiplus gdi32 user32)

# 服务器规则（/chatrule）单元测试
add_executable(test_server_rules tests/test_server_rules.cpp)
target_link_libraries(test_server_rules PRIVATE dchat_protocol)

# 输入框历史记录测试（上键翻历史）
add_executable(test_history tests/test_history.cpp)
target_link_libraries(test_history PRIVATE dchat_protocol)

# 服务端指令解析测试（/ban /kick /unban 与时长写法）
add_executable(test_server_command tests/test_server_command.cpp)
target_link_libraries(test_server_command PRIVATE dchat_protocol)

# 账号与密码测试
add_executable(test_auth tests/test_auth.cpp)
target_link_libraries(test_auth PRIVATE dchat_protocol)

# 文件传输公共部分测试（Base64 / 文件名清理 / 分块）
add_executable(test_file_transfer tests/test_file_transfer.cpp)
target_link_libraries(test_file_transfer PRIVATE dchat_protocol)

# 传输加密测试（ECDH / HKDF / AES-GCM / 会话密钥）
add_executable(test_crypto tests/test_crypto.cpp)
target_link_libraries(test_crypto PRIVATE dchat_protocol)

endif()  # 界面层 & 只依赖它的测试到此为止；下面是跨平台目标

# ---------------------------------------------------------------------------
# Linux 客户端（终端界面 + 批处理模式）。
# 和 Windows 客户端共用协议层；界面那一套（GDI+）Linux 上没有，所以单独写一份。
# ---------------------------------------------------------------------------
if(NOT WIN32)
    # ---------------------------------------------------------------------------
    # 客户端代码分两层：
    #   client_core/  可移植核心（网络 / 信任 / 附件 / 颜色 / 语音）—— Linux 与 macOS 共用
    #   linux/client/ 终端界面 + 入口（Linux 专有）
    #
    # 分层的意义：界面怎么写都不影响逻辑，逻辑都留在能单测、能跨平台编译的核心层。
    # 后加 macOS 时只补界面，核心一行不用改。
    # ---------------------------------------------------------------------------
    add_library(dchat_client_core STATIC
        client_core/net.cpp
        client_core/trust.cpp
        client_core/chat_color.cpp
        client_core/files.cpp
        client_core/files_parse.cpp
        client_core/voice.cpp
        client_core/chat_core.cpp     # 会话逻辑：两个界面共用同一套指令解析
        client_core/cli_render.cpp)   # ANSI 渲染：纯函数，可单测、可交叉编译
    target_link_libraries(dchat_client_core PUBLIC dchat_protocol Threads::Threads)
    target_include_directories(dchat_client_core PUBLIC client_core)

    add_executable(dchat_client_linux
        linux/client/main.cpp
        linux/client/terminal.cpp
        linux/client/ui.cpp)
    target_link_libraries(dchat_client_linux PRIVATE dchat_client_core)
    target_include_directories(dchat_client_linux PRIVATE linux/client)

    # 网页版网关：一边 HTTP/WebSocket 对浏览器，一边用真实 dchat 协议连服务器。
    # 它**复用 client_core**（ChatCore + ClientConnection），所以网页版天然具备
    # 群聊/私聊/文件等全部能力，而且协议只在 client_core 里实现一份。
    # 只在这里构建（client_core 依赖 POSIX：voice.cpp 要 fork/exec arecord）。
    add_executable(dchat_webd webd/webd.cpp)
    target_link_libraries(dchat_webd PRIVATE dchat_client_core)
    target_include_directories(dchat_webd PRIVATE client_core)

    # 终端渲染层单独建一个库，好让 test_cli_render 直接链它。
    # 渲染错了不报错、只是"显示不对"，所以必须能单测。
    add_library(dchat_cli_render STATIC client_core/cli_render.cpp)
    target_link_libraries(dchat_cli_render PUBLIC dchat_protocol)
    target_include_directories(dchat_cli_render PUBLIC client_core)

    # 客户端核心的纯逻辑测试（色码解析 / TOFU / 附件解析 / 语音判定 / 多会话解析）。
    # 这几块错了不会报错，只会"显示不对"、"安全判定悄悄错"或者"群列表一片空白"，
    # 所以单独钉住。
    #
    # **直接链 dchat_client_core**，不再手工挑源文件：多会话那部分测试要构造
    # ChatCore（它在 chat_core.cpp 里），手工挑的话每加一个依赖就得改一次
    # 这个列表，漏一个就是一堆 undefined reference（第一次加房间测试就踩了）。
    # voice.cpp 里 fork 子进程的那部分在测试里不会被调用（只链接、不执行）。
    add_executable(test_linux_client tests/test_linux_client.cpp)
    target_link_libraries(test_linux_client PRIVATE dchat_client_core)
    target_include_directories(test_linux_client PRIVATE client_core)

    # 终端渲染层的测试：折行不能截断 ANSI、@我 高亮、色码按段上色、
    # 服务器关掉彩色时色码要原样显示。这些错了都只是"显示不对"，
    # 不测就只能在真终端里靠眼睛发现。
    add_executable(test_cli_render
        tests/test_cli_render.cpp
        client_core/cli_render.cpp
        client_core/chat_color.cpp)
    target_link_libraries(test_cli_render PRIVATE dchat_protocol)
    target_include_directories(test_cli_render PRIVATE client_core)
endif()

# ---------------------------------------------------------------------------
# macOS：客户端是原生 Cocoa 界面（ObjC++），复用 client_core + dchat_protocol。
#
# ⚠️ 这一段**从未被验证过**：开发机上没有 macOS SDK，zig 也不带 AppKit 头，
#    连语法检查都做不了。macos/main.mm 里刻意只用最保守的 AppKit API，
#    tools/check_objc_static.py 会在文本层面查"调用了没实现的 selector"，
#    但真正的编译错误只能在 Mac 上发现。
#
# 需要：brew install cmake ninja openssl@3 sox
# 构建：cmake -S . -B build -G Ninja -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
# ---------------------------------------------------------------------------
if(APPLE)
    add_library(dchat_client_core STATIC
        client_core/net.cpp
        client_core/trust.cpp
        client_core/chat_color.cpp
        client_core/files.cpp
        client_core/files_parse.cpp
        client_core/voice.cpp
        client_core/chat_core.cpp
        client_core/cli_render.cpp)
    target_link_libraries(dchat_client_core PUBLIC dchat_protocol Threads::Threads)
    target_include_directories(dchat_client_core PUBLIC client_core)

    add_executable(dchat_client_macos MACOSX_BUNDLE macos/main.mm)
    target_link_libraries(dchat_client_macos PRIVATE
        dchat_client_core
        "-framework Cocoa"
        "-framework Foundation"
        "-framework AppKit")
    target_compile_options(dchat_client_macos PRIVATE -fobjc-arc)
    set_target_properties(dchat_client_macos PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "dchat"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.dongfang20101113.dchat"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")

    # 客户端核心的纯逻辑测试在 macOS 上也能跑 —— 等于把三端一致性再验一次
    add_executable(test_client_core
        tests/test_linux_client.cpp
        client_core/chat_color.cpp
        client_core/trust.cpp
        client_core/files_parse.cpp
        client_core/voice.cpp)
    target_link_libraries(test_client_core PRIVATE dchat_protocol)
    target_include_directories(test_client_core PRIVATE client_core)
endif()

# ---------------------------------------------------------------------------
# 跨平台的测试目标：只依赖 dchat_protocol，Windows / Linux 都构建。
# Windows 上它们已经在上面（界面层区里）建过了，这里用 if(NOT TARGET) 兜住，
# 免得两边重复定义。
# ---------------------------------------------------------------------------
if(NOT TARGET test_protocol)
    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_render)
    add_executable(test_render tests/test_render.cpp)
    target_link_libraries(test_render PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_server_rules)
    add_executable(test_server_rules tests/test_server_rules.cpp)
    target_link_libraries(test_server_rules PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_history)
    add_executable(test_history tests/test_history.cpp)
    target_link_libraries(test_history PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_server_command)
    add_executable(test_server_command tests/test_server_command.cpp)
    target_link_libraries(test_server_command PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_auth)
    add_executable(test_auth tests/test_auth.cpp)
    target_link_libraries(test_auth PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_file_transfer)
    add_executable(test_file_transfer tests/test_file_transfer.cpp)
    target_link_libraries(test_file_transfer PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_crypto)
    add_executable(test_crypto tests/test_crypto.cpp)
    target_link_libraries(test_crypto PRIVATE dchat_protocol)
endif()
# 群文件加密（DCHATENC1 格式、IV 不重用、错密钥必须失败）。
# **必须注册进 ctest**：之前只在命令行手工跑过，ctest 里一直没它，
# 等于 CI 层面完全没覆盖加密这块。
if(NOT TARGET test_file_crypto)
    add_executable(test_file_crypto tests/test_file_crypto.cpp)
    target_link_libraries(test_file_crypto PRIVATE dchat_protocol)
endif()
# 断线重连的退避策略（纯函数）。重连这条路在真机上很难稳定复现
# （要拔网线、要服务器崩），所以把"等多久 / 什么时候放弃"抽出来单测。
if(NOT TARGET test_reconnect)
    add_executable(test_reconnect tests/test_reconnect.cpp)
    target_include_directories(test_reconnect PRIVATE src)
endif()
if(NOT TARGET test_history_store)
    add_executable(test_history_store tests/test_history_store.cpp)
    target_link_libraries(test_history_store PRIVATE dchat_protocol)
endif()
if(NOT TARGET test_register_guard)
    add_executable(test_register_guard tests/test_register_guard.cpp)
    target_link_libraries(test_register_guard PRIVATE dchat_protocol)
endif()
# 房间模型（大厅/群聊/私聊）的纯逻辑测试：ID 规则、加入退出审核踢人、群设置权限、
# 搜索、序列化。跨平台目标 —— Windows 和 Linux 都要能编、能跑。
if(NOT TARGET test_room)
    add_executable(test_room tests/test_room.cpp)
    target_link_libraries(test_room PRIVATE dchat_protocol)
endif()
# 服务端房间注册表：房间生命周期、私聊幂等、会话列表顺序、搜索、每房间独立历史、预算裁剪。
if(NOT TARGET test_room_registry)
    add_executable(test_room_registry tests/test_room_registry.cpp)
    target_link_libraries(test_room_registry PRIVATE dchat_protocol)
endif()
# 多会话协议的端到端探针：直接说协议、不依赖任何界面。
# 它测的是单测测不到的东西（命令有没有真的接上、消息有没有真的发对人），
# 由 tools/run-rooms-e2e.ps1 驱动。两端都建：Linux 上也能拿它验服务端。
add_executable(room_probe tools/room_probe.cpp)
if(WIN32)
    target_link_libraries(room_probe PRIVATE dchat_protocol ws2_32 Threads::Threads)
else()
    target_link_libraries(room_probe PRIVATE dchat_protocol Threads::Threads)
endif()
# ---------------------------------------------------------------------------
# DeepSeek Harness ↔ dchat 机器人插件（**可选**，默认构建但完全独立）
#
# 它不在服务端进程里：一个普通账号 + 一套自己的协议客户端，装不装、启不启都不影响
# dchat_server。关掉它的构建：cmake -DDCHAT_DSH_BOT=OFF
#
# 纯逻辑部分（配置 / 分条 / 白黑名单 / 事件解析 / 子进程）单独成一个库，
# 这样单测不用起 socket、也不用起模型。
# ---------------------------------------------------------------------------
option(DCHAT_DSH_BOT "构建 DeepSeek Harness 机器人插件（dchat-dsh-bot）" ON)
if(DCHAT_DSH_BOT)
    add_library(dchat_dsh_bot_core STATIC
        plugins/dsh-bot/bot_config.cpp
        plugins/dsh-bot/bot_chunk.cpp
        plugins/dsh-bot/bot_policy.cpp
        plugins/dsh-bot/bot_event.cpp
        plugins/dsh-bot/bot_file.cpp
        plugins/dsh-bot/bot_json.cpp
        plugins/dsh-bot/bot_http.cpp
        plugins/dsh-bot/bot_process.cpp)
    if(WIN32)
        # bot_http.cpp 用了 socket_util（Winsock2）
        target_link_libraries(dchat_dsh_bot_core PUBLIC dchat_protocol ws2_32 Threads::Threads)
    else()
        target_link_libraries(dchat_dsh_bot_core PUBLIC dchat_protocol Threads::Threads)
    endif()
    target_include_directories(dchat_dsh_bot_core PUBLIC plugins/dsh-bot)

    add_executable(dchat-dsh-bot plugins/dsh-bot/main.cpp plugins/dsh-bot/bot.cpp)
    if(WIN32)
        # dchat_protocol 必须排在 ws2_32 **前面**：静态库是按顺序解析的，
        # 反过来的话 socket_util.cpp 里的 WSAStartup 之类会找不到定义。
        target_link_libraries(dchat-dsh-bot PRIVATE dchat_dsh_bot_core dchat_protocol ws2_32
                                  Threads::Threads)
    else()
        target_link_libraries(dchat-dsh-bot PRIVATE dchat_dsh_bot_core Threads::Threads)
    endif()

    # 端到端测试用的"假大脑"：只吐 DSH 的 NDJSON，不联网不调模型
    add_executable(fake_dsh_brain tests/fake_dsh_brain.cpp)
    if(WIN32)
        # shell32：CommandLineToArgvW（取 UTF-8 的 argv，见文件里的说明）
        target_link_libraries(fake_dsh_brain PRIVATE shell32)
    endif()

    add_executable(test_dsh_bot tests/test_dsh_bot.cpp)
    target_link_libraries(test_dsh_bot PRIVATE dchat_dsh_bot_core)
endif()

# ---- 编译告警选项：跨平台目标（用 if(TARGET) 兜住两端差异）----
foreach(target_name dchat_protocol dchat_server test_protocol test_render test_server_rules
                     test_history test_server_command test_auth test_file_transfer test_crypto
                     test_register_guard test_history_store test_room test_room_registry
                     dchat_dsh_bot_core dchat-dsh-bot test_dsh_bot fake_dsh_brain)
    if(NOT TARGET ${target_name})
        continue()
    endif()
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
endforeach()

# ---- 编译告警选项：界面层目标（只有 Windows 有）----
foreach(target_name dchat_client dchat_ui test_rounded test_bubble test_image_preview
                     test_voice_notes test_ui_layout)
    if(NOT TARGET ${target_name})
        continue()
    endif()
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra)
    endif()
endforeach()
enable_testing()

# 按"目标是否存在"注册测试：界面层的测试只在 Windows 上有。
function(dchat_add_test name target)
    if(TARGET ${target})
        add_test(NAME ${name} COMMAND ${target})
    endif()
endfunction()

dchat_add_test(protocol_tests test_protocol)
dchat_add_test(render_tests test_render)
dchat_add_test(rounded_tests test_rounded)
dchat_add_test(bubble_tests test_bubble)
dchat_add_test(image_preview_tests test_image_preview)
dchat_add_test(server_rules_tests test_server_rules)
dchat_add_test(history_tests test_history)
dchat_add_test(server_command_tests test_server_command)
dchat_add_test(auth_tests test_auth)
dchat_add_test(file_transfer_tests test_file_transfer)
dchat_add_test(crypto_tests test_crypto)
dchat_add_test(file_crypto_tests test_file_crypto)
dchat_add_test(reconnect_tests test_reconnect)
dchat_add_test(register_guard_tests test_register_guard)
dchat_add_test(history_store_tests test_history_store)
dchat_add_test(room_tests test_room)
dchat_add_test(room_registry_tests test_room_registry)
dchat_add_test(dsh_bot_tests test_dsh_bot)
dchat_add_test(voice_notes_tests test_voice_notes)
dchat_add_test(ui_layout_tests test_ui_layout)
dchat_add_test(chat_color_tests test_chat_color)
dchat_add_test(color_picker_tests test_color_picker)
dchat_add_test(linux_client_tests test_linux_client)
dchat_add_test(client_core_tests test_client_core)
dchat_add_test(cli_render_tests test_cli_render)