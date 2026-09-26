# 端到端自测：启动服务器 → 注册/登录 → 验证广播 / 重名 / 在线名单 / 离开通知 / 账号持久化 → 关闭服务器
# 用法：powershell -ExecutionPolicy Bypass -File tools\e2e-test.ps1
#
# 注意：这里在字节层面自己拆行，故意不用 StreamReader——
# StreamReader 会把后续数据读进自己的内部缓冲区，用 Socket.Available 判断会误判成"没有数据"。
param(
    [int]$Port = 5599,
    [string]$ServerExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_server.exe'),
    [string]$UsersFile = (Join-Path $env:TEMP ('dchat-e2e-users-{0}.txt' -f $PID))
)

$ErrorActionPreference = 'Stop'
$script:checks = 0
$script:failures = 0

# 测试账号（每次跑测试都用全新的账号文件，所以可以放心写死）
$AliceOldPassword = 'alicepass123'
$AlicePassword = 'alicepass123'   # 中途会通过 /changepassword 换掉，后面统一用这个变量
$BobPassword = 'bobpass123'
$Alice2Password = 'alice2pass123'
$Bob2Password = 'bob2pass123'

function Check([bool]$ok, [string]$what) {
    $script:checks++
    if ($ok) {
        Write-Host ("  ok   {0}" -f $what)
    } else {
        $script:failures++
        Write-Host ("  FAIL {0}" -f $what)
    }
}

# 只负责建立连接并收下 WELCOME，登录/注册由调用方自己发，方便逐个场景断言
function New-DchatClient([string]$label) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    $bytes = New-Object 'System.Collections.Generic.List[byte]'
    return [pscustomobject]@{
        Label = $label; Client = $client; Sock = $client.Client; Bytes = $bytes
        Pending = New-Object 'System.Collections.Generic.List[string]'
    }
}

function Send-Line($conn, [string]$text) {
    $payload = [System.Text.Encoding]::UTF8.GetBytes($text + "`n")
    [void]$conn.Sock.Send($payload)
}

# 把已到达的字节收进缓冲，再按 \n 切成完整行
function Pump($conn) {
    $chunk = New-Object byte[] 4096
    while ($conn.Sock.Available -gt 0) {
        $n = $conn.Sock.Receive($chunk)
        if ($n -le 0) { break }
        for ($i = 0; $i -lt $n; $i++) { [void]$conn.Bytes.Add($chunk[$i]) }
    }

    $lines = New-Object System.Collections.Generic.List[string]
    while ($true) {
        $index = $conn.Bytes.IndexOf([byte]10)
        if ($index -lt 0) { break }
        $lineBytes = $conn.Bytes.GetRange(0, $index).ToArray()
        $conn.Bytes.RemoveRange(0, $index + 1)
        $line = [System.Text.Encoding]::UTF8.GetString($lineBytes)
        if ($line.EndsWith("`r")) { $line = $line.Substring(0, $line.Length - 1) }
        [void]$lines.Add($line)
    }
    return $lines
}

# 一直读到匹配到预期内容为止（而不是死等固定时间），返回 { ok, lines }
# 同一批里还没被读到的行会放回 Pending，留给下一次读取，避免丢消息
function Wait-For($conn, [string]$pattern, [int]$timeoutMs = 3000) {
    $collected = New-Object System.Collections.Generic.List[string]
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $batch = @()
        if ($conn.Pending.Count -gt 0) {
            $batch = $conn.Pending.ToArray()
            $conn.Pending.Clear()
        } else {
            $batch = @(Pump $conn)
        }
        for ($i = 0; $i -lt $batch.Count; $i++) {
            [void]$collected.Add($batch[$i])
            if ($batch[$i] -match $pattern) {
                for ($j = $i + 1; $j -lt $batch.Count; $j++) { [void]$conn.Pending.Add($batch[$j]) }
                return [pscustomobject]@{ ok = $true; lines = $collected }
            }
        }
        Start-Sleep -Milliseconds 20
    }
    return [pscustomobject]@{ ok = $false; lines = $collected }
}

function Close-Conn($conn) {
    if ($null -ne $conn) { try { $conn.Client.Close() } catch { } }
}

if (-not (Test-Path -LiteralPath $ServerExe)) {
    Write-Host ("找不到服务器程序: {0}" -f $ServerExe)
    exit 1
}

# 每次测试都用干净的账号文件，避免污染真实数据
if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force }

$serverArgs = @('--port', $Port, '--users', $UsersFile)
$server = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs -PassThru -WindowStyle Hidden
$alice = $null
$bob = $null
$extra = @()

