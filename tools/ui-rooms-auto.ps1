# 多会话（群聊 / 私聊）**全自动**联调：起服务端 + 两个客户端，用真实窗口消息驱动界面，
# 再从客户端自己导出的状态文件断言结果。全程不需要人点。
#
# ## 为什么这么设计
#
# 1. **不截图断言**：这台机器上屏幕抓图在 DWM 下会拿到旧内容（实测），截图证明不了
#    "界面上到底有没有这条消息"。改成客户端带 `--state-file` 启动、周期性把内部状态
#    写成文本，脚本读文件断言 —— 这是唯一可靠的验收方式。
# 2. **不在脚本里重算布局**：客户端会把**它这一帧真正用来命中的矩形**一起导出
#    （菜单栏、左栏每一行、搜索/新建按钮、弹窗里每一行的可点区域），脚本照着点。
#    布局两边各算一次、迟早错位 —— 这个坑项目里已经吃过。
# 3. **打字用 WM_SETTEXT**（不是 WM_KEYDOWN）：客户端提交时是 GetWindowTextW 从控件
#    里读文本，所以直接设文本必然生效。
# 4. **点击一律 PostMessage，绝不用 SendMessage**：点到会弹模态对话框的按钮时，
#    SendMessage 会阻塞到那个对话框关掉为止（实测卡死 10 分钟）。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-rooms-auto.ps1
#   ... -Port 5603 -KeepProcesses     # 留进程方便手动看现场
#
# 每次运行都会清空 build\rooms-auto-server（全新账号/房间/历史），失败退出码为 1。
param(
    [int]$Port = 5602,
    [string]$DataDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\rooms-auto-server'),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ui-rooms-auto'),
    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'

$ServerExe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_server.exe'
$ClientExe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_client.exe'
foreach ($exe in @($ServerExe, $ClientExe)) {
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "找不到 $exe，先构建：cmake --build build --target dchat_server dchat_client" -ForegroundColor Red
        exit 1
    }
}

