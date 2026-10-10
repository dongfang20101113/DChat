# 本地联调服务器：起一个**新版** dchat 服务端，供桌面端 / 安卓端连上去试群聊私聊。
#
# 为什么不直接连线上：线上跑的还是旧版（没有群聊/私聊）。旧版确实也能连，
# 但那会走"退回只有大厅"的降级路径 —— 那条路径也要验，只是不该是**唯一**能验的东西。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-local-test-server.ps1
#   powershell ... -File tools\run-local-test-server.ps1 -Port 5600 -Fresh
#
# 数据目录默认 build\local-server\（单独一份账号、历史、房间名单，**不碰线上数据**）。
# -Fresh 会清空它，等价于"一台刚装好的服务器"。
param(
    [int]$Port = 5599,
    [string]$DataDir = "$PSScriptRoot\..\build\local-server",
    [string]$BuildDir = "$PSScriptRoot\..\build",
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'

$server = Join-Path $BuildDir 'dchat_server.exe'
if (-not (Test-Path $server)) {
    Write-Host "找不到 $server" -ForegroundColor Red
    Write-Host "先构建：cmake --build build --target dchat_server" -ForegroundColor Yellow
    exit 1
}

if ($Fresh -and (Test-Path $DataDir)) {
    Write-Host "清空数据目录（-Fresh）：$DataDir" -ForegroundColor Yellow
    Remove-Item $DataDir -Recurse -Force
}
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

# 打开"保留聊天记录"。默认是关的，关着就不写历史文件，
# 于是"重启后历史还在"这条根本验不了。正常运行中它只能从服务器控制台改，
# 所以这里直接写规则文件（服务端启动时读它）。
$rulesPath = Join-Path $DataDir 'dchat-rules.txt'
if (-not (Test-Path $rulesPath)) {
    Set-Content -Path $rulesPath -Value '# 联调用的规则：把聊天记录留下来，方便验证"重启后历史还在"' -Encoding ASCII
    Add-Content -Path $rulesPath -Value 'keepchathistory true' -Encoding ASCII
}

# 本机地址：客户端填这个（不是 127.0.0.1 的话，同一局域网里的手机才能连）
$ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    Select-Object -ExpandProperty IPAddress
if (-not $ips) {
    # 拿不到网卡列表（权限/精简系统）时不能让用户干瞪眼：服务端启动时会自己打印
    # "本机地址"那几行，照那个填就行
    $ips = @()
}

Write-Host ""
Write-Host "== dchat 本地联调服务器 ==" -ForegroundColor Cyan
Write-Host "  数据目录 : $DataDir"
Write-Host "  端口     : $Port"
if ($ips) {
    Write-Host "  桌面端填 : 127.0.0.1 : $Port"
    foreach ($ip in $ips) { Write-Host "  安卓端填 : $ip : $Port   （手机要和电脑同一个 WiFi）" }
}
else {
    Write-Host "  桌面端填 : 127.0.0.1 : $Port"
    Write-Host "  安卓端填 : 见下方服务端启动日志的「本机地址」那一行（手机要和电脑同一个 WiFi）"
}
Write-Host ""
Write-Host "  验证顺序建议：" -ForegroundColor Cyan
Write-Host "   1. 桌面端注册两个账号（比如 a、b），大厅能互相看到发言"
Write-Host "   2. a 点右下角「＋ 新建 / 搜索」→ 填群聊 ID + 群名 → 创建群聊"
Write-Host "   3. b 搜索那个群 → 加入（默认「管理员审核」，会进待审队列）"
Write-Host "   4. a 右键左侧那个群 → 会话设置 → 入群申请 → 同意"
Write-Host "   5. 群里互相发消息；大厅的消息不应该跑到群里，反之亦然"
Write-Host "   6. 会话设置里改群名 / 最大人数 / 加群方式 / 向公众展示 / 总缓存 / 群公告"
Write-Host "   7. 会话设置里勾「免打扰」→ 那个群未读变成小圆点、不闪任务栏"
Write-Host "   8. 私聊：会话设置里「拉黑对方」→ 双向都发不出消息"
Write-Host "   9. 大厅：不能退出、不能删、默认免打扰"
Write-Host "  10. 关掉服务器再起一次（不加 -Fresh）→ 群、历史、设置都还在"
Write-Host ""
Write-Host "  旧客户端兼容（可选）：随便连一个旧版客户端进来，它应该只看得到大厅，"
Write-Host "  收不到任何群聊/私聊的东西。"
Write-Host ""
Write-Host "按 Ctrl+C 停止服务器。" -ForegroundColor Yellow
Write-Host ""

# 前台跑，日志直接打在控制台（服务器自己的日志就是 UTF-8 中文）
& $server --port $Port --bind 0.0.0.0
