# A/B of POPT masks in the first droid fight (scene 300-600 window). powershell -File tools/poptab.ps1 mask1 mask2 ...
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Masks)
foreach ($m in $Masks) {
  $out = & "$PSScriptRoot\battlebench.ps1" -Start -Scenario battle_prof -Frames 640 -Popt $m -Label "popt_$m" 2>&1
  $line = ($out | Select-String "scene   600").Line
  "{0}  {1}" -f $m, $line
}
