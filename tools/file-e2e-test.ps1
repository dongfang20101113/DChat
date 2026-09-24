# 文件传输端到端测试（QQ 式：上传到服务器 -> 别人收到卡片 -> 点击下载）：
#   A) 协议级：alice 上传一个二进制文件；bob 收到 FILE_OFFER，点（FILE_GET）之后下载下来，
#              逐字节比对 SHA256；上传者本人不会收到 FILE_OFFER；同一个文件可以重复下载
#   B) 真实界面：界面客户端发文件（WM_COPYDATA 上传，对端下载比对）；
#              对端上传文件后界面客户端收到卡片，**模拟鼠标点击卡片**，检查文件落盘且内容一致
#   C) 拒绝路径：未登录、文件过大、非法 ID、参数不全、文件不存在、上传取消、上传不完整
param(
    [string]$ServerExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_server.exe'),
    [string]$ClientExe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\dchat_client.exe'),
    [int]$Port = 5700
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace UI -Name Api -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendText(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);
[DllImport("user32.dll", EntryPoint = "PostMessageW")] public static extern bool PostCmd(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll", EntryPoint = "GetWindowRect")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendCopyData(IntPtr hWnd, uint msg, IntPtr wParam, ref COPYDATASTRUCT lParam);
public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
[StructLayout(LayoutKind.Sequential)] public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@

$script:checks = 0
$script:failures = 0
function Check([bool]$ok, [string]$what) {
    $script:checks++
    if ($ok) { Write-Host ("  ok   {0}" -f $what) }
    else { $script:failures++; Write-Host ("  FAIL {0}" -f $what) }
}

function Get-BytesHash([byte[]]$data) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($data))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function New-Peer {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', $Port)
    return [pscustomobject]@{
        Client = $client
        Sock   = $client.Client
        Bytes  = New-Object 'System.Collections.Generic.List[byte]'
        Lines  = New-Object 'System.Collections.Generic.List[string]'
    }
}

function Send-Line($peer, [string]$text) {
    $payload = [System.Text.Encoding]::UTF8.GetBytes($text + "`n")
    [void]$peer.Sock.Send($payload)
}

function Pump($peer) {
    $chunk = New-Object byte[] 65536
    while ($peer.Sock.Available -gt 0) {
        $n = $peer.Sock.Receive($chunk)
        if ($n -le 0) { break }
        for ($i = 0; $i -lt $n; $i++) { [void]$peer.Bytes.Add($chunk[$i]) }
    }
    while ($true) {
        $index = $peer.Bytes.IndexOf([byte]10)
        if ($index -lt 0) { break }
        $lineBytes = $peer.Bytes.GetRange(0, $index).ToArray()
        $peer.Bytes.RemoveRange(0, $index + 1)
        $line = [System.Text.Encoding]::UTF8.GetString($lineBytes)
        if ($line.EndsWith("`r")) { $line = $line.Substring(0, $line.Length - 1) }
        [void]$peer.Lines.Add($line)
    }
}

# 从第 $From 行开始找匹配；返回 { ok, lines }
function Wait-For($peer, [string]$pattern, [int]$timeoutMs = 4000, [int]$From = 0) {
    $collected = New-Object System.Collections.Generic.List[string]
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        Pump $peer
        for ($i = $peer.Lines.Count - 1; $i -ge $From; $i--) {
            if ($peer.Lines[$i] -match $pattern) {
                for ($j = $From; $j -le $i; $j++) { [void]$collected.Add($peer.Lines[$j]) }
                return [pscustomobject]@{ ok = $true; lines = $collected }
            }
        }
        Start-Sleep -Milliseconds 20
    }
    for ($j = $From; $j -lt $peer.Lines.Count; $j++) { [void]$collected.Add($peer.Lines[$j]) }
    return [pscustomobject]@{ ok = $false; lines = $collected }
}

function Close-Peer($peer) {
    if ($null -ne $peer) { try { $peer.Client.Close() } catch { } }
}

function Find-ProcessWindow([int]$processId, [string]$className) {
    $script:wantPid = [uint32]$processId
    $script:wantClass = $className
    $script:found = [IntPtr]::Zero
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $owner = [uint32]0
        [void][UI.Api]::GetWindowThreadProcessId($hwnd, [ref]$owner)
        if ($owner -eq $script:wantPid) {
            $name = New-Object System.Text.StringBuilder 256
            [void][UI.Api]::GetClassName($hwnd, $name, 256)
            if ($name.ToString() -eq $script:wantClass) { $script:found = $hwnd; return $false }
        }
        return $true
    }
    [void][UI.Api]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Wait-Window([scriptblock]$finder, [int]$timeoutMs = 6000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $h = & $finder
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 120
    }
    return [IntPtr]::Zero
}

