# Azahar run that starts IN the saved battle (slot 91). One-time: -MakeSave creates the save at level frame 1450.
#   powershell -NoProfile -File tools/battlebench.ps1 -MakeSave          (once; writes save/Zanzi91.sav)
#   powershell -NoProfile -File tools/battlebench.ps1 -Start -Label s1   (no save: normal level start = the first droid fight)
#   powershell -NoProfile -File tools/battlebench.ps1 -Label b1 [-Frames 1500] [-Exe tpm3ds.3dsx] [-NoLimit]
param([string]$Scenario = "battle_bench", [switch]$MakeSave, [switch]$Start, [switch]$Prof, [switch]$Fake, [switch]$Async, [switch]$Split, [string]$Popt = "", [int]$Frames = 1500, [string]$Label = "battle", [string]$Exe = "tpm3ds.3dsx", [switch]$NoLimit, [int]$Slot = 0)
$root = Split-Path $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\tpm"
Copy-Item (Join-Path $root $Exe) "$sd\tpm3ds.3dsx" -Force
if ($NoLimit) { Set-Content -Encoding ascii "$sd\nolimit.txt" "" } else { Remove-Item "$sd\nolimit.txt" -ErrorAction SilentlyContinue }
if ($MakeSave) {
  Set-Content -Encoding ascii "$sd\skip.txt" ""; Remove-Item "$sd\battle.txt", "$sd\battle_start.txt" -ErrorAction SilentlyContinue
  Copy-Item "$root\tests\scenarios\battle_mksave.txt" "$sd\scenario.txt" -Force
} else {
  Remove-Item "$sd\skip.txt", "$sd\battle.txt", "$sd\battle_start.txt" -ErrorAction SilentlyContinue
  if ($Start) { Set-Content -Encoding ascii "$sd\battle_start.txt" "" } else { Set-Content -Encoding ascii "$sd\battle.txt" $(if ($Slot -gt 0) { "$Slot" } else { "" }) }
  $sc = (Get-Content "$root\tests\scenarios\$Scenario.txt" -Raw) -replace 'quit_after_frames \d+', "quit_after_frames $Frames"
  Set-Content -Encoding ascii "$sd\scenario.txt" $sc
}
if ($Popt) { Set-Content -Encoding ascii -NoNewline "$sd\popt.txt" $Popt } else { Remove-Item "$sd\popt.txt" -ErrorAction SilentlyContinue }
foreach ($f in @(@("fakeclock.txt",$Fake),@("async.txt",$Async),@("split.txt",$Split))) { if ($f[1]) { Set-Content -Encoding ascii "$sd\$($f[0])" "" } else { Remove-Item "$sd\$($f[0])" -ErrorAction SilentlyContinue } }
if ($Prof) { Set-Content -Encoding ascii "$sd\prof.txt" "" } else { Remove-Item "$sd\prof.txt" -ErrorAction SilentlyContinue }
$t0 = Get-Date
$p = Start-Process "C:\Program Files\Azahar\azahar.exe" -ArgumentList "`"$sd\tpm3ds.3dsx`"" -PassThru
while (((Get-Date) - $t0).TotalSeconds -lt 600) {
  Start-Sleep -Milliseconds 250
  if ((Get-Item "$sd\log.txt").LastWriteTime -gt $t0 -and (Select-String -Path "$sd\log.txt" -Pattern "exit: closing" -Quiet)) { break }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
"wall: {0:N1} s" -f ((Get-Date) - $t0).TotalSeconds
Copy-Item "$sd\log.txt" "$root\bench\${Label}_log.txt" -Force
Select-String -Path "$sd\log.txt" -Pattern "battle:|savebattle|quick save|load slot|STOP|FAULT|gmem faults" -CaseSensitive | ForEach-Object { $_.Line }
$prev = $null
foreach ($l in (Select-String -Path "$sd\log.txt" -Pattern "^\[\s*(\d+) ms\] r3d: scene (\d+):" -CaseSensitive)) {
  $ms = [int]$l.Matches[0].Groups[1].Value; $sc = [int]$l.Matches[0].Groups[2].Value
  if ($prev -and $sc -gt 1) { "scene {0,5}: {1,6:N1} ms/frame ({2,4:N1} fps)" -f $sc, (($ms - $prev) / 300.0), (300000.0 / ($ms - $prev)) }
  $prev = $ms
}
Select-String -Path "$sd\log.txt" -Pattern "frames/300" -CaseSensitive | ForEach-Object { $_.Line }
$f50 = Select-String -Path "$sd\log.txt" -Pattern "f50: 50 frames ending at (\d+) avg (\d+) us" -CaseSensitive | ForEach-Object { "{0}:{1:N1}" -f $_.Matches[0].Groups[1].Value, ([int]$_.Matches[0].Groups[2].Value / 1000.0) }
"f50 (frame:ms) " + ($f50 -join " ")

Select-String -Path "$sd\log.txt" -Pattern "^\[\s*\d+ ms\] prof:" | ForEach-Object { $_.Line }
