# chatrule 新增 `webuploadmb`：网页版单文件上传上限

## 为什么以前管不到

网页版和桌面端传文件的机制不一样：

- **桌面端**：读盘、分块、按协议传。上限就是服务器的 `documentsize`。
- **网页版**：浏览器把整个文件读成 Base64，塞进**一帧 WebSocket** 发给网关。

所以它的上限原本是写在 `webd/webd.cpp` 里的一个常量 `8 MB`，出现了三个不一致：

1. 服务器 `documentsize` 调大也没用，网页版还是 8 MB
2. 页面上那句「上限 8 MB」和按钮提示都写死
3. 管理员在 `/admin` 里看不到、也改不了这一项

## 现在的做法

新增一条 chatrule：**`webuploadmb`**（MB，1–64，默认 8）。

```bash
# 控制台
/chatrule webuploadmb set 20

# 或网页 /admin 里，规则列表最后一条
```

改完立刻生效（网关缓存 5 秒），会同时改三处：

| 位置 | 变化 |
| --- | --- |
| 页头「📎」按钮提示 | `发文件（上限 20 MB）` |
| 前端预检 | `const UPLOAD_MB = 20` → 超过就在选文件时直接拦下 |
| 网关真正的闸 | 解码后超过 `20 * 1024 * 1024` 直接回 ERROR |

网关用的值和 `/admin` 页面**同一个来源**（管理通道 `LIST`），不会出现两套数。

## 几条边界（故意的）

- **默认 8**：和原来写死的值一样，升级后行为不变。
- **不能超过 `documentsize`**：超了服务器本来也存不下，两边说法会打架。
  设置时会被拒绝并提示先调大 `documentsize`。
- **`documentsize` 降到网页版上限以下时，`webuploadmb` 自动跟着降**，
  并在返回信息里注一句——不然网页会放行一个服务器铁定不收的大小。
- **最大 64 MB**：再大就是「一帧 WebSocket 塞几十兆 Base64」，
  浏览器和网关都吃不消。要放开就改 `server_rules.cpp` 里
  `kRanges` 的 `{"webuploadmb", 1, 64, ...}`，但先想清楚内存。

## 验证

### 单元测试（`tests/test_server_rules.cpp` 的 `[3b]`，7 项全过）

```
默认 8 MB（升级后行为不变）
可以调到 32 MB
超过 documentsize 时被拒绝，且原值不动
提示里指出卡在 documentsize
documentsize 降到 10 时 webuploadmb 同步降到 10
低于下限被夹到 1 MB
超上限不会越过 documentsize
```

`ctest` 16/16。

### 端到端（沙箱里真 server + 真 webd，7 项全过）

| 步骤 | 结果 |
| --- | --- |
| 默认页面 | `const UPLOAD_MB = 8;`、`上限 8 MB` |
| `SET webuploadmb 1` 后等 6 秒 | 页面变成 `1 MB` |
| 用 2 MB 文件上传 | 被拒绝，错误里写明「1 MB」和 `webuploadmb` |
| `SET webuploadmb 20` 后等 6 秒 | 页面变成 `20 MB` |
| 同一个 2 MB 文件再传 | **被接受** |

### 线上（120.26.223.225）

```
规则条数 16 → 17
webuploadmb    8
documentsize   256        ← 线上单文件上限，网页版可以一路调到 64
外网 /room/hall 里的注入：const UPLOAD_MB = 8;  上限 8 MB
```

## 改动文件

- `src/server_rules.h` / `.cpp` — 新规则（范围表、元数据、取值、描述、
  读写、序列化、越界夹取、与 `documentsize` 互锁）
- `webd/webd.cpp` — `WebUploadLimitMb()`（5 秒缓存，从管理通道 LIST 取）、
  网关侧的闸改用规则值、页面注入 `__UPLOAD_MB__`
- `tests/test_server_rules.cpp` — 新增 `[3b]` 7 项
