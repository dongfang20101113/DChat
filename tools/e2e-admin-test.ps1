# 管理员指令端到端测试（真实启动服务器并接管它的控制台输入）：
#   /ban <昵称> [时长]   封禁（不给时长 = 永久）；到时间自动解封；封禁/踢人/解封会广播给房间里其他人
#   /kick <昵称>         踢下线（不封禁，可立刻重连）
#   /unban <昵称>        解封
#   /op、/deop           授予/取消管理员权限；管理员可以在聊天框里用 / 开头的指令
#                        非管理员发 / 开头的指令会被拒绝；'/’不在首位则按普通消息处理
param(
    [string]$ServerExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_server.exe'),
    [int]$Port = 5620,
    [string]$UsersFile = (Join-Path $env:TEMP ('dchat-admin-users-{0}.txt' -f $PID)),
    [string]$RulesFile = (Join-Path $env:TEMP ('dchat-admin-rules-{0}.txt' -f $PID))
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
$script:failures = 0

# 测试账号的统一密码（测试用全新的账号文件，不会碰到真实数据）
$TestPassword = 'e2epass123'

function Check([bool]$ok, [string]$what) {
    $script:checks++
    if ($ok) { Write-Host ("  ok   {0}" -f $what) } else { $script:failures++; Write-Host ("  FAIL {0}" -f $what) }
}

function New-Peer([string]$nick) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    $peer = [pscustomobject]@{
        Client = $client; Sock = $client.Client; Nick = $nick
        Bytes  = New-Object 'System.Collections.Generic.List[byte]'
        Pending = New-Object 'System.Collections.Generic.List[string]'
    }
    return $peer
}

function Send-Line($peer, [string]$text) {
    $payload = [System.Text.Encoding]::UTF8.GetBytes($text + "`n")
    [void]$peer.Sock.Send($payload)
}

function Pump($peer) {
    $chunk = New-Object byte[] 4096
    while ($peer.Sock.Available -gt 0) {
        $n = $peer.Sock.Receive($chunk)
        if ($n -le 0) { break }
        for ($i = 0; $i -lt $n; $i++) { [void]$peer.Bytes.Add($chunk[$i]) }
    }
    $lines = New-Object System.Collections.Generic.List[string]
    while ($true) {
        $index = $peer.Bytes.IndexOf([byte]10)
        if ($index -lt 0) { break }
        $lineBytes = $peer.Bytes.GetRange(0, $index).ToArray()
        $peer.Bytes.RemoveRange(0, $index + 1)
        $line = [System.Text.Encoding]::UTF8.GetString($lineBytes)
        if ($line.EndsWith("`r")) { $line = $line.Substring(0, $line.Length - 1) }
        [void]$lines.Add($line)
    }
    return $lines
}

function Wait-For($peer, [string]$pattern, [int]$timeoutMs = 3000) {
    $collected = New-Object System.Collections.Generic.List[string]
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $batch = @()
        if ($peer.Pending.Count -gt 0) {
            $batch = $peer.Pending.ToArray()
            $peer.Pending.Clear()
        } else {
            $batch = @(Pump $peer)
        }
        for ($i = 0; $i -lt $batch.Count; $i++) {
            [void]$collected.Add($batch[$i])
            if ($batch[$i] -match $pattern) {
                for ($j = $i + 1; $j -lt $batch.Count; $j++) { [void]$peer.Pending.Add($batch[$j]) }
                return [pscustomobject]@{ ok = $true; lines = $collected }
            }
        }
        Start-Sleep -Milliseconds 20
    }
    return [pscustomobject]@{ ok = $false; lines = $collected }
}

function Wait-Closed($peer, [int]$timeoutMs = 3000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        try {
            if ($peer.Sock.Available -gt 0) {
                $chunk = New-Object byte[] 1024
                $n = $peer.Sock.Receive($chunk)
                if ($n -le 0) { return $true }
            } elseif ($peer.Sock.Poll(50000, [System.Net.Sockets.SelectMode]::SelectRead) -and
                      $peer.Sock.Available -eq 0) {
                return $true
            }
        } catch { return $true }
        Start-Sleep -Milliseconds 40
    }
    return $false
}

