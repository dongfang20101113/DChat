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

add_library(dchat_protocol STATIC src/protocol.cpp src/render.cpp src/input_history.cpp src/server_command.cpp src/auth.cpp src/file_transfer.cpp src/server_rules.cpp src/crypto.cpp ${DCHAT_CRYPTO_BACKEND})
target_include_directories(dchat_protocol PUBLIC src)
# 密码哈希与传输加密都用 Windows 自带的加密库（CNG / bcrypt.dll）
target_link_libraries(dchat_protocol PUBLIC bcrypt)

# 服务器：控制台程序，支持多客户端 + 广播
add_executable(dchat_server src/server.cpp)
target_link_libraries(dchat_server PRIVATE dchat_protocol ws2_32 Threads::Threads)

# 界面绘制库：圆角控件（GDI+ 抗锯齿）+ 气泡布局，独立成库以便做像素级/几何测试
# 预览用 Windows 自带的缩略图提供者（shell32/ole32），所以放在 dchat_ui 里
# 语音：录音（waveIn/winmm）、压缩（ACM/msacm32）、播放（PlaySound/winmm）。
# 放在 dchat_ui 里是因为它和预览一样属于"系统能力"，不属于聊天协议。
add_library(dchat_ui STATIC src/rounded.cpp src/bubble.cpp src/image_preview.cpp src/voice_notes.cpp src/ui_layout.cpp src/chat_color.cpp src/color_picker.cpp src/picker_dialog.cpp)
target_include_directories(dchat_ui PUBLIC src)
target_link_libraries(dchat_ui PUBLIC gdiplus gdi32 user32 shell32 ole32 winmm)

# 客户端：Win32 图形界面（微信式气泡 + 圆角控件）
add_executable(dchat_client WIN32 src/client.cpp)
# dwmapi/uxtheme：让滚动条与窗口标题栏跟随深色模式
# comdlg32：选文件的对话框；shell32：窗口接受拖放文件
target_link_libraries(dchat_client PRIVATE dchat_ui dchat_protocol ws2_32 user32 gdi32 dwmapi uxtheme comdlg32 shell32 Threads::Threads)
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

if(MSVC)
    target_compile_options(dchat_protocol PRIVATE /W4)
    target_compile_options(dchat_server PRIVATE /W4)
    target_compile_options(dchat_client PRIVATE /W4)
    target_compile_options(test_protocol PRIVATE /W4)
    target_compile_options(test_render PRIVATE /W4)
    target_compile_options(dchat_ui PRIVATE /W4)
    target_compile_options(test_rounded PRIVATE /W4)
    target_compile_options(test_bubble PRIVATE /W4)
    target_compile_options(test_image_preview PRIVATE /W4)
    target_compile_options(test_server_rules PRIVATE /W4)
    target_compile_options(test_history PRIVATE /W4)
    target_compile_options(test_server_command PRIVATE /W4)
    target_compile_options(test_auth PRIVATE /W4)
    target_compile_options(test_file_transfer PRIVATE /W4)
    target_compile_options(test_crypto PRIVATE /W4)
else()
    target_compile_options(dchat_protocol PRIVATE -Wall -Wextra)
    target_compile_options(dchat_server PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(dchat_client PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_render PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(dchat_ui PRIVATE -Wall -Wextra)
    target_compile_options(test_rounded PRIVATE -Wall -Wextra)
    target_compile_options(test_bubble PRIVATE -Wall -Wextra)
    target_compile_options(test_image_preview PRIVATE -Wall -Wextra)
    target_compile_options(test_server_rules PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_history PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_server_command PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_auth PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_file_transfer PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_crypto PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_voice_notes PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(test_ui_layout PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

enable_testing()
add_test(NAME protocol_tests COMMAND test_protocol)
add_test(NAME render_tests COMMAND test_render)
add_test(NAME rounded_tests COMMAND test_rounded)
add_test(NAME bubble_tests COMMAND test_bubble)
add_test(NAME image_preview_tests COMMAND test_image_preview)
add_test(NAME server_rules_tests COMMAND test_server_rules)
add_test(NAME history_tests COMMAND test_history)
add_test(NAME server_command_tests COMMAND test_server_command)
add_test(NAME auth_tests COMMAND test_auth)
add_test(NAME file_transfer_tests COMMAND test_file_transfer)
add_test(NAME crypto_tests COMMAND test_crypto)
add_test(NAME voice_notes_tests COMMAND test_voice_notes)
add_test(NAME ui_layout_tests COMMAND test_ui_layout)
add_test(NAME chat_color_tests COMMAND test_chat_color)
add_test(NAME color_picker_tests COMMAND test_color_picker)


