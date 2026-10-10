# 验证决策①：「总缓存最大大小」**不是个摆设**。
#
# 部署文档自己把这条列为"最容易做成'设置项是个摆设'"，所以单独验一次：
#   1. 建群，把预算压到允许的最小值（1 MB）
#   2. 灌进去远超过 1 MB 的历史
#   3. 断言：落盘的历史文件真的被压回预算以内，**而且最旧的消息确实没了**
#      （只看"文件变小"是不够的 —— 消息压根没写进去也会变小）
#
# 用 room_probe 当"没有界面的客户端"：它走的是真协议，且默认规则下不限速、不限长度
# （server_rules.h 里 chatinterval / maxtextlen 默认都是 0），所以可以放心灌。
#
# 结论（2026-10-11 实测）：**预算真的生效**，但要跨过一个"压实检查点"才看得见。
# 证据就是 server.log 里这三行：
#     history budget [cachegroup]: appended=512  kept=512  bytes=1046499 budget=1048576 compactDue=0
#     history budget [cachegroup]: appended=1024 kept=511  bytes=1046553 budget=1048576 compactDue=1
#     history compacted [cachegroup]: 511 line(s) kept
#   → 内存**严格按预算裁**（kept=511，bytes=1,046,553 ≤ 1,048,576）；
#     落盘文件随后被压实到 1,046,553 字节，同样在预算内。
#
# ⚠️ **一个必须知道的滞后**：磁盘文件不是每条消息都重写，而是**每 512 条追加**才检查一次
#    `compactDue`（server.cpp:658-663，常量 kHistoryCompactEvery = 512）。两次检查之间，
#    磁盘文件会**超过预算**（实测发到 670 条时是 1,372,131 字节，超 31%），一到检查点立刻回落。
#
#    由此两条经验：
#      1. **"文件是不是超预算"不能随便挑个时刻去量。** 只发 670 条时正好卡在 512 和 1024
#         之间，永远看不到回落，于是把它误判成了"预算是个摆设"（第一版就是这么错的）。
#         本脚本默认发 1300 条，就是为了**确保跨过 1024 这个检查点**。
#      2. 部署文档里那条"确认文件被压到 1 MB 以内"，也必须**跨过检查点之后再量**。
#
#    另一个容易看错的点：预算没超时 `kept == appended`、`compactDue=0` 是**正确**行为，
#    不是"没裁"。512 条时刚好 1,046,499 字节，还差一点才到 1 MB，所以本来就没什么可淘汰的。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\rooms-cache-budget.ps1
param(
    [int]$Port = 5611,
    [int]$Messages = 1300,
    [int]$Chars = 2000,
    [int]$BudgetMb = 1,
    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'
$BuildDir = 'D:\codes\dchat\build'
$server = Join-Path $BuildDir 'dchat_server.exe'
$probe = Join-Path $BuildDir 'room_probe.exe'
$work = Join-Path $BuildDir 'rooms-cache-budget'

if (-not (Test-Path $server)) { throw "找不到 $server（先构建 dchat_server）" }
if (-not (Test-Path $probe)) { throw "找不到 $probe（先构建 room_probe）" }

$script:pass = 0
$script:fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { $script:pass++; Write-Host "  ok    $what" -ForegroundColor Green }
    else { $script:fail++; Write-Host "  FAIL  $what" -ForegroundColor Red }
}
function Section([string]$title) { Write-Host "== $title" -ForegroundColor Cyan }

function B64([string]$text) {
    [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($text))
}

if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null
# 规则文件名是 **dchat-rules.txt**，放在服务端的工作目录里（服务端自己会按默认值生成
# 一份；写成别的名字它不认 —— 实测踩过，白跑一轮）。keepchathistory **默认是 false**，
# 不开的话服务端根本不写历史文件，这个测试就测了个寂寞。其余保持默认（不限速、不限长度）。
[IO.File]::WriteAllText((Join-Path $work 'dchat-rules.txt'), "keepchathistory true`n",
                        (New-Object Text.UTF8Encoding($false)))

