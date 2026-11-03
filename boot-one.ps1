<#
.SYNOPSIS
  一键在 Redmi K40 / POCO F3 (alioth) 上临时启动 HackiOS（iOS 16.5 用户态）。
  全程只写 RAM：fastboot boot + fastboot stage，不写任何分区；重启即回到原 Android/HyperOS。

.DESCRIPTION
  和同目录的 boot-one.sh 等价，纯 Windows PowerShell 实现（截图用 .NET 转 PNG，不需要 Python）。
  本包 boot/ 里已含 UEFI、loader、内核载荷、iOS 系统卷和 GPU 固件（在 root.apfs 内），
  启动不下载任何固件；只有从零重建 system.apfs 时才需要 Apple IPSW（那时自己运行
  fetch-firmware.ps1），见 README §5.4。
  若 boot/system.apfs 缺失，脚本会自动降级为“只启动内核”，并提示重建方法。

.PARAMETER Secs
  oem run 之后等多少秒再截图，默认 45（主屏约在内核启动后 20 秒出现）。

.PARAMETER Shots
  连拍张数，默认 3。

.PARAMETER Gap
  两次截图间隔秒数，默认 5。

.PARAMETER Serial
  设备序列号；只有一台 adb 设备时可省略。

.PARAMETER NoSystem
  不 stage 2.5 GB 的 system.apfs，只用载荷自带的 root.apfs 当根（内核/驱动调试用）。

.PARAMETER Reboot
  跑完回到 Android。默认不回安卓：脚本拷完日志/截图后，手机继续留在 iOS 里运行。

.EXAMPLE
  .\boot-one.ps1

.EXAMPLE
  .\boot-one.ps1 -Secs 60 -Shots 3 -Gap 5

.EXAMPLE
  .\boot-one.ps1 -NoSystem
#>
#requires -Version 5.1
[CmdletBinding()]
param(
    [int]$Secs = 45,
    [int]$Shots = 3,
    [int]$Gap = 5,
    [string]$Serial = "",
    [string]$Image = "alioth-boot.img",
    [string]$Payload = "run-payload.bin",
    [string]$SystemImage = "system.apfs",
    [string]$OutDir = "",
    [switch]$NoSystem,
    [switch]$Reboot
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BootDir = Join-Path $Root "boot"
if (-not $OutDir) { $OutDir = Join-Path $Root "runs" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Stamp = Get-Date -Format "HHmmss"
$Tag = "run$Stamp"
$LogPath = Join-Path $OutDir "$Tag.log"

function Say([string]$Text) {
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Resolve-Input([string]$Name) {
    if ([IO.Path]::IsPathRooted($Name)) { return $Name }
    return (Join-Path $BootDir $Name)
}

function Find-Tool([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\$Name.exe"),
        "C:\Android\Sdk\platform-tools\$Name.exe",
        "D:\Android\Sdk\platform-tools\$Name.exe",
        (Join-Path $env:ProgramFiles "platform-tools\$Name.exe")
    )
    foreach ($p in $candidates) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }
    throw "找不到 $Name.exe，请安装 Android platform-tools 并加入 PATH。"
}

# 运行外部命令并限时；超时就杀掉，避免 fastboot 在没有设备时永远打印 "< waiting for any device >"。
function Invoke-Tool {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$TimeoutSec = 30
    )
    $argLine = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $argLine
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) { throw "无法启动：$Exe" }
    # 异步读走两个流，避免 WaitForExit(ms) 时管道写满导致死锁。
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        throw "命令超时 ($TimeoutSec s): $(Split-Path -Leaf $Exe) $($Arguments -join ' ')"
    }
    $text = "$($outTask.Result)$($errTask.Result)"
    if ($proc.ExitCode -ne 0) {
        throw "命令失败 (exit $($proc.ExitCode)): $(Split-Path -Leaf $Exe) $($Arguments -join ' '):`n$($text.Trim())"
    }
    return $text
}

