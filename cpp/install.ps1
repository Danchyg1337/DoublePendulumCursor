# install.ps1 -- set up the pendulum cursor to start at logon, the RIGHT way.
#
# Why not the Startup folder? Two reasons you may have hit:
#   1. The Startup folder launches EVERY file in it, so Windows also tries to
#      "open" pendulum.conf / cursors.conf (in Notepad or a file picker).
#   2. Apps started from the Startup folder are deliberately delayed/throttled
#      by Windows' startup-impact manager, so they feel slow to appear.
#
# This installs the exe + configs into %LOCALAPPDATA%\PendulumCursor and creates
# a per-user "at log on" scheduled task, which starts promptly, at normal
# priority, and never touches the .conf files. No admin rights required.

$ErrorActionPreference = 'Stop'
$src  = $PSScriptRoot
$dest = Join-Path $env:LOCALAPPDATA 'PendulumCursor'
$taskName = 'PendulumCursor'

# 1. Copy the app + configs to a stable location.
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item (Join-Path $src 'pendulum_cursor.exe') $dest -Force
foreach ($f in 'pendulum.conf','cursors.conf') {
    $p = Join-Path $src $f
    if (Test-Path $p) { Copy-Item $p $dest -Force }
}
# The beat dancer's GIFs (.gifbpm) live in an actors\ folder next to the exe.
$actors = Join-Path $src 'actors'
if (Test-Path $actors) {
    $destActors = Join-Path $dest 'actors'
    New-Item -ItemType Directory -Force -Path $destActors | Out-Null
    Copy-Item (Join-Path $actors '*') $destActors -Recurse -Force
}
$exe = Join-Path $dest 'pendulum_cursor.exe'

# 2. (Re)create the logon task.
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

$action    = New-ScheduledTaskAction -Execute $exe
$trigger   = New-ScheduledTaskTrigger -AtLogOn
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
                -DontStopIfGoingOnBatteries -StartWhenAvailable `
                -ExecutionTimeLimit ([TimeSpan]::Zero)
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) `
                -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal | Out-Null

# 3. Force Normal priority (Task Scheduler default is 7 = below normal) and no
#    start delay, so it launches promptly and runs smoothly.
$t = Get-ScheduledTask -TaskName $taskName
$t.Settings.Priority = 5
if ($t.Triggers.Count -gt 0) { $t.Triggers[0].Delay = 'PT0S' }
Set-ScheduledTask -TaskName $taskName -Settings $t.Settings -Trigger $t.Triggers | Out-Null

Write-Host "Installed to: $dest"
Write-Host "It will start automatically at every logon (task '$taskName')."
Write-Host "Reminder: remove pendulum_cursor.exe and the .conf files from your"
Write-Host "Startup folder (Win+R -> shell:startup) so they don't launch twice."
Write-Host ""
Write-Host "Starting it now..."
Start-Process $exe