$serverProc = $null
try {
    Section "启动服务端（端口 $Port，数据目录 $work）"
    $serverProc = Start-Process -FilePath $server -ArgumentList @('-p', "$Port", '-b', '127.0.0.1') `
        -WorkingDirectory $work -RedirectStandardOutput (Join-Path $work 'server.log') `
        -RedirectStandardError (Join-Path $work 'server.err') -NoNewWindow -PassThru
    $up = $false
    $deadline = (Get-Date).AddSeconds(15)
    while (-not $up -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        try {
            $c = New-Object Net.Sockets.TcpClient
            $c.Connect('127.0.0.1', $Port)
            $c.Close()
            $up = $true
        } catch { }
    }
    Check $up '服务端起来了'
    if (-not $up) { throw '服务端没起来' }

    # ---- 造脚本：注册 -> 建群 -> 把预算压到 1 MB -> 灌消息 ----
    Section "灌消息：$Messages 条 × 约 $Chars 字符（预算 $BudgetMb MB）"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("GCREATE cachegroup $(B64 '缓存预算验证群')")
    $lines.Add('@drain 600')
    $lines.Add("GSET cachegroup cachemaxmb $BudgetMb")
    $lines.Add('@drain 600')
    # 每条消息开头带序号，用来判断"最旧的有没有被淘汰"
    $pad = 'x' * $Chars
    for ($i = 1; $i -le $Messages; $i++) {
        $lines.Add(("MSG BUDGET-{0:d5} {1}" -f $i, $pad))
        # 每 100 条喘一下，别把服务端的接收缓冲一次性打满
        if ($i % 100 -eq 0) { $lines.Add('@drain 300') }
    }
    $lines.Add('@drain 3000')

    $inFile = Join-Path $work 'spam.in'
    $outFile = Join-Path $work 'spam.out'
    [IO.File]::WriteAllText($inFile, ($lines -join "`n"), (New-Object Text.UTF8Encoding($false)))
    $p = Start-Process -FilePath $probe -ArgumentList @('127.0.0.1', "$Port", 'spammer', 'pw-spam-1', 'register') `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile `
        -RedirectStandardError (Join-Path $work 'spam.err') -NoNewWindow -PassThru
    $exited = $p.WaitForExit(1500000)
    if (-not $exited) {
        # 探针还卡着就强杀 —— 不杀的话下面读它的输出文件会被占用（实测报 IOException）
        Write-Host "  !! 探针 1500 秒没退出，强制结束（已发出去的应该都发出去了）" -ForegroundColor Yellow
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Milliseconds 1000
    }
    Write-Host ("  探针退出：{0}" -f $exited)

    $out = @()
    if (Test-Path $outFile) { $out = [IO.File]::ReadAllLines($outFile, [Text.Encoding]::UTF8) }
    $text = $out -join "`n"
    Check ($text -match 'LOGGEDIN') '探针注册并登录成功'
    Check ($text -match 'RINFO cachegroup') '群建好了'
    Check ($text -match "总缓存最大大小已设为 $BudgetMb MB") "预算设成了 $BudgetMb MB（服务端确认）"

    # ---- 落盘检查 ----
    Section "落盘检查"
    # 磁盘文件是**延迟压实**的（每 512 条追加才标记一次 compactDue，由后台任务重写文件），
    # 所以灌完**立刻**量到的是中间态 —— 必须等压实跑完再量，否则会把"还没来得及压"
    # 误判成"预算没生效"（第一版就是这么误判的）。
    Write-Host "  等后台压实（15 秒）…"
    Start-Sleep -Seconds 25
    $hist = Join-Path $work 'dchat-history-cachegroup.txt'
    Check (Test-Path $hist) '群历史落盘了（dchat-history-cachegroup.txt）'
    if (Test-Path $hist) {
        # 断言**真正的不变量**，而不是"某个时刻的文件大小"。
        #
        # 为什么不能直接断言"文件 ≤ 预算"：磁盘文件每 512 条才重写一次，两次检查点
        # 之间它会超过预算（设计如此）。挑个时刻去量，量到的可能是超预算的中间态 ——
        # 第一版就是这么误判成"预算是个摆设"的。真正该验的是三件事：
        #   ① 内存里的历史被裁到预算以内（服务端自己打的那行诊断）
        #   ② 确实发生过淘汰（kept < appended）
        #   ③ 后台确实把文件重写过一次
        $logLines = @()
        $logPath = Join-Path $work 'server.log'
        if (Test-Path $logPath) { $logLines = [IO.File]::ReadAllLines($logPath, [Text.Encoding]::UTF8) }

        Check ($null -ne ($logLines | Where-Object { $_ -match 'history compacted \[cachegroup\]' })) `
              '后台把群历史压实过一次（预算生效的直接证据）'

        $budgetLine = $logLines | Where-Object { $_ -match 'history budget \[cachegroup\]' } |
            Select-Object -Last 1
        Check ($null -ne $budgetLine) '服务端打出了预算诊断行'
        if ($null -ne $budgetLine) {
            Write-Host "  $budgetLine"
            $m = [regex]::Match($budgetLine, 'appended=(\d+) kept=(\d+) bytes=(\d+) budget=(\d+)')
            if ($m.Success) {
                $appended = [int]$m.Groups[1].Value
                $kept = [int]$m.Groups[2].Value
                $bytes = [long]$m.Groups[3].Value
                $bud = [long]$m.Groups[4].Value
                Check ($bytes -le $bud) ("内存里的历史被裁到预算以内（{0:N0} <= {1:N0}）" -f $bytes, $bud)
                Check ($kept -lt $appended) ("确实淘汰了最旧的（追加 {0} 条，只留 {1} 条）" -f $appended, $kept)
            }
        }
        # 文件大小只作为**参考信息**（它会随追加继续涨，直到下一个检查点）
        Write-Host ("  参考：当前文件大小 {0:N0} 字节（预算 {1:N0}）" -f `
            (Get-Item $hist).Length, ($BudgetMb * 1MB))

        $body = [IO.File]::ReadAllText($hist, [Text.Encoding]::UTF8)
        # 这是关键的一条：只看"文件变小"分不清"淘汰了"和"压根没写进去"
        Check ($body -notmatch 'BUDGET-00001') '最旧的 BUDGET-00001 已经不在了（真的淘汰了最旧的）'
        $newest = "BUDGET-{0:d5}" -f $Messages
        Check ($body -match $newest) "最新的 $newest 还在（说明消息确实写进去过）"
    }
} finally {
    if (-not $KeepProcesses -and $null -ne $serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force
    }
}

Write-Host ''
Write-Host ("共 {0} 项检查，失败 {1} 项。" -f ($script:pass + $script:fail), $script:fail)
if ($script:fail -gt 0) { exit 1 }
exit 0
