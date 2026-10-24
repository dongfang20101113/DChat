# 断线自动重连的**全自动**验证：真服务端 + 真客户端窗口 + 真消息驱动。
#
# 验的是四件事（缺一条都算失败）：
#   1. 正常登录后 conn.authed=1
#   2. **服务端被杀掉** → reconnect.wanted 变 1（说明识别出"被动掉线"）
#   3. 重连尝试真的在跑 → reconnect.attempt 往上加
#   4. **服务端恢复后，不碰鼠标键盘也自己登回去** → conn.authed 重新变 1
#
# 为什么不看界面文字：状态栏是自绘的，脚本读不到；截图在 DWM 下会拿到旧内容（实测）。
# 客户端会周期把内部状态写成文本（--state-file），断言那里面就够。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-reconnect-auto.ps1
#   ... -Port 5621 -KeepProcesses
param(
    [int]$Port = 5621,
    [string]$Root = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ui-reconnect-auto'),
    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
$ServerExe = Join-Path $Repo 'build\dchat_server.exe'
$ClientExe = Join-Path $Repo 'build\dchat_client.exe'
foreach ($exe in @($ServerExe, $ClientExe)) {
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "找不到 $exe，先构建：cmake --build build --target dchat_server dchat_client" -ForegroundColor Red
        exit 1
    }
}

Add-Type -Namespace Api -Name Rc -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string l);
[DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostCmd(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
public delegate bool EnumProcRc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProcRc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProcRc cb, IntPtr l);
'@

$script:failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok   $what" -ForegroundColor Green }
    else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures++ }
    return [void]$ok
}
function Section([string]$t) { Write-Host "`n== $t ==" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 状态文件
function Read-State([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try { $lines = Get-Content -LiteralPath $path -Encoding UTF8 -ErrorAction Stop }
    catch { return $null }
    $map = @{}
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -lt 1) { continue }
        $map[$line.Substring(0, $eq)] = $line.Substring($eq + 1)
    }
    if ($map.ContainsKey('seq')) { return $map }
    return $null
}
function Wait-State([string]$path, [scriptblock]$pred, [int]$timeoutMs = 8000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $s = Read-State $path
        if ($null -ne $s) { if (& $pred $s) { return $s } }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

# ---------------------------------------------------------------- 窗口
function Find-ProcessWindow([int]$pid_, [string]$cls) {
    $found = [IntPtr]::Zero
    $cb = [Api.Rc+EnumProcRc]{
        param($h, $l)
        $p = 0
        [void][Api.Rc]::GetWindowThreadProcessId($h, [ref]$p)
        if ($p -eq $pid_) {
            $sb = New-Object System.Text.StringBuilder 256
            [void][Api.Rc]::GetClassName($h, $sb, 256)
            if ($sb.ToString() -eq $cls) { $script:foundWin = $h; return $false }
        }
        return $true
    }
    $script:foundWin = [IntPtr]::Zero
    [void][Api.Rc]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:foundWin
}
function Wait-Window([scriptblock]$find, [int]$timeoutMs = 8000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $h = & $find
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 100
    }
    return [IntPtr]::Zero
}
function Get-ChildEdits([IntPtr]$parent) {
    $list = New-Object System.Collections.ArrayList
    $cb = [Api.Rc+EnumProcRc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][Api.Rc]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'Edit') { [void]$list.Add($h) }
        return $true
    }
    [void][Api.Rc]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $list
}
function Set-Edit([IntPtr]$h, [string]$text) {
    [void][Api.Rc]::SendText($h, 0x000C, [IntPtr]::Zero, $text)   # WM_SETTEXT
}

# 点一个矩形中心。**必须先发一次 WM_MOUSEMOVE**，否则客户端的悬停状态没建立，
# 点击会被当成点空（这是本项目实测踩过的坑）。
function Click-Rect([IntPtr]$hwnd, [string]$rect) {
    $p = $rect.Split(',')
    $x = [int]((( [int]$p[0]) + ([int]$p[2])) / 2)
    $y = [int]((( [int]$p[1]) + ([int]$p[3])) / 2)
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][Api.Rc]::PostCmd($hwnd, 0x0200, [IntPtr]0, $lp)          # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 40
    [void][Api.Rc]::PostCmd($hwnd, 0x0201, [IntPtr]1, $lp)          # WM_LBUTTONDOWN
    [void][Api.Rc]::PostCmd($hwnd, 0x0202, [IntPtr]0, $lp)          # WM_LBUTTONUP
}