# 把 loader 的 BGRX 帧缓冲（stride 可能大于 width*4）转成 24 位 PNG。
function Convert-RawToPng {
    param([string]$RawPath, [string]$PngPath, [int]$Width, [int]$Height, [int]$Stride)
    Add-Type -AssemblyName System.Drawing
    $data = [IO.File]::ReadAllBytes($RawPath)
    $need = [int64]$Stride * $Height
    if ($data.LongLength -lt $need) { throw "帧缓冲太小：$($data.LongLength) < $need" }
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    try {
        if ($Stride -eq ($Width * 4) -and $bd.Stride -eq ($Width * 4)) {
            [Runtime.InteropServices.Marshal]::Copy($data, 0, $bd.Scan0, ($Width * 4 * $Height))
        } else {
            $row = New-Object byte[] ($Width * 4)
            for ($y = 0; $y -lt $Height; $y++) {
                [Array]::Copy($data, ($y * $Stride), $row, 0, ($Width * 4))
                [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]::Add($bd.Scan0, ($y * $bd.Stride)), $row.Length)
            }
        }
    } finally {
        $bmp.UnlockBits($bd)
    }
    $bmp.Save($PngPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$Adb = Find-Tool "adb"
$Fastboot = Find-Tool "fastboot"

$ImagePath = Resolve-Input $Image
$PayloadPath = Resolve-Input $Payload
$SystemPath = Resolve-Input $SystemImage

Say "这个包需要什么（以及不需要什么）"
if (-not (Test-Path -LiteralPath $ImagePath)) { throw "缺少 loader 镜像：$ImagePath" }
if (-not (Test-Path -LiteralPath $PayloadPath)) { throw "缺少载荷：$PayloadPath" }
$UseSystem = -not $NoSystem
if ($UseSystem -and -not (Test-Path -LiteralPath $SystemPath)) {
    Write-Host "system 镜像不存在：$SystemPath" -ForegroundColor Yellow
    Write-Host "改为只启动内核（载荷自带 root.apfs 当根）。" -ForegroundColor Yellow
    Write-Host "要完整 iOS 桌面，请恢复 boot/system.apfs，或用" -ForegroundColor Yellow
    Write-Host "  src\hackios-xnu-userland-main\ios165\fetch.sh + userland\build.sh 从 IPSW 重建。" -ForegroundColor Yellow
    $UseSystem = $false
}
if ($UseSystem) {
    Write-Host "镜像：$([IO.Path]::GetFileName($ImagePath)), $([IO.Path]::GetFileName($PayloadPath)), $([IO.Path]::GetFileName($SystemPath))"
} else {
    Write-Host "镜像：$([IO.Path]::GetFileName($ImagePath)), $([IO.Path]::GetFileName($PayloadPath)) —— 仅内核（不 stage 系统卷）"
}

Say "phone state"
$devicesOut = Invoke-Tool -Exe $Adb -Arguments @("devices") -TimeoutSec 30
Write-Host $devicesOut.Trim()
if (-not $Serial) {
    $serials = @($devicesOut -split "`r?`n" | Where-Object { $_ -match '\s+device\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($serials.Count -ne 1) {
        throw "期望恰好一台 adb 设备，实际 $($serials.Count) 台：$($serials -join ', ')；可用 -Serial 指定。"
    }
    $Serial = $serials[0]
}
Write-Host "serial: $Serial"
$devTag = @("-s", $Serial)
Invoke-Tool -Exe $Adb -Arguments ($devTag + @("shell", "true")) -TimeoutSec 20 | Out-Null

Say "to the bootloader"
try { Invoke-Tool -Exe $Adb -Arguments ($devTag + @("reboot", "bootloader")) -TimeoutSec 30 | Out-Null } catch { }
$fbDev = $false
for ($i = 0; $i -lt 30; $i++) {
    try { $text = Invoke-Tool -Exe $Fastboot -Arguments @("devices") -TimeoutSec 10 } catch { $text = "" }
    if ($text -match 'fastboot') { $fbDev = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $fbDev) { throw "$Serial 没有进入 bootloader（还停在 Android）；不继续等待。" }

Say "erase the logdump partition (one clean log per run)"
try { $null = Invoke-Tool -Exe $Fastboot -Arguments @("erase", "logdump") -TimeoutSec 60 } catch { Write-Host "logdump erase failed: $_" }

Say "boot the loader (fastboot boot: RAM only)"
$null = Invoke-Tool -Exe $Fastboot -Arguments @("boot", $ImagePath) -TimeoutSec 60
$loaderUp = $false
for ($i = 0; $i -lt 40; $i++) {
    try { $text = Invoke-Tool -Exe $Fastboot -Arguments @("getvar", "product") -TimeoutSec 30 } catch { $text = "" }
    if ($text -match 'product:\s*xnuloader') { $loaderUp = $true; break }
    Start-Sleep -Seconds 1
}
if (-not $loaderUp) { throw "loader 没有起来（现在还是 bootloader 吗？）" }
Write-Host "the loader is up"

if ($UseSystem) {
    Say "resident buffer for the system image, then the image itself"
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "ramdisk") -TimeoutSec 30
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("stage", $SystemPath) -TimeoutSec 1800
    $sw.Stop()
    Write-Host ("system image staged in {0:N1} s" -f $sw.Elapsed.TotalSeconds)
} else {
    Say "no system image: the payload's ramdisk is the root"
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "ramdisk", "none") -TimeoutSec 30
}

Say "the payload"
$null = Invoke-Tool -Exe $Fastboot -Arguments @("stage", $PayloadPath) -TimeoutSec 300

Say "run, then $Secs s of the kernel"
$null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "run") -TimeoutSec 30
Start-Sleep -Seconds $Secs

