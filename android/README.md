# dchat Android 客户端

[`D:\codes\dchat`](../) 的安卓界面客户端。用 **Kotlin + Jetpack Compose** 写，复用桌面端同一套线协议，
**纯客户端**——服务端仍然是那个 C++ 的 `dchat_server.exe`，不用改一行。

> 与桌面端保持一致的细节：昵称配色调色板、`@提及` 的判定规则、气泡左右分栏、
> 公告/系统提示的宽度比例（88% / 70%）、输入历史上限、聊天记录上限 400 条。

---

## 实机验证后修掉的两个问题

这两个都是**装到真机上才暴露出来的**，离屏渲染和单测都发现不了。

### 问题一：打开文件上传后几秒自动掉线

**症状**：点「📎」打开系统文件选择器，几秒后连接断开、界面弹回连接页。

**根因**（架构缺陷，不是偶发 bug）：连接原本挂在 `AndroidViewModel` 的
`viewModelScope` 上——

```
点「📎」→ 系统文件选择器（DocumentsUI）切到前台 → MainActivity 被 stop
        → 系统内存紧张时销毁 Activity → ViewModel 被清除
        → viewModelScope 取消 → socket 被连带关闭 → 界面弹回连接页
```

旋转屏幕、切深色模式、开发者选项里的「不保留活动」都会触发同一问题。

**修法**：把连接**和聊天状态**一起搬到进程级单例 [`net/DchatSession.kt`](app/src/main/java/com/dongfang20101113/dchat/net/DchatSession.kt)，
由 `Application.onCreate` 初始化。Activity 重建后重新订阅一下就有完整状态
（聊天记录、在线名单、未读数都在），不再丢。

**回归测试**：`ServerInteropTest.没人收集消息的那段时间服务端发来的消息不会丢` ——
故意 2 秒不收集（模拟 Activity 被销毁那几秒），然后恢复收集，断言服务端在连接瞬间
发出的 `WELCOME` 仍在、连接仍是活的、还能正常收发消息。

> ⚠️ **局限（要说清楚）**：这只保住了 **Activity** 的生命周期，**保不住进程**。
> 如果系统把整个 App 进程回收了（低内存、厂商省电策略），连接还是会断。
> 要连进程一起保住得上前台服务（会常驻一条通知），目前**没做**。

### 问题二：选大文件会闪退（顺手查出来的）

**根因**：`resolveFile` 里，当 provider 不返回文件大小（`OpenableColumns.SIZE` 为 -1，
网盘、部分相册、下载管理器都这样）时会走 `stream.readBytes()`
——**把整个文件读进内存**，选个大视频就是一次 OutOfMemoryError。

**修法**：新出纯函数 [`copyWithLimit`](app/src/main/java/com/dongfang20101113/dchat/protocol/FileTransfer.kt)，
先流式复制到缓存文件、边读边计数、超限立刻中止，堆占用恒为一个 64 KB 缓冲区。

**回归测试**：`BoundedCopyTest`（10 项），其中一条直接拿一个 **4 GB 的流**去跑，
断言它到上限就停——4 GB 远超测试堆，实现只要敢读进内存这条必然 OOM。

### 问题三：应用图标

清单里**根本没写 `android:icon`**，所以系统一直用默认的绿色机器人。

**修法**：新增自适应图标（`mipmap-anydpi-v26/`，minSdk 26 所以不需要再放 PNG），
蓝色底 `#0078D7`（与桌面端一致）+ 白色气泡 + 三个点；
另附 Android 13+ 的主题化单色层。

**验证**：`ScreenshotTest.渲染应用图标_方形与圆形遮罩` 会额外输出
`icon-square.png` / `icon-round.png` / `icon-monochrome.png`，
其中**圆形遮罩版专门用来验证图案没超出安全区被启动器裁掉**（已验证，四周留白充足）。

---

## 快速开始

### 构建

前置：JDK 21、Android SDK、网络能到国内 Maven 镜像（见下方「构建环境」）。

```powershell
$env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.9.10-hotspot'
$env:ANDROID_HOME = 'D:\codes\tools\android-sdk'
cd D:\codes\dchat\android

.\gradlew.bat test            # 跑全部单测（152 项）
.\gradlew.bat assembleDebug   # 产出 app\build\outputs\apk\debug\app-debug.apk
```

