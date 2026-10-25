# dchat × DeepSeek Harness 机器人插件的端到端测试。
#
# 起一个**真的** dchat_server，一个**真的** dchat-dsh-bot 进程，用 room_probe 当
# "群里的另一个人"发消息。大脑用的是 fake_dsh_brain（按 DSH 的 NDJSON 格式吐事件），
# 所以除了"模型怎么想"以外的每一环都是真跑：连服务器、登录、GJOIN、收 RMSG、
# @ 判定、白黑名单、起子进程、读事件、分条、按节奏发出去。
#
# ⚠️ 这个文件必须以 **UTF-8 带 BOM** 保存：Windows PowerShell 5.1 读无 BOM 的 .ps1
#    会按系统代码页（GBK）解码，中文字符串全成乱码、脚本直接语法错误。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/run-dsh-bot-e2e.ps1
param(
    [int]$Port = 5601,
    [int]$UiPort = 5701,
    [string]$BuildDir = "$PSScriptRoot\..\build"
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
$script:failures = 0
$script:lastOut = @{}
$script:botProcs = @()
$script:serverProc = $null

function Check {
    param([bool]$ok, [string]$what, [string]$tag = '')
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

function B64 {
    param([string]$text)
    return [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($text))
}

# 读一个"别的进程正在写"的文件。直接用 [IO.File]::ReadAllText 会抛
# "being used by another process" —— 机器人的 stdout 重定向文件一直是打开着的。
function Read-Shared {
    param([string]$path)
    if (-not (Test-Path $path)) { return '' }
    try {
        $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $reader = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
        $reader.Close()
        return $text
    } catch {
        return ''
    }
}

$serverExe = Join-Path $BuildDir 'dchat_server.exe'
$probeExe = Join-Path $BuildDir 'room_probe.exe'
$botExe = Join-Path $BuildDir 'dchat-dsh-bot.exe'
$brainExe = Join-Path $BuildDir 'fake_dsh_brain.exe'
foreach ($exe in @($serverExe, $probeExe, $botExe, $brainExe)) {
    if (-not (Test-Path $exe)) {
        throw "找不到 $exe（先构建 dchat_server / room_probe / dchat-dsh-bot / fake_dsh_brain）"
    }
}

$work = Join-Path $BuildDir 'dsh-bot-e2e'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

# 打开 keepchathistory + offlinemessages：线上就是这么配的，而**登录时服务端会用
# 原始 SAY/RMSG 行重放历史**——这正是"机器人一上线就对着老消息逐条回答"的来源。
# 阶段 5 专门验证它不会那么干。
[IO.File]::WriteAllText("$work\dchat-rules.txt",
    "keepchathistory true`nofflinemessages 50`n",
    (New-Object Text.UTF8Encoding($false)))

$server = Start-Process -FilePath $serverExe -ArgumentList @('--port', "$Port") `
    -WorkingDirectory $work -RedirectStandardOutput "$work\server.out" `
    -RedirectStandardError "$work\server.err" -NoNewWindow -PassThru
$script:serverProc = $server

# 中途失败时也必须把服务器和机器人杀干净：它们继承了脚本的 stdout 管道，
# 不杀的话调用方（以及后台 job）会一直等管道关闭，看起来像"测试卡住了"。
trap {
    Write-Host "  !! 异常：$_" -ForegroundColor Red
    foreach ($p in $script:botProcs) { try { Stop-Process -Id $p.Id -Force } catch { } }
    if ($null -ne $script:serverProc) { try { Stop-Process -Id $script:serverProc.Id -Force } catch { } }
    exit 1
}

function Wait-Port {
    param([int]$p, [int]$timeoutMs)
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        try {
            $client = New-Object Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $p)
            $client.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 150
        }
    }
    return $false
}

