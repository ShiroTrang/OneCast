<#
.SYNOPSIS
    Builds a distributable Windows package for OneCast.

.DESCRIPTION
    Produces a self-contained folder + ZIP that runs on a clean Windows 10/11
    x64 machine with nothing installed:

      dist/OneCast-<version>-win64/        the portable package
      dist/OneCast-<version>-win64.zip     the same thing, zipped
      dist/OneCast-<version>-Setup.exe     optional single-file installer (-MakeSetupExe)

    "Self-contained" means the required Visual C++ runtime DLLs are placed next
    to the exe (Microsoft's supported "app-local deployment"), so the recipient
    does not need the VC++ Redistributable. The DLLs are derived from the exe's
    real import table rather than hard-coded, so this keeps working if the
    toolchain changes what it links against.

    The ASIO SDK is compiled into the exe and is not redistributable, so it is
    never copied into the package. RNNoise is statically linked; its licence text
    ships inside THIRD-PARTY-NOTICES.txt.

    Nothing is committed to git: dist/ is ignored, the package is reproducible
    from source plus a Visual Studio toolchain.

    Runs on Windows PowerShell 5.1 (the one every Windows 10/11 machine has) and
    on PowerShell 7. Keep this file ASCII: 5.1 reads a BOM-less script as ANSI.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/package.ps1
    pwsh -File tools/package.ps1 -MakeSetupExe
#>
[CmdletBinding()]
param(
    # Build configuration to package (must already be built).
    [string] $Configuration = 'Release',

    # Override the version string; by default it is read from CMakeLists.txt.
    [string] $Version,

    # Also produce a single-file self-extracting installer (uses the built-in
    # IExpress, so no third-party tooling is required).
    [switch] $MakeSetupExe
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------
function Write-Step  ([string] $m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok    ([string] $m) { Write-Host "    $m" -ForegroundColor Green }
function Write-Warn2 ([string] $m) { Write-Host "    $m" -ForegroundColor Yellow }

function Get-ProjectVersion {
    $cmake = Join-Path $repoRoot 'CMakeLists.txt'
    $m = Select-String -Path $cmake -Pattern 'project\s*\(\s*\w+\s+VERSION\s+([0-9][0-9.]*)' |
         Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return '0.0.0'
}

function Get-VsRoots {
    @("$env:ProgramFiles\Microsoft Visual Studio",
      "${env:ProgramFiles(x86)}\Microsoft Visual Studio") |
        Where-Object { Test-Path $_ }
}

# ---------------------------------------------------------------------------
#  Locate the build output
# ---------------------------------------------------------------------------
if (-not $Version) { $Version = Get-ProjectVersion }

$packageName = "OneCast-$Version-win64"
$exePath = Join-Path $repoRoot "build\OneCast_artefacts\$Configuration\OneCast.exe"

Write-Step "Packaging OneCast $Version ($Configuration)"

if (-not (Test-Path $exePath)) {
    throw "Build output not found: $exePath`nBuild first, e.g.`n  cmake --build build --config $Configuration --target OneCast"
}
Write-Ok ("exe: {0:N1} MB" -f ((Get-Item $exePath).Length / 1MB))

# ---------------------------------------------------------------------------
#  Refuse a build without ASIO
# ---------------------------------------------------------------------------
# The packaged exe is the one users run with an audio interface: a build with
# ASIO disabled would silently fall back to Windows Audio and the M-Track's
# low-latency path would be missing. CMake records the SDK path it was
# configured with, so the cache is the honest source for this check.
$cachePath = Join-Path $repoRoot 'build\CMakeCache.txt'

if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "Build cache not found: $cachePath`nConfigure the build before packaging."
}

$asioEntry = Select-String -Path $cachePath -Pattern '^ASIO_SDK_PATH:[^=]*=(.*)$' |
             Select-Object -First 1
$asioPath = if ($asioEntry) { $asioEntry.Matches[0].Groups[1].Value.Trim() } else { '' }

if ([string]::IsNullOrWhiteSpace($asioPath) -or -not (Test-Path -LiteralPath $asioPath -PathType Container)) {
    throw "Refusing to package an exe built without ASIO support.`n" +
          "ASIO_SDK_PATH in $cachePath is '$asioPath', which is not an existing directory.`n" +
          "Re-configure with -DASIO_SDK_PATH=<Steinberg ASIO SDK> and rebuild."
}
Write-Ok "ASIO SDK: $asioPath"

# ---------------------------------------------------------------------------
#  Work out which Visual C++ runtime DLLs the exe actually needs
# ---------------------------------------------------------------------------
$vsRoots = Get-VsRoots

$dumpbin = Get-ChildItem $vsRoots -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -match 'Hostx64\\x64' } |
           Select-Object -First 1

# Several Visual Studio installs can sit side by side. App-local runtime DLLs
# must be at least as new as the toolset that built the exe, so pick the folder
# whose vcruntime140.dll has the highest file version. (Sorting by path would
# prefer "2022" over "18" and ship an older runtime than the exe was built with.)
$crtDir = Get-ChildItem $vsRoots -Recurse -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\x64\\' -and $_.FullName -notmatch 'debug_nonredist|onecore' } |
          ForEach-Object {
              $probe = Join-Path $_.FullName 'vcruntime140.dll'
              $ver = [version]'0.0'
              if (Test-Path $probe) {
                  $raw = (Get-Item $probe).VersionInfo
                  $ver = [version]('{0}.{1}.{2}.{3}' -f $raw.FileMajorPart, $raw.FileMinorPart,
                                                         $raw.FileBuildPart, $raw.FilePrivatePart)
              }
              [pscustomobject]@{ Dir = $_; Version = $ver }
          } |
          Sort-Object Version, @{ Expression = { $_.Dir.FullName } } -Descending |
          Select-Object -First 1 -ExpandProperty Dir

if (-not $crtDir) {
    throw "Could not find the Visual C++ app-local runtime folder (Microsoft.VC*.CRT under a VS install)."
}
Write-Ok "CRT source: $($crtDir.FullName)"

$runtimeDlls = @()

if ($dumpbin) {
    $deps = & $dumpbin.FullName /dependents $exePath 2>$null

    $runtimeDlls = $deps |
        Select-String -Pattern '^\s+((?:msvcp|vcruntime|concrt)\w*\.dll)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Sort-Object -Unique
}

if (-not $runtimeDlls -or $runtimeDlls.Count -eq 0) {
    Write-Warn2 "Could not read the import table; falling back to the known runtime set."
    $runtimeDlls = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
}

Write-Ok ("runtime DLLs to ship: " + ($runtimeDlls -join ', '))

# ---------------------------------------------------------------------------
#  Stage the portable package
# ---------------------------------------------------------------------------
$distRoot = Join-Path $repoRoot 'dist'
$stage    = Join-Path $distRoot $packageName

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item -LiteralPath $exePath -Destination $stage

$missing = @()
foreach ($dll in $runtimeDlls) {
    $src = Join-Path $crtDir.FullName $dll

    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination $stage
    } else {
        $missing += $dll
    }
}

