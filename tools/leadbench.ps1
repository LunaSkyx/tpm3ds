# Lead bench: run a scenario on the current tpm3ds.3dsx in Azahar and print the
# per-300-frame window table plus any prof report. Unlike azbench.ps1 this takes
# an explicit scenario file and does not restore anything.
#   powershell -NoProfile -File tools/leadbench.ps1 -Scenario tests/scenarios/lead_prof.txt -Frames 2100 -Label prof1
param(
  [string]$Scenario = "tests/scenarios/lead_prof.txt",
  [int]$Frames = 2100,
  [string]$Label = "lead",
  [string]$Exe = "tpm3ds.3dsx",
  [switch]$Prof,
  [switch]$NoLimit
)
$root = Split-Path $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\tpm"
Copy-Item (Join-Path $root $Exe) "$sd\tpm3ds.3dsx" -Force
if ($NoLimit) { Set-Content -Encoding ascii "$sd\nolimit.txt" "" } else { Remove-Item "$sd\nolimit.txt" -ErrorAction SilentlyContinue }
$sc = if (Test-Path $Scenario) { Get-Content $Scenario -Raw } else { Get-Content "$root\$Scenario" -Raw }
Set-Content -Encoding ascii "$sd\scenario.txt" $sc
if ($Prof) { Set-Content -Encoding ascii "$sd\prof.txt" "" } else { Remove-Item "$sd\prof.txt" -ErrorAction SilentlyContinue }
$t0 = Get-Date
$p = Start-Process "C:\Program Files\Azahar\azahar.exe" -ArgumentList "`"$sd\tpm3ds.3dsx`"" -PassThru
while (((Get-Date) - $t0).TotalSeconds -lt 900) {
  Start-Sleep 3
  if ((Get-Item "$sd\log.txt").LastWriteTime -gt $t0 -and (Select-String -Path "$sd\log.txt" -Pattern "exit: closing" -Quiet)) { break }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Copy-Item "$sd\log.txt" "$root\bench\${Label}_log.txt" -Force
$prev = $null
foreach ($l in (Select-String -Path "$sd\log.txt" -Pattern "^\[\s*(\d+) ms\] r3d: scene (\d+):" -CaseSensitive)) {
  $ms = [int]$l.Matches[0].Groups[1].Value; $scn = [int]$l.Matches[0].Groups[2].Value
  if ($prev -and $scn -ge 900) { "scene {0,5}: {1,6:N1} ms/frame ({2,4:N1} fps)" -f $scn, (($ms - $prev) / 300.0), (300000.0 / ($ms - $prev)) }
  $prev = $ms
}
Select-String -Path "$sd\log.txt" -Pattern "^\[\s*\d+ ms\] prof:" | ForEach-Object { $_.Line }
Select-String -Path "$sd\log.txt" -Pattern "STOP|FAULT|gmem faults|exit: guest" | ForEach-Object { $_.Line }
