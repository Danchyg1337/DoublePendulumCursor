# uninstall.ps1 -- remove the logon task, stop the app, restore the cursor.
$ErrorActionPreference = 'SilentlyContinue'
$taskName = 'PendulumCursor'
$dest = Join-Path $env:LOCALAPPDATA 'PendulumCursor'

Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
Get-Process 'pendulum_cursor' | Stop-Process -Force
# Restore the default cursor scheme in case it was still active.
rundll32.exe user32.dll,UpdatePerUserSystemParameters

Write-Host "Removed the '$taskName' logon task and stopped the app."
Write-Host "App files are still in: $dest"
Write-Host "Delete that folder manually if you want them gone."
