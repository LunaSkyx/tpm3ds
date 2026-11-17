# tpm3ds one-click setup (Windows).
# Reads YOUR copy of the game (ISO or GAMEDATA folder), converts and builds it, and puts everything on the SD card.
#   double-click setup.bat           (asks two questions), or
#   powershell -File setup.ps1 -Game D:\TPM.iso -Sd E
# Nothing from the game is ever uploaded or committed; source\gen and the .3dsx stay on your PC / SD card.
param([string]$Game, [string]$Sd, [switch]$NoCopy)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }

# ---- 1. tools
Step "Checking tools"
$dk = $env:DEVKITPRO
if (-not $dk -or -not (Test-Path (Join-Path $dk 'msys2\usr\bin\bash.exe'))) { $dk = 'C:\devkitPro' }   # DEVKITPRO may hold an MSYS-style path (/opt/devkitpro)
$bash = Join-Path $dk 'msys2\usr\bin\bash.exe'
if (-not (Test-Path $bash) -or -not (Test-Path (Join-Path $dk 'devkitARM'))) {
    Fail "devkitPro (with 3DS development) is not installed. Get it from https://devkitpro.org/wiki/Getting_Started, tick '3DS Development', then run this again."
}
$py = $null
foreach ($c in @('py', 'python')) { if (Get-Command $c -ErrorAction SilentlyContinue) { $py = $c; break } }
if (-not $py) { Fail "Python 3 is not installed. Get it from https://www.python.org (tick 'Add python.exe to PATH'), then run this again." }
$pyArgs = @(); if ($py -eq 'py') { $pyArgs = @('-3') }
& $py @pyArgs -m pip install --quiet --disable-pip-version-check capstone
if ($LASTEXITCODE) { Fail "pip could not install 'capstone'." }
Write-Host "devkitPro: $dk   python: $py   capstone: ok"

# ---- 2. the game
Step "Finding your game files"
if (-not $Game) { $Game = Read-Host "Path to the game's ISO or to its GAMEDATA folder (you can drag it into this window)" }
$Game = $Game.Trim().Trim('"')
if (-not (Test-Path $Game)) { Fail "Not found: $Game" }
$mounted = $null
if ((Get-Item $Game).PSIsContainer) { $root = $Game }
else {
    Write-Host "Mounting the ISO..."
    $mounted = $Game
    $img = Mount-DiskImage -ImagePath (Resolve-Path $Game).Path -PassThru
    $root = ($img | Get-Volume).DriveLetter + ':\'
}
try {
    $gd = $null
    foreach ($cand in @((Join-Path $root 'GAMEDATA'), $root)) {
        if (Test-Path (Join-Path $cand 'BIN\WMAIN.EXE')) { $gd = $cand; break }
    }
    if (-not $gd) { Fail "No GAMEDATA\BIN\WMAIN.EXE found in $root. Point me at the disc, the ISO or the GAMEDATA folder." }
    Write-Host "GAMEDATA: $gd"
    $want = @{ 'WMAIN.EXE' = '49d57871b57278270af929dcdf6e3f7ccf3b1adc'; 'BINKW32.DLL' = '8dc84b70f15c3de093bbeddb5f0ea58fdca9090b'; 'IMUSE.DLL' = '589f25d77fa2bf5ed15d92def65692e0e9931a40' }
    foreach ($f in $want.Keys) {
        $h = (Get-FileHash -Algorithm SHA1 (Join-Path $gd "BIN\$f")).Hash.ToLower()
        if ($h -ne $want[$f]) { Fail "$f is not the version this port was made for (SHA-1 $h). Only the English retail CD is supported." }
    }
    Write-Host "Game version: ok (English retail CD)"

    # ---- 3. convert + build
    Step "Converting the game code (a few minutes)"
    & $py @pyArgs -I (Join-Path $repo 'tools\lifter\lift_all.py') $gd (Join-Path $repo 'source\gen')
    if ($LASTEXITCODE) { Fail "The converter failed (see above)." }

    Step "Building tpm3ds.3dsx (several minutes)"
    $m = '/' + $repo.Substring(0, 1).ToLower() + $repo.Substring(2).Replace('\', '/')
    $env:MSYSTEM = 'MSYS'
    & $bash -lc "cd '$m' && make"
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $repo 'tpm3ds.3dsx'))) { Fail "The build failed (see above)." }

    # ---- 4. SD card
    if (-not $NoCopy) {
        Step "Copying to the SD card"
        if (-not $PSBoundParameters.ContainsKey('Sd')) { $Sd = Read-Host "SD card drive letter (e.g. E), or just press Enter to collect everything in the folder 'sdcard' here" }
        $Sd = $Sd.Trim().Trim('"')
        if ($Sd -match '^[A-Za-z]:?\\?$') { $out = $Sd.Substring(0, 1) + ':\' }     # a drive letter
        elseif ($Sd) { $out = $Sd }                                                  # or any folder
        else { $out = Join-Path $repo 'sdcard' }
        $tpm = Join-Path $out '3ds\tpm'
        New-Item -ItemType Directory -Force -Path (Join-Path $tpm 'data') | Out-Null
        Copy-Item (Join-Path $repo 'tpm3ds.3dsx') $tpm -Force
        foreach ($d in 'BIN', 'ETC', 'GOBS', 'JCG', 'LEVEL', 'MOVIE', 'MUSIC') {
            if (-not (Test-Path (Join-Path $gd $d))) { continue }
            Write-Host "  $d ..."
            robocopy (Join-Path $gd $d) (Join-Path $tpm "data\$d") /E /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { Fail "Copying $d failed." }
        }
        Write-Host "`nDONE. Game folder on the card: $tpm" -ForegroundColor Green
        if (-not $Sd) { Write-Host "Copy the folder '3ds' inside '$out' to the root of your 3DS SD card." }
        Write-Host "Start 'tpm3ds' from the Homebrew Launcher. The first start unpacks data (a few minutes)."
    } else {
        Write-Host "`nDONE. Built: $(Join-Path $repo 'tpm3ds.3dsx')" -ForegroundColor Green
    }
}
finally {
    if ($mounted) { Dismount-DiskImage -ImagePath (Resolve-Path $mounted).Path | Out-Null }
}