if ($missing.Count -gt 0) {
    throw "Missing runtime DLL(s) in $($crtDir.FullName): $($missing -join ', ')"
}

# Copy the documentation, re-encoded as UTF-8 with BOM so Notepad renders the
# Vietnamese text correctly on every Windows version.
$contentsDir = Join-Path $PSScriptRoot 'package-contents'
$utf8Bom = New-Object System.Text.UTF8Encoding $true

foreach ($required in @('HUONG-DAN.txt', 'THIRD-PARTY-NOTICES.txt')) {
    if (-not (Test-Path (Join-Path $contentsDir $required))) {
        throw "Missing $required in $contentsDir"
    }
}

foreach ($f in Get-ChildItem $contentsDir -File) {
    $text = Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8
    [System.IO.File]::WriteAllText((Join-Path $stage $f.Name), $text, $utf8Bom)
}

$fileCount = (Get-ChildItem $stage -File).Count
Write-Ok "staged $fileCount files in dist\$packageName"

# ---------------------------------------------------------------------------
#  Zip it
# ---------------------------------------------------------------------------
$zipPath = Join-Path $distRoot "$packageName.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal
Write-Ok ("zip: {0:N1} MB -> dist\{1}.zip" -f ((Get-Item $zipPath).Length / 1MB), $packageName)