function Close-Peer($peer) {
    if ($null -ne $peer) { try { $peer.Client.Close() } catch { } }
}

if (-not (Test-Path -LiteralPath $ServerExe)) { Write-Host "找不到 dchat_server.exe"; exit 1 }

# 用独立的账号文件，避免污染真实账号
if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force }
# 规则也要用独立的文件（顺便验证规则会落盘）
if (Test-Path -LiteralPath $RulesFile) { Remove-Item -LiteralPath $RulesFile -Force }

$log = [hashtable]::Synchronized(@{ lines = New-Object System.Collections.ArrayList })
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ServerExe
$psi.Arguments = "--port $Port --users `"$UsersFile`" --rules `"$RulesFile`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
# 说明：.NET Framework 的 ProcessStartInfo 没有 StandardInputEncoding（.NET Core 才有），
# 而 PowerShell 写标准输入时第一行会带上 UTF-8 BOM，所以服务器那边做了 BOM 容错。
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8

$server = [System.Diagnostics.Process]::Start($psi)
$subscription = Register-ObjectEvent -InputObject $server -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) { [void]$Event.MessageData.lines.Add($EventArgs.Data) }
} -MessageData $log
$server.BeginOutputReadLine()
$stdin = $server.StandardInput
$alice = $null; $bob = $null; $carol = $null; $dave = $null

function Send-Command([string]$text) {
    $stdin.WriteLine($text)
    $stdin.Flush()
    Start-Sleep -Milliseconds 250
}
function Server-Log() { return ($log.lines.ToArray() -join "`n") }

# 服务器的日志是异步收集的，断言前轮询等待一下，别写死 sleep
function Wait-Log([string]$pattern, [int]$timeoutMs = 2500) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ((Server-Log) -match $pattern) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return $false
}

# 注册（第一次）或登录（账号已存在），成功后返回连接。
# 昵称被占用 / 账号已存在等情况会重试几次，避免和上一个连接的清理竞态。
function Join-Peer([string]$nick, [int]$attempts = 4) {
    for ($i = 1; $i -le $attempts; $i++) {
        $peer = New-Peer $nick
        Send-Line $peer ("REGISTER " + $nick + " " + $TestPassword)
        if ((Wait-For $peer 'NAMES' 1500).ok) { return $peer }
        Send-Line $peer ("LOGIN " + $nick + " " + $TestPassword)
        if ((Wait-For $peer 'NAMES' 1500).ok) { return $peer }
        Close-Peer $peer
        Start-Sleep -Milliseconds 400
    }
    return $null
}

