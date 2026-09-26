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
[string]$RulesShot = (Join-Path $env:TEMP 'dchat-ui-rules-suggest.png'),
[string]$AuthShot = (Join-Path $env:TEMP 'dchat-ui-register.png'),
[string]$LoginShot = (Join-Path $env:TEMP 'dchat-ui-login.png'),
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

    # 点「连接」（IDC_CONNECT = 1001），弹出第一步的连接对话框。
    # 注意要用 PostMessage：对话框是模态循环，SendMessage 会一直卡在这里不返回。
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1001, [IntPtr]::Zero)
    $clientPid = $client.Id
    $dialog = Wait-Window { Find-ProcessWindow $clientPid 'DchatConnectDlg' } 4000
    Check ($dialog -ne [IntPtr]::Zero) '连接对话框已弹出'
    $connectEdits = Get-ChildEdits $dialog
    Check ($connectEdits.Count -eq 2) `
          ("连接对话框只有 2 个输入框：地址 + 端口（实际 {0}）" -f $connectEdits.Count)
    Save-WindowShot $dialog $DialogShot
    Check (Test-Path -LiteralPath $DialogShot) ("连接对话框截图已保存：{0}" -f $DialogShot)

    # ---- 第一步：填地址连上去（这一步没有账号输入框）----
    if ($connectEdits.Count -ge 2) {
        [void][UI.Api]::SendText($connectEdits[0], 0x000C, [IntPtr]::Zero, '127.0.0.1')
        [void][UI.Api]::SendText($connectEdits[1], 0x000C, [IntPtr]::Zero, "$Port")
    }
    Start-Sleep -Milliseconds 150
    [void][UI.Api]::PostCmd($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    $login = Wait-Window { Find-ProcessWindow $clientPid 'DchatLoginDlg' } 6000
    Check ($login -ne [IntPtr]::Zero) '连上服务器之后才弹出「登录」界面'
    Check ($null -ne (Wait-Text $main '已连接 .*未登录')) '连上但还没登录时，状态栏显示"未登录"'

    # ---- 先故意用一个不存在的账号登录一次：原因显示在窗口里，连接不断 ----
    $loginEdits = Get-ChildEdits $login
    Check ($loginEdits.Count -eq 3) `
          ("登录界面有 3 个输入框：用户名 / 密码 /（隐藏的）确认密码，实际 {0}" -f $loginEdits.Count)
    if ($loginEdits.Count -ge 2) {
        [void][UI.Api]::SendText($loginEdits[0], 0x000C, [IntPtr]::Zero, 'nobody')
        [void][UI.Api]::SendText($loginEdits[1], 0x000C, [IntPtr]::Zero, 'nobodypass123')
    }
    Start-Sleep -Milliseconds 150
    [void][UI.Api]::PostCmd($login, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
    Check ([UI.Api]::IsWindowVisible($login)) '登录被拒绝后窗口还开着（可以直接改，不用重连）'
    Check ($null -ne (Find-ChildText $main '已连接 .*未登录')) '登录失败后连接还在（状态栏仍是"未登录"，没有断开）'
    Save-WindowShot $login (Join-Path $env:TEMP 'dchat-ui-login-error.png')

    # ---- 点「断开」：结束这次连接，回到未连接状态 ----
    [void][UI.Api]::PostCmd($login, 0x0111, [IntPtr]2, [IntPtr]::Zero)  # IDD_CANCEL
    $backToIdle = Wait-Text $main '未连接' 8000
    Check ($null -ne $backToIdle) '账号窗口点「断开」后回到"未连接"（没有卡在已连接状态）'

    # ---- 再来一次：这次走到注册界面，注册 alice ----
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1001, [IntPtr]::Zero)
    $dialog = Wait-Window { Find-ProcessWindow $clientPid 'DchatConnectDlg' } 4000
    Check ($dialog -ne [IntPtr]::Zero) '断开后能重新打开连接对话框（可以重连）'
    [void][UI.Api]::PostCmd($dialog, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    $login = Wait-Window { Find-ProcessWindow $clientPid 'DchatLoginDlg' } 6000
    Check ($login -ne [IntPtr]::Zero) '再次连上后又弹出「登录」界面'

    # 「登录」和「注册」是两个不同的窗口：先填登录界面，再点左下角按钮换界面
    $loginEdits2 = Get-ChildEdits $login
    if ($loginEdits2.Count -ge 2) {
        [void][UI.Api]::SendText($loginEdits2[0], 0x000C, [IntPtr]::Zero, 'alice')
        [void][UI.Api]::SendText($loginEdits2[1], 0x000C, [IntPtr]::Zero, 'alicepass123')
        Save-WindowShot $login $LoginShot
        Check (Test-Path -LiteralPath $LoginShot) ("登录界面截图已保存：{0}" -f $LoginShot)
    }
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($login, 0x0111, [IntPtr]106, [IntPtr]::Zero)  # IDD_SWITCH
    $reg = Wait-Window { Find-ProcessWindow $clientPid 'DchatRegisterDlg' } 4000
    Check ($reg -ne [IntPtr]::Zero) '点「没有账号？注册新账号」换到了「注册」界面（另一个窗口）'
    Check ((Find-ProcessWindow $clientPid 'DchatLoginDlg') -eq [IntPtr]::Zero) `
          '切换之后「登录」窗口已经关掉（不会两个窗口叠在一起）'

    $regEdits = Get-ChildEdits $reg
    Check ($regEdits.Count -eq 3) `
          ("注册界面有 3 个输入框：用户名 / 密码 / 确认密码（实际 {0}）" -f $regEdits.Count)
    if ($regEdits.Count -ge 3) {
        Check ((Get-WindowText $regEdits[0]) -eq 'alice') '切到注册界面后，刚才填的用户名跟着带过来了'
        [void][UI.Api]::SendText($regEdits[0], 0x000C, [IntPtr]::Zero, 'alice')
        [void][UI.Api]::SendText($regEdits[1], 0x000C, [IntPtr]::Zero, 'alicepass123')
        [void][UI.Api]::SendText($regEdits[2], 0x000C, [IntPtr]::Zero, 'alicepass123')
        # 从登录界面切过来时，刚才填的用户名会带过来（这里被上面的 alice 覆盖了）
        $masked = (([UI.Api]::GetWindowLong($regEdits[1], -16) -band 0x20) -ne 0) -and
                  (([UI.Api]::GetWindowLong($regEdits[2], -16) -band 0x20) -ne 0)
        Check $masked '密码 / 确认密码输入框带密码样式（显示为圆点）'
        Save-WindowShot $reg $AuthShot
        Check (Test-Path -LiteralPath $AuthShot) ("注册界面截图已保存：{0}" -f $AuthShot)
        # 「显示密码」勾选框：勾上后密码框的遮挡字符（●）被清成 0 = 明文
        $maskBefore = [UI.Api]::SendCmd($regEdits[1], 0x00D2, [IntPtr]::Zero, [IntPtr]::Zero)
        [void][UI.Api]::PostCmd($reg, 0x0111, [IntPtr]105, [IntPtr]::Zero)  # IDD_SHOW
        Start-Sleep -Milliseconds 300
        $maskAfter = [UI.Api]::SendCmd($regEdits[1], 0x00D2, [IntPtr]::Zero, [IntPtr]::Zero)
        Check (($maskBefore.ToInt64() -ne 0) -and ($maskAfter.ToInt64() -eq 0)) `
              '勾上「显示密码」后密码框改成明文（遮挡字符被清掉）'
    }
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($reg, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
    $status = Wait-Text $main '已连接 .*用户：alice'
    Check ($null -ne $status) '注册成功后状态栏显示"已连接 … 用户：alice"'

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
    # /ip 的昵称补全（和 /ban /op 一样按在线 + 已注册名单补）
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/ip a')
    Start-Sleep -Milliseconds 150
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/ip alice') 'Tab 补全 /ip 的昵称（/ip a -> /ip alice）'
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '')
    Start-Sleep -Milliseconds 150
    # /chatrule 的规则名补全
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/chatrule ch')
    Start-Sleep -Milliseconds 200
    $suggest2 = Find-ChildWindow $main 'DchatSuggestWnd'
    Check (($suggest2 -ne [IntPtr]::Zero) -and [UI.Api]::IsWindowVisible($suggest2)) `
          '打 /chatrule 规则名时出现候选浮层'
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/chatrule chatinterval') `
          'Tab 补全 /chatrule 的规则名（/chatrule ch -> /chatrule chatinterval）'
    # 顺手留一张"规则候选"的截图，方便肉眼确认灰色说明和分组
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/chatrule ')
    Start-Sleep -Milliseconds 300
    Save-WindowShot $main $RulesShot
    Check (Test-Path -LiteralPath $RulesShot) ("规则候选浮层截图已保存：{0}" -f $RulesShot)
    [void][UI.Api]::SendText($input, 0x000C, [IntPtr]::Zero, '/chatrule keepchathistory ')
    Start-Sleep -Milliseconds 250
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/chatrule keepchathistory set') `
          '布尔规则第 2 个参数先补 set'
    Send-Key $input 0x09
    Start-Sleep -Milliseconds 250
    Check ((Get-WindowText $input) -eq '/chatrule keepchathistory true') `
          '继续按 Tab 轮到 true'
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