### 安装

```powershell
# 手机开 USB 调试后
D:\codes\tools\android-sdk\platform-tools\adb.exe install -r app\build\outputs\apk\debug\app-debug.apk
```

或者把 `app-debug.apk` 传到手机上直接点安装（debug 包已用 Android 调试证书签名，可直接装）。

> `app-release-unsigned.apk` 体积小得多（1.37 MB vs 19 MB）但**未签名，装不上**，
> 要用自己的 keystore 签一次才能安装。见文末。

### 使用

1. 填服务器地址和端口（默认 5555），点「连接」
2. 登录或注册（注册成功后**不会自动登录**，要再点一次登录——与桌面端一致）
3. 进聊天界面：发消息、看在线成员、点文件卡片下载

---

## 屏幕适配：这一版专门解决的事

桌面端是**写死像素**的（窗口 940×660、字号 -16 像素），直接搬到手机上会崩。
这一版的做法是：

### 1. 尺寸决策全部是纯函数，可脱离设备单测

`ui/layout/ChatLayout.kt` 里 `chatMetrics(宽dp, 高dp, 字体缩放)` 算出**全部**尺寸：
气泡上限、内边距、字号、行距、输入框高度、成员栏宽度、是否分栏。
它**不引用任何 Android API**，所以 20 多种真实屏幕规格能在 JVM 上验证（见 `ChatLayoutTest` 的 26 项）。

三条硬规则：

| 规则 | 为什么 |
| --- | --- |
| 一律用 dp / sp，不写像素 | dp 随屏幕密度换算；sp 还会跟随系统字号设置 |
| 宽度 = 百分比 **且** 绝对上限 | 桌面端只有百分比（62%），在 1280dp 平板上会算出 793dp 的一行，人眼横跨不了；手机上加绝对上限反而更窄，两边都要 |
| 触控目标 ≥ 48dp | 桌面端那些 20 像素高的按钮，手指点不中 |

### 2. 断点（对齐 Material 3 的 window size class）

| 宽度 | 档位 | 行为 |
| --- | --- | --- |
| < 600dp | COMPACT | 手机竖屏：气泡上限 82%、**不显示头像**（把宽度让给正文）、成员列表走可开合浮层 |
| 600–839dp | MEDIUM | 大屏手机横屏 / 小折叠：气泡 72%、显示头像 |
| ≥ 840dp | EXPANDED | 平板：气泡 62%、**左右分栏**（右侧固定 280dp 成员栏） |

### 3. 系统级的适配

| 情况 | 处理 |
| --- | --- |
| **刘海 / 挖孔 / 状态栏** | 根节点 `safeDrawingPadding()`，内容不会钻到摄像头下面 |
| **键盘弹出** | 整页 `imePadding()` + `windowSoftInputMode="adjustResize"`，输入框永远贴在键盘上方；矮屏（<640dp）加键盘时自动收起顶栏把空间让给消息 |
| **手势条** | 输入栏额外 `navigationBarsPadding()` |
| **旋转屏幕** | **不拦截 `configChanges`**，让系统重建 Activity，Compose 用新宽高重算一遍——比手动处理更不容易错（尤其是字体缩放） |
| **系统字号放大** | 字号用 sp（自动跟随）；同时按缩放比例**适度收窄气泡上限**，避免字大之后一行只剩三四个字 |
| **深色 / 浅色** | 跟随系统，两套配色逐条抄自桌面端 `client.cpp` 的 `kLightPalette` / `kDarkPalette` |

### 4. 多尺寸截图验证

没有可用模拟器（本机 `HypervisorPresent = False`，跑不了 WHPX 加速），
所以用 **Robolectric 原生图形模式离屏渲染出真实像素**，产出 9 张 PNG：

```
app\build\screenshots\
  chat-phone-320x568-dark.png      ← 极窄屏
  chat-phone-360x640-dark.png      ← 小屏
  chat-phone-393x851-dark.png      ← 主流机型
  chat-phone-393x851-light.png     ← 浅色主题
  chat-phone-land-851x393-dark.png ← 横屏（最容易把输入框挤没）
  chat-tablet-1024x768-dark.png    ← 平板（应分栏）
  chat-tablet-1280x800-dark.png    ← 大屏
  entry-connect.png                ← 连接界面
  entry-auth-error.png             ← 登录界面（含错误提示）
```