Add-Type -Namespace Api -Name W -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string l);
[DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendCmd(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostCmd(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
'@

# ---------------------------------------------------------------- 基础工具

$script:checks = 0
$script:failures = 0

function Check([bool]$ok, [string]$what) {
    $script:checks++
    if ($ok) {
        Write-Host ("  ok   {0}" -f $what) -ForegroundColor DarkGreen
    } else {
        $script:failures++
        Write-Host ("  FAIL {0}" -f $what) -ForegroundColor Red
    }
}

function Section([string]$title) { Write-Host ""; Write-Host ("== {0} ==" -f $title) -ForegroundColor Cyan }

function Find-ProcessWindow([int]$processId, [string]$className) {
    $script:fwPid = [uint32]$processId
    $script:fwCls = $className
    $script:fwFound = [IntPtr]::Zero
    $cb = [Api.W+EnumProc]{
        param($hwnd, $lparam)
        $owner = [uint32]0
        [void][Api.W]::GetWindowThreadProcessId($hwnd, [ref]$owner)
        if ($owner -eq $script:fwPid) {
            $name = New-Object System.Text.StringBuilder 256
            [void][Api.W]::GetClassName($hwnd, $name, 256)
            if ($name.ToString() -eq $script:fwCls) { $script:fwFound = $hwnd; return $false }
        }
        return $true
    }
    [void][Api.W]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:fwFound
}

function Wait-Window([scriptblock]$finder, [int]$timeoutMs = 6000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $hwnd = & $finder
        if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }
        Start-Sleep -Milliseconds 120
    }
    return [IntPtr]::Zero
}

function Get-ChildEdits([IntPtr]$parent) {
    $script:ceList = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [Api.W+EnumProc]{
        param($hwnd, $lparam)
        $name = New-Object System.Text.StringBuilder 256
        [void][Api.W]::GetClassName($hwnd, $name, 256)
        if ($name.ToString() -eq 'Edit') { [void]$script:ceList.Add($hwnd) }
        return $true
    }
    [void][Api.W]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:ceList
}

function Set-Edit([IntPtr]$edit, [string]$text) {
    if ($edit -eq [IntPtr]::Zero) { return }
    [void][Api.W]::SendText($edit, 0x000C, [IntPtr]::Zero, $text)   # WM_SETTEXT
}

# 按客户区坐标点一下。**必须用 PostMessage**：点到会弹模态对话框的按钮时，
# SendMessage 会一直阻塞到那个对话框关掉（实测卡死 10 分钟没动静）。
function Click-Rect([IntPtr]$hwnd, [string]$rect, [int]$button = 0) {
    if ([string]::IsNullOrWhiteSpace($rect)) { return $false }
    $p = $rect -split ','
    if ($p.Count -lt 4) { return $false }
    $x = [int](([int]$p[0] + [int]$p[2]) / 2)
    $y = [int](([int]$p[1] + [int]$p[3]) / 2)
    if ($x -lt 0 -or $y -lt 0) { return $false }
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    # **先发一次 WM_MOUSEMOVE**：会话设置窗的点击处理用的是 state->hover
    # （见 RoomSettingsProc 的 WM_LBUTTONDOWN 分支），而 hover 只在 WM_MOUSEMOVE
    # 里更新。不先移动，hover 一直是 -1，点了等于没点 —— 真实鼠标本来也必然先移动。
    [void][Api.W]::PostCmd($hwnd, 0x0200, [IntPtr]0, $lp)   # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 100
    if ($button -eq 0) {
        [void][Api.W]::PostCmd($hwnd, 0x0201, [IntPtr]1, $lp)   # WM_LBUTTONDOWN
        [void][Api.W]::PostCmd($hwnd, 0x0202, [IntPtr]0, $lp)   # WM_LBUTTONUP
    } else {
        [void][Api.W]::PostCmd($hwnd, 0x0204, [IntPtr]2, $lp)   # WM_RBUTTONDOWN
        [void][Api.W]::PostCmd($hwnd, 0x0205, [IntPtr]0, $lp)   # WM_RBUTTONUP
    }
    Start-Sleep -Milliseconds 420
    return $true
}

# ---------------------------------------------------------------- 读客户端导出的状态

# 读一份状态快照。写到一半的文件会被 END=1 挡掉，读不到就重试。
function Read-State([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    for ($attempt = 0; $attempt -lt 12; $attempt++) {
        $text = $null
        try { $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8) } catch { }
        if ($text -and $text -match 'END=1') {
            $map = @{}
            foreach ($line in ($text -split "`n")) {
                $i = $line.IndexOf('=')
                if ($i -gt 0) { $map[$line.Substring(0, $i)] = $line.Substring($i + 1) }
            }
            return $map
        }
        Start-Sleep -Milliseconds 80
    }
    return $null
}

# 反复读直到条件成立。超时返回 $null，最后一次快照留在 $script:lastState 里好排查。
function Wait-State([string]$path, [scriptblock]$pred, [int]$timeoutMs = 8000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    $script:lastState = $null
    while ((Get-Date) -lt $deadline) {
        $s = Read-State $path
        if ($null -ne $s) {
            $script:lastState = $s
            if (& $pred $s) { return $s }
        }
        Start-Sleep -Milliseconds 150
    }
    return $null
}

function Sess-Index($s, [string]$id) {
    $n = [int]$s['session.count']
    for ($i = 0; $i -lt $n; $i++) { if ($s["session.$i.id"] -eq $id) { return $i } }
    return -1
}

function Sess-RowRect($s, [string]$id) {
    $n = [int]$s['sessionList.rows']
    for ($i = 0; $i -lt $n; $i++) {
        if ($s["sessionList.row.$i.session"] -eq $id) { return $s["sessionList.row.$i.rect"] }
    }
    return $null
}

function Dm-Id($s) {
    for ($i = 0; $i -lt [int]$s['session.count']; $i++) {
        if ($s["session.$i.kind"] -eq 'dm') { return $s["session.$i.id"] }
    }
    return ''
}

function Tail-Text($s, [int]$idx) {
    if ($idx -lt 0) { return '' }
    $out = @()
    $n = [int]$s["session.$idx.tail.count"]
    for ($j = 0; $j -lt $n; $j++) { $out += [string]$s["session.$idx.tail.$j.text"] }
    return ($out -join "`n")
}

function Tail-Has($s, [int]$idx, [string]$needle) {
    if ($idx -lt 0) { return $false }
    return (Tail-Text $s $idx) -like "*$needle*"
}

# ---------------------------------------------------------------- 界面动作

function Switch-To([IntPtr]$main, [string]$statePath, [string]$id) {
    $s = Read-State $statePath
    if ($null -eq $s) { return $false }
    $rect = Sess-RowRect $s $id
    if ($null -eq $rect) { return $false }
    [void](Click-Rect $main $rect 0)
    return ($null -ne (Wait-State $statePath { param($x) $x['current.id'] -eq $id } 4000))
}

# 在当前会话里发一条消息：设输入框文本 -> 点「发送」(IDC_SEND = 1003)
function Send-Message([IntPtr]$main, [string]$statePath, [string]$text) {
    $s = Read-State $statePath
    if ($null -eq $s) { return $false }
    Set-Edit ([IntPtr][long]$s['main.input']) $text
    Start-Sleep -Milliseconds 150
    [void][Api.W]::PostCmd($main, 0x0111, [IntPtr]1003, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 550
    return $true
}

# 菜单栏上某一项的矩形（改版后菜单栏是自绘的，没有控件 ID）
function Menu-Rect($s, [string]$action) {
    for ($i = 0; $i -lt [int]$s['menu.count']; $i++) {
        if ($s["menu.$i.action"] -eq $action) { return $s["menu.$i.rect"] }
    }
    return $null
}

# 从菜单栏「连接」一路走到注册成功
# -Existing：用**已有账号登录**（不切到注册界面）。验证"重启后本地设置不丢"要用它。
function Connect-And-Register([IntPtr]$main, [int]$clientPid, [string]$statePath, [string]$user,
                              [string]$pass, [switch]$Existing) {
    # 菜单栏是**自绘**的：「连接」不再是有控件 ID 的按钮。tools\ui-smoke-test.ps1 里那句
    # WM_COMMAND 1001 是界面改版**之前**的写法，现在点了没反应（实测弹不出对话框）。
    $s = Wait-State $statePath { param($x) [int]$x['menu.count'] -ge 1 } 6000
    if ($null -eq $s) { return $false }
    $connectRect = Menu-Rect $s 'connect'
    if ($null -eq $connectRect) { return $false }
    [void](Click-Rect $main $connectRect 0)

    $dlg = Wait-Window { Find-ProcessWindow $clientPid 'DchatConnectDlg' } 6000
    if ($dlg -eq [IntPtr]::Zero) { return $false }
    $edits = Get-ChildEdits $dlg
    if ($edits.Count -lt 2) { return $false }
    Set-Edit $edits[0] '127.0.0.1'
    Set-Edit $edits[1] "$Port"
    Start-Sleep -Milliseconds 150
    [void][Api.W]::PostCmd($dlg, 0x0111, [IntPtr]1, [IntPtr]::Zero)     # 连接
    $login = Wait-Window { Find-ProcessWindow $clientPid 'DchatLoginDlg' } 8000
    if ($login -eq [IntPtr]::Zero) { return $false }
    $le = Get-ChildEdits $login
    if ($le.Count -ge 2) { Set-Edit $le[0] $user; Set-Edit $le[1] $pass }
    Start-Sleep -Milliseconds 150
    if ($Existing) {
        [void][Api.W]::PostCmd($login, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # 登录
    } else {
        [void][Api.W]::PostCmd($login, 0x0111, [IntPtr]106, [IntPtr]::Zero)  # IDD_SWITCH -> 注册界面
        $reg = Wait-Window { Find-ProcessWindow $clientPid 'DchatRegisterDlg' } 6000
        if ($reg -eq [IntPtr]::Zero) { return $false }
        $re = Get-ChildEdits $reg
        if ($re.Count -ge 3) { Set-Edit $re[0] $user; Set-Edit $re[1] $pass; Set-Edit $re[2] $pass }
        Start-Sleep -Milliseconds 150
        [void][Api.W]::PostCmd($reg, 0x0111, [IntPtr]1, [IntPtr]::Zero)     # 注册
    }
    # 断言登录状态要看**连接状态字段**：状态栏在界面改版后是自绘的，
    # 测试脚本没法再从子控件里读到"已连接 … 用户：xxx"那句话了。
    return ($null -ne (Wait-State $statePath { param($x) $x['conn.nick'] -eq $user } 10000))
}

# 右键某一行的会话 → 会话设置窗打开
function Open-Room-Settings([IntPtr]$main, [string]$statePath, [string]$id, [int]$timeoutMs = 6000) {
    $s = Read-State $statePath
    if ($null -eq $s) { return $null }
    $rect = Sess-RowRect $s $id
    if ($null -eq $rect) { return $null }
    [void](Click-Rect $main $rect 1)     # 右键
    return (Wait-State $statePath { param($x) $x['dialog.roomSettings.open'] -eq '1' } $timeoutMs)
}

function Close-Room-Settings($s) {
    if ($null -eq $s) { return }
    $h = [IntPtr][long]$s['dialog.roomSettings.hwnd']
    if ($h -ne [IntPtr]::Zero) {
        [void][Api.W]::PostCmd($h, 0x0010, [IntPtr]0, [IntPtr]::Zero)   # WM_CLOSE
        Start-Sleep -Milliseconds 500
    }
}

function Room-RowIndex($s, [string]$label) {
    for ($i = 0; $i -lt [int]$s['dialog.roomSettings.rows']; $i++) {
        if ($s["dialog.roomSettings.row.$i.label"] -eq $label) { return $i }
    }
    return -1
}

function Room-RowRect($s, [string]$label, [string]$which = 'box') {
    $i = Room-RowIndex $s $label
    if ($i -lt 0) { return $null }
    return $s["dialog.roomSettings.row.$i.$which"]
}

# 会话设置窗里当前有哪些行（按标签）。断言"该有的有、不该有的没有"用它，
# 而不是在脚本里硬编码一份行清单 —— 清单在客户端里，改一处就够了。
function Room-Labels($rs) {
    $out = @()
    for ($i = 0; $i -lt [int]$rs['dialog.roomSettings.rows']; $i++) {
        $out += [string]$rs["dialog.roomSettings.row.$i.label"]
    }
    return $out
}

# 找出某个人的**成员行**下标。-1 = 没有。
# 申请行和成员行的 label/arg 都是昵称，只能靠 role 区分：成员行的 role 有值
# （owner/admin/member），申请行的 role 是空的。
function Find-MemberRow($rs, [string]$nick) {
    for ($i = 0; $i -lt [int]$rs['dialog.roomSettings.rows']; $i++) {
        if ($rs["dialog.roomSettings.row.$i.arg"] -eq $nick -and
            -not [string]::IsNullOrEmpty($rs["dialog.roomSettings.row.$i.role"])) { return $i }
    }
    return -1
}

# 找出某个人的**入群申请行**下标（role 为空的那个）。-1 = 没有。
function Find-RequestRow($rs, [string]$nick) {
    for ($i = 0; $i -lt [int]$rs['dialog.roomSettings.rows']; $i++) {
        if ($rs["dialog.roomSettings.row.$i.arg"] -eq $nick -and
            [string]::IsNullOrEmpty($rs["dialog.roomSettings.row.$i.role"])) { return $i }
    }
    return -1
}

# 打开会话设置 -> 点某一行的控件 -> 关窗
function Click-Room-Row([IntPtr]$main, [string]$statePath, [string]$id, [string]$label, [string]$which = 'box') {
    $rs = Open-Room-Settings $main $statePath $id
    if ($null -eq $rs) { return $false }
    $rect = Room-RowRect $rs $label $which
    $ok = $false
    if (-not [string]::IsNullOrWhiteSpace($rect)) {
        [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) $rect 0)
        Start-Sleep -Milliseconds 400
        $ok = $true
    }
    Close-Room-Settings $rs
    return $ok
}

# 会话设置窗里的文本框（群名 / 群公告 / 数字）：点一下会**就地开一个 EDIT**，
# 填完**关窗即提交**（RoomSettingsProc 的 WM_CLOSE 里会 CommitRoomRowEdit），
# 所以不用去找"确定"按钮。
function Set-Room-Text([IntPtr]$main, [string]$statePath, [string]$id, [string]$label, [string]$text) {
    $rs = Open-Room-Settings $main $statePath $id
    if ($null -eq $rs) { return $false }
    $rect = Room-RowRect $rs $label 'box'
    if ([string]::IsNullOrWhiteSpace($rect)) { Close-Room-Settings $rs; return $false }
    [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) $rect 0)
    $opened = Wait-State $statePath { param($s) $s['dialog.roomSettings.edit'] -ne '0' } 4000
    if ($null -eq $opened) { Close-Room-Settings $rs; return $false }
    Set-Edit ([IntPtr][long]$opened['dialog.roomSettings.edit']) $text
    Start-Sleep -Milliseconds 250
    Close-Room-Settings $opened
    return $true
}

# 确保群聊窗口开着，返回它的状态快照。
# -Reopen：先关掉再重开。提示词（hint）在窗口刚打开时是空的，只有搜索**真正收到结果**
# 才会被写上 —— 所以重开一次能把"搜了但没结果"和"根本没搜成"区分开，
# 否则这两种情况的现场看起来一模一样。
function Ensure-Group-Window([IntPtr]$main, [string]$statePath, [switch]$Reopen) {
    $s = Read-State $statePath
    if ($null -eq $s) { return $null }
    if ($Reopen -and $s['dialog.group.open'] -eq '1') {
        $h = [IntPtr][long]$s['dialog.group.hwnd']
        if ($h -ne [IntPtr]::Zero) {
            [void][Api.W]::PostCmd($h, 0x0010, [IntPtr]0, [IntPtr]::Zero)   # WM_CLOSE
            # **等到状态文件确实变成"关掉了"再往下走**。状态文件 200ms 才刷一次，
            # 关完立刻读会拿到还写着 open=1 的旧快照 —— 于是后面每一次点击都在往一个
            # 已经销毁的窗口发消息，看起来"点了没反应"，实际是发给了空气。
            # （实测踩过：整个第 5 步一次搜索都没到服务端，日志里一条都没有。）
            [void](Wait-State $statePath { param($x) $x['dialog.group.open'] -eq '0' } 4000)
            Start-Sleep -Milliseconds 250
        }
        $s = Read-State $statePath
    }
    if ($s['dialog.group.open'] -ne '1') {
        [void](Click-Rect $main $s['sessionList.search'] 0)
    }
    return (Wait-State $statePath { param($x) $x['dialog.group.open'] -eq '1' } 5000)
}

# ---------------------------------------------------------------- 准备环境

if (Test-Path -LiteralPath $DataDir) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# 打开"保留聊天记录"，否则"重启后历史还在"这条验不了（服务端启动时读这个文件）
Set-Content -Path (Join-Path $DataDir 'dchat-rules.txt') `
    -Value @('# 自动化联调用的规则', 'keepchathistory true') -Encoding ASCII

$stateA = Join-Path $OutDir 'state-alice.txt'
$stateB = Join-Path $OutDir 'state-bob.txt'
foreach ($f in @($stateA, $stateB)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }

$server = $null
$clientA = $null
$clientB = $null

try {
    Section "启动服务端（端口 $Port，数据目录 $DataDir）"
    # 服务端日志落盘：出问题时"服务端到底怎么判的"是第一现场，
    # 藏在隐藏窗口里的话每次都得重跑一遍才能看。
    $serverLog = Join-Path $OutDir 'server.log'
    if (Test-Path -LiteralPath $serverLog) { Remove-Item -LiteralPath $serverLog -Force }
    $server = Start-Process -FilePath $ServerExe -ArgumentList '--port', $Port `
                            -WorkingDirectory $DataDir -PassThru -WindowStyle Hidden `
                            -RedirectStandardOutput $serverLog
    Start-Sleep -Milliseconds 1200
    Check (-not $server.HasExited) '服务端进程在跑'

    Section "启动两个客户端（各带 --state-file，各自独立的工作目录）"
    # **每个客户端一个工作目录**：`dchat-rooms.prefs`（本地设置）、`dchat-known-servers.txt`
    # （服务器指纹 TOFU）都是相对路径的本地文件。共用一个目录会有两个后果，都实测踩过：
    #   1. 两个客户端**互相覆盖**本地设置；
    #   2. 上一轮跑剩的值会被这一轮读进来，于是"点一下开关"可能是在**关**而不是开，
    #      测试就不再是幂等的（表现为 A 点完免打扰却变成 0）。
    $runA = Join-Path $OutDir 'client-a'
    $runB = Join-Path $OutDir 'client-b'
    foreach ($dir in @($runA, $runB)) {
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $clientA = Start-Process -FilePath $ClientExe -ArgumentList '--state-file', $stateA `
                             -WorkingDirectory $runA -PassThru
    $clientB = Start-Process -FilePath $ClientExe -ArgumentList '--state-file', $stateB `
                             -WorkingDirectory $runB -PassThru
    $mainA = Wait-Window { Find-ProcessWindow $clientA.Id 'DchatClientWnd' } 8000
    $mainB = Wait-Window { Find-ProcessWindow $clientB.Id 'DchatClientWnd' } 8000
    Check ($mainA -ne [IntPtr]::Zero) '客户端 A 主窗口已创建'
    Check ($mainB -ne [IntPtr]::Zero) '客户端 B 主窗口已创建'
    Check ($null -ne (Wait-State $stateA { param($s) $true } 4000)) 'A 的状态文件已开始导出'
    Check ($null -ne (Wait-State $stateB { param($s) $true } 4000)) 'B 的状态文件已开始导出'

    Section "登录：alice（A）/ bob（B）"
    Check (Connect-And-Register $mainA $clientA.Id $stateA 'alice' 'alicepass123') 'A 走到注册成功、已登录 alice'
    Check (Connect-And-Register $mainB $clientB.Id $stateB 'bob' 'bobpass123') 'B 走到注册成功、已登录 bob'
    Check ($null -ne (Wait-State $stateA { param($s) $s['rooms.enabled'] -eq '1' } 10000)) 'A 收到 FEATURES rooms1（多会话模式开启）'
    Check ($null -ne (Wait-State $stateB { param($s) $s['rooms.enabled'] -eq '1' } 10000)) 'B 收到 FEATURES rooms1'

    # ---------------------------------------------------------- 1. 大厅
    Section "1. 大厅：两个人互相看得见"
    [void](Switch-To $mainA $stateA 'hall')
    [void](Send-Message $mainA $stateA '大厅一号')
    Check ($null -ne (Wait-State $stateB { param($s) (Tail-Has $s (Sess-Index $s 'hall') '大厅一号') } 8000)) `
          'B 在大厅收到了 A 发的「大厅一号」'

    # ---------------------------------------------------------- 2. 建群
    Section "2. 建群：A 点表头「新建」→ 填 ID + 群名 → 创建"
    $sA = Read-State $stateA
    Check (-not [string]::IsNullOrWhiteSpace($sA['sessionList.new'])) '状态里导出了表头「新建」按钮的矩形'
    [void](Click-Rect $mainA $sA['sessionList.new'] 0)
    $gA = Wait-State $stateA { param($s) $s['dialog.group.open'] -eq '1' } 5000
    Check ($null -ne $gA) '群聊窗口已弹出'
    $created = $null
    if ($null -ne $gA) {
        Set-Edit ([IntPtr][long]$gA['dialog.group.edit.id']) 'room1'
        Set-Edit ([IntPtr][long]$gA['dialog.group.edit.name']) '测试群'
        Start-Sleep -Milliseconds 250
        [void](Click-Rect ([IntPtr][long]$gA['dialog.group.hwnd']) $gA['dialog.group.create'] 0)
        $created = Wait-State $stateA { param($s) (Sess-Index $s 'room1') -ge 0 } 8000
        Check ($null -ne $created) 'A 的左栏里出现了新会话 room1'
    }
    if ($null -ne $created) {
        $i = Sess-Index $created 'room1'
        Check ($created["session.$i.kind"] -eq 'group') 'room1 的类型是 group'
        Check ($created["session.$i.name"] -eq '测试群') 'room1 的群名是「测试群」'
        Check ($created["session.$i.myRole"] -eq 'owner') 'A 是群主'
        Check ($created["session.$i.listed"] -eq '0') '新群默认**不公开**（勾了「向公众展示」才进搜索）'
    }

    # ---------------------------------------------------------- 3. 搜群（此时不该搜到）
    Section "3. 搜群：还没公开，B 应该搜不到"
    $sB = Read-State $stateB
    Check (-not [string]::IsNullOrWhiteSpace($sB['sessionList.search'])) '状态里导出了表头「搜索」按钮的矩形'
    $gB = Ensure-Group-Window $mainB $stateB
    Check ($null -ne $gB) 'B 的群聊窗口已弹出'
    if ($null -ne $gB) {
        Set-Edit ([IntPtr][long]$gB['dialog.group.edit.keyword']) 'room1'
        Start-Sleep -Milliseconds 250
        [void](Click-Rect ([IntPtr][long]$gB['dialog.group.hwnd']) $gB['dialog.group.search'] 0)
        Start-Sleep -Milliseconds 1500
        $sB2 = Read-State $stateB
        Check ([int]$sB2['groupHits.count'] -eq 0) '未公开时搜不到（默认私密，符合设计）'
    }

    # ---------------------------------------------------------- 4. 勾选「向公众展示」
    Section "4. A 在会话设置里勾「向公众展示」"
    Check (Click-Room-Row $mainA $stateA 'room1' '向公众展示') '点到了「向公众展示」这一行'
    Check ($null -ne (Wait-State $stateA { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return $s["session.$i.listed"] -eq '1' } 8000)) 'room1 变成已公开（客户端本地）'

    # 客户端那句只是**乐观回显**，服务端到底收没收到要另找一份证据：服务端把房间名单
    # 落盘在 dchat-rooms.txt，R 行的第 8 个字段就是 listed。少了这一步的话，
    # "客户端显示已公开、服务端其实没收到"会被当成通过。
    $roomsFile = Join-Path $DataDir 'dchat-rooms.txt'
    $listedOnServer = $false
    for ($try = 0; $try -lt 20 -and -not $listedOnServer; $try++) {
        if (Test-Path -LiteralPath $roomsFile) {
            foreach ($line in (Get-Content -LiteralPath $roomsFile -Encoding UTF8)) {
                if ($line.StartsWith("R`troom1`t")) {
                    $f = $line -split "`t"
                    if ($f.Count -gt 7 -and $f[7] -eq '1') { $listedOnServer = $true }
                }
            }
        }
        if (-not $listedOnServer) { Start-Sleep -Milliseconds 300 }
    }
    Check $listedOnServer '服务端也记下了「已公开」（不只看客户端的乐观回显）'

    # ---------------------------------------------------------- 5. 再搜
    Section "5. 再搜：这次应该搜得到"
    # 重试几轮：A 的 GSET 要过一趟 TCP 才到服务端，紧接着搜有可能赶在它前面
    # （实测就是这样：客户端确实收到了 GHIT_END，但那一轮服务端还没把群标成公开）。
    $hits = $null
    $reopenNote = ''
    for ($attempt = 1; $attempt -le 6 -and $null -eq $hits; $attempt++) {
        # 每轮都**关掉重开**群聊窗，走和第一次完全一样的路（第一次搜索是好的）。
        # 上一轮之所以没敢这么做：关窗会把整个客户端杀掉（群聊窗的 WM_DESTROY 里
        # 有 PostQuitMessage）—— 那个 bug 已经修掉，所以现在可以放心关。
        $gB = Ensure-Group-Window $mainB $stateB -Reopen
        # **关窗之后第一件事就是看客户端还在不在**。客户端一旦退出，状态文件会停更，
        # 之后所有断言读到的都是最后那份旧快照、失败得"看起来很正常"——
        # 这正是这个问题藏了这么久的原因（服务端日志里那句 `left (bob)` 才是现场）。
        if ($clientB.HasExited) {
            Write-Host ("  !! B 的客户端在关窗后已退出，退出码 {0}" -f $clientB.ExitCode) -ForegroundColor Red
            $reopenNote = '客户端已退出'
            break
        }
        if ($null -eq $gB) { $reopenNote = '（第 {0} 轮：群聊窗没开起来）' -f $attempt; break }
        if ($gB['dialog.group.visible'] -eq '0') { $reopenNote = '（群聊窗存在但不可见）' }
        Set-Edit ([IntPtr][long]$gB['dialog.group.edit.keyword']) 'room1'
        Start-Sleep -Milliseconds 250
        [void](Click-Rect ([IntPtr][long]$gB['dialog.group.hwnd']) $gB['dialog.group.search'] 0)
        $hits = Wait-State $stateB { param($s) [int]$s['groupHits.count'] -ge 1 } 3000
        if ($null -eq $hits -and $attempt -lt 6) { Start-Sleep -Milliseconds 800 }
    }
    Check ($null -ne $hits) '公开之后 B 搜到了结果'
    $sB = Read-State $stateB
    if ($reopenNote) { Write-Host ("       诊断：{0}" -f $reopenNote) -ForegroundColor Yellow }
    # 判断"点击到底有没有生效"要看服务端日志里的 gsearch，不要看这里的状态文件：
    # 客户端一旦出事（比如被关窗杀掉），状态文件会停更，读到的是旧快照，会假通过。
    Check (-not [string]::IsNullOrWhiteSpace($sB['dialog.group.hint'])) `
          ("点「搜索」确实发出去了（hint = {0}）" -f $sB['dialog.group.hint'])
    if ($null -ne $hits) {
        Check ($hits['groupHits.0.id'] -eq 'room1') '搜到的第一个就是 room1'
    }

    # ---------------------------------------------------------- 6. 加入
    Section "6. 加入：B 点「加入」（默认管理员审核，会进待审队列）"
    if ($null -ne $hits) {
        [void](Click-Rect ([IntPtr][long]$hits['dialog.group.hwnd']) $hits['dialog.group.join.0'] 0)
        Check ($null -ne (Wait-State $stateB { param($s) $s['dialog.group.hint'] -like '*已申请*' } 6000)) `
              'B 看到「已申请加入」提示'
    } else {
        Check $false '（搜群没成功，加入这一步跳过）'
    }

    # ---------------------------------------------------------- 7. 审核
    Section "7. 审核入群：A 右键 room1 → 会话设置 → 同意"
    $rs = Open-Room-Settings $mainA $stateA 'room1'
    Check ($null -ne $rs) 'A 的会话设置窗已打开'
    if ($null -ne $rs) {
        Check ($rs['dialog.roomSettings.id'] -eq 'room1') '打开的是 room1 的设置'
        $pendIdx = -1
        for ($i = 0; $i -lt [int]$rs['dialog.roomSettings.rows']; $i++) {
            if ($rs["dialog.roomSettings.row.$i.arg"] -eq 'bob') { $pendIdx = $i }
        }
        Check ($pendIdx -ge 0) '待审列表里出现了 bob 的申请'
        if ($pendIdx -ge 0) {
            $approve = $rs["dialog.roomSettings.row.$pendIdx.box"]
            if ([string]::IsNullOrWhiteSpace($approve)) { $approve = $rs["dialog.roomSettings.row.$pendIdx.row"] }
            [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) $approve 0)
            Check ($null -ne (Wait-State $stateA {
                param($s)
                $i = Sess-Index $s 'room1'
                if ($i -lt 0) { return $false }
                return [int]$s["session.$i.members"] -ge 2 } 8000)) '同意后 room1 成员数变成 2'
        }
    }
    Close-Room-Settings $rs
    Check ($null -ne (Wait-State $stateB { param($s) (Sess-Index $s 'room1') -ge 0 } 8000)) 'B 那边也出现了 room1 会话'

    # ---------------------------------------------------------- 8. 群消息不乱窜
    Section "8. 群消息不乱窜（历史上真出过：消息投到了别的会话）"
    [void](Switch-To $mainA $stateA 'room1')
    [void](Send-Message $mainA $stateA '群里一号')
    [void](Switch-To $mainB $stateB 'room1')
    [void](Send-Message $mainB $stateB '群里二号')
    Check ($null -ne (Wait-State $stateA { param($s) (Tail-Has $s (Sess-Index $s 'room1') '群里二号') } 8000)) `
          'A 在群里收到了 B 发的「群里二号」'
    $sA = Read-State $stateA
    $sB = Read-State $stateB
    Check (Tail-Has $sA (Sess-Index $sA 'room1') '群里一号') 'A 的群里能看到自己发的「群里一号」'
    Check (Tail-Has $sB (Sess-Index $sB 'room1') '群里一号') 'B 的群里收到了「群里一号」'
    Check (-not (Tail-Has $sA (Sess-Index $sA 'hall') '群里一号')) 'A 的大厅里**没有**群消息（没乱窜）'
    Check (-not (Tail-Has $sB (Sess-Index $sB 'hall') '群里二号')) 'B 的大厅里**没有**群消息（没乱窜）'
    Check (-not (Tail-Has $sA (Sess-Index $sA 'room1') '大厅一号')) '群记录里**没有**大厅的消息（切会话没串内容）'

    # ---------------------------------------------------------- 9. 本地设置乐观回显
    Section "9. 会话设置：免打扰 / 置顶 点了立刻回显（真机反馈过「点了不变」）"
    Check (Click-Room-Row $mainA $stateA 'room1' '免打扰') '点到了「免打扰」'
    Check ($null -ne (Wait-State $stateA { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return $s["session.$i.muted"] -eq '1' } 4000)) '点完立刻变成已开启（乐观回显，不用等服务端）'
    Check (Click-Room-Row $mainA $stateA 'room1' '置顶') '点到了「置顶」'
    Check ($null -ne (Wait-State $stateA { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return $s["session.$i.pinned"] -eq '1' } 4000)) '置顶立刻生效'
    $sA = Read-State $stateA
    Check ($sA['sessionList.row.0.session'] -eq 'room1') '置顶后排到左栏第一行'

    # ---------------------------------------------------------- 10. 私聊
    Section "10. 私聊：B 用 /dm alice 发起"
    [void](Switch-To $mainB $stateB 'hall')
    [void](Send-Message $mainB $stateB '/dm alice')
    Check ($null -ne (Wait-State $stateB { param($s) (Dm-Id $s) -ne '' } 8000)) 'B 那边出现了私聊会话'
    Check ($null -ne (Wait-State $stateA { param($s) (Dm-Id $s) -ne '' } 8000)) 'A 那边也出现了同一个私聊会话（不用重新登录）'

    $sB = Read-State $stateB
    $dmId = Dm-Id $sB
    if ($dmId -ne '') {
        Check ($sB["session.$(Sess-Index $sB $dmId).title"] -eq 'alice') 'B 的私聊标题是对方昵称 alice'
        [void](Send-Message $mainB $stateB '私聊一句')
        Check ($null -ne (Wait-State $stateA { param($s) (Tail-Has $s (Sess-Index $s $dmId) '私聊一句') } 8000)) `
              'A 在私聊里收到了「私聊一句」'
        $sA = Read-State $stateA
        Check (-not (Tail-Has $sA (Sess-Index $sA 'hall') '私聊一句')) '私聊消息**没有**掉进大厅（历史上真出过）'
        Check (-not (Tail-Has $sA (Sess-Index $sA 'room1') '私聊一句')) '私聊消息也**没有**掉进群聊'
    } else {
        Check $false '拿到了 B 的私聊会话 ID'
    }

    # ---------------------------------------------------------- 11. 大厅
    Section "11. 大厅：默认免打扰、不可退、不可删（决策③）"
    $sA = Read-State $stateA
    $hallIdx = Sess-Index $sA 'hall'
    Check ($sA["session.$hallIdx.muted"] -eq '1') '大厅默认就是免打扰'
    $rs = Open-Room-Settings $mainA $stateA 'hall'
    Check ($null -ne $rs) '大厅的会话设置打开了'
    if ($null -ne $rs) {
        $labels = Room-Labels $rs
        Check ($labels -contains '删除本地聊天记录') '大厅有「删除本地聊天记录」'
        Check (-not ($labels -contains '退出群聊')) '大厅**没有**「退出群聊」'
        Check (-not ($labels -contains '解散群聊')) '大厅**没有**「解散群聊」'
        Check (-not ($labels -contains '拉黑对方')) '大厅**没有**「拉黑对方」'
    }
    Close-Room-Settings $rs

    # ---------------------------------------------------------- 12. 私聊设置
    Section "12. 私聊设置：免打扰 / 置顶 / 黑名单"
    $sB = Read-State $stateB
    $dmId = Dm-Id $sB
    $rs = Open-Room-Settings $mainB $stateB $dmId
    Check ($null -ne $rs) '私聊的会话设置打开了'
    if ($null -ne $rs) {
        $labels = Room-Labels $rs
        Check ($labels -contains '免打扰') '私聊设置里有「免打扰」'
        Check ($labels -contains '置顶') '私聊设置里有「置顶」'
        Check ($labels -contains '拉黑对方') '私聊设置里有「拉黑对方」'
    }
    Close-Room-Settings $rs

    Check (Click-Room-Row $mainB $stateB $dmId '拉黑对方') '点到了「拉黑对方」'
    Check ($null -ne (Wait-State $stateB { param($s) [int]$s['block.count'] -ge 1 } 8000)) `
          '客户端的黑名单里出现了对方'
    # 会话还得在 —— 否则下面两条"发不出去"会因为找不到会话而**假通过**
    Check ((Sess-Index (Read-State $stateB) $dmId) -ge 0) '拉黑后私聊会话仍然在（只是发不出消息）'

    # 拉黑是**双向**生效的：两边都发不出去（在服务器那头拦，不是客户端自己不发）
    [void](Switch-To $mainB $stateB $dmId)
    [void](Send-Message $mainB $stateB '拉黑后B发的这条不该到')
    Start-Sleep -Milliseconds 1500
    $sA = Read-State $stateA
    Check (-not (Tail-Has $sA (Sess-Index $sA $dmId) '拉黑后B发的这条不该到')) 'B 被拉黑后发不出去（A 收不到）'
    [void](Switch-To $mainA $stateA $dmId)
    [void](Send-Message $mainA $stateA 'A发的这条也该被拦')
    Start-Sleep -Milliseconds 1500
    $sB = Read-State $stateB
    Check (-not (Tail-Has $sB (Sess-Index $sB $dmId) 'A发的这条也该被拦')) 'A 也发不出去（拉黑双向生效）'

    # ---------------------------------------------------------- 13. 退群
    # ---------------------------------------------------------- 12b. 群公告
    # 放在这里而不是最后：后面的"退群 / 踢人"会把 bob 弄出群，而这一节需要 bob 在群里。
    Section "12b. 群公告（管理员设置项，走服务端并广播）"
    Check (Set-Room-Text $mainA $stateA 'room1' '群公告' '自动测试写的群公告') '在设置里填了群公告（关窗即提交）'
    # 断言刻意放在 **B 那边**：B 什么都没点，它的值只可能来自服务端广播的 RSET ——
    # 只看 A 的话分辨不出"服务端确认了"还是"客户端自己乐观回显"。
    Check ($null -ne (Wait-State $stateB { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return $s["session.$i.announcement"] -like '*自动测试写的群公告*' } 8000)) `
          'B 那边也收到了新公告（服务端广播，不是本地回显）'

    # ---------------------------------------------------------- 12c. 群昵称
    Section "12c. 群昵称：B 用 /nick 设置"
    [void](Switch-To $mainB $stateB 'room1')
    [void](Send-Message $mainB $stateB '/nick 安卓小助手')
    # 成员表不在状态导出的常规字段里（只有开着会话设置窗才导出），所以这里
    # 反复开窗看 —— 顺带也验证了"改群昵称会广播给其他成员"。
    $seen = $null
    for ($try = 0; $try -lt 8 -and $null -eq $seen; $try++) {
        $rs = Open-Room-Settings $mainA $stateA 'room1'
        if ($null -ne $rs) {
            $row = Find-MemberRow $rs 'bob'
            if ($row -ge 0 -and $rs["dialog.roomSettings.row.$row.value"] -eq '安卓小助手') { $seen = $rs }
        }
        if ($null -eq $seen) { Close-Room-Settings $rs; Start-Sleep -Milliseconds 700 }
    }
    Check ($null -ne $seen) 'A 的成员列表里 bob 的群昵称变成了「安卓小助手」（广播过去了）'
    Close-Room-Settings $seen

    Section "13. 退群：B 退出 room1"
    Check (Click-Room-Row $mainB $stateB 'room1' '退出群聊') '点到了「退出群聊」'
    Check ($null -ne (Wait-State $stateB { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $true }
        return [int]$s["session.$i.members"] -eq 0 } 8000)) 'B 退群后 room1 不再属于他'
    Check ($null -ne (Wait-State $stateA { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return [int]$s["session.$i.members"] -le 1 } 8000)) 'A 那边 room1 的成员数降回 1'

    # ---------------------------------------------------------- 14. 重新加入
    # 退群之后要能把人加回来（可重复入群），顺便给后面"成员管理 / 踢人"准备一个在群里的 B。
    Section "14. B 重新加入 room1"
    $gB = Ensure-Group-Window $mainB $stateB
    $hits = $null
    if ($null -ne $gB) {
        Set-Edit ([IntPtr][long]$gB['dialog.group.edit.keyword']) 'room1'
        Start-Sleep -Milliseconds 250
        [void](Click-Rect ([IntPtr][long]$gB['dialog.group.hwnd']) $gB['dialog.group.search'] 0)
        $hits = Wait-State $stateB { param($s) [int]$s['groupHits.count'] -ge 1 } 8000
    }
    Check ($null -ne $hits) '退群后还能重新搜到 room1（它仍然是公开的）'
    $rejoined = $false
    if ($null -ne $hits) {
        [void](Click-Rect ([IntPtr][long]$hits['dialog.group.hwnd']) $hits['dialog.group.join.0'] 0)
        Start-Sleep -Milliseconds 900
        $rs = Open-Room-Settings $mainA $stateA 'room1'
        $pendIdx = -1
        if ($null -ne $rs) { $pendIdx = Find-RequestRow $rs 'bob' }
        Check ($pendIdx -ge 0) 'A 那边又出现了 bob 的入群申请'
        if ($pendIdx -ge 0) {
            [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) `
                              $rs["dialog.roomSettings.row.$pendIdx.box"] 0)
        }
        Close-Room-Settings $rs
        $rejoined = ($null -ne (Wait-State $stateA { param($s)
            $i = Sess-Index $s 'room1'
            if ($i -lt 0) { return $false }
            return [int]$s["session.$i.members"] -ge 2 } 8000))
        Check $rejoined 'B 又进群了（成员数回到 2）'
    }

    # ---------------------------------------------------------- 15. 成员管理
    Section "15. 成员管理：设为管理员 / 降回成员"
    if ($rejoined) {
        $rs = Open-Room-Settings $mainA $stateA 'room1'
        $row = -1
        if ($null -ne $rs) { $row = Find-MemberRow $rs 'bob' }
        Check ($row -ge 0) 'A 的设置窗里能看到 bob 的成员行'
        if ($row -ge 0) {
            [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) `
                              $rs["dialog.roomSettings.row.$row.box"] 0)
        }
        Close-Room-Settings $rs
        Check ($null -ne (Wait-State $stateB { param($s)
            $i = Sess-Index $s 'room1'
            if ($i -lt 0) { return $false }
            return $s["session.$i.myRole"] -eq 'admin' } 8000)) 'B 那边自己的角色变成了管理员'

        $rs = Open-Room-Settings $mainA $stateA 'room1'
        $row = -1
        if ($null -ne $rs) { $row = Find-MemberRow $rs 'bob' }
        if ($row -ge 0) {
            [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) `
                              $rs["dialog.roomSettings.row.$row.box"] 0)
        }
        Close-Room-Settings $rs
        Check ($null -ne (Wait-State $stateB { param($s)
            $i = Sess-Index $s 'room1'
            if ($i -lt 0) { return $false }
            return $s["session.$i.myRole"] -eq 'member' } 8000)) '再点一次降回普通成员'
    } else {
        Check $false '（B 没回到群里，成员管理这节跳过）'
    }

    # ---------------------------------------------------------- 16. 踢人
    Section "16. 踢人：A 把 B 踢出 room1"
    $rs = Open-Room-Settings $mainA $stateA 'room1'
    $row = -1
    if ($null -ne $rs) { $row = Find-MemberRow $rs 'bob' }
    Check ($row -ge 0) 'A 的设置窗里能看到 bob 的成员行（踢人按钮在它右边）'
    if ($row -ge 0) {
        [void](Click-Rect ([IntPtr][long]$rs['dialog.roomSettings.hwnd']) `
                          $rs["dialog.roomSettings.row.$row.extra"] 0)
    }
    Close-Room-Settings $rs
    Check ($null -ne (Wait-State $stateB { param($s) (Sess-Index $s 'room1') -lt 0 } 8000)) `
          'B 的左栏里 room1 没了（被踢掉）'
    Check ($null -ne (Wait-State $stateA { param($s)
        $i = Sess-Index $s 'room1'
        if ($i -lt 0) { return $false }
        return [int]$s["session.$i.members"] -le 1 } 8000)) 'A 那边 room1 的成员数降回 1'

    # ---------------------------------------------------------- 17. 重启后本地设置不丢
    Section "17. 本地设置重启后不丢（成员本地项永不上传）"
    $before = Read-State $stateA
    $i = Sess-Index $before 'room1'
    Check (($i -ge 0) -and ($before["session.$i.muted"] -eq '1') -and
           ($before["session.$i.pinned"] -eq '1')) '重启前 room1 是「免打扰 + 置顶」'
    # 关掉 A 的客户端再重开。免打扰 / 置顶只存在于本机的 dchat-rooms.prefs 里，
    # **服务端根本不知道它们** —— 重启之后还在，才说明"本地项不上传也丢不了"。
    if (-not $clientA.HasExited) { Stop-Process -Id $clientA.Id -Force }
    Start-Sleep -Milliseconds 1500
    if (Test-Path -LiteralPath $stateA) { Remove-Item -LiteralPath $stateA -Force }
    $clientA = Start-Process -FilePath $ClientExe -ArgumentList '--state-file', $stateA `
                             -WorkingDirectory $runA -PassThru
    $mainA = Wait-Window { Find-ProcessWindow $clientA.Id 'DchatClientWnd' } 8000
    Check ($mainA -ne [IntPtr]::Zero) 'A 的客户端重启后窗口已创建'
    Check (Connect-And-Register $mainA $clientA.Id $stateA 'alice' 'alicepass123' -Existing) `
          'A 用已有账号重新登录成功'
    Check ($null -ne (Wait-State $stateA { param($s)
        $j = Sess-Index $s 'room1'
        if ($j -lt 0) { return $false }
        return ($s["session.$j.muted"] -eq '1') -and ($s["session.$j.pinned"] -eq '1') } 12000)) `
          '重启 + 重新登录之后，room1 的免打扰和置顶都还在'
    # 反面证据：服务端的房名单里不该有这两个字段（本地项确实没被上传）
    $roomsText = if (Test-Path -LiteralPath $roomsFile) {
        Get-Content -LiteralPath $roomsFile -Raw -Encoding UTF8
    } else { '' }
    Check ($roomsText -notmatch 'muted|pinned') '服务端的房名单里没有 muted / pinned（本地项确实没上传）'

    Section "小结"
    Write-Host ("  共 {0} 项检查，失败 {1} 项。" -f $script:checks, $script:failures) `
        -ForegroundColor $(if ($script:failures -eq 0) { 'Green' } else { 'Red' })
    Write-Host ("  状态文件：{0} / {1}" -f $stateA, $stateB)
} finally {
    if (-not $KeepProcesses) {
        foreach ($p in @($clientA, $clientB, $server)) {
            if ($null -ne $p) { try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } catch { } }
        }
        Write-Host '已关闭测试用的进程'
    } else {
        Write-Host '（-KeepProcesses：进程留着，方便手动看现场）'
    }
}

if ($script:failures -eq 0) { exit 0 } else { exit 1 }
