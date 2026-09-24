# 界面冒烟测试：自动启动服务器与客户端 → 自动填连接对话框并连接 → 用模拟用户发一批消息
# → 再让客户端自己发一条 → 抓取客户端窗口截图（含滚动条/标题栏/气泡）→ 清理进程
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\ui-smoke-test.ps1
#   powershell -ExecutionPolicy Bypass -File tools\ui-smoke-test.ps1 -Shot C:\temp\shot.png
param(
    [string]$ServerExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_server.exe'),
    [string]$ClientExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_client.exe'),
    [int]$Port = 5601,
    [string]$Shot = (Join-Path $env:TEMP 'dchat-ui-smoke.png'),
    [string]$DialogShot = (Join-Path $env:TEMP 'dchat-ui-connect-dialog.png'),
    [string]$UsersFile = (Join-Path $env:TEMP ('dchat-ui-users-{0}.txt' -f $PID)),
    [int]$BulkMessages = 12
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace UI -Name Api -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowW")] public static extern IntPtr FindWindow(string className, string windowName);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowExW")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr child, string className, string windowName);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendCmd(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendGet(IntPtr hWnd, uint msg, IntPtr wParam, System.Text.StringBuilder lParam);
[DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostCmd(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll", EntryPoint = "GetWindowRect")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll", EntryPoint = "PrintWindow")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
[DllImport("user32.dll", EntryPoint = "IsWindowVisible")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll", EntryPoint = "GetWindowLongW")] public static extern int GetWindowLong(IntPtr hWnd, int nIndex);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@

function Wait-Window([scriptblock]$finder, [int]$timeoutMs = 6000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $hwnd = & $finder
        if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }
        Start-Sleep -Milliseconds 120
    }
    return [IntPtr]::Zero
}

# 按顺序找出某个窗口下的所有 Edit 子窗口（本程序里依次是 地址 / 端口 / 昵称）
function Get-ChildEdits([IntPtr]$parent) {
    # 用 EnumChildWindows 收集（实测 FindWindowEx 在这种跨进程场景下不稳定）
    $script:collectedEdits = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $name = New-Object System.Text.StringBuilder 256
        [void][UI.Api]::GetClassName($hwnd, $name, 256)
        if ($name.ToString() -eq 'Edit') { [void]$script:collectedEdits.Add($hwnd) }
        return $true
    }
    [void][UI.Api]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:collectedEdits
}

# 按类名找子窗口（候选浮层是子窗口）
function Find-ChildWindow([IntPtr]$parent, [string]$className) {
    $script:childFound = [IntPtr]::Zero
    $script:childWant = $className
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $name = New-Object System.Text.StringBuilder 256
        [void][UI.Api]::GetClassName($hwnd, $name, 256)
        if ($name.ToString() -eq $script:childWant) { $script:childFound = $hwnd; return $false }
        return $true
    }
    [void][UI.Api]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:childFound
}

# 读取某个控件的文本（用于验证"上键翻历史"这类行为）
function Get-WindowText([IntPtr]$hwnd) {
    $buffer = New-Object System.Text.StringBuilder 512
    [void][UI.Api]::SendGet($hwnd, 0x000D, [IntPtr]512, $buffer)  # WM_GETTEXT
    return $buffer.ToString()
}

# 在所有子控件里找第一个文本匹配某个正则的控件（用来确认状态栏文案）
function Find-ChildText([IntPtr]$parent, [string]$pattern) {
    $script:foundText = $null
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $buffer = New-Object System.Text.StringBuilder 512
        [void][UI.Api]::SendGet($hwnd, 0x000D, [IntPtr]512, $buffer)
        $text = $buffer.ToString()
        if ($text -and ($text -match $script:textPattern)) {
            $script:foundText = $text
            return $false
        }
        return $true
    }
    $script:textPattern = $pattern
    [void][UI.Api]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:foundText
}

# 反复查找直到某个子控件文本匹配（返回匹配到的文本，超时返回 $null）
function Wait-Text([IntPtr]$parent, [string]$pattern, [int]$timeoutMs = 6000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $text = Find-ChildText $parent $pattern
        if ($null -ne $text) { return $text }
        Start-Sleep -Milliseconds 120
    }
    return $null
}