# ---------------------------------------------------------------------------
#  Optional: single-file self-extracting installer via the built-in IExpress
# ---------------------------------------------------------------------------
if ($MakeSetupExe) {
    Write-Step 'Building single-file installer (IExpress)'

    $sfxStage = Join-Path $distRoot '_sfx-stage'
    if (Test-Path $sfxStage) { Remove-Item $sfxStage -Recurse -Force }
    New-Item -ItemType Directory -Path $sfxStage -Force | Out-Null

    Copy-Item -Path (Join-Path $stage '*') -Destination $sfxStage -Recurse

    # install.ps1 plus uninstall.ps1: the uninstaller has to live outside the
    # program folder, so the installer copies it out at install time.
    $sfxScripts = Get-ChildItem (Join-Path $PSScriptRoot 'sfx') -Filter *.ps1 -File

    if ($sfxScripts.Count -eq 0) { throw "No installer scripts found in $PSScriptRoot\sfx" }

    foreach ($script in $sfxScripts) {
        $scriptText = Get-Content -LiteralPath $script.FullName -Raw -Encoding UTF8
        [System.IO.File]::WriteAllText((Join-Path $sfxStage $script.Name), $scriptText, $utf8Bom)
    }

    # A .cmd shim, because IExpress can only launch an executable/command.
    $cmd = @"
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
"@
    [System.IO.File]::WriteAllText((Join-Path $sfxStage 'install.cmd'), $cmd,
                                   (New-Object System.Text.ASCIIEncoding))

    $setupExe = Join-Path $distRoot "$packageName-Setup.exe"
    if (Test-Path $setupExe) { Remove-Item $setupExe -Force }

    $files = Get-ChildItem $sfxStage -File | Sort-Object Name

    $strings = @()
    $sourceEntries = @()
    for ($i = 0; $i -lt $files.Count; $i++) {
        $strings += ('FILE{0}="{1}"' -f $i, $files[$i].Name)
        $sourceEntries += ('%FILE{0}%=' -f $i)
    }

    $sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$setupExe
FriendlyName=OneCast $Version Setup
AppLaunched=install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles
[Strings]
$($strings -join "`r`n")
[SourceFiles]
SourceFiles0=$sfxStage\
[SourceFiles0]
$($sourceEntries -join "`r`n")
"@

    $sedPath = Join-Path $distRoot '_onecast.sed'
    [System.IO.File]::WriteAllText($sedPath, $sed, (New-Object System.Text.ASCIIEncoding))

    $iexpress = Join-Path $env:SystemRoot 'System32\iexpress.exe'
    if (-not (Test-Path $iexpress)) {
        Write-Warn2 'iexpress.exe not available; skipping the single-file installer.'
    } else {
        $proc = Start-Process -FilePath $iexpress -ArgumentList @('/N', '/Q', $sedPath) `
                              -Wait -PassThru -NoNewWindow

        if ($proc.ExitCode -eq 0 -and (Test-Path $setupExe)) {
            Write-Ok ("setup exe: {0:N1} MB -> dist\{1}-Setup.exe" -f `
                      ((Get-Item $setupExe).Length / 1MB), $packageName)
        } else {
            Write-Warn2 "IExpress failed (exit $($proc.ExitCode)); the ZIP is still usable."
        }
    }

    Remove-Item $sfxStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $sedPath -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
#  Summary
# ---------------------------------------------------------------------------
Write-Host ''
Write-Step 'Package contents'
Get-ChildItem $stage | Sort-Object Name |
    Select-Object @{n = 'Name'; e = { $_.Name } },
                  @{n = 'KB';   e = { [math]::Round($_.Length / 1KB) } } |
    Format-Table -AutoSize

Write-Host ''
Write-Host 'Done. Ship ONE of:' -ForegroundColor Cyan
Write-Host "  dist\$packageName.zip          (recipient unzips and runs 'OneCast.exe')"
if (Test-Path (Join-Path $distRoot "$packageName-Setup.exe")) {
    Write-Host "  dist\$packageName-Setup.exe    (single file, installs + makes shortcuts)"
}
