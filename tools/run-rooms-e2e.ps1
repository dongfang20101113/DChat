# 多会话（私聊 / 群聊）端到端测试。
#
# 起一个真的 dchat_server，用 room_probe 当"没有界面的客户端"把协议说一遍，
# 然后对着输出做断言。这一步测的是单测**测不到**的东西：
# 命令有没有真的接上、消息有没有真的发对人、老客户端有没有被波及。
#
# ⚠️ 这个文件必须以 **UTF-8 带 BOM** 保存：Windows PowerShell 5.1 读无 BOM 的
#    .ps1 会按系统代码页（GBK）解码，中文全成乱码、脚本直接语法错误。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/run-rooms-e2e.ps1
param(
    [int]$Port = 5566,
    [string]$BuildDir = "$PSScriptRoot\..\build"
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
$script:failures = 0
$script:procs = @()
$script:lastOut = @{}

function Check([bool]$ok, [string]$what, [string]$tag = '') {
    $script:checks++
    if ($ok) {
        Write-Host "  ok   $what"
    } else {
        $script:failures++
        Write-Host "  FAIL $what" -ForegroundColor Red
        if ($tag -and $script:lastOut.ContainsKey($tag)) {
            Write-Host "       ---- $tag 的实际输出 ----" -ForegroundColor DarkGray
            foreach ($l in $script:lastOut[$tag]) { Write-Host "       $l" -ForegroundColor DarkGray }
            Write-Host "       ------------------------" -ForegroundColor DarkGray
        }
    }
}

function Contains([string[]]$haystack, [string]$needle) {
    if ($null -eq $haystack) { return $false }
    return ($haystack -join "`n").Contains($needle)
}

function B64([string]$text) {
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($text))
}

$server = Join-Path $BuildDir 'dchat_server.exe'
$probe = Join-Path $BuildDir 'room_probe.exe'
if (-not (Test-Path $server)) { throw "找不到 $server（先构建 dchat_server）" }
if (-not (Test-Path $probe)) { throw "找不到 $probe（先构建 room_probe）" }

$work = Join-Path $BuildDir 'rooms-e2e'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

# keepchathistory 默认是关的，关着就不写历史文件。要验证"重启后历史还在"、
# "每个房间一个历史文件"就必须先把它打开。正常运行中这条只能从服务器控制台改，
# 所以这里直接写规则文件（服务端启动时读它）。
Set-Content -Path "$work\dchat-rules.txt" -Value 'keepchathistory true' -Encoding ASCII

function Start-Probe([string]$tag, [string]$nick, [string]$pass, [string]$mode, [string[]]$script, [switch]$noFeatures) {
    $inFile = Join-Path $work "$tag.in"
    $outFile = Join-Path $work "$tag.out"
    # 探针按 UTF-8 读 stdin；PowerShell 5.1 的 Set-Content -Encoding UTF8 会写 BOM，
    # 所以用 .NET 的 UTF8Encoding($false) 写成无 BOM
    [IO.File]::WriteAllText($inFile, ($script -join "`n"), (New-Object Text.UTF8Encoding($false)))
    $probeArgs = @('127.0.0.1', "$Port", $nick, $pass, $mode)
    if ($noFeatures) { $probeArgs += 'no-features' }
    $p = Start-Process -FilePath $probe -ArgumentList $probeArgs `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile `
        -RedirectStandardError "$work\$tag.err" -NoNewWindow -PassThru
    $script:procs += $p
    return $p
}

function Wait-Probe($p, [string]$tag) {
    $p.WaitForExit(40000) | Out-Null
    $outFile = Join-Path $work "$tag.out"
    $lines = @()
    if (Test-Path $outFile) { $lines = [IO.File]::ReadAllLines($outFile, [Text.Encoding]::UTF8) }
    $script:lastOut[$tag] = $lines
    $errFile = Join-Path $work "$tag.err"
    if ((Test-Path $errFile) -and (Get-Item $errFile).Length -gt 0) {
        Write-Host "  !! $tag 的探针报错：" -ForegroundColor Yellow
        Get-Content $errFile -Encoding UTF8 | ForEach-Object { Write-Host "     $_" -ForegroundColor Yellow }
    }
    return $lines
}

Write-Host "== dchat 多会话端到端测试 =="

