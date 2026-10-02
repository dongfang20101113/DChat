# 新版主界面的真机冒烟：启动客户端 -> 点「＋」（截菜单）-> 点齿轮（截设置窗口）。
#
# 两条实测教训：
#   1. 点击坐标按**客户区坐标**发消息（WM_LBUTTONDOWN），不要靠 SetCursorPos + mouse_event：
#      合成鼠标事件在这台机器上不稳（窗口收不到），而 SendMessage 是确定的。
#   2. 窗口位置会被系统摆放得不一样，所以用 ClientToScreen 换算，或者干脆只发"客户区坐标"。
param([string]$OutDir = "D:\codes\dchat\build\ui-live")

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class SmokeWin {
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr u, uint f);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public static IntPtr ByClass(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var sb = new StringBuilder(256);
            GetClassNameW(h, sb, 256);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512); return sb.ToString(); }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

function Shoot([string]$name, [IntPtr]$h) {
    [SmokeWin]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero, 0x0001 -bor 0x0100 -bor 0x0400) | Out-Null
    Start-Sleep -Milliseconds 700
    $r = New-Object SmokeWin+RECT
    [SmokeWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
    if ($w -le 0) { Write-Host "  窗口矩形无效"; return }
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host ("  截图 {0}" -f $name)
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$proc = Start-Process "D:\codes\dchat\build\dchat_client.exe" -PassThru
Start-Sleep -Seconds 3
$proc.Refresh()
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "拿不到主窗口"; exit 1 }

$cr = New-Object SmokeWin+RECT
[SmokeWin]::GetClientRect($hwnd, [ref]$cr) | Out-Null
$cw = $cr.Right; $ch = $cr.Bottom
Write-Host ("  客户区 {0}x{1}（布局常量见 src/ui_layout.h）" -f $cw, $ch)

function ClickClient([IntPtr]$h, [int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor $x)
    [SmokeWin]::SendMessageW($h, 0x0201, [IntPtr]0, $lp) | Out-Null
    [SmokeWin]::SendMessageW($h, 0x0202, [IntPtr]0, $lp) | Out-Null
    Start-Sleep -Milliseconds 450
}

Shoot "ui-new-main.png" $hwnd

# 「＋」：右边界 - 12(边距) - 78(发送) - 8(间距) - 20(半径)
$plusX = $cw - 12 - 78 - 8 - 20
$plusY = $ch - 24 - 28
Write-Host ("  点「＋」{0},{1}" -f $plusX, $plusY)
ClickClient $hwnd $plusX $plusY
Shoot "ui-new-plus-menu.png" $hwnd

# 菜单面板：宽 180、右边界贴「＋」右边界；两项各 34 高
$itemX = $plusX
$itemY = $plusY - 8 - 34 * 2 - 12 + 6 + 17
Write-Host ("  点面板里的第一项 {0},{1}" -f $itemX, $itemY)
ClickClient $hwnd $itemX $itemY

# 齿轮：菜单栏最右侧（齿轮宽 32、右边距 8）
Write-Host "  点齿轮"
ClickClient $hwnd ($cw - 24) 17
Start-Sleep -Milliseconds 1200
$settings = [SmokeWin]::ByClass("DchatSettingsDlg")
if ($settings -ne [IntPtr]::Zero) {
    Shoot "ui-new-settings.png" $settings
    Write-Host "  设置窗口：已打开 ✓"
    [SmokeWin]::SendMessageW($settings, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null  # WM_CLOSE
    Start-Sleep -Milliseconds 600
} else {
    Write-Host "  设置窗口：没打开 ✗"
}

Shoot "ui-new-after.png" $hwnd
$proc.CloseMainWindow() | Out-Null
Start-Sleep -Milliseconds 900
if (-not $proc.HasExited) { $proc.Kill() }
Write-Host "完成：$OutDir"