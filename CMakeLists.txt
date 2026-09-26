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
add_library(dchat_protocol STATIC src/protocol.cpp src/render.cpp src/input_history.cpp src/server_command.cpp src/auth.cpp src/file_transfer.cpp src/server_rules.cpp)
target_include_directories(dchat_protocol PUBLIC src)
# 密码哈希用 Windows 自带的加密库
target_link_libraries(dchat_protocol PUBLIC bcrypt)

# 服务器：控制台程序，支持多客户端 + 广播
add_executable(dchat_server src/server.cpp)
target_link_libraries(dchat_server PRIVATE dchat_protocol ws2_32 Threads::Threads)

# 界面绘制库：圆角控件（GDI+ 抗锯齿）+ 气泡布局，独立成库以便做像素级/几何测试
# 预览用 Windows 自带的缩略图提供者（shell32/ole32），所以放在 dchat_ui 里
add_library(dchat_ui STATIC src/rounded.cpp src/bubble.cpp src/image_preview.cpp)
target_include_directories(dchat_ui PUBLIC src)
target_link_libraries(dchat_ui PUBLIC gdiplus gdi32 user32 shell32 ole32)

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
