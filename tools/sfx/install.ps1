# Installs OneCast for the CURRENT USER only -- no administrator rights needed.
# Invoked by install.cmd inside the self-extracting setup executable.

$ErrorActionPreference = 'Stop'

$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = Join-Path $env:LOCALAPPDATA 'Programs\OneCast'
$appData = Join-Path $env:APPDATA 'OneCast'
$exe    = Join-Path $target 'OneCast.exe'

# Files that belong to the installer itself and must not be copied into the
# program folder.
$installerFiles = @('install.cmd', 'install.ps1', 'uninstall.cmd', 'uninstall.ps1')

Write-Host ''
Write-Host '  ============================================' -ForegroundColor DarkCyan
Write-Host '   OneCast - dang cai dat' -ForegroundColor Cyan
Write-Host '  ============================================' -ForegroundColor DarkCyan
Write-Host ''

# If a previous copy is running, the files are locked and the copy would fail
# halfway. Say so plainly instead. OneCast hides in the tray when its window is
# closed, so the user may not know it is still running.
$running = Get-Process -Name 'OneCast' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host '  OneCast dang chay (co the dang nam o khay he thong).' -ForegroundColor Yellow
    Write-Host '  Chuot phai bieu tuong OneCast o khay > Thoat, roi chay lai setup.' -ForegroundColor Yellow
    Write-Host ''
    Read-Host '  Bam Enter de thoat'
    exit 1
}

New-Item -ItemType Directory -Path $target -Force | Out-Null

Get-ChildItem -LiteralPath $source -File |
    Where-Object { $installerFiles -notcontains $_.Name } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target -Force }

Write-Host "  Da cai dat vao:" -ForegroundColor Green
Write-Host "    $target" -ForegroundColor Gray
Write-Host ''

# ---------------------------------------------------------------- uninstaller
# The real uninstall logic lives OUTSIDE the program folder, because it deletes
# that folder: a script cannot reliably keep running from a directory it is in
# the middle of deleting (cmd.exe reads .cmd files line by line and dies when the
# file disappears). So:
#   %APPDATA%\OneCast\uninstall.ps1     the logic, safe to self-delete because
#                                       PowerShell compiles it up front
#   <program folder>\uninstall.cmd      a shim that hands off and exits at once,
#                                       so cmd.exe is gone before the folder goes
New-Item -ItemType Directory -Path $appData -Force | Out-Null

$uninstallSource = Join-Path $source 'uninstall.ps1'
if (-not (Test-Path $uninstallSource)) {
    throw "Missing uninstall.ps1 next to install.ps1 (looked in $source)"
}

$uninstallScript = Join-Path $appData 'uninstall.ps1'
Copy-Item -LiteralPath $uninstallSource -Destination $uninstallScript -Force

$uninstallShim = @"
@echo off
start "" powershell -NoProfile -ExecutionPolicy Bypass -File "$uninstallScript"
exit
"@
Set-Content -LiteralPath (Join-Path $target 'uninstall.cmd') -Value $uninstallShim -Encoding Ascii

# ------------------------------------------------------------------ shortcuts
$shell       = New-Object -ComObject WScript.Shell
$startMenu   = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$desktop     = [Environment]::GetFolderPath('Desktop')

$appLink = Join-Path $startMenu 'OneCast.lnk'
$s = $shell.CreateShortcut($appLink)
$s.TargetPath       = $exe
$s.WorkingDirectory = $target
$s.Description      = 'OneCast - voice cleaner for Discord and OBS'
$s.Save()

$desktopLink = Join-Path $desktop 'OneCast.lnk'
$s2 = $shell.CreateShortcut($desktopLink)
$s2.TargetPath       = $exe
$s2.WorkingDirectory = $target
$s2.Description      = 'OneCast - voice cleaner for Discord and OBS'
$s2.Save()

$unLink = Join-Path $startMenu 'Go OneCast.lnk'
$s3 = $shell.CreateShortcut($unLink)
$s3.TargetPath       = Join-Path $target 'uninstall.cmd'
$s3.WorkingDirectory = $target
$s3.Save()

Write-Host '  Da tao shortcut: Desktop + Start Menu' -ForegroundColor Green
Write-Host '  De go: Start Menu > "Go OneCast"' -ForegroundColor Gray
Write-Host ''

# ---------------------------------------------------------------------- run
Write-Host '  Dang mo OneCast...' -ForegroundColor Cyan
Start-Process -FilePath $exe -WorkingDirectory $target

Start-Sleep -Seconds 3
exit 0