function Start-Server {
    $exe = $ServerExe
    $dir = Join-Path $Root 'server'
    $p = Start-Process -FilePath $exe -ArgumentList '--port', "$Port", '--bind', '127.0.0.1' `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 400
    return $p
}

# ---------------------------------------------------------------- 准备
if (Test-Path -LiteralPath $Root) { Remove-Item -Recurse -Force -LiteralPath $Root }
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'server') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'client') | Out-Null
$statePath = Join-Path $Root 'client\state.txt'

$server = $null
$client = $null
$exit = 0
try {
    Section '起服务端 + 客户端'
    $server = Start-Server
    Check (-not $server.HasExited) '服务端已启动'

    $client = Start-Process -FilePath $ClientExe `
        -ArgumentList '--state-file', $statePath -WorkingDirectory (Join-Path $Root 'client') -PassThru
    $main = Wait-Window { Find-ProcessWindow $client.Id 'DchatClientWnd' } 8000
    Check ($main -ne [IntPtr]::Zero) '客户端主窗口已创建'
    $s = Wait-State $statePath { param($x) [int]$x['menu.count'] -ge 1 } 8000
    Check ($null -ne $s) '状态文件已开始导出'

    Section '登录（注册一个新账号）'
    $connectRect = $null
    foreach ($k in $s.Keys) {
        if ($k -like 'menu.*.action' -and $s[$k] -eq 'connect') {
            $connectRect = $s[($k -replace '\.action$', '.rect')]
        }
    }
    Check ($null -ne $connectRect) '拿到「连接」菜单的矩形'
    Click-Rect $main $connectRect

    $dlg = Wait-Window { Find-ProcessWindow $client.Id 'DchatConnectDlg' } 6000
    Check ($dlg -ne [IntPtr]::Zero) '连接对话框弹出'
    $e = Get-ChildEdits $dlg
    Set-Edit $e[0] '127.0.0.1'
    Set-Edit $e[1] "$Port"
    Start-Sleep -Milliseconds 150
    [void][Api.Rc]::PostCmd($dlg, 0x0111, [IntPtr]1, [IntPtr]::Zero)

    $login = Wait-Window { Find-ProcessWindow $client.Id 'DchatLoginDlg' } 8000
    Check ($login -ne [IntPtr]::Zero) '登录对话框弹出'
    $le = Get-ChildEdits $login
    Set-Edit $le[0] 'rctest'
    Set-Edit $le[1] 'rcpass123'
    Start-Sleep -Milliseconds 150
    [void][Api.Rc]::PostCmd($login, 0x0111, [IntPtr]106, [IntPtr]::Zero)   # 切到注册
    $reg = Wait-Window { Find-ProcessWindow $client.Id 'DchatRegisterDlg' } 6000
    Check ($reg -ne [IntPtr]::Zero) '注册对话框弹出'
    $re = Get-ChildEdits $reg
    Set-Edit $re[0] 'rctest'
    Set-Edit $re[1] 'rcpass123'
    Set-Edit $re[2] 'rcpass123'
    Start-Sleep -Milliseconds 150
    [void][Api.Rc]::PostCmd($reg, 0x0111, [IntPtr]1, [IntPtr]::Zero)

    $s = Wait-State $statePath { param($x) $x['conn.authed'] -eq '1' } 15000
    Check ($null -ne $s) '登录成功（conn.authed=1）'
    if ($null -eq $s) { throw '登录没成功，后面的重连测试没有意义' }

    Section '杀掉服务端 → 应该进入自动重连'
    Stop-Process -Id $server.Id -Force
    $s = Wait-State $statePath { param($x) $x['reconnect.wanted'] -eq '1' } 8000
    Check ($null -ne $s) '识别出被动掉线（reconnect.wanted=1）'
    $s = Wait-State $statePath { param($x) [int]$x['reconnect.attempt'] -ge 1 } 20000
    Check ($null -ne $s) '重连尝试真的在跑（attempt>=1）'
    if ($null -ne $s) { Write-Host ("     attempt=" + $s['reconnect.attempt']) }

    Section '服务端恢复 → 不碰鼠标键盘也要自己登回去'
    $server = Start-Server
    Check (-not $server.HasExited) '服务端已重启'
    $s = Wait-State $statePath { param($x) $x['conn.authed'] -eq '1' -and $x['reconnect.wanted'] -eq '0' } 45000
    Check ($null -ne $s) '自动重连成功，已重新登录（conn.authed=1 且 wanted=0）'
    if ($null -eq $s) {
        $cur = Read-State $statePath
        if ($null -ne $cur) { Write-Host ("     当前 conn.connected=" + $cur['conn.connected'] + " wanted=" + $cur['reconnect.wanted'] + " attempt=" + $cur['reconnect.attempt']) }
    }
}
catch {
    Write-Host ("异常：" + $_.Exception.Message) -ForegroundColor Red
    $script:failures++
}
finally {
    if (-not $KeepProcesses) {
        if ($null -ne $client -and -not $client.HasExited) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($null -ne $server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
        Write-Host "`n已关闭测试用的进程"
    }
}

Write-Host ''
if ($script:failures -eq 0) {
    Write-Host '断线自动重连：全部通过' -ForegroundColor Green
} else {
    Write-Host ("断线自动重连：$script:failures 项失败") -ForegroundColor Red
    $exit = 1
}
exit $exit
