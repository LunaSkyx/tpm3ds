# Host build wrapper: WinLibs gcc needs a writable TMP; msys make from Git Bash loses it.
param([Parameter(ValueFromRemainingArguments = $true)] $MakeArgs)
if (-not $env:HOSTMAKE_TMP) { $env:HOSTMAKE_TMP = $env:LOCALAPPDATA + "\Temp" }
$env:TMP = $env:HOSTMAKE_TMP; $env:TEMP = $env:HOSTMAKE_TMP
$env:PATH = "C:\devkitPro\msys2\usr\bin;" + $env:PATH
Set-Location (Split-Path $PSScriptRoot)
& C:\devkitPro\msys2\usr\bin\make.exe -f Makefile.host -j12 @MakeArgs 2>&1 | ForEach-Object { "$_" }
exit $LASTEXITCODE