try {
    Start-Sleep -Milliseconds 900
    Write-Host ("== 管理员指令端到端测试（端口 {0}） ==" -f $Port)
    Check (Wait-Log 'listening on port') '服务器已启动'

    $alice = Join-Peer 'alice'
    Check ($null -ne $alice) 'alice 注册并加入'

    # ---- 1) 定时封禁 + 广播 ----
    Send-Command '/ban bob 2s'
    Check (Wait-Log 'banned bob for 2s') '日志记录了封禁（含时长）'
    $aliceSawBan = Wait-For $alice '已被管理员封禁'
    Check $aliceSawBan.ok '封禁信息广播给了房间里的其他人'
    $bob = New-Peer 'bob'
    Send-Line $bob ("REGISTER bob " + $TestPassword)
    Check ((Wait-For $bob 'ERROR').ok) '被封禁的 bob 被拒绝加入'
    Check (Wait-Closed $bob) '被封禁的连接被服务器断开'
    $bob = $null

    # ---- 2) 永久封禁（不给时长）----
    Send-Command '/ban dave'
    Check (Wait-Log 'banned dave for forever') '不给时长 = 永久封禁'
    $dave = New-Peer 'dave'
    Send-Line $dave ("REGISTER dave " + $TestPassword)
    $daveRejected = Wait-For $dave 'ERROR'
    Check ($daveRejected.ok -and (($daveRejected.lines -join '|') -match '永久封禁')) '永久封禁的 dave 收到"永久封禁"提示'
    $dave = $null
    Send-Command '/bans'
    Check (Wait-Log 'dave\(永久\)') '/bans 显示永久封禁'

    # ---- 3) /op 让普通用户获得管理员权限 ----
    Send-Command '/op alice'
    Check (Wait-Log 'op: alice') '日志记录了 /op'
    Check ((Wait-For $alice '已成为管理员').ok) 'alice 收到"已成为管理员"的提示'
    Send-Command '/ops'
    Check (Wait-Log 'op list: alice') '/ops 能列出管理员名单'
    # 控制台的 /ops 只写日志；要让管理员本人看到名单，得由他在聊天框里发
    Send-Line $alice 'MSG /ops'
    Check ((Wait-For $alice '管理员名单：alice').ok) '管理员在聊天框发 /ops 能看到名单'

    # ---- 3.5) /say 全服公告：控制台发 + 管理员在聊天框发 ----
    Send-Command '/say 服务器将在 10 分钟后维护'
    Check (Wait-Log 'announce: 服务器将在 10 分钟后维护') '日志记录了控制台公告'
    $announce1 = Wait-For $alice 'ANNOUNCE .*服务器将在 10 分钟后维护'
    Check $announce1.ok '房间里的用户收到公告消息（客户端会大字居中显示）'
    Send-Line $alice 'MSG /say 大家好，我是管理员 alice'
    $announce2 = Wait-For $alice 'ANNOUNCE .*大家好，我是管理员 alice'
    Check $announce2.ok '管理员在聊天框发的公告也会广播给所有人'

    # ---- 3.6) 打错的指令只在本地报错，不能当成聊天消息发出去（Minecraft 的行为）----
    Send-Line $alice 'MSG /bann bob 1h'
    Check ((Wait-For $alice '未知指令').ok) '打错的指令只在本地收到"未知指令"提示'
    Check (Wait-Log 'unknown command from alice') '服务器记录了这条未知指令'
    $leaked = Wait-For $alice 'SAY .*bann' 700
    Check (-not $leaked.ok) '打错的指令不会被当成聊天消息广播'
    Send-Line $alice 'MSG /ban'
    Check ((Wait-For $alice '用法').ok) '指令名对但参数不对时提示用法（不广播）'

    # ---- 4) 管理员在聊天框里用指令：/kick carol ----
    $carol = Join-Peer 'carol'
    Check ($null -ne $carol) 'carol 注册并加入'
    Send-Line $alice 'MSG /kick carol'
    Check (Wait-Log 'op command from alice') '服务器把 alice 的消息识别成指令'
    Check ((Wait-For $alice '已把 carol 移出房间').ok) 'alice 收到执行结果'
    Start-Sleep -Milliseconds 300
    Check (Wait-Closed $carol) 'carol 被管理员踢下线'
    $carol = $null

    # ---- 5) 非管理员用指令会被拒绝 ----
    $carol = Join-Peer 'carol'
    Check ($null -ne $carol) 'carol 重新加入'
    Send-Line $carol 'MSG /kick alice'
    Check ((Wait-For $carol '没有管理员权限').ok) '非管理员用指令被拒绝'
    Check (Wait-Log 'command denied for non-op: carol') '服务器记录了拒绝原因'

    # ---- 6) '/' 不在首位 → 普通消息 ----
    Send-Line $carol 'MSG 我/你 这种斜杠是普通消息'
    $normal = Wait-For $alice '我/你 这种斜杠是普通消息'
    Check $normal.ok '"/" 不在首位时按普通消息广播'

    # ---- 7) /deop 之后不能再执行指令 ----
    Send-Command '/deop alice'
    Check (Wait-Log 'deop: alice') '日志记录了 /deop'
    Check ((Wait-For $alice '管理员权限已取消').ok) 'alice 收到权限取消提示'
    Send-Command '/ops'
    Check (Wait-Log 'op list: \(没有管理员\)') '/deop 之后管理员名单为空'
    Send-Line $alice 'MSG /kick carol'
    Check ((Wait-For $alice '没有管理员权限').ok) '/deop 之后 alice 无法再执行指令'

    # ---- 8) 到时间自动解封 ----
    Start-Sleep -Milliseconds 1800
    Check (Wait-Log 'ban expired: bob') '日志记录了封禁到期'
    $bob = Join-Peer 'bob'
    Check ($null -ne $bob) '封禁到时间后 bob 可以加入'

    # ---- 9) 永久封禁不会自动过期，只能 /unban ----
    $dave = New-Peer 'dave'
    Send-Line $dave ("REGISTER dave " + $TestPassword)
    Check ((Wait-For $dave 'ERROR').ok) '永久封禁的 dave 仍然连不上'
    $dave = $null
    Send-Command '/unban dave'
    Check (Wait-Log 'unbanned dave') '日志记录了 /unban'
    $dave = Join-Peer 'dave'
    Check ($null -ne $dave) '/unban 之后 dave 可以加入'

    # ---- 10) /changepassword：控制台给别的账号改密码 ----
    Send-Command '/changepassword'
    Check (Wait-Log '用法：/changepassword <新密码>') '控制台不给参数时提示用法'
    # 只给一个参数：控制台没有"自己"这个概念，应该提醒要写昵称
    Send-Command '/changepassword somepass123'
    Check (Wait-Log '用法：/changepassword <昵称> <新密码>') '控制台只给密码时提醒要写昵称'
    Send-Command '/changepassword dave davepass999'
    Check (Wait-Log 'password changed for account: dave') '控制台改密码成功'
    Check ((Server-Log) -notmatch 'davepass999') '服务器日志里不会出现密码明文'
    Check ((Wait-For $dave '你的密码已被管理员修改').ok) '在线的账号会收到"密码已被管理员修改"的通知'

    Close-Peer $dave
    $dave = New-Peer 'dave'
    Send-Line $dave ('LOGIN dave davepass999')
    Check ((Wait-For $dave 'NAMES').ok) '用新密码可以登录'
    $daveOld = New-Peer 'dave'
    Send-Line $daveOld ('LOGIN dave ' + $TestPassword)
    Check ((Wait-For $daveOld 'ERROR .*密码错误').ok) '旧密码已经不能用了'

    # ---- 11) /ip：查在线客户端的 IP 和端口（只能服务器控制台用）----
    Send-Command '/ip carol'
    Check (Wait-Log 'ip of carol: 127\.0\.0\.1:\d+') '控制台 /ip 能查到在线客户端的 IP 和端口'
    Send-Command '/ip nobodyhere'
    Check (Wait-Log 'ip lookup: nobodyhere 不在线') '查不在线的人会提示不在线'
    Send-Line $carol 'MSG /ip alice'
    Check ((Wait-For $carol 'ERROR .*只能在服务器控制台').ok) '聊天框里 /ip 被拒绝（仅控制台可用）'

    # ---- 12) /cp 简写：控制台改别人的密码 ----
    Send-Command '/cp dave davepass888'
    Check (Wait-Log 'password changed for account: dave') '控制台 /cp 也能给别的账号改密码（与 /changepassword 等价）'

    # ---- 13) /chatrule：服务器规则（仅控制台）----
    Send-Command '/chatrule'
    Check (Wait-Log 'chatinterval = 0 ms') '/chatrule 列出全部规则（chatinterval 默认 0）'
    Check (Wait-Log 'maxservertemp = 1048 MB') '默认 maxservertemp = 1048 MB'
    Send-Command '/chatrule chatinterval set 400'
    Check (Wait-Log 'chatrule: chatinterval = 400 ms') 'set 生效（间隔 400ms）'
    # 规则改动要落盘：dchat-rules.txt 里应该能看到新值
    $rulesText = if (Test-Path -LiteralPath $RulesFile) {
        [System.IO.File]::ReadAllText($RulesFile, [System.Text.Encoding]::UTF8)
    } else { '' }
    Check ($rulesText -match 'chatinterval 400') '规则改动写进了 dchat-rules.txt（重启后仍生效）'
    Check ($rulesText -match 'keepchathistory (true|false)') '规则文件里四类规则都写全了'
    Send-Line $bob 'MSG 间隔内的第一条'
    Check ((Wait-For $alice 'SAY .*bob 间隔内的第一条').ok) '间隔内的第一条正常广播'
    Send-Line $bob 'MSG 紧接着的第二条'
    Check ((Wait-For $bob 'ERROR .*发言太快').ok) '紧接着的第二条被 chatinterval 拦下'
    $leaked = Wait-For $alice 'SAY .*bob 紧接着的第二条' 700
    Check (-not $leaked.ok) '被拦下的消息不会广播给别人'
    Send-Command '/chatrule chatinterval set 0'
    Check (Wait-Log 'chatrule: chatinterval = 0 ms') '改回 0 = 不限制'
    Send-Line $bob 'MSG 恢复后的消息'
    Check ((Wait-For $alice 'SAY .*bob 恢复后的消息').ok) '恢复后又能正常发言'

    # keepchathistory：新加入的人能看到之前的聊天记录
    Send-Command '/chatrule keepchathistory true'
    Check (Wait-Log 'chatrule: keepchathistory = true') '打开聊天记录保留'
    $later = New-Peer 'erin'
    Send-Line $later ("REGISTER erin " + $TestPassword)
    Check ((Wait-For $later 'NAMES').ok) 'erin 注册并加入'
    # 登录后服务器会下发"已注册名单"（KNOWN）：客户端拿它补 /ban /op /unban /ip 的参数
    $knownLine = Wait-For $later 'KNOWN' 2000
    $knownText = $knownLine.lines -join '|'
    Check ($knownLine.ok -and ($knownText -match 'alice') -and ($knownText -match 'bob')) `
          '登录后收到已注册名单（不在线的人也能被 Tab 补出来）'
    Check ((Wait-For $later 'SAY .*恢复后的消息').ok) '新加入的 erin 能看到之前的聊天记录'
    Send-Command '/chatrule keepchathistory false'
    Check (Wait-Log 'chatrule: keepchathistory = false') '关闭聊天记录保留'

    # documentsize / maxservertemp 与它们之间的约束
    Send-Command '/chatrule documentsize set 1'
    Check (Wait-Log 'chatrule: documentsize = 1 MB') 'documentsize 改成 1 MB'
    $bigName = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes('big.bin'))
    Send-Line $bob ("FILE_SEND big1 " + $bigName + " 2097152")
    Check ((Wait-For $bob 'ERROR .*文件太大了').ok) '超过 documentsize 的文件被拒绝'
    Send-Command '/chatrule maxservertemp set 4'
    Check (Wait-Log 'maxservertemp = 16 MB') '低于最小值会被夹到 16 MB'
    Send-Command '/chatrule documentsize set 2000'
    Check (Wait-Log '不能超过 maxservertemp') 'documentsize 不能超过 maxservertemp'
    Send-Command '/chatrule maxservertemp set 512'
    Check (Wait-Log 'chatrule: maxservertemp = 512 MB') 'maxservertemp 可以调大'
    Send-Command '/chatrule documentsize set 64'
    Check (Wait-Log 'chatrule: documentsize = 64 MB') 'documentsize 可以调回 64 MB'
    Send-Command '/chatrule maxservertemp set 32'
    Check (Wait-Log '不能小于当前的 documentsize') 'maxservertemp 不能小于 documentsize'
    Send-Line $bob 'MSG /chatrule chatinterval set 100'
    Check ((Wait-For $bob 'ERROR .*只能在服务器控制台').ok) '聊天框里 /chatrule 被拒绝（仅控制台）'

    Write-Host ("共 {0} 项检查，失败 {1} 项。" -f $script:checks, $script:failures)
} finally {
    foreach ($peer in @($alice, $bob, $carol, $dave, $daveOld, $later)) { Close-Peer $peer }
    try { $stdin.Close() } catch { }
    if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    Unregister-Event -SubscriptionId $subscription.Id -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $RulesFile) { Remove-Item -LiteralPath $RulesFile -Force -ErrorAction SilentlyContinue }
    Write-Host '已关闭测试用的服务器进程'
}

if ($script:failures -eq 0) { exit 0 } else { exit 1 }