# 把某个窗口画进 PNG（跨进程 PrintWindow）
function Save-WindowShot([IntPtr]$hwnd, [string]$path) {
    $rect = New-Object UI.Api+RECT
    [void][UI.Api]::GetWindowRect($hwnd, [ref]$rect)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][UI.Api]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# 发送一次按键（WM_KEYDOWN）
function Send-Key([IntPtr]$hwnd, [int]$virtualKey) {
    [void][UI.Api]::SendCmd($hwnd, 0x0100, [IntPtr]$virtualKey, [IntPtr]::Zero)
}

# 按类名找出某个进程的顶层窗口（比 FindWindow 可靠：不受同进程隐藏窗口干扰）
function Find-ProcessWindow([int]$processId, [string]$className) {
    $found = [IntPtr]::Zero
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $owner = [uint32]0
        [void][UI.Api]::GetWindowThreadProcessId($hwnd, [ref]$owner)
        if ($owner -eq $script:targetPid) {
            $name = New-Object System.Text.StringBuilder 256
            [void][UI.Api]::GetClassName($hwnd, $name, 256)
            if ($name.ToString() -eq $script:targetClass) {
                $script:foundWindow = $hwnd
                return $false
            }
        }
        return $true
    }
    $script:targetPid = [uint32]$processId
    $script:targetClass = $className
    $script:foundWindow = [IntPtr]::Zero
    [void][UI.Api]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:foundWindow
}

if (-not (Test-Path -LiteralPath $ServerExe) -or -not (Test-Path -LiteralPath $ClientExe)) {
    Write-Host "找不到 dchat_server.exe 或 dchat_client.exe，请先构建"
    exit 1
}