function Invoke-Probe {
    param([string]$tag, [string[]]$lines, [string]$nick = 'alice',
          [string]$pass = 'alicepass1', [string]$mode = 'login')
    $inFile = Join-Path $work "$tag.in"
    $outFile = Join-Path $work "$tag.out"
    [IO.File]::WriteAllText($inFile, ($lines -join "`n"), (New-Object Text.UTF8Encoding($false)))
    $probeArgs = @('127.0.0.1', "$Port", $nick, $pass, $mode)
    $p = Start-Process -FilePath $probeExe -ArgumentList $probeArgs `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile `
        -RedirectStandardError "$work\$tag.err" -NoNewWindow -PassThru
    $p.WaitForExit(60000) | Out-Null
    $text = ''
    if (Test-Path $outFile) { $text = [IO.File]::ReadAllText($outFile, [Text.Encoding]::UTF8) }
    $script:lastOut[$tag] = ($text -split "`r?`n")
    return $text
}

function Write-BotConfig {
    param([string]$tag, [string]$rooms, [string]$mode, [string]$list,
          [string]$ack, [string]$brainArgs, [string]$cwd = '', [string]$perm = 'read-only',
          [string]$roots = '')
    $path = Join-Path $work "$tag.conf"
    $config = @"
# 端到端测试生成的配置（$tag）
server.host = 127.0.0.1
server.port = $Port
server.encrypt = auto
ui.port = $UiPort
bot.nick = 小d
bot.password = botpass123
bot.register = true
rooms = $rooms
rooms.autojoin = true
trigger.mentiononly = true
reply.mode = $mode
reply.list = $list
reply.acktext = $ack
typing.maxchars = 40
typing.minchars = 1
typing.maxchunks = 12
typing.mingapms = 200
typing.percharms = 30
typing.maxgapms = 800
typing.stripmarkdown = true
brain.command = "$brainExe" $brainArgs
brain.permissionmode = $perm
brain.cwd = $cwd
file.maxmb = 32
file.roots = $roots
brain.timeoutms = 20000
brain.sessionperroom = true
brain.maxconcurrent = 1
brain.maxqueue = 4
log.path = $work\bot-$tag.log
log.verbose = false
state.sessionfile = $work\bot-$tag.sessions
reconnect.minms = 500
reconnect.maxms = 2000
"@
    [IO.File]::WriteAllText($path, $config, (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Start-Bot {
    param([string]$tag, [string]$configPath)
    $out = Join-Path $work "bot-$tag.out"
    $p = Start-Process -FilePath $botExe -ArgumentList @('--config', $configPath) `
        -WorkingDirectory $work -RedirectStandardOutput $out `
        -RedirectStandardError "$work\bot-$tag.err" -NoNewWindow -PassThru
    $script:botProcs += $p
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        if ((Read-Shared $out).Contains('已登录为')) { return $p }
        Start-Sleep -Milliseconds 200
    }
    Get-Content $out -Encoding UTF8 | ForEach-Object { Write-Host "     $_" -ForegroundColor Yellow }
    throw "机器人 $tag 没有在 25 秒内登录"
}

function Stop-Bot {
    param($p)
    if ($null -eq $p) { return }
    try { Stop-Process -Id $p.Id -Force } catch { }
    Start-Sleep -Milliseconds 500
}

function BotLines {
    param([string]$text, [string]$room, [string]$nick)
    # 大厅走老格式 SAY（没有房间号），其余房间走 RMSG
    if ($room -eq 'hall') {
        $pattern = '^SAY [0-9:]+ ' + [regex]::Escape($nick) + ' (.*)$'
    } else {
        $pattern = '^RMSG [0-9:]+ ' + [regex]::Escape($room) + ' ' + [regex]::Escape($nick) + ' (.*)$'
    }
    $result = @()
    foreach ($line in ($text -split "`r?`n")) {
        $m = [regex]::Match($line, $pattern)
        if ($m.Success) { $result += $m.Groups[1].Value }
    }
    return $result
}

Write-Host "== dchat × DeepSeek Harness 机器人 端到端测试 =="

if (-not (Wait-Port $Port 15000)) {
    try { Stop-Process -Id $server.Id -Force } catch { }
    throw "服务器没起来（端口 $Port）"
}
Write-Host "  服务器已就绪（端口 $Port）"

# ---- 准备账号和群 ----
$setup = Invoke-Probe 'setup' @(
    ('GCREATE grp1 ' + (B64 '测试一群')),
    'GSET grp1 joinmode direct',
    ('GCREATE grp2 ' + (B64 '测试二群')),
    'GSET grp2 joinmode direct',
    ('GCREATE grp3 ' + (B64 '测试三群')),
    'GSET grp3 joinmode direct',
    ('GCREATE grp4 ' + (B64 '测试四群')),
    'GSET grp4 joinmode direct',
    ('GCREATE grp6 ' + (B64 '测试六群')),
    'GSET grp6 joinmode direct'
) -mode register
# 服务端的 LOGGEDIN 带时间前缀（`LOGGEDIN 09:45 alice`），所以用正则而不是子串
Check ([regex]::IsMatch($setup, 'LOGGEDIN [0-9:]+ alice')) '注册并登录 alice' 'setup'
Check (-not $setup.Contains('ERROR')) '建群与设置全部成功（没有 ERROR）' 'setup'

# ---- 阶段 1：被 @ 才回、回执在先、回答分条 ----
Write-Host "[1] @ 触发 + 分条发送"
$conf1 = Write-BotConfig 'p1' 'grp1' 'all' '' '（收到）' '--text 这是第一句。这是第二句！这是第三句？'
$bot1 = Start-Bot 'p1' $conf1
$talk1 = Invoke-Probe 'talk1' @(
    'ROOM grp1',
    '@sleep 600',
    'MSG 大家好呀',
    '@sleep 2500',
    'MSG @小d 说点什么',
    '@drain 9000'
)
$lines1 = BotLines $talk1 'grp1' '小d'
Check ($talk1.Contains('RMEMBERS grp1 小d ')) '机器人自动加入了群 grp1' 'talk1'
Check ($lines1.Count -eq 4) ("非 @ 的那条没触发；@ 之后回了 1 条回执 + 3 条正文（实际 " + $lines1.Count + " 条）") 'talk1'
if ($lines1.Count -ge 1) {
    Check ($lines1[0] -eq '（收到）') '先回一句"正在想"' 'talk1'
}
if ($lines1.Count -ge 4) {
    Check ($lines1[1] -eq '这是第一句。') '第一条正文' 'talk1'
    Check ($lines1[2] -eq '这是第二句！') '第二条正文（分开的，没有整段发）' 'talk1'
    Check ($lines1[3] -eq '这是第三句？') '第三条正文' 'talk1'
}
$botLogrp1 = Read-Shared "$work\bot-p1.out"
Check ($botLogrp1.Contains('已加密连接')) '用加密通道登录（HELLO 握手生效）'
Check ($botLogrp1.Contains('收到 @alice')) '日志里记了谁 @ 的它'
Stop-Bot $bot1

# ---- 阶段 2：黑名单里的人 @ 它，不回 ----
Write-Host "[2] 黑名单"
$conf2 = Write-BotConfig 'p2' 'grp2' 'blacklist' 'alice' '' '--text 我不该说话。'
$bot2 = Start-Bot 'p2' $conf2
$talk2 = Invoke-Probe 'talk2' @(
    'ROOM grp2',
    '@sleep 600',
    'MSG @小d 你在吗',
    '@drain 6000'
)
$lines2 = BotLines $talk2 'grp2' '小d'
Check ($lines2.Count -eq 0) ("黑名单里的 alice 被 @ 也不回（实际 " + $lines2.Count + " 条）") 'talk2'
Stop-Bot $bot2

# ---- 阶段 3：白名单里的人 @ 它，回；提示词里带上房间名和昵称 ----
Write-Host "[3] 白名单 + 提示词内容"
$conf3 = Write-BotConfig 'p3' 'grp3' 'whitelist' 'alice' '' '--echo'
$bot3 = Start-Bot 'p3' $conf3
$talk3 = Invoke-Probe 'talk3' @(
    'ROOM grp3',
    '@sleep 600',
    'MSG @小d 说点什么',
    '@drain 9000'
)
$lines3 = BotLines $talk3 'grp3' '小d'
$joined3 = $lines3 -join ''
Check ($lines3.Count -ge 1) ("白名单里的 alice 被 @ 会回（" + $lines3.Count + " 条）") 'talk3'
Check ($joined3.Contains('测试三群')) '提示词里带上了房间名（模板 {room} 生效）' 'talk3'
Check ($joined3.Contains('alice')) '提示词里带上了说话人' 'talk3'
Check ($joined3.Contains('说点什么')) '提示词里带上了正文' 'talk3'
Check (-not $joined3.Contains('@小d')) '正文里的 @ 已经被去掉，不会原样复述' 'talk3'
Stop-Bot $bot3

# ---- 阶段 4：没配置的群一律不理 ----
Write-Host "[4] 只接入配置里的群"
$conf4 = Write-BotConfig 'p4' 'hall' 'all' '' '' '--text 大厅专用。'
$bot4 = Start-Bot 'p4' $conf4
$talk4 = Invoke-Probe 'talk4' @(
    'ROOM grp4',
    '@sleep 600',
    'MSG @小d 说点什么',
    '@drain 6000'
)
$lines4 = BotLines $talk4 'grp4' '小d'
Check ($lines4.Count -eq 0) ("没在 rooms 里的群不回复（实际 " + $lines4.Count + " 条）") 'talk4'
$talk5 = Invoke-Probe 'talk5' @(
    'MSG @小d 大厅里喊你',
    '@drain 8000'
)
$lines5 = BotLines $talk5 'hall' '小d'
Check ($lines5.Count -ge 1) ("大厅是配置里的，会回（" + $lines5.Count + " 条）") 'talk5'
Stop-Bot $bot4

# ---- 阶段 5：登录时回放的历史消息不能被当成"有人刚 @ 我" ----
Write-Host "[5] 历史回放不触发回答"
# 机器人不在线时，有人在它被 @ 过：这条会进大厅历史，登录时被重放回来
$hist = Invoke-Probe 'hist' @(
    'MSG @小d 历史里的问题'
)
Check ($hist.Contains('历史里的问题')) '先把一条 @ 消息留在历史里' 'hist'
$conf5 = Write-BotConfig 'p5' 'hall' 'all' '' '' '--echo'
$bot5 = Start-Bot 'p5' $conf5
# 3 秒后才发实时消息：登录后的静默期是 2 秒（history.settlems）
$talk6 = Invoke-Probe 'talk6' @(
    '@sleep 3000',
    'MSG @小d 实时问题',
    '@drain 12000'
)
$lines6 = BotLines $talk6 'hall' '小d'
$joined6 = $lines6 -join ''
Check ($lines6.Count -ge 1) ("实时消息照常回（" + $lines6.Count + " 条）") 'talk6'
Check ($joined6.Contains('实时问题')) '回的是实时那条（提示词里带着它的正文）' 'talk6'
Check (-not $joined6.Contains('历史里的问题')) '历史里被 @ 的那条没有被回答' 'talk6'
$botLog5 = Read-Shared "$work\bot-p5.out"
Check ($botLog5.Contains('忽略了')) '日志里说明了丢掉了几条回放消息' 'talk6'
Stop-Bot $bot5

# ---- 阶段 6：本机管理界面（白黑名单 / 接入的群都能点着改，改完立刻生效）----
Write-Host "[6] 管理界面"
$conf6 = Write-BotConfig 'p6' 'grp6' 'all' '' '' '--text 界面改完之后的行为。'
$bot6 = Start-Bot 'p6' $conf6
$ui = "http://127.0.0.1:$UiPort"
$page = ''
try { $page = (Invoke-WebRequest -Uri "$ui/" -UseBasicParsing -TimeoutSec 5).Content } catch { }
Check ($page.Contains('dchat 机器人')) '首页能打开（GET / 返回页面）' 'ui'

$state = $null
try { $state = Invoke-RestMethod -Uri "$ui/api/state" -TimeoutSec 5 } catch { }
Check ($null -ne $state -and $state.ok) '状态接口可用（GET /api/state）' 'ui'
Check ($state.nick -eq '小d') '状态里带着机器人昵称' 'ui'
Check (($state.rooms -join ',') -eq 'grp6') '状态里带着当前接入的群' 'ui'
Check (($state.known | Where-Object { $_.id -eq 'grp6' }) -ne $null) '已加入的群出现在列表里' 'ui'

# 先用界面把 alice 拉黑：这之后她在群里 @ 它就不该有回应
$post = '{"replyMode":"blacklist","replyList":["alice"],"rooms":["grp6"]}'
try {
    $saved = Invoke-RestMethod -Method Post -Uri "$ui/api/config" -Headers @{ 'X-Dchat-Bot' = '1' } `
        -ContentType 'application/json' -Body $post -TimeoutSec 8
} catch {
    $saved = $null
}
Check ($null -ne $saved -and $saved.ok) '保存接口可用（POST /api/config）' 'ui'
Check ($saved.state.replyMode -eq 'blacklist') '保存后返回的状态是新值' 'ui'
$confText = [IO.File]::ReadAllText((Join-Path $work 'p6.conf'), [Text.Encoding]::UTF8)
Check ($confText.Contains('reply.mode = blacklist')) '改动写回了配置文件' 'ui'
Check ($confText.Contains('reply.list = alice')) '白黑名单写回了配置文件' 'ui'
Check ($confText.Contains('# 端到端测试生成的配置')) '写回时没把注释抹掉' 'ui'

$talk7 = Invoke-Probe 'talk7' @(
    'ROOM grp6',
    '@sleep 600',
    'MSG @小d 界面拉黑之后还回吗',
    '@drain 6000'
)
$lines7 = BotLines $talk7 'grp6' '小d'
Check ($lines7.Count -eq 0) ("界面改成黑名单后立刻不回了（实际 " + $lines7.Count + " 条）") 'talk7'

# 再改回"谁都回"，验证方向反过来也立刻生效
$post = '{"replyMode":"all","rooms":["grp6"]}'
try {
    $saved = Invoke-RestMethod -Method Post -Uri "$ui/api/config" -Headers @{ 'X-Dchat-Bot' = '1' } `
        -ContentType 'application/json' -Body $post -TimeoutSec 8
} catch {
    $saved = $null
}
Check ($null -ne $saved -and $saved.ok) '改回 all 也成功' 'ui'
$talk8 = Invoke-Probe 'talk8' @(
    'ROOM grp6',
    '@sleep 600',
    'MSG @小d 现在回了吗',
    '@drain 8000'
)
$lines8 = BotLines $talk8 'grp6' '小d'
Check ($lines8.Count -ge 1) ("改回 all 之后立刻又回了（" + $lines8.Count + " 条）") 'talk8'

# 没有自定义头的 POST 必须被拒（防止别的网站顺手改我们的配置）
$blocked = $null
try {
    $blocked = Invoke-RestMethod -Method Post -Uri "$ui/api/config" -ContentType 'application/json' `
        -Body '{"replyMode":"all"}' -TimeoutSec 5
} catch {
    $blocked = $null
}
Check ($null -eq $blocked) '缺自定义头的 POST 被拒（403）' 'ui'
Stop-Bot $bot6

# ---- 阶段 7：本机操作（发文件 / 工作目录 / 权限模式开关）----
Write-Host "[7] 本机操作"
$ws = Join-Path $work 'ws'
New-Item -ItemType Directory -Path $ws -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $ws 'hello.txt'), '本地文件内容', (New-Object Text.UTF8Encoding($false)))
# 工作目录**外**的一个文件：只有配了 file.roots 才允许发（只读，不能改）
$outside = Join-Path $work 'outside'
New-Item -ItemType Directory -Path $outside -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'secret.txt'), '只读目录里的文件', (New-Object Text.UTF8Encoding($false)))
$conf7 = Write-BotConfig 'p7' 'grp6' 'all' '' '' `
    ('--text "发你两个：[[file: hello.txt]][[file: ' + (Join-Path $outside 'secret.txt') + ']]"') `
    -cwd $ws -perm 'workspace-write' -roots $outside
$bot7 = Start-Bot 'p7' $conf7
$talk9 = Invoke-Probe 'talk9' @(
    'ROOM grp6',
    '@sleep 600',
    'MSG @小d 把 hello.txt 发我',
    '@drain 10000'
)
$lines9 = BotLines $talk9 'grp6' '小d'
$joined9 = $lines9 -join ''
Check ($joined9.Contains('发你两个')) '回答了正文' 'talk9'
Check (-not $joined9.Contains('[[file:')) '发文件指令不会原样显示在群里' 'talk9'
# 服务器把附件卡片广播成 RFILE_OFFER <时间> <房间> <昵称> <文件ID> <文件名B64> ...
$offerHello = 'RFILE_OFFER [0-9:]+ grp6 小d [A-Za-z0-9]+ ' + [regex]::Escape((B64 'hello.txt'))
$offerSecret = 'RFILE_OFFER [0-9:]+ grp6 小d [A-Za-z0-9]+ ' + [regex]::Escape((B64 'secret.txt'))
Check ($talk9 -match $offerHello) '工作目录里的文件传上去了（群里收到附件卡片）' 'talk9'
Check ($talk9 -match $offerSecret) '只读目录里（工作目录外）的文件也能发' 'talk9'

$state7 = $null
try { $state7 = Invoke-RestMethod -Uri "http://127.0.0.1:$UiPort/api/state" -TimeoutSec 5 } catch { }
Check ($null -ne $state7 -and $state7.brainMode -eq 'local') '状态里显示"允许改本机文件"' 'ui'
Check ($state7.workDir -eq $ws) '状态里带着工作目录' 'ui'
Check ($state7.patch.Contains('dsh-local-ops.yml')) '本机操作模式下用的是 local-ops 补丁' 'ui'

# 界面切回"只能聊天"：权限模式和补丁要一起换回来
$post = '{"brainMode":"chat"}'
$saved7 = $null
try {
    $saved7 = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$UiPort/api/config" `
        -Headers @{ 'X-Dchat-Bot' = '1' } -ContentType 'application/json' -Body $post -TimeoutSec 8
} catch { }
Check ($null -ne $saved7 -and $saved7.ok) '切换到"只能聊天"成功' 'ui'
Check ($saved7.state.brainMode -eq 'chat') '切换后模式是 chat' 'ui'
Check ($saved7.state.patch.Contains('dsh-chat-only.yml')) '切回后补丁换成了 chat-only' 'ui'
$confText7 = [IO.File]::ReadAllText((Join-Path $work 'p7.conf'), [Text.Encoding]::UTF8)
Check ($confText7.Contains('brain.permissionmode = read-only')) '权限模式写回配置文件' 'ui'
Stop-Bot $bot7

# ---- 收尾 ----
if ($null -ne $server) {
    try { Stop-Process -Id $server.Id -Force } catch { }
}

Write-Host ""
Write-Host ("{0} 项检查，{1} 项失败" -f $script:checks, $script:failures)
if ($script:failures -gt 0) { exit 1 }
exit 0
