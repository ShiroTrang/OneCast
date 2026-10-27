# Uninstalls OneCast for the current user.
#
# This script deliberately lives OUTSIDE the program folder: the installer copies
# it to %APPDATA%\OneCast. The first version of LiveVocal's uninstaller put the
# whole uninstall in a uninstall.cmd *inside* the program folder and had it
# delete that folder while cmd.exe was still reading the script from it, so cmd
# died halfway through and left the shortcuts and settings behind.
#
# PowerShell reads and compiles the entire script before executing it, so it is
# safe for this script to delete the folder it lives in, provided that happens
# last.

$ErrorActionPreference = 'SilentlyContinue'

# The "Go OneCast" shortcut starts uninstall.cmd with the program folder as its
# working directory, and that cmd shim inherits it when it launches us. A
# PowerShell whose current directory is inside $target cannot delete $target
# (the folder is in use), so move out of the way before touching anything.
Set-Location $env:TEMP
[Environment]::CurrentDirectory = $env:TEMP

$target   = Join-Path $env:LOCALAPPDATA 'Programs\OneCast'
$appData  = Join-Path $env:APPDATA 'OneCast'
$startDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$desktop  = [Environment]::GetFolderPath('Desktop')

# The "Chay cung Windows" option writes this value (HKCU, so no admin needed).
# Left behind it would point at a program that no longer exists.
$runKey   = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

Write-Host ''
Write-Host '  ============================================' -ForegroundColor DarkCyan
Write-Host '   OneCast - dang go cai dat' -ForegroundColor Cyan
Write-Host '  ============================================' -ForegroundColor DarkCyan
Write-Host ''

# Give the cmd.exe that launched us time to exit, otherwise the program folder
# is still considered in use.
Start-Sleep -Milliseconds 900

Write-Host '  Dang dong OneCast...' -ForegroundColor Gray
Get-Process -Name 'OneCast' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Write-Host '  Dang xoa chuong trinh...' -ForegroundColor Gray
if (Test-Path $target) { Remove-Item -LiteralPath $target -Recurse -Force }

Write-Host '  Dang xoa shortcut...' -ForegroundColor Gray
foreach ($lnk in @((Join-Path $startDir 'OneCast.lnk'),
                   (Join-Path $startDir 'Go OneCast.lnk'),
                   (Join-Path $desktop  'OneCast.lnk')))
{
    if (Test-Path $lnk) { Remove-Item -LiteralPath $lnk -Force }
}

Write-Host '  Dang tat che do chay cung Windows...' -ForegroundColor Gray
Remove-ItemProperty -Path $runKey -Name 'OneCast' -ErrorAction SilentlyContinue

Write-Host '  Dang xoa du lieu nguoi dung (cai dat + preset)...' -ForegroundColor Gray
# Last: this folder contains the script that is currently running.
if (Test-Path $appData) { Remove-Item -LiteralPath $appData -Recurse -Force }

Write-Host ''
if (Test-Path $target) {
    Write-Host '  Mot phan chua xoa duoc (co the app van dang chay).' -ForegroundColor Yellow
    Write-Host '  Hay dong OneCast roi chay lai trinh go cai.' -ForegroundColor Yellow
} else {
    Write-Host '  Da go xong OneCast.' -ForegroundColor Green
}
Write-Host ''
Read-Host '  Bam Enter de dong'
