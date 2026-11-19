# tpm3ds one-click setup (Windows).
# Reads YOUR copy of the game (the CD in a drive, an ISO or a GAMEDATA folder), installs whatever tools are missing, converts and builds the game,
# makes a CIA and puts everything on the SD card.
#   double-click setup.bat           (asks two questions), or
#   powershell -File setup.ps1 -Game D:\TPM.iso -Sd E       (or -Sd C:\some\folder)
# Nothing from the game is ever uploaded or committed; source\gen, tpm3ds.3dsx and tpm3ds.cia stay on your PC / SD card.
param([string]$Game, [string]$Sd, [switch]$NoCopy)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = $PSScriptRoot
function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }
# curl.exe ships with Windows 10/11 and is more reliable than Invoke-WebRequest behind proxies / old TLS settings
function Download($url, $out) {
    & curl.exe -L -s -S --fail --max-time 600 -o $out $url
    if ($LASTEXITCODE) { throw "download failed: $url" }
}
function GitHubAsset($repoName, $pattern) {
    $json = & curl.exe -L -s -S --fail --max-time 60 "https://api.github.com/repos/$repoName/releases/latest"
    if ($LASTEXITCODE) { throw "could not reach github.com" }
    $rel = ($json -join "`n") | ConvertFrom-Json
    $a = $rel.assets | Where-Object { $_.name -like $pattern } | Select-Object -First 1
    if (-not $a) { throw "no download matching $pattern in $repoName" }
    return $a.browser_download_url
}
function WorkingPython {
    # 'python' can be the Microsoft Store stub, so run each candidate
    $cands = @(@('py', '-3'), @('python'))
    $cands += Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue | ForEach-Object { , @($_.FullName) }
    $cands += Get-ChildItem "$env:ProgramFiles\Python3*\python.exe" -ErrorAction SilentlyContinue | ForEach-Object { , @($_.FullName) }
    foreach ($c in $cands) {
        if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
        try { $v = (& $c[0] @($c | Select-Object -Skip 1) --version 2>&1) | Out-String } catch { continue }
        if ($LASTEXITCODE -eq 0 -and $v -match 'Python 3') { return , $c }
    }
    return $null
}
function MsysBash($cmd) { $env:MSYSTEM = 'MSYS'; & $script:bash -lc $cmd }

# ---- 1. Python
Step "Checking Python"
$py = WorkingPython
if (-not $py) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "Python 3 is missing and 'winget' is not available. Install Python from https://www.python.org (tick 'Add python.exe to PATH'), then run this again." }
    Write-Host "Installing Python 3 (winget)..."
    winget install -e --id Python.Python.3.12 --scope user --silent --accept-package-agreements --accept-source-agreements | Out-Null
    $py = WorkingPython
    if (-not $py) { Fail "Python was installed but is not found yet. Close this window and double-click setup.bat again." }
}
$pyExe = $py[0]; $pyArgs = @($py | Select-Object -Skip 1)
& $pyExe @pyArgs -m pip install --quiet --disable-pip-version-check capstone
if ($LASTEXITCODE) { Fail "pip could not install 'capstone' (internet connection?)." }
Write-Host "python: ok   capstone: ok"

# ---- 2. devkitPro (compiler + 3DS libraries)
Step "Checking devkitPro"
$dk = $env:DEVKITPRO
if (-not $dk -or -not (Test-Path (Join-Path $dk 'msys2\usr\bin\bash.exe'))) { $dk = 'C:\devkitPro' }   # DEVKITPRO may hold an MSYS-style path (/opt/devkitpro)
$bash = Join-Path $dk 'msys2\usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    Write-Host "devkitPro is not installed. Downloading its installer (official, from github.com/devkitPro)..."
    $inst = Join-Path $env:TEMP 'devkitProUpdater.exe'
    Download (GitHubAsset 'devkitPro/installer' 'devkitProUpdater*.exe') $inst
    Write-Host ""
    Write-Host "The devkitPro installer opens now. Click through it with the defaults (Next, Install);" -ForegroundColor Yellow
    Write-Host "make sure 'Nintendo 3DS' is ticked. This script continues by itself when it is finished." -ForegroundColor Yellow
    Start-Process -FilePath $inst -Wait
    $dk = 'C:\devkitPro'; $bash = Join-Path $dk 'msys2\usr\bin\bash.exe'
    if (-not (Test-Path $bash)) { Fail "devkitPro was not installed (expected $dk). Run setup.bat again to retry." }
}
# the 3DS toolchain and libraries (does nothing when they are there already)
MsysBash 'pacman -Qg 3ds-dev | grep -q devkitARM || pacman -Sy --noconfirm --needed 3ds-dev'
if ($LASTEXITCODE) { Fail "Could not install the devkitPro '3ds-dev' packages (internet connection?)." }
Write-Host "devkitPro: $dk"

