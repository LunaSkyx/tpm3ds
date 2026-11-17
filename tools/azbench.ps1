# Azahar benchmark of the current tpm3ds.3dsx: main menu -> level 1, prints the
# scene timestamps and ms per frame for the level windows (scene 900..1500).
#   powershell -File tools/azbench.ps1 [-Frames 2000] [-Label name]
# The SD's scenario.txt is restored afterwards (from scenario_bench.bak).
param([int]$Frames = 2000, [string]$Label = "")
$root = Split-Path $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\tpm"
Copy-Item "$root\tpm3ds.3dsx" "$sd\tpm3ds.3dsx" -Force
"at 460 key RETURN tap`nat 1000 key RETURN tap`nat 1300 prof_reset`nquit_after_frames $Frames" | Set-Content -Encoding ascii "$sd\scenario.txt"
$t0 = Get-Date
$p = Start-Process "C:\Program Files\Azahar\azahar.exe" -ArgumentList "`"$sd\tpm3ds.3dsx`"" -PassThru
while (((Get-Date) - $t0).TotalSeconds -lt 600) {
  Start-Sleep 3
  if ((Get-Item "$sd\log.txt").LastWriteTime -gt $t0 -and (Select-String -Path "$sd\log.txt" -Pattern "exit: closing" -Quiet)) { break }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
if (Test-Path "$sd\scenario_bench.bak") { Copy-Item "$sd\scenario_bench.bak" "$sd\scenario.txt" -Force }
if ($Label) { Copy-Item "$sd\log.txt" "$root\bench\${Label}_log.txt" -Force }
$prev = $null
foreach ($l in (Select-String -Path "$sd\log.txt" -Pattern "^\[\s*(\d+) ms\] r3d: scene (\d+):" -CaseSensitive)) {
  $ms = [int]$l.Matches[0].Groups[1].Value; $sc = [int]$l.Matches[0].Groups[2].Value
  if ($prev -and $sc -ge 900) { "scene {0,5}: {1,6:N1} ms/frame ({2,4:N1} fps)" -f $sc, (($ms - $prev) / 300.0), (300000.0 / ($ms - $prev)) }
  $prev = $ms
}
Select-String -Path "$sd\log.txt" -Pattern "STOP|FAULT|gmem faults" -CaseSensitive | ForEach-Object { $_.Line }
