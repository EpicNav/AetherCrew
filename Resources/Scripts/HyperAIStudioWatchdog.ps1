# Games by Hyper 2026.
# Opens the editor again after a crash. The editor starts this, hidden, once its window is up, and keeps Session current:
# a restart plan for this editor (its build, how it opens again, its chats). A clean exit deletes Session first. When the
# editor ends with Session still there and a crash report of its own, the plan goes to the restart helper, which builds,
# opens the project again and gives the chats back.
param([Parameter(Mandatory = $true)][string]$Session)

$ErrorActionPreference = 'Continue'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Folder = Split-Path -Parent $Session
. (Join-Path $PSScriptRoot 'HyperAIStudioRecovery.ps1')

function Say([string]$Text) {
  [IO.File]::AppendAllText((Join-Path $Folder 'watchdog.log'), ('{0}  {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $Text) + [Environment]::NewLine, $Utf8)
}

function Read-Plan {
  try { [IO.File]::ReadAllText($Session) | ConvertFrom-Json -ErrorAction Stop } catch { $null }
}

# 1. Wait for the editor to end. Holding its handle keeps the process readable after it exits.
$Plan = Read-Plan
$Editor = if ($Plan) { Get-Process -Id $Plan.editorProcessId -ErrorAction SilentlyContinue }
if (-not $Editor) { exit 0 }
$null = $Editor.Handle
$Since = $Editor.StartTime.ToUniversalTime()
$Editor.WaitForExit()
if (-not (Test-Path -LiteralPath $Session)) { exit 0 }
# Its last chats; a plan cut off mid-write falls back to the first one.
$Last = Read-Plan
if ($Last) { $Plan = $Last }
Remove-Item -LiteralPath $Session -Force -ErrorAction SilentlyContinue

# 2. Its own crash report. A killed editor leaves none, an ensure's is not fatal, and one from closing is not a reason to
# open it again. The report is written before the process ends; a moment's grace covers a slow disk.
$Crash = $null
for ($Try = 0; $Try -lt 10 -and -not $Crash; $Try++) {
  if ($Try) { Start-Sleep -Milliseconds 500 }
  foreach ($Dir in @(Get-ChildItem -LiteralPath $Plan.crashes -Directory -ErrorAction SilentlyContinue | Where-Object { $_.CreationTimeUtc -ge $Since })) {
    # Its kind, error, first frames and the last lines it logged: what the chats are told (HyperAIStudioRecovery.ps1).
    $Report = Read-CrashReport -Folder $Dir.FullName -ProcessId $Plan.editorProcessId
    if (-not $Report -or $Report.type -notin 'Crash', 'Assert', 'GPUCrash', 'OutOfMemory' -or $Report.requestingExit) { continue }
    $Crash = $Report
    $Crash.Remove('requestingExit')
  }
}
if (-not $Crash) {
  Say "The editor ($($Plan.editorProcessId)) ended without a crash report of its own, so it stays closed."
  exit 0
}

# 3. The third crash in half an hour leaves it closed: something breaks it as soon as it opens.
$History = Join-Path $Folder 'crashes.txt'
$Recent = @(Get-Content -LiteralPath $History -ErrorAction SilentlyContinue | Where-Object { $_ } |
  ForEach-Object { [DateTime]::Parse($_) } | Where-Object { $_ -gt (Get-Date).AddMinutes(-30) })
[IO.File]::WriteAllLines($History, [string[]](@($Recent | ForEach-Object { $_.ToString('o') }) + (Get-Date).ToString('o')), $Utf8)
$Reopen = $Recent.Count -lt 2

# 4. The plan goes to the restart helper, in a window of its own so the build and the countdown show.
$Plan.editorProcessId = 0
$Plan.requestedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'")
$Plan | Add-Member -NotePropertyName reason -NotePropertyValue 'crash' -Force
$Plan | Add-Member -NotePropertyName reopen -NotePropertyValue $Reopen -Force
$Plan | Add-Member -NotePropertyName crash -NotePropertyValue $Crash -Force
$Request = if ($Reopen) { Join-Path $Plan.restartFolder 'request.json' } else { Join-Path $Folder 'left-closed.json' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Request) | Out-Null
Remove-Item -LiteralPath (Join-Path (Split-Path -Parent $Request) 'result.json') -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllText($Request, ($Plan | ConvertTo-Json -Depth 8), $Utf8)
Say "The editor ($($Plan.project)) crashed: $($Crash.error) $(if ($Reopen) { 'Opening it again.' } else { 'Its third crash in half an hour, so it stays closed.' })"
$Window = if ($Plan.helperWindow) { $Plan.helperWindow } else { 'Normal' }
Start-Process -FilePath 'powershell.exe' -WindowStyle $Window -WorkingDirectory $Plan.relaunch.workingDirectory -ArgumentList @(
  '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'HyperAIStudioRestart.ps1')`"", '-Request', "`"$Request`"")