---

## 代码结构

```
app/src/main/java/com/dongfang20101113/dchat/
├── protocol/              ← 纯逻辑，零 Android 依赖，可脱离界面单测
│   ├── DchatProtocol.kt       行协议：ParseLine/BuildLine、昵称校验、UTF-8 码点、时间字段
│   ├── LineBuffer.kt          TCP 字节流切行：半包 / 粘包 / CRLF / 超长行
│   ├── Display.kt             @提及判定、昵称配色(FNV-1a 按 UTF-8 字节)、SAY/提示解析、未读规则
│   ├── ServerLine.kt          服务器→客户端全部命令的强类型模型
│   └── FileTransfer.kt        Base64、文件名清理、分块、字节数格式化
├── net/
│   └── DchatConnection.kt     TCP + 协程：接收循环、心跳(45s)、加锁发送、4 秒连接超时
├── data/
│   └── SettingsStore.kt       只持久化「地址 + 端口」（刻意不存密码和用户名）
├── ui/
│   ├── ChatState.kt           不可变状态 + **纯函数 reducer**（收到每条消息界面怎么变）
│   ├── ChatViewModel.kt       网络接线、文件落盘、发送
│   ├── layout/ChatLayout.kt   ★ 屏幕适配的全部尺寸决策（纯函数）
│   ├── theme/DchatTheme.kt    深浅两套配色（抄自桌面端）
│   ├── components/            气泡 / 系统提示 / 公告 / 文件卡片
│   └── screens/               连接 / 登录注册 / 聊天
└── MainActivity.kt            按阶段切换三屏
```

---

## 测试

```powershell
.\gradlew.bat test
```

**166 项，覆盖 10 个测试类：**

| 测试类 | 项数 | 覆盖什么 |
| --- | --- | --- |
| `ChatLayoutTest` | 26 | **屏幕适配**：20+ 种真实屏幕宽度的不变量、断点、分栏、字体缩放、横竖屏、气泡摆放、键盘避让 |
| `DisplayTest` | 25 | @提及边界（`@alicex` 不命中 `@alice`、中文标点、邮箱写法）、昵称配色稳定性、公告识别 |
| `FileTransferTest` | 25 | Base64 往返与严格性、**文件名清理（路径穿越/非法字符/结尾点/超长）**、分块、最坏行长度 |
| `ChatStateReducerTest` | 23 | 状态机：哪些消息进记录哪些不进、名单维护、文件卡片状态流转、400 条上限 |
| `DchatProtocolTest` | 18 | 行协议：命令名允许下划线、协议注入防护、昵称按码点计数、超长行不切坏字符 |
| `ServerLineTest` | 15 | 全部服务器命令解析，**含 6 个未写进 README 的** |
| `LineBufferTest` | 10 | 半包/粘包/CRLF/超长行/逐字节喂入 |
| `BoundedCopyTest` | 10 | **有上限的流式复制**：4 GB 的流不撑爆内存、超限立刻停、边界值 |
| `ScreenshotTest` | 10 | 多尺寸离屏渲染出 PNG（9 张界面 + 应用图标 3 张） |
| `ServerInteropTest` | 4 | **连真实的 C++ 服务端**：基础互通、切出去回来不丢消息、主动断开、服务端消失 |

### 最有价值的两组

**`ServerInteropTest`** —— 其余测试都是"自己跟自己对"，只有它证明
**手机端发的字节 C++ 服务端听得懂、服务端回的字节手机端解析得对**：

```
连接 → 注册 → 登录（LOGGEDIN）→ 收 NAMES / KNOWN / RULES
     → 发消息 → 收到自己的 SAY 回显 → /help → QUIT
```

它还会自动启动 `D:\codes\dchat\build\dchat_server.exe`（临时账号文件 + 系统分配的随机端口），
跑完清理；找不到 exe 时自动跳过，别人 clone 下来构建不会失败。

另外三条专门盯**真机上暴露过的事故**：切出去再回来消息不能丢、用户主动断开不能被当成掉线、
服务端没了必须变成带原因的 `Lost` 而不是静默变 `Disconnected`。

**`BoundedCopyTest`** —— 直接拿一个 4 GB 的流跑，4 GB 远超测试堆，
实现只要敢把数据读进内存，测试必然 OOM 失败。

---

