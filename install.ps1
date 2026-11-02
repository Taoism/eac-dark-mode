# Installs (or with -Uninstall removes) the EAC dark mode proxy DLL.
param([switch]$Uninstall, [string]$EacDir = "C:\Program Files (x86)\Exact Audio Copy")
$ErrorActionPreference = 'Stop'

$eac = Get-Process EAC -ErrorAction SilentlyContinue
if ($eac) {
    Write-Host "Closing EAC..."
    $eac | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    $eac | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    if (Get-Process EAC -ErrorAction SilentlyContinue) { throw "EAC is still running - close it and retry." }
}

$dll = Join-Path $EacDir "msacm32.dll"
if ($Uninstall) {
    if (Test-Path $dll) { Remove-Item $dll -Force; Write-Host "Removed $dll" }
    Write-Host "Dark mode uninstalled (eacdark.ini left in place)."
    return
}

Copy-Item (Join-Path $PSScriptRoot "build\msacm32.dll") $dll -Force
$ini = Join-Path $EacDir "eacdark.ini"
if (-not (Test-Path $ini)) { Set-Content $ini "[EacDark]`r`nEnabled=1`r`nDebug=0`r`n" -Encoding ASCII }
Write-Host "Installed $dll"
