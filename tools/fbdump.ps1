# Runs a scenario with the framebuffer dump (the 8 frames the display really shows from presentation N on).
#   powershell -NoProfile -File tools/fbdump.ps1 -Scenario start_run -Frames 330 -At 235 -Label x [-Start|-Slot 2] [-DrawThread] [-Lod]
param([string]$Scenario = "start_run", [int]$Frames = 330, [int]$At = 235, [string]$Label = "fbd", [switch]$Start, [int]$Slot = 0, [switch]$DrawThread, [switch]$Lod, [switch]$Det, [int]$Stress = 0)
$root = Split-Path $PSScriptRoot
$sd = "$env:APPDATA\Azahar\sdmc\3ds\tpm"
Remove-Item "$sd\fbd_*.bmp" -ErrorAction SilentlyContinue
Set-Content -Encoding ascii "$sd\fbdump.txt" "$At"
if ($DrawThread) { Set-Content -Encoding ascii "$sd\drawthread.txt" $(if ($Stress -gt 0) { "$Stress" } else { "" }) } else { Remove-Item "$sd\drawthread.txt" -ErrorAction SilentlyContinue }
if ($Lod) { Set-Content -Encoding ascii "$sd\lod.txt" "" } else { Remove-Item "$sd\lod.txt" -ErrorAction SilentlyContinue }
$a = @("-Scenario", $Scenario, "-Frames", $Frames, "-Label", $Label)
if ($Det) { $a += @("-Fake", "-NoLimit") }
if ($Start) { $a += "-Start" }
if ($Slot -gt 0) { $a += @("-Slot", $Slot) }
& powershell -NoProfile -File "$root\tools\battlebench.ps1" @a | Select-String "f50|FAULT|wall:"
Remove-Item "$sd\fbdump.txt" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$root\bench\$Label" | Out-Null
Copy-Item "$sd\fbd_*.bmp" "$root\bench\$Label\" -Force
(Get-ChildItem "$root\bench\$Label\fbd_*.bmp").Count