# ---- 3. makerom (for the CIA)
$makerom = Join-Path $repo 'tools\cia\bin\makerom.exe'
if (-not (Test-Path $makerom)) {
    try {
        Step "Downloading makerom (CIA maker)"
        $zip = Join-Path $env:TEMP 'makerom.zip'
        Download (GitHubAsset '3DSGuy/Project_CTR' 'makerom*win_x86_64.zip') $zip
        New-Item -ItemType Directory -Force -Path (Split-Path $makerom) | Out-Null
        Expand-Archive -Path $zip -DestinationPath (Split-Path $makerom) -Force
        Remove-Item $zip -ErrorAction SilentlyContinue
    } catch { Write-Host "Could not get makerom ($_): no CIA will be built, the .3dsx still works." -ForegroundColor Yellow }
}

# ---- 4. the game
Step "Finding your game files"
if (-not $Game) {
    # a game CD that is in a drive right now (a mounted ISO shows up as a drive as well)
    $disc = @([IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'CDRom' -and $_.IsReady -and (Test-Path (Join-Path $_.Name 'GAMEDATA\BIN\WMAIN.EXE')) } | ForEach-Object { $_.Name })
    if ($disc.Count -ge 1) {
        Write-Host "Found the game disc in drive $($disc[0])"
        $ans = "$(Read-Host "Press Enter to use it, or give the path of an ISO / GAMEDATA folder / drive letter")".Trim()
        $Game = if ($ans) { $ans } else { $disc[0] }
    } else {
        $Game = "$(Read-Host "Put the game CD in the drive, or give the path of its ISO / GAMEDATA folder (you can drag it into this window), or a drive letter")"
    }
}
$Game = "$Game".Trim().Trim('"')
if ($Game -match '^[A-Za-z]:?\\?$') { $Game = $Game.Substring(0, 1) + ':\' }     # "F", "F:" or "F:\" = a drive
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
        if ($h -ne $want[$f]) { Fail "$f is not the version this port was made for (SHA-1 $h). Tested: the English and the German retail CD." }
    }
    $title = ''
    $ini = Join-Path (Split-Path $gd) 'INSTALL\LAUNCHER.INI'
    if (Test-Path $ini) { $m0 = Select-String -Path $ini -Pattern '^\s*Name\s*=\s*"?([^"\r\n]+)' | Select-Object -First 1; if ($m0) { $title = " - " + $m0.Matches[0].Groups[1].Value.Trim() } }
    Write-Host "Game version: ok$title"

    # ---- 5. convert + build
    Step "Converting the game code (a few minutes)"
    & $pyExe @pyArgs -I (Join-Path $repo 'tools\lifter\lift_all.py') $gd (Join-Path $repo 'source\gen')
    if ($LASTEXITCODE) { Fail "The converter failed (see above)." }

    Step "Building tpm3ds.3dsx (several minutes)"
    $m = '/' + $repo.Substring(0, 1).ToLower() + $repo.Substring(2).Replace('\', '/')
    MsysBash "cd '$m' && make"
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $repo 'tpm3ds.3dsx'))) { Fail "The build failed (see above)." }

    $cia = Join-Path $repo 'tpm3ds.cia'
    if (Test-Path $makerom) {
        Step "Making tpm3ds.cia"
        Remove-Item $cia -ErrorAction SilentlyContinue
        MsysBash "cd '$m' && sh tools/cia/build_cia.sh"
        if (-not (Test-Path $cia)) { Write-Host "The CIA could not be made (see above); the .3dsx still works." -ForegroundColor Yellow }
    }

    # ---- 6. SD card
    if (-not $NoCopy) {
        Step "Copying to the SD card"
        if (-not $PSBoundParameters.ContainsKey('Sd')) { $Sd = "$(Read-Host "SD card drive letter (e.g. E), or just press Enter to collect everything in the folder 'sdcard' here")" }
        $Sd = "$Sd".Trim().Trim('"')
        if ($Sd -match '^[A-Za-z]:?\\?$') { $out = $Sd.Substring(0, 1) + ':\' }     # a drive letter
        elseif ($Sd) { $out = $Sd }                                                  # or any folder
        else { $out = Join-Path $repo 'sdcard' }
        $tpm = Join-Path $out '3ds\tpm'
        New-Item -ItemType Directory -Force -Path (Join-Path $tpm 'data') | Out-Null
        Copy-Item (Join-Path $repo 'tpm3ds.3dsx') $tpm -Force
        if (Test-Path $cia) {
            New-Item -ItemType Directory -Force -Path (Join-Path $out 'cia') | Out-Null
            Copy-Item $cia (Join-Path $out 'cia') -Force
        }
        foreach ($d in 'BIN', 'ETC', 'GOBS', 'JCG', 'LEVEL', 'MOVIE', 'MUSIC') {
            if (-not (Test-Path (Join-Path $gd $d))) { continue }
            Write-Host "  $d ..."
            # BIG.Z is copied below. MENACE.DAT (German CD) is not used by the port and is a copy-protection file
            # whose sectors can hang a CD drive, so it is never read.
            $skip = @(); if ($d -eq 'GOBS') { $skip = @('/XF', 'BIG.Z', 'MENACE.DAT') }
            robocopy (Join-Path $gd $d) (Join-Path $tpm "data\$d") /E @skip /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { Fail "Copying $d failed." }
        }
        # GOBS\BIG.Z: the German CD pads this file to ~700 MB, so copy only the real archive (its own header says how long it is)
        $bz = Join-Path $gd 'GOBS\BIG.Z'
        if (Test-Path $bz) {
            $in = [IO.File]::OpenRead($bz); $hd = New-Object byte[] 0x20; [void]$in.Read($hd, 0, 0x20)
            $n = [int64][BitConverter]::ToUInt32($hd, 0x12)
            if ($n -lt 1MB -or $n -gt $in.Length) { $n = $in.Length }
            $dest = Join-Path $tpm 'data\GOBS\BIG.Z'
            if (-not ((Test-Path $dest) -and (Get-Item $dest).Length -eq $n)) {
                Write-Host "  GOBS\BIG.Z ..."
                [void]$in.Seek(0, 'Begin'); $out2 = [IO.File]::Create($dest); $buf = New-Object byte[] 4MB; $left = $n
                while ($left -gt 0) { $k = $in.Read($buf, 0, [int][Math]::Min($buf.Length, $left)); if ($k -le 0) { break }; $out2.Write($buf, 0, $k); $left -= $k }
                $out2.Close()
                if ($left -ne 0) { $in.Close(); Fail "Copying GOBS\BIG.Z failed." }
            }
            $in.Close()
        }
        Write-Host "`nDONE." -ForegroundColor Green
        if (-not $Sd) { Write-Host "Copy the folders inside '$out' ('3ds' and 'cia') to the root of your 3DS SD card." }
        Write-Host "On the 3DS: install  cia/tpm3ds.cia  with FBI (it gets a HOME menu icon), or start 3ds/tpm/tpm3ds.3dsx from the Homebrew Launcher."
        Write-Host "The first start unpacks data (a few minutes, with a percentage)."
    } else {
        Write-Host "`nDONE. Built: $(Join-Path $repo 'tpm3ds.3dsx')" -ForegroundColor Green
    }
}
finally {
    if ($mounted) { Dismount-DiskImage -ImagePath (Resolve-Path $mounted).Path | Out-Null }
}