function Get-ChildEdits([IntPtr]$parent) {
    $script:edits = New-Object System.Collections.Generic.List[IntPtr]
    $cb = [UI.Api+EnumProc]{
        param($hwnd, $lparam)
        $name = New-Object System.Text.StringBuilder 256
        [void][UI.Api]::GetClassName($hwnd, $name, 256)
        if ($name.ToString() -eq 'Edit') { [void]$script:edits.Add($hwnd) }
        return $true
    }
    [void][UI.Api]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:edits
}

# 记录区是子窗口，得用 EnumChildWindows 找
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

# 把一段行里的 FILE_DATA 拼回原始字节
function Get-AssembledBytes($lines, [int]$From = 0) {
    $stream = New-Object System.IO.MemoryStream
    $count = 0
    for ($i = $From; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^FILE_DATA ') { continue }
        $parts = $lines[$i] -split ' '
        $bytes = [Convert]::FromBase64String($parts[$parts.Count - 1])
        $stream.Write($bytes, 0, $bytes.Length)
        $count++
    }
    return [pscustomobject]@{ Bytes = $stream.ToArray(); Chunks = $count }
}

# 模拟一个客户端把文件上传到服务器（分块发）
function Upload-FileAsPeer($peer, [string]$id, [string]$name, [byte[]]$data) {
    $b64Name = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($name))
    Send-Line $peer ("FILE_SEND " + $id + " " + $b64Name + " " + $data.Length)
    Start-Sleep -Milliseconds 80
    for ($offset = 0; $offset -lt $data.Length; $offset += 2048) {
        $len = [Math]::Min(2048, $data.Length - $offset)
        $slice = New-Object byte[] $len
        [Array]::Copy($data, $offset, $slice, 0, $len)
        Send-Line $peer ("FILE_CHUNK " + $id + " " + [Convert]::ToBase64String($slice))
        Start-Sleep -Milliseconds 5
    }
    Send-Line $peer ("FILE_END " + $id)
}

# 上传时带一张缩略图（真实的客户端会自己生成：图片缩小图 / 视频第一帧）
function Upload-FileWithThumb($peer, [string]$id, [string]$name, [byte[]]$data, [byte[]]$thumb) {
    $b64Name = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($name))
    Send-Line $peer ("FILE_SEND " + $id + " " + $b64Name + " " + $data.Length + " 1")
    Start-Sleep -Milliseconds 60
    for ($offset = 0; $offset -lt $thumb.Length; $offset += 2048) {
        $len = [Math]::Min(2048, $thumb.Length - $offset)
        $slice = New-Object byte[] $len
        [Array]::Copy($thumb, $offset, $slice, 0, $len)
        Send-Line $peer ("FILE_THUMB " + $id + " " + [Convert]::ToBase64String($slice))
        Start-Sleep -Milliseconds 4
    }
    for ($offset = 0; $offset -lt $data.Length; $offset += 2048) {
        $len = [Math]::Min(2048, $data.Length - $offset)
        $slice = New-Object byte[] $len
        [Array]::Copy($data, $offset, $slice, 0, $len)
        Send-Line $peer ("FILE_CHUNK " + $id + " " + [Convert]::ToBase64String($slice))
        Start-Sleep -Milliseconds 4
    }
    Send-Line $peer ("FILE_END " + $id)
}

# 只要缩略图（视频预览走这条路，不下载整份文件）
function Download-ThumbAsPeer($peer, [string]$fileId, [int]$timeoutMs = 6000) {
    $mark = $peer.Lines.Count
    Send-Line $peer ("FILE_THUMB_GET " + $fileId)
    $done = Wait-For $peer ("FILE_THUMB_END " + [regex]::Escape($fileId)) $timeoutMs $mark
    $stream = New-Object System.IO.MemoryStream
    foreach ($line in ($peer.Lines.ToArray() | Select-Object -Skip $mark)) {
        if ($line -notmatch '^FILE_THUMB_DATA ') { continue }
        $parts = $line -split ' '
        $bytes = [Convert]::FromBase64String($parts[$parts.Count - 1])
        $stream.Write($bytes, 0, $bytes.Length)
    }
    return [pscustomobject]@{ ok = $done.ok; bytes = $stream.ToArray() }
}