try {
    Start-Sleep -Milliseconds 900
    Write-Host ("== 聊天端到端测试（端口 {0}） ==" -f $Port)
    Write-Host ("   测试账号文件：{0}" -f $UsersFile)

    # ---------------------------------------------------------------- 注册 / 登录
    $alice = New-DchatClient 'alice'
    $welcomeA = Wait-For $alice 'WELCOME \d{2}:\d{2}'
    Check $welcomeA.ok '客户端 A 收到 WELCOME（带时间戳）'

    Send-Line $alice 'MSG 我还没登录'
    Check ((Wait-For $alice 'ERROR .*请先登录').ok) '未登录时发言被服务器拒绝'

    Send-Line $alice 'NICK alice'
    Check ((Wait-For $alice 'ERROR .*LOGIN').ok) 'NICK 已废弃，提示改用 LOGIN'

    Send-Line $alice ('LOGIN alice ' + $AlicePassword)
    Check ((Wait-For $alice 'ERROR .*用户名不存在').ok) '没注册就登录被拒绝（提示先注册）'

    Send-Line $alice 'REGISTER alice 123'
    Check ((Wait-For $alice 'ERROR .*密码至少').ok) '注册时太短的密码被拒绝'

    Send-Line $alice ('REGISTER alice ' + $AlicePassword)
    Check ((Wait-For $alice 'SYS .*注册成功').ok) '注册成功并收到确认'
    Check ((Wait-For $alice 'LOGGEDIN .*alice').ok) '服务器明确回了 LOGGEDIN（认证通过的标志）'
    $afterJoinA = Wait-For $alice 'NAMES'
    Check ($afterJoinA.ok -and (($afterJoinA.lines -join '|') -match 'alice')) 'A 注册后进入房间并收到在线名单'
    # 登录后服务器会把当前规则发过来（客户端据此调整单文件上限等本地检查）
    $rules = Wait-For $alice 'RULES \d+ \d+ [01]'
    Check $rules.ok '登录后收到服务器规则（RULES 行）'

    # ---------------------------------------------------------------- 改密码
    # 在聊天框里改自己的密码：自助操作，不需要管理员权限
    Send-Line $alice 'MSG /changepassword alicepass456'
    Check ((Wait-For $alice 'SYS .*密码已修改').ok) '在聊天框里改自己的密码成功（不需要管理员）'
    # 在聊天框里给别的账号改密码：必须被拒绝
    Send-Line $alice 'MSG /changepassword bob bobpass999'
    Check ((Wait-For $alice 'ERROR .*只能在服务器控制台').ok) '在聊天框里给别的账号改密码被拒绝'
    # 弱密码
    Send-Line $alice 'MSG /changepassword 123'
    Check ((Wait-For $alice 'ERROR .*密码至少').ok) '改密码时太短的密码被拒绝'
    # 日志里不能出现密码（服务器回的内容里也不该有）
    $AlicePassword = 'alicepass456'

    # ---- /cp 是 /changepassword 的简写，行为完全一样 ----
    Send-Line $alice 'MSG /cp alicepass789'
    Check ((Wait-For $alice 'SYS .*密码已修改').ok) '/cp 等价于 /changepassword（聊天框里改密码成功）'
    $cpOld = New-DchatClient 'alice-after-cp'
    $extra += $cpOld
    [void](Wait-For $cpOld 'WELCOME')
    Send-Line $cpOld 'LOGIN alice alicepass456'
    Check ((Wait-For $cpOld 'ERROR .*密码错误').ok) '用 /cp 改完之后，上一个密码也失效了'
    Send-Line $cpOld 'QUIT'
    $AlicePassword = 'alicepass789'

    $oldLogin = New-DchatClient 'alice-old-password'
    $extra += $oldLogin
    [void](Wait-For $oldLogin 'WELCOME')
    Send-Line $oldLogin ('LOGIN alice ' + $AliceOldPassword)
    Check ((Wait-For $oldLogin 'ERROR .*密码错误').ok) '改完密码后旧密码立刻失效'
    Send-Line $oldLogin 'QUIT'

    # 重复注册同名
    $dup = New-DchatClient 'alice-dup'
    $extra += $dup
    [void](Wait-For $dup 'WELCOME')
    Send-Line $dup ('REGISTER alice ' + $Alice2Password)
    Check ((Wait-For $dup 'ERROR .*用户名已存在').ok) '重复注册同名账号被拒绝'

    # 密码错误
    $intruder = New-DchatClient 'intruder'
    $extra += $intruder
    [void](Wait-For $intruder 'WELCOME')
    Send-Line $intruder 'LOGIN alice totally-wrong-1'
    Check ((Wait-For $intruder 'ERROR .*密码错误').ok) '密码错误被拒绝'

    # 同名在线（密码正确也不能重复顶号）
    Send-Line $intruder ('LOGIN alice ' + $AlicePassword)
    Check ((Wait-For $intruder 'ERROR .*昵称已被占用').ok) '同一账号重复上线被拒绝'
    Send-Line $intruder 'QUIT'

    # ---------------------------------------------------------------- 第二个用户
    $bob = New-DchatClient 'bob'
    [void](Wait-For $bob 'WELCOME')
    Send-Line $bob ('REGISTER bob ' + $BobPassword)
    $afterJoinB = Wait-For $bob 'NAMES'
    $noticeA = Wait-For $alice 'JOINED \d{2}:\d{2} bob'
    Check ($afterJoinB.ok -and (($afterJoinB.lines -join '|') -match 'bob')) 'B 注册成功并收到名单'
    Check $noticeA.ok 'A 收到 "JOINED bob" 通知'

    # ---------------------------------------------------------------- 账号文件
    $raw = [System.IO.File]::ReadAllText($UsersFile, [System.Text.Encoding]::UTF8)
    Check ($raw -match 'alice' -and $raw -match 'bob') '账号文件里保存了用户名'
    Check ($raw -notmatch [regex]::Escape($AlicePassword) -and
           $raw -notmatch [regex]::Escape($AliceOldPassword) -and
           $raw -notmatch [regex]::Escape($BobPassword)) '账号文件里不含明文密码（只有加盐哈希）'
    Check ($raw -match 'pbkdf2') '账号文件标明了密码哈希算法'

    # ---------------------------------------------------------------- 聊天内容
    Send-Line $alice 'MSG 大家好，我是 alice'
    $sayA = Wait-For $alice 'SAY \d{2}:\d{2} alice'
    $sayB = Wait-For $bob 'SAY \d{2}:\d{2} alice'
    Check ($sayA.ok -and $sayB.ok) 'A 的发言既回显给自己也广播给 B'

    Send-Line $bob 'MSG 你好 alice'
    Check ((Wait-For $alice 'SAY \d{2}:\d{2} bob 你好 alice').ok) 'A 收到 B 的回复'

    # @提及：内容原样送达对端（"高亮"是客户端渲染的事，服务器只负责转发）
    Send-Line $alice 'MSG @bob 在吗'
    Check ((Wait-For $bob 'SAY \d{2}:\d{2} alice @bob 在吗').ok) '@提及文本原样送达对端'

    # @all：同样只是普通文本，服务器原样转发（高亮由各客户端自己判断）
    Send-Line $bob 'MSG @all 大家好'
    Check ((Wait-For $alice 'SAY \d{2}:\d{2} bob @all 大家好').ok) '@all 文本原样送达对端'

    Send-Line $bob 'LIST'
    $listB = Wait-For $bob 'NAMES'
    $names = ($listB.lines -join '|')
    Check ($listB.ok -and $names -match 'alice' -and $names -match 'bob') '在线名单包含两个成员'

    Send-Line $alice 'PING'
    Check ((Wait-For $alice 'PONG').ok) 'PING 得到 PONG 回应'

    Send-Line $alice 'QUIT'
    Check ((Wait-For $bob 'LEFT \d{2}:\d{2} alice').ok) 'B 收到 A 离开的通知'

    # ---------------------------------------------------------------- 持久化
    Stop-Process -Id $server.Id -Force
    Start-Sleep -Milliseconds 600
    $server = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 900

    $again = New-DchatClient 'alice-again'
    $extra += $again
    [void](Wait-For $again 'WELCOME')
    Send-Line $again ('LOGIN alice ' + $AlicePassword)
    Check ((Wait-For $again 'LOGGEDIN .*alice').ok) '重启后登录同样收到 LOGGEDIN'
    $relogin = Wait-For $again 'NAMES'
    Check ($relogin.ok -and (($relogin.lines -join '|') -match 'alice')) '重启服务器后老账号仍能登录（账号已落盘）'
    Send-Line $again ('LOGIN alice ' + $AlicePassword)
    Check ((Wait-For $again 'ERROR .*无需再次登录').ok) '已登录的连接重复发 LOGIN 会被提示'

    # 改过的密码也落盘了：重启之后旧的仍然不能用
    $stale = New-DchatClient 'alice-stale-password'
    $extra += $stale
    [void](Wait-For $stale 'WELCOME')
    Send-Line $stale ('LOGIN alice ' + $AliceOldPassword)
    Check ((Wait-For $stale 'ERROR .*密码错误').ok) '重启之后旧密码依然无效（改动已写进账号文件）'
    Send-Line $stale 'QUIT'

    Write-Host ""
    Write-Host "交流记录（B 视角收到的内容）:"
    foreach ($item in @($afterJoinB, $sayB)) {
        foreach ($line in $item.lines) { Write-Host ("    " + $line) }
    }
    Write-Host ("共 {0} 项检查，失败 {1} 项。" -f $script:checks, $script:failures)
} finally {
    foreach ($conn in (@($alice, $bob) + $extra)) { Close-Conn $conn }
    if ($null -ne $server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        Write-Host "已关闭测试用服务器进程"
    }
    if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force -ErrorAction SilentlyContinue }
}

if ($script:failures -eq 0) { exit 0 } else { exit 1 }
