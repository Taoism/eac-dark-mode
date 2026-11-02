param([int]$Stage, [int]$Enabled = 1, [int]$Wait = 8)
$d = "C:\Program Files (x86)\Exact Audio Copy"
Get-Process EAC -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1
Copy-Item "$PSScriptRoot\..\build\msacm32.dll" "$d\msacm32.dll" -Force
Set-Content "$d\eacdark.ini" "[EacDark]`r`nEnabled=$Enabled`r`nDebug=1`r`nStage=$Stage`r`n" -Encoding ASCII
Remove-Item "$d\eacdark.log" -ErrorAction SilentlyContinue
$p = Start-Process "$d\EAC.exe" -WorkingDirectory $d -PassThru
Start-Sleep $Wait
$p.Refresh(); "stage=$Stage pid=$($p.Id) title='$($p.MainWindowTitle)'"