# 造一张小 PNG 当作缩略图
function New-PngThumb([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(210, 60, 140))
    $g.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $stream.ToArray()
}

# 点"下载"：请求服务器把某个文件发给自己，并把结果拼起来
function Download-FileAsPeer($peer, [string]$fileId, [int]$timeoutMs = 8000) {
    $mark = $peer.Lines.Count
    Send-Line $peer ("FILE_GET " + $fileId)
    $done = Wait-For $peer ("FILE_(END|FAIL) " + [regex]::Escape($fileId)) $timeoutMs $mark
    $lines = $peer.Lines.ToArray()
    $begin = ($lines | Select-Object -Skip $mark | Where-Object { $_ -match ('^FILE_BEGIN ' + [regex]::Escape($fileId) + ' ') } | Select-Object -First 1)
    $fail = ($lines | Select-Object -Skip $mark | Where-Object { $_ -match ('^FILE_FAIL ' + [regex]::Escape($fileId) + ' ') } | Select-Object -First 1)
    $assembled = Get-AssembledBytes $lines $mark
    return [pscustomobject]@{
        ok      = ($done.ok -and $null -ne $begin -and $null -eq $fail)
        begin   = $begin
        fail    = $fail
        chunks  = $assembled.Chunks
        bytes   = $assembled.Bytes
    }
}

if (-not (Test-Path -LiteralPath $ServerExe) -or -not (Test-Path -LiteralPath $ClientExe)) {
    Write-Host "找不到 dchat_server.exe 或 dchat_client.exe，请先构建"
    exit 1
}

$testDir = Join-Path $env:TEMP 'dchat-file-e2e'
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
$payloadBytes = New-Object byte[] (2048 * 2 + 1000)   # 正好 3 块
for ($i = 0; $i -lt $payloadBytes.Length; $i++) { $payloadBytes[$i] = [byte]($i % 256) }
$payloadPath = Join-Path $testDir 'payload.bin'
[System.IO.File]::WriteAllBytes($payloadPath, $payloadBytes)
$payloadFileHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash
$payloadB64Name = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes('payload.bin'))

