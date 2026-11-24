# Deterministic A/B (fake clock, nolimit): the 8 frames the display shows from presentation N on, once without and once with the draw thread
#   powershell -NoProfile -File tools/detab.ps1 -Scenario det_start -Frames 400 -At 300 [-Start | -Slot 2] -Label d
param([string]$Scenario = "det_start", [int]$Frames = 400, [int]$At = 300, [string]$Label = "d", [switch]$Start, [int]$Slot = 0, [switch]$Lod, [int]$Stress = 0)
$root = Split-Path $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\tpm"
foreach ($mode in @("ref", "dt")) {
  $a = @("-Det", "-Scenario", $Scenario, "-Frames", $Frames, "-At", $At, "-Label", "${Label}_$mode")
  if ($Start) { $a += "-Start" }
  if ($Slot -gt 0) { $a += @("-Slot", $Slot) }
  if ($mode -eq "dt") { $a += "-DrawThread"; if ($Stress -gt 0) { $a += @("-Stress", $Stress) } }
  if ($Lod) { $a += "-Lod" }
  Set-Content -Encoding ascii "$sd\fakeclock.txt" ""
  Set-Content -Encoding ascii "$sd\nolimit.txt" ""
  & powershell -NoProfile -File "$root\tools\fbdump.ps1" @a
}
Remove-Item "$sd\fakeclock.txt", "$sd\nolimit.txt" -ErrorAction SilentlyContinue