$serverLog = Join-Path $work 'server.log'
$serverProc = Start-Process -FilePath $server -ArgumentList @('-p', "$Port", '-b', '127.0.0.1') `
    -WorkingDirectory $work -RedirectStandardOutput $serverLog `
    -RedirectStandardError "$work\server.err" -NoNewWindow -PassThru

function Wait-Port([int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.Close()
            return $true
        } catch { }
    }
    return $false
}

try {
    if (-not (Wait-Port 15000)) { throw "服务器没起来（端口 $Port 连不上）" }

    # ---- 阶段 1：alice 注册、建群 ----
    Write-Host "[1] 建群"
    $alice1 = Wait-Probe (Start-Probe 'a1' 'alice' 'pw-alice-1' 'register' @(
        "GCREATE mygroup $(B64 '我的群聊')", '@drain 500')) 'a1'
    Check (Contains $alice1 'LOGGEDIN') 'alice 注册并登录成功' 'a1'
    Check (Contains $alice1 'FEATURES rooms1') '服务端确认了 rooms1 能力' 'a1'
    Check (Contains $alice1 'ROOMLIST hall hall') '会话列表里有大厅' 'a1'
    Check (Contains $alice1 'ROOMLIST mygroup group') '新建的群出现在会话列表里' 'a1'
    Check (Contains $alice1 'RINFO mygroup group') '收到了新群的 RINFO' 'a1'
    Check (Contains $alice1 'RSET mygroup name') '群名通过 RSET 下发（Base64）' 'a1'
    Check (Contains $alice1 'RSET mygroup maxmembers') '最大人数下发了' 'a1'
    Check (Contains $alice1 'RSET mygroup cachemaxmb') '总缓存最大大小下发了' 'a1'
    Check (Contains $alice1 'ID 不可更改') '提示了群 ID 不可更改' 'a1'
    Check (Contains $alice1 'REVENT') -and (Contains $alice1 'mygroup alice 创建了群聊') '建群事件广播给了群成员' 'a1'

    # ---- 阶段 2：bob 搜索与申请入群 ----
    Write-Host "[2] 搜索与申请入群"
    $bob1 = Wait-Probe (Start-Probe 'b1' 'bob' 'pw-bob-1' 'register' @(
        "GSEARCH $(B64 'my')", '@drain 400', 'GJOIN mygroup', '@drain 500')) 'b1'
    Check (Contains $bob1 'LOGGEDIN') 'bob 注册并登录成功' 'b1'
    Check (-not (Contains $bob1 'GHIT mygroup')) '默认不向公众展示时搜不到这个群' 'b1'
    Check (Contains $bob1 '等待管理员审核') 'bob 的入群申请进入待审队列' 'b1'
    Check (-not (Contains $bob1 'RINFO mygroup')) '待审期间拿不到群信息' 'b1'

    # ---- 阶段 3：alice 审核通过 ----
    Write-Host "[3] 审核入群"
    $alice2 = Wait-Probe (Start-Probe 'a2' 'alice' 'pw-alice-1' 'login' @(
        'GREQS mygroup', '@drain 400', 'GREQ mygroup bob ok', '@drain 500')) 'a2'
    Check (Contains $alice2 'GREQ mygroup bob') 'alice 看到了 bob 的入群申请' 'a2'
    Check (Contains $alice2 '同意') 'alice 同意了申请' 'a2'

    # ---- 阶段 4：普通成员改不了群设置 ----
    Write-Host "[4] 权限：普通成员改不了群设置"
    $bob2 = Wait-Probe (Start-Probe 'b2' 'bob' 'pw-bob-1' 'login' @(
        "GSET mygroup name $(B64 '偷改群名')", '@drain 400')) 'b2'
    Check (Contains $bob2 '只有群主和管理员') '普通成员改群名被拒' 'b2'

    # ---- 阶段 5：勾选向公众展示之后能搜到 ----
    Write-Host "[5] 向公众展示"
    $alice3 = Wait-Probe (Start-Probe 'a3' 'alice' 'pw-alice-1' 'login' @(
        'GSET mygroup listed true', '@drain 400')) 'a3'
    Check (Contains $alice3 'RSET mygroup listed true') '设置为公开后收到 RSET 广播' 'a3'

    $bob3 = Wait-Probe (Start-Probe 'b3' 'bob' 'pw-bob-1' 'login' @(
        "GSEARCH $(B64 '我的群')", '@drain 400')) 'b3'
    Check (Contains $bob3 'GHIT mygroup') '勾选后能在群搜索里找到（按群名搜）' 'b3'

    # ---- 阶段 6：群消息按房间路由 ----
    Write-Host "[6] 群消息带房间号"
    $bob4 = Wait-Probe (Start-Probe 'b4' 'bob' 'pw-bob-1' 'login' @(
        'ROOM mygroup', '@drain 500', 'MSG 大家好', '@drain 600')) 'b4'
    Check (Contains $bob4 'RINFO mygroup group') 'bob 切进群拿到了群信息' 'b4'
    Check (Contains $bob4 'RMEMBERS mygroup alice owner') '成员表里有群主' 'b4'
    Check (Contains $bob4 'RMSG') -and (Contains $bob4 'mygroup bob 大家好') '群消息走 RMSG 并带上房间号（时间在前、房间号紧随）' 'b4'
    Check (-not (Contains $bob4 'SAY ')) '群里发言不会掉进大厅的 SAY' 'b4'

    # ---- 阶段 7：私聊 ----
    Write-Host "[7] 私聊"
    $alice4 = Wait-Probe (Start-Probe 'a4' 'alice' 'pw-alice-1' 'login' @(
        'DM bob', '@drain 700', 'MSG 私聊一句', '@drain 600')) 'a4'
    Check (Contains $alice4 'GDMLINK dm-') 'alice 打开了与 bob 的私聊' 'a4'
    Check (Contains $alice4 'RINFO dm-000001 dm') '私聊会话的 RINFO 种类是 dm' 'a4'
    Check (Contains $alice4 'RMSG') -and (Contains $alice4 'dm-000001 alice 私聊一句') '私聊消息也走 RMSG（不会掉进大厅）' 'a4'

    # ---- 阶段 8：黑名单拦住私聊 ----
    Write-Host "[8] 黑名单"
    $bob5 = Wait-Probe (Start-Probe 'b5' 'bob' 'pw-bob-1' 'login' @(
        'BLOCK alice', '@drain 400', 'DM alice', '@drain 400', 'BLOCKS', '@drain 400')) 'b5'
    Check (Contains $bob5 '加入黑名单') 'bob 拉黑了 alice' 'b5'
    Check (Contains $bob5 '拉黑') '拉黑之后再打开私聊被拒绝' 'b5'
    Check (Contains $bob5 'GBLOCKS alice') '黑名单列表能列出来' 'b5'

    $alice5 = Wait-Probe (Start-Probe 'a5' 'alice' 'pw-alice-1' 'login' @(
        'ROOM dm-000001', '@drain 400', 'MSG 这条应该发不出去', '@drain 400')) 'a5'
    Check (Contains $alice5 '私聊发不出去') '被拉黑的一方发不出私聊' 'a5'

    # ---- 阶段 9：老客户端完全不受影响 ----
    Write-Host "[9] 老客户端（不声明 rooms1）"
    # carol 挂一条长 drain 在后台，同时 alice 在大厅和群里发言
    $carol = Start-Probe 'c1' 'carol' 'pw-carol-1' 'register' @('@drain 6000') -noFeatures
    Start-Sleep -Milliseconds 900
    $alice6 = Wait-Probe (Start-Probe 'a6' 'alice' 'pw-alice-1' 'login' @(
        'MSG 大厅消息', '@drain 300', 'ROOM mygroup', '@drain 300',
        'MSG 群里的消息', '@drain 400')) 'a6'
    $carolOut = Wait-Probe $carol 'c1'
    Check (Contains $carolOut 'SAY') '老客户端照常收到大厅的 SAY' 'c1'
    Check (-not (Contains $carolOut 'RMSG')) '老客户端收不到任何 RMSG' 'c1'
    Check (-not (Contains $carolOut 'ROOMLIST')) '老客户端不会被塞会话列表' 'c1'
    Check (-not (Contains $carolOut 'FEATURES')) '服务端不会给老客户端发 FEATURES' 'c1'
    Check (-not (Contains $carolOut 'REVENT')) '服务端不会给老客户端发 REVENT' 'c1'

    # ---- 阶段 10：群文件按房间隔离 ----
    Write-Host "[10] 群文件"
    $payloadBytes = [Text.Encoding]::UTF8.GetBytes('hello-dchat-group-file')
    $fileB64 = B64 'groupfile.txt'
    $chunk = [Convert]::ToBase64String($payloadBytes)
    $alice7 = Wait-Probe (Start-Probe 'a7' 'alice' 'pw-alice-1' 'login' @(
        'ROOM mygroup', '@drain 400',
        "FILE_SEND up1 $fileB64 $($payloadBytes.Length)", '@drain 300',
        "FILE_CHUNK up1 $chunk", '@drain 300',
        'FILE_END up1', '@drain 600')) 'a7'
    Check (Contains $alice7 'RFILE_OFFER') -and (Contains $alice7 'mygroup alice F1') '群文件用 RFILE_OFFER 带上房间号' 'a7'
    Check (Test-Path (Join-Path $work 'dchat-files\mygroup')) '群文件放进 dchat-files\mygroup（暂存根目录下按房间分目录）' 'a7'
    Check (-not (Contains $alice7 'FILE_OFFER alice')) '群文件没有退化成大厅用的 FILE_OFFER' 'a7'

    # ---- 阶段 11：退群与大厅保护 ----
    Write-Host "[11] 退群与大厅不可退"
    $bob6 = Wait-Probe (Start-Probe 'b6' 'bob' 'pw-bob-1' 'login' @(
        'GLEAVE hall', '@drain 400', 'GLEAVE mygroup', '@drain 500')) 'b6'
    Check (Contains $bob6 '大厅不能退出') '大厅拒绝退出' 'b6'
    Check (Contains $bob6 '已退出群聊') '普通群可以退出' 'b6'

    # ---- 阶段 12：重启后房间、历史、设置都还在 ----
    Write-Host "[12] 重启后房间与历史都还在"
    Stop-Process -Id $serverProc.Id -Force
    Start-Sleep -Milliseconds 900
    $serverProc = Start-Process -FilePath $server -ArgumentList @('-p', "$Port", '-b', '127.0.0.1') `
        -WorkingDirectory $work -RedirectStandardOutput "$work\server2.log" `
        -RedirectStandardError "$work\server2.err" -NoNewWindow -PassThru
    Check (Wait-Port 15000) '服务器重启后重新监听'

    $alice8 = Wait-Probe (Start-Probe 'a8' 'alice' 'pw-alice-1' 'login' @(
        'ROOM mygroup', '@drain 900')) 'a8'
    Check (Contains $alice8 'ROOMLIST mygroup group') '重启后群还在会话列表里' 'a8'
    Check (Contains $alice8 'RHIST mygroup') '切进群能拉到该群的历史' 'a8'
    Check (Contains $alice8 'mygroup bob 大家好') '历史里是群消息（房间号对得上）' 'a8'
    Check (Contains $alice8 'RSET mygroup listed true') '重启后群设置（公开）没丢' 'a8'
    Check (Contains $alice8 'RSET mygroup name') '重启后群名没丢' 'a8'

    Write-Host "[13] 落盘的路径"
    Check (Test-Path (Join-Path $work 'dchat-rooms.txt')) '房名单落盘 dchat-rooms.txt'
    Check (Test-Path (Join-Path $work 'dchat-blocks.txt')) '黑名单落盘 dchat-blocks.txt'
    Check (Test-Path (Join-Path $work 'dchat-history.txt')) '大厅历史仍用原文件名 dchat-history.txt'
    Check (Test-Path (Join-Path $work 'dchat-history-mygroup.txt')) '群历史单独一个文件 dchat-history-mygroup.txt'
    Check (Test-Path (Join-Path $work 'dchat-history-dm-000001.txt')) '私聊历史也单独一个文件'
    Check (Test-Path (Join-Path $work 'dchat-files')) '暂存根目录仍是 dchat-files'
    $roomsText = Get-Content (Join-Path $work 'dchat-rooms.txt') -Raw -Encoding UTF8
    Check ($roomsText.Contains('mygroup')) '房名单里有这个群'
    Check (-not $roomsText.Contains("hall`t")) '大厅不写进房名单'
}
finally {
    foreach ($p in $script:procs) { if (-not $p.HasExited) { try { $p.Kill() } catch { } } }
    if ($serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "$($script:checks) checks, $($script:failures) failures"
if ($script:failures -gt 0) { exit 1 }
exit 0