## 构建环境（这台机器的实测配置）

| 组件 | 版本 / 位置 |
| --- | --- |
| JDK | `C:\Program Files\Microsoft\jdk-21.0.9.10-hotspot` |
| Android SDK | `D:\codes\tools\android-sdk`（build-tools 35/36、platforms 35/36/**37.1/37.2**、platform-tools 37.0.1） |
| Gradle | 9.8.0（wrapper，复用已有缓存） |
| AGP / Kotlin | 9.4.1 / 2.4.20 |
| Compose BOM | 2026.09.00 |
| compileSdk / minSdk / targetSdk | **37.2**（`compileSdk = 37` + `compileSdkMinor = 2`）/ 26 / 37 |

`local.properties` 里写着 `sdk.dir`，**不进 git**。

### 踩过的坑（都是实测撞出来的）

| # | 现象 | 原因与解法 |
| --- | --- | --- |
| 1 | 装 SDK 包报 `Package platforms not found` | **PowerShell 5.1 会把原生命令参数里的 `;` 当语句分隔符拆开**，`"platforms;android-35"` 变成两个参数。用 `cmd /c` 包一层 |
| 2 | 插件坐标 `com.android.application:9.4.1` 解析不到 | `dl.google.com` 极不稳定（梯子开着也 0/4 成功）。**改用 `maven.aliyun.com/repository/google`**，构建彻底不依赖梯子 |
| 3 | `The 'org.jetbrains.kotlin.android' plugin is no longer required` | **AGP 9.0 起内置 Kotlin 支持**，不能再应用该插件，只留 Compose 编译器插件 |
| 4 | `checkDebugAarMetadata` 要求 compileSdk ≥ 37 | AndroidX 1.19.1 / Compose 1.12.1 的要求。而 `platforms;android-37` **不存在**，实际是 **37.1 / 37.2**；AGP 9 用 `compileSdk` + `compileSdkMinor` 两个属性表达（这个属性是反编译 AGP 的 DSL 接口找到的） |
| 5 | Robolectric 报 `Found unrecognized trailing qualifier segments` | 资源限定符**顺序有严格要求**：`语言-区域` 在前、`宽-高` 居中、`密度` 在后。写成 `w393dp-...-zh-rCN` 会直接抛异常 |
| 6 | 互操作测试单独跑过、全量跑偶发失败 | **真 bug**：`MutableSharedFlow` 在没有订阅者时**直接丢弃发射**，服务端连上就发的 `WELCOME` 会永久丢。改用 `Channel(UNLIMITED)` + `receiveAsFlow()` |
| 7 | R8 报 `Supplied proguard configuration does not exist` | `build.gradle.kts` 引用了 `proguard-rules.pro` 但文件没建 |
| 8 | 测试类被 JUnit 拒绝：`should be void` | `fun x() = runBlocking { ... }` 的最后一个表达式返回了 `Boolean`，方法签名就不是 void 了 |

---

## 发布签名（release 包当前未签名）

```powershell
# 1. 生成 keystore（自己保管好，丢了就再也无法更新同一个应用）
keytool -genkeypair -v -keystore dchat-release.jks -keyalg RSA -keysize 2048 `
        -validity 10000 -alias dchat

# 2. 在 app/build.gradle.kts 里加 signingConfigs 并让 release 使用它
# 3. .\gradlew.bat assembleRelease
```

**不要把 keystore 和密码提交进 git**（`.gitignore` 已经挡了 `*.keystore` / `*.jks`）。

---

## 参考

- 桌面端 README：[`../README.md`](../README.md)
- 服务端源码：[`../src/server.cpp`](../src/server.cpp)（协议的实际行为以它为准）
- 协议解析的实现参照：[`../src/protocol.cpp`](../src/protocol.cpp)、[`../src/render.cpp`](../src/render.cpp)、[`../src/file_transfer.cpp`](../src/file_transfer.cpp)

> ⚠️ **协议表在 README 里是不全的**：实际还有 `ANNOUNCE`（`/say` 公告走的命令）、`RULES`（服务器下发规则）、
> `FILE_THUMB` / `FILE_THUMB_GET` / `FILE_THUMB_DATA` / `FILE_THUMB_END`（缩略图）这 6 个命令
> 没有写进文档。本客户端是按**服务端源码**实现的，不是照 README 猜的。