$usersFile = Join-Path $env:TEMP ('dchat-file-users-{0}.txt' -f $PID)
if (Test-Path $usersFile) { Remove-Item $usersFile -Force }
$receivedDir = Join-Path (Split-Path -Parent $ClientExe) 'received'
$server = Start-Process -FilePath $ServerExe -ArgumentList '--port', $Port, '--users', $usersFile -PassThru -WindowStyle Hidden
$alice = $null; $bob = $null; $guest = $null; $client = $null
try {
    Start-Sleep -Milliseconds 900
    Write-Host ("== 文件传输端到端测试（端口 {0}） ==" -f $Port)
    Write-Host ("   界面客户端的接收目录：{0}" -f $receivedDir)

    # ---------------------------------------------------------- A) 协议级
    $alice = New-Peer
    Send-Line $alice 'REGISTER alice alicepass123'
    Check ((Wait-For $alice 'NAMES').ok) 'alice 注册并加入'
    $bob = New-Peer
    Send-Line $bob 'REGISTER bob bobpass123'
    Check ((Wait-For $bob 'NAMES').ok) 'bob 注册并加入'

    # alice 上传；bob 应该收到一个"可以下载"的卡片通知
    $bobMark = $bob.Lines.Count
    $aliceMark = $alice.Lines.Count
    Upload-FileAsPeer $alice 'up1' 'payload.bin' $payloadBytes
    Check ((Wait-For $alice 'SYS .*文件已上传' 6000 $aliceMark).ok) '上传者收到"文件已上传"的确认'
    $offer = Wait-For $bob 'FILE_OFFER ' 6000 $bobMark
    Check $offer.ok 'bob 收到 FILE_OFFER（有文件可以下载了）'
    $offerLine = ($bob.Lines.ToArray() | Where-Object { $_ -match '^FILE_OFFER ' } | Select-Object -First 1)
    $parts = $offerLine -split ' '
    $fileId = $parts[3]                       # FILE_OFFER <时间> <昵称> <文件ID> <文件名> <大小>
    Check ($parts[2] -eq 'alice' -and $parts[4] -eq $payloadB64Name -and
           $parts[5] -eq "$($payloadBytes.Length)") '卡片信息正确（谁发的 / 文件名 / 大小）'
    Check ($parts[3] -match '^F\d+$') ("服务器分配了文件 ID（{0}）" -f $fileId)
    $uploaderSawOffer = $alice.Lines.ToArray() | Where-Object { $_ -match '^FILE_OFFER ' }
    Check ($null -eq $uploaderSawOffer) '上传者自己不会收到 FILE_OFFER'

    # bob 点击下载
    $download = Download-FileAsPeer $bob $fileId
    Check $download.ok '点下载后拿到 FILE_BEGIN + 数据 + FILE_END'
    Check ($download.chunks -eq 3) ("3 块数据都到了（实际 {0}）" -f $download.chunks)
    Check ((Get-BytesHash $download.bytes) -eq (Get-BytesHash $payloadBytes)) '下载下来的字节和原文件一致（SHA256）'
    Check ((Get-BytesHash $download.bytes) -eq $payloadFileHash) '和磁盘上原始文件的 SHA256 一致'

    # 同一个文件可以再下一次（服务器按需重复发送）
    $again = Download-FileAsPeer $bob $fileId
    Check ($again.ok -and (Get-BytesHash $again.bytes) -eq $payloadFileHash) '同一个文件可以重复下载'

    # 空文件 / 非法 ID / 超大 / 参数不全 / 不存在的文件
    $guest = New-Peer
    Send-Line $guest ("FILE_SEND g1 " + $payloadB64Name + " 10")
    Check ((Wait-For $guest 'ERROR .*请先登录').ok) '未登录不能上传'
    Send-Line $guest ("FILE_GET " + $fileId)
    Check ((Wait-For $guest 'ERROR .*请先登录').ok) '未登录不能下载'
    Send-Line $alice ("FILE_GET bad!id")
    Check ((Wait-For $alice 'ERROR .*用法').ok) '非法文件 ID 被拒绝'
    Send-Line $alice ("FILE_SEND big1 " + $payloadB64Name + " 999999999999")
    Check ((Wait-For $alice 'ERROR .*太大').ok) '超过 64 MB 的文件被拒绝'
    Send-Line $alice ("FILE_SEND noSize " + $payloadB64Name)
    Check ((Wait-For $alice 'ERROR .*用法').ok) '参数不全时提示用法'
    $miss = Download-FileAsPeer $alice 'F99999'
    Check (($null -ne $miss.fail) -and $miss.fail -match '不存在|过期') '下载不存在的文件会收到 FILE_FAIL'

    # 上传取消：不应该冒出 FILE_OFFER
    $mark2 = $bob.Lines.Count
    Send-Line $alice ("FILE_SEND up2 " + $payloadB64Name + " 100")
    Start-Sleep -Milliseconds 150
    Send-Line $alice 'FILE_CANCEL up2'
    Start-Sleep -Milliseconds 500
    $ghost = $bob.Lines.ToArray() | Select-Object -Skip $mark2 | Where-Object { $_ -match '^FILE_OFFER ' }
    Check ($null -eq $ghost) '取消上传后不会产生可下载的文件'

    # 上传不完整：声明 100 字节只发 10 字节
    Send-Line $alice ("FILE_SEND up3 " + $payloadB64Name + " 100")
    Start-Sleep -Milliseconds 120
    Send-Line $alice ("FILE_CHUNK up3 " + [Convert]::ToBase64String((New-Object byte[] 10)))
    Start-Sleep -Milliseconds 120
    Send-Line $alice 'FILE_END up3'
    Check ((Wait-For $alice 'ERROR .*不完整' 4000).ok) '上传不完整会被拒绝'

    # ---- 缩略图：图片缩小图 / 视频第一帧 ----
    $thumb = New-PngThumb 48 36
    $imageBytes = New-Object byte[] 6000
    for ($i = 0; $i -lt $imageBytes.Length; $i++) { $imageBytes[$i] = [byte](($i * 17) % 256) }
    $markImg = $bob.Lines.Count
    Upload-FileWithThumb $alice 'img1' 'preview-test.png' $imageBytes $thumb
    $offerImg = Wait-For $bob 'FILE_OFFER ' 8000 $markImg
    $lineImg = ($bob.Lines.ToArray() | Select-Object -Skip $markImg |
                Where-Object { $_ -match '^FILE_OFFER ' } | Select-Object -First 1)
    Check ($offerImg.ok -and $lineImg -match ' 1$') '带缩略图上传时 FILE_OFFER 标了"有预览"'
    $imgId = ($lineImg -split ' ')[3]
    $gotThumb = Download-ThumbAsPeer $bob $imgId
    Check ($gotThumb.ok -and (Get-BytesHash $gotThumb.bytes) -eq (Get-BytesHash $thumb)) `
          '按 FILE_THUMB_GET 能把缩略图原样取回来（PNG 字节一致）'

    # 没有缩略图的文件：请求缩略图应该收到 FILE_FAIL
    $markNo = $bob.Lines.Count
    Upload-FileAsPeer $alice 'noThumb' 'plain.bin' $payloadBytes
    [void](Wait-For $bob 'FILE_OFFER ' 8000 $markNo)
    $lineNo = ($bob.Lines.ToArray() | Select-Object -Skip $markNo |
               Where-Object { $_ -match '^FILE_OFFER ' } | Select-Object -First 1)
    Check ($lineNo -match ' 0$') '没有缩略图的文件标成"无预览"'
    $noId = ($lineNo -split ' ')[3]
    $missThumb = Download-ThumbAsPeer $bob $noId 4000
    Check (-not $missThumb.ok) '没有缩略图时请求会失败（不回数据）'

    # ---------------------------------------------------------- B) 真实界面
    $client = Start-Process -FilePath $ClientExe -PassThru
    Start-Sleep -Milliseconds 1800
    $main = Wait-Window { Find-ProcessWindow $client.Id 'DchatClientWnd' }
    Check ($main -ne [IntPtr]::Zero) '界面客户端已启动'
    [void][UI.Api]::PostCmd($main, 0x0111, [IntPtr]1001, [IntPtr]::Zero)
    $dlg = Wait-Window { Find-ProcessWindow $client.Id 'DchatConnectDlg' }
    $edits = Get-ChildEdits $dlg
    [void][UI.Api]::SendText($edits[0], 0x000C, [IntPtr]::Zero, '127.0.0.1')
    [void][UI.Api]::SendText($edits[1], 0x000C, [IntPtr]::Zero, "$Port")
    [void][UI.Api]::SendText($edits[2], 0x000C, [IntPtr]::Zero, 'carol')
    [void][UI.Api]::SendText($edits[3], 0x000C, [IntPtr]::Zero, 'carolpass123')
    [void][UI.Api]::SendText($edits[4], 0x000C, [IntPtr]::Zero, 'carolpass123')
    Start-Sleep -Milliseconds 200
    [void][UI.Api]::PostCmd($dlg, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1500
    Check ((Wait-For $bob 'JOINED .*carol' 4000).ok) '界面客户端 carol 连上来了'

    # B1) 界面客户端上传文件（WM_COPYDATA 交给它），bob 收到 FILE_OFFER 后下载并比对
    $payload2 = New-Object byte[] 4096
    for ($i = 0; $i -lt $payload2.Length; $i++) { $payload2[$i] = [byte](255 - ($i % 256)) }
    $path2 = Join-Path $testDir 'from-gui.bin'
    [System.IO.File]::WriteAllBytes($path2, $payload2)
    $wantHash2 = (Get-BytesHash $payload2)
    $mark3 = $bob.Lines.Count

    $utf8 = [System.Text.Encoding]::UTF8.GetBytes($path2 + "`0")
    $buffer = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($utf8.Length)
    [System.Runtime.InteropServices.Marshal]::Copy($utf8, 0, $buffer, $utf8.Length)
    $cds = New-Object UI.Api+COPYDATASTRUCT
    $cds.dwData = [IntPtr]0x43484131
    $cds.cbData = $utf8.Length
    $cds.lpData = $buffer
    [void][UI.Api]::SendCopyData($main, 0x004A, [IntPtr]::Zero, [ref]$cds)
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)

    $offer2 = Wait-For $bob 'FILE_OFFER ' 10000 $mark3
    Check $offer2.ok 'bob 收到界面客户端上传的文件卡片'
    $offerLine2 = ($bob.Lines.ToArray() | Select-Object -Skip $mark3 |
                   Where-Object { $_ -match '^FILE_OFFER ' } | Select-Object -First 1)
    $guiFileId = ($offerLine2 -split ' ')[3]
    $download2 = Download-FileAsPeer $bob $guiFileId
    Check ($download2.ok -and (Get-BytesHash $download2.bytes) -eq $wantHash2) `
          '界面客户端上传的文件，对端下载下来逐字节一致（SHA256）'

    # B2) 对端上传，界面客户端收到卡片后**用鼠标点一下**才下载
    if (Test-Path $receivedDir) {
        Get-ChildItem $receivedDir -Filter 'from-bob*.bin' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $payload3 = New-Object byte[] 5000
    for ($i = 0; $i -lt $payload3.Length; $i++) { $payload3[$i] = [byte](($i * 7) % 256) }
    $wantHash3 = (Get-BytesHash $payload3)
    Upload-FileAsPeer $bob 'tobob1' 'from-bob.bin' $payload3
    Start-Sleep -Milliseconds 800

    $savedPath = Join-Path $receivedDir 'from-bob.bin'
    Check (-not (Test-Path -LiteralPath $savedPath)) '收到卡片时**不会**自动下载（QQ 式：点了才下）'

    # 点卡片：在记录区里扫几个点（点中卡片就会开始下载，点空处没有副作用）
    $view = Wait-Window { Find-ChildWindow $main 'DchatBubbleView' } 4000
    Check ($view -ne [IntPtr]::Zero) '找到记录区窗口'
    $vr = New-Object UI.Api+RECT
    [void][UI.Api]::GetWindowRect($view, [ref]$vr)
    $clicks = 0
    for ($y = 60; $y -lt ($vr.Bottom - $vr.Top) - 20 -and $clicks -lt 40; $y += 18) {
        $lp = [IntPtr](($y -shl 16) -bor 40)   # x=40, y 从 60 开始往下扫
        [void][UI.Api]::PostCmd($view, 0x0201, [IntPtr]1, $lp)  # WM_LBUTTONDOWN
        $clicks++
        Start-Sleep -Milliseconds 60
        if (Test-Path -LiteralPath $savedPath) { break }
    }
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $savedPath)) {
        Start-Sleep -Milliseconds 200
    }
    Check (Test-Path -LiteralPath $savedPath) ("点一下卡片之后文件才落盘（点了 {0} 个位置）" -f $clicks)
    if (Test-Path -LiteralPath $savedPath) {
        $savedHash = (Get-FileHash -LiteralPath $savedPath -Algorithm SHA256).Hash
        Check ($savedHash -eq $wantHash3) '点下载存下来的文件和上传方逐字节一致（SHA256）'
    }

    # B3) 图片：不用点，界面客户端应该**自动下载**（下载完还会自己出缩略图）
    if (Test-Path $receivedDir) {
        Get-ChildItem $receivedDir -Filter 'auto-*.png' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $autoBytes = New-Object byte[] 7000
    for ($i = 0; $i -lt $autoBytes.Length; $i++) { $autoBytes[$i] = [byte](($i * 23) % 256) }
    Upload-FileWithThumb $bob 'autoimg' 'auto-pic.png' $autoBytes (New-PngThumb 64 48)
    $autoPath = Join-Path $receivedDir 'auto-pic.png'
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $autoPath)) {
        Start-Sleep -Milliseconds 200
    }
    Check (Test-Path -LiteralPath $autoPath) '图片不用点，界面客户端自动下载'
    if (Test-Path -LiteralPath $autoPath) {
        Check ((Get-FileHash -LiteralPath $autoPath -Algorithm SHA256).Hash -eq
               (Get-BytesHash $autoBytes)) '自动下载的图片内容一致（SHA256）'
    }

    # B4) 视频：**不**自动下载整份，只用服务器上的第一帧做预览
    if (Test-Path $receivedDir) {
        Get-ChildItem $receivedDir -Filter 'auto-*.mp4' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $videoBytes = New-Object byte[] 9000
    Upload-FileWithThumb $bob 'autovideo' 'auto-clip.mp4' $videoBytes (New-PngThumb 64 48)
    Start-Sleep -Seconds 3
    Check (-not (Test-Path -LiteralPath (Join-Path $receivedDir 'auto-clip.mp4'))) `
          '视频不会自动下载整份（只用第一帧做预览）'

    Write-Host ("共 {0} 项检查，失败 {1} 项。" -f $script:checks, $script:failures)
} finally {
    Close-Peer $alice
    Close-Peer $bob
    Close-Peer $guest
    if ($null -ne $server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    if (Test-Path $usersFile) { Remove-Item $usersFile -Force -ErrorAction SilentlyContinue }
    if ($null -ne $client -and -not $client.HasExited) { Stop-Process -Id $client.Id -Force }
    if (Test-Path $receivedDir) {
        Get-ChildItem $receivedDir -Filter 'from-bob*.bin' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $testDir) { Remove-Item $testDir -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host '已清理测试用的进程与临时文件'
}

if ($script:failures -eq 0) { exit 0 } else { exit 1 }