Say "screenshot"
function Save-Shot([string]$Name) {
    $geom = ""
    try { $geom = Invoke-Tool -Exe $Fastboot -Arguments @("getvar", "screen") -TimeoutSec 30 } catch { }
    $line = @($geom -split "`r?`n" | Where-Object { $_ -match '^\s*screen:' } | Select-Object -First 1)
    if (-not $line) { Write-Host "no screen geometry"; return }
    $nums = @([regex]::Matches($line[0], '\d+') | ForEach-Object { [int]$_.Value })
    if ($nums.Count -lt 3) { Write-Host "bad screen geometry: $($line[0])"; return }
    $w, $h, $stride = $nums[0], $nums[1], $nums[2]

    try { $shot = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "screenshot") -TimeoutSec 60 } catch { Write-Host "no screenshot: $_"; return }
    if ($shot -notmatch 'OKAY') { Write-Host "no screenshot acknowledgement"; return }

    $raw = Join-Path $OutDir "$Name.raw"
    Remove-Item -LiteralPath $raw -Force -ErrorAction SilentlyContinue
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("get_staged", $raw) -TimeoutSec 120
    if (-not (Test-Path -LiteralPath $raw) -or (Get-Item -LiteralPath $raw).Length -eq 0) {
        Write-Host "empty framebuffer"; return
    }
    $png = Join-Path $OutDir "$Name.png"
    try {
        Convert-RawToPng -RawPath $raw -PngPath $png -Width $w -Height $h -Stride $stride
        Write-Host ("PNG {0}: {1} x {2}, {3} bytes" -f $png, $w, $h, (Get-Item -LiteralPath $png).Length)
    } finally {
        Remove-Item -LiteralPath $raw -Force -ErrorAction SilentlyContinue
    }
}
Save-Shot $Tag
for ($i = 1; $i -lt $Shots; $i++) {
    Start-Sleep -Seconds $Gap
    Save-Shot "$Tag-$i"
}

Say "log"
try { $null = Invoke-Tool -Exe $Fastboot -Arguments @("oem", "log") -TimeoutSec 30 } catch { }
try {
    $null = Invoke-Tool -Exe $Fastboot -Arguments @("get_staged", $LogPath) -TimeoutSec 120
    if ((Test-Path -LiteralPath $LogPath) -and (Get-Item -LiteralPath $LogPath).Length -gt 0) {
        $clean = ([IO.File]::ReadAllText($LogPath)) -replace "`0", ""
        $logTxt = "$LogPath.txt"
        [IO.File]::WriteAllText($logTxt, $clean)
        Remove-Item -LiteralPath $LogPath -Force
        Write-Host ("log: {0} ({1} bytes)" -f $logTxt, (Get-Item -LiteralPath $logTxt).Length)
    }
} catch { Write-Host "no log: $_" }

if ($Reboot) {
    Say "back to Android (-Reboot)"
    try { $null = Invoke-Tool -Exe $Fastboot -Arguments @("reboot") -TimeoutSec 30 } catch { }
    for ($i = 0; $i -lt 90; $i++) {
        try { $text = Invoke-Tool -Exe $Adb -Arguments @("devices") -TimeoutSec 15 } catch { $text = "" }
        if ($text -match "$([regex]::Escape($Serial))\s+device") { break }
        Start-Sleep -Seconds 1
    }
    $null = Invoke-Tool -Exe $Adb -Arguments @("devices") -TimeoutSec 15
} else {
    Write-Host ""
    Write-Host "left in iOS: the phone keeps running (loader_return_after=1800 s)." -ForegroundColor Green
    Write-Host "to go back to Android: 'fastboot reboot' (or rerun with -Reboot)" -ForegroundColor Yellow
}