# 用独立的账号文件，避免污染真实账号
if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force }
$server = Start-Process -FilePath $ServerExe -ArgumentList '--port', $Port, '--users', $UsersFile `
                       -PassThru -WindowStyle Hidden
$client = $null
$peer = $null
$checks = 0
$failures = 0

function Check([bool]$ok, [string]$what) {
    $script:checks++
    if ($ok) { Write-Host ("  ok   {0}" -f $what) } else { $script:failures++; Write-Host ("  FAIL {0}" -f $what) }
}

try {
    Start-Sleep -Milliseconds 900
    Write-Host ("== 界面冒烟测试（服务器端口 {0}） ==" -f $Port)

    $client = Start-Process -FilePath $ClientExe -PassThru
    Start-Sleep -Milliseconds 1800
    $client.Refresh()
    # 不要用 MainWindowHandle：这台机器上它有时会指到别的顶层窗口，按类名找才稳
    $main = Wait-Window { Find-ProcessWindow $client.Id 'DchatClientWnd' } 6000
    Check ($main -ne [IntPtr]::Zero) '客户端窗口已创建'

    # 点「连接」（IDC_CONNECT = 1001），弹出连接对话框。
    # 注意要用 PostMessage：对话框是模态循环，SendMessage 会一直卡在这里不返回。
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1001, [IntPtr]::Zero)
    $clientPid = $client.Id
    $dialog = Wait-Window { Find-ProcessWindow $clientPid 'DchatConnectDlg' } 4000
    Check ($dialog -ne [IntPtr]::Zero) '连接对话框已弹出'

    # ---- 先故意用一个不存在的账号登录一次：客户端应当提示失败并断开，而不是卡住 ----
    $probe = Get-ChildEdits $dialog
    if ($probe.Count -ge 5) {
        [void][UI.Api]::SendText($probe[0], 0x000C, [IntPtr]::Zero, '127.0.0.1')
        [void][UI.Api]::SendText($probe[1], 0x000C, [IntPtr]::Zero, "$Port")
        [void][UI.Api]::SendText($probe[2], 0x000C, [IntPtr]::Zero, 'nobody')
        [void][UI.Api]::SendText($probe[3], 0x000C, [IntPtr]::Zero, 'nobodypass123')
    }
    Start-Sleep -Milliseconds 150
    [void][UI.Api]::PostCmd($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    # 状态栏回到"未连接" = 客户端没有卡在"已连接"状态（服务器拒绝后会主动断开）
    $backToIdle = Wait-Text $main '未连接' 8000
    Check ($null -ne $backToIdle) '用不存在的账号登录后客户端回到"未连接"（没有卡在已连接状态）'

    # ---- 再来一次：注册新账号 alice ----
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1001, [IntPtr]::Zero)
    $dialog = Wait-Window { Find-ProcessWindow $clientPid 'DchatConnectDlg' } 4000
    Check ($dialog -ne [IntPtr]::Zero) '登录失败后能重新打开连接对话框（说明已经断开、可以重连）'

    # 填地址 / 端口 / 用户名 / 密码 / 确认密码，然后点「登录 / 注册」（IDD_OK = 1）
    $edits = Get-ChildEdits $dialog
    Check ($edits.Count -ge 5) ("对话框里有 5 个输入框：地址/端口/用户名/密码/确认密码（实际 {0}）" -f $edits.Count)
    if ($edits.Count -ge 5) {
        [void][UI.Api]::SendText($edits[0], 0x000C, [IntPtr]::Zero, '127.0.0.1')
        [void][UI.Api]::SendText($edits[1], 0x000C, [IntPtr]::Zero, "$Port")
        [void][UI.Api]::SendText($edits[2], 0x000C, [IntPtr]::Zero, 'alice')
        [void][UI.Api]::SendText($edits[3], 0x000C, [IntPtr]::Zero, 'alicepass123')
        [void][UI.Api]::SendText($edits[4], 0x000C, [IntPtr]::Zero, 'alicepass123')
    }
    # 密码框应当带 ES_PASSWORD 样式（输入内容显示成圆点而不是明文）
    if ($edits.Count -ge 5) {
        $masked = (([UI.Api]::GetWindowLong($edits[3], -16) -band 0x20) -ne 0) -and
                  (([UI.Api]::GetWindowLong($edits[4], -16) -band 0x20) -ne 0)
        Check $masked '密码 / 确认密码输入框带密码样式（显示为圆点）'
    }
    if ($edits.Count -ge 5) {
        Save-WindowShot $dialog $DialogShot
        Check (Test-Path -LiteralPath $DialogShot) ("连接对话框截图已保存：{0}" -f $DialogShot)
    }
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
    $status = Wait-Text $main '已连接 .*用户：alice'
    Check ($null -ne $status) '登录/注册成功后状态栏显示"已连接 … 用户：alice"'

    # 模拟另一个用户 bob，发一批消息把记录区填满（这样才能看到滚动条）
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.Connect('127.0.0.1', $Port)
    $stream = $tcp.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    $writer.WriteLine('REGISTER bob bobpass123')
    Start-Sleep -Milliseconds 300
    for ($i = 1; $i -le $BulkMessages; $i++) {
        if ($i -eq 3) { $writer.WriteLine('MSG @alice 这条是在叫你（第 3 条）') }
        else { $writer.WriteLine(("MSG 我是 bob，这是第 {0} 条测试消息" -f $i)) }
    }
    Start-Sleep -Milliseconds 1200

    # 让客户端自己发一条（写输入框 + 点「发送」IDC_SEND = 1003）
    $mainEdits = Get-ChildEdits $main
    $input = if ($mainEdits.Count -gt 0) { $mainEdits[0] } else { [IntPtr]::Zero }
    Check ($input -ne [IntPtr]::Zero) '找到客户端输入框'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '我是 alice，这条应该显示在右边')
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1003, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1000

    # 指令也要能按 ↑ 翻回来；改密码那条只记指令名，密码不能带出来
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/changepassword alicepass999')
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1003, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800

    # 上键翻出刚发出去的内容（类似 Minecraft 的行为）
    $sentText = '我是 alice，这条应该显示在右边'
    Send-Key $input 0x26  # VK_UP
    Start-Sleep -Milliseconds 300
    $recalled = Get-WindowText $input
    Check ($recalled -eq '/changepassword ') '按上键能翻出刚发过的指令（密码部分不会带出来）'
    Check ($recalled -notmatch 'alicepass999') '翻出来的指令里没有密码'
    Send-Key $input 0x26
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq $sentText) '再按上键翻出更早的聊天消息'
    Send-Key $input 0x26
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq $sentText) '已经在最旧一条时再按上键内容不变'
    Send-Key $input 0x28  # VK_DOWN
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/changepassword ') '按下键往回翻到更新的那条指令'
    Send-Key $input 0x28
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '') '按下键回到原来的空草稿'

    # Tab 指令补全（类似 Minecraft）
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/he')
    Start-Sleep -Milliseconds 150
    Send-Key $input 0x09  # VK_TAB
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/help') 'Tab 补全指令（/he -> /help）'
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/help') '只有一个候选时再按 Tab 不变'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/')
    Start-Sleep -Milliseconds 150
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/ban') '只输入 / 时按 Tab 从第一个指令开始循环'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '')
    Start-Sleep -Milliseconds 150
    # 参数补全：/kick + 空格 后 Tab 应该补上在线成员（这时房间里只有 alice 自己）
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/kick ')
    Start-Sleep -Milliseconds 150
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/kick alice') 'Tab 补全昵称参数（/kick -> /kick alice）'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '')
    Start-Sleep -Milliseconds 150
    # 候选浮层：输入 '/' 时应该在输入框上方冒出来，清空后收起
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/')
    Start-Sleep -Milliseconds 300
    $suggest = Find-ChildWindow $main 'DchatSuggestWnd'
    Check (($suggest -ne [IntPtr]::Zero) -and [UI.Api]::IsWindowVisible($suggest)) `
          '输入 / 时在输入框上方出现候选浮层'
    $sr = New-Object UI.Api+RECT
    [void][UI.Api]::GetWindowRect($suggest, [ref]$sr)
    $ir = New-Object UI.Api+RECT
    [void][UI.Api]::GetWindowRect($input, [ref]$ir)
    Check ($sr.Bottom -le $ir.Top) '浮层就在输入框正上方（不遮住输入框）'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '')
    Start-Sleep -Milliseconds 300
    Check (-not [UI.Api]::IsWindowVisible($suggest)) '清空输入后候选浮层收起'

    # ↑↓ 在候选里移动（浮层开着时方向键用来选候选，不去翻聊天历史）
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/')
    Start-Sleep -Milliseconds 300
    Send-Key $input 0x28  # VK_DOWN
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/ban') '浮层开着时按 ↓ 选中第一个候选'
    Send-Key $input 0x28
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/kick') '再按 ↓ 选下一个候选'
    Send-Key $input 0x26  # VK_UP
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/ban') '按 ↑ 回到上一个候选'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '')
    Start-Sleep -Milliseconds 250

    # 从 bob 的连接里读回服务器广播，确认客户端真的把消息发出去了
    $buffer = New-Object byte[] 8192
    $text = ''
    $deadline = (Get-Date).AddSeconds(2)
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buffer, 0, $buffer.Length)
            if ($n -gt 0) { $text += [System.Text.Encoding]::UTF8.GetString($buffer, 0, $n) }
        } else { Start-Sleep -Milliseconds 60 }
    }
    Check ($text -match 'SAY .* alice 我是 alice') '客户端界面上发出的消息被服务器广播'
    # bob 是后加入的，收不到 alice 加入时的 JOINED 广播，但会在在线名单里看到 alice
    Check ($text -match 'NAMES .*alice') 'bob 看到 alice 在线（说明客户端昵称已设置成功）'

    # 抓窗口截图
    $rect = New-Object UI.Api+RECT
    [void][UI.Api]::GetWindowRect($main, [ref]$rect)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][UI.Api]::PrintWindow($main, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Check (Test-Path -LiteralPath $Shot) ("截图已保存：{0}" -f $Shot)

    Write-Host ("共 {0} 项检查，失败 {1} 项。" -f $checks, $failures)
} finally {
    if ($null -ne $peer) { try { $peer.Close() } catch { } }
    if ($null -ne $client -and -not $client.HasExited) { Stop-Process -Id $client.Id -Force }
    if ($null -ne $server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    if (Test-Path -LiteralPath $UsersFile) { Remove-Item -LiteralPath $UsersFile -Force -ErrorAction SilentlyContinue }
    Write-Host '已关闭测试用的客户端与服务器进程'
}

if ($failures -eq 0) { exit 0 } else { exit 1 }
