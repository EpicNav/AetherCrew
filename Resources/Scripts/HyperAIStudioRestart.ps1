# Games by Hyper 2026.
# The part of hyper_editor_restart that outlives the editor: wait for it to close, build it, note the result and
# open the same project again. The editor writes the plan (request.json) and starts this in its own window. After a
# crash, the watchdog writes the plan (reason crash) and starts this instead.
param([Parameter(Mandatory = $true)][string]$Request)

$ErrorActionPreference = 'Continue'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Plan = [IO.File]::ReadAllText($Request) | ConvertFrom-Json
$ResultPath = Join-Path (Split-Path -Parent $Request) 'result.json'
# What this window says, kept for when nobody watched it: helper.log beside the plan.
$HelperLog = Join-Path (Split-Path -Parent $Request) 'helper.log'
[IO.File]::WriteAllText($HelperLog, '', $Utf8)
# Seconds the window stays up to be read: after a success, after a failure. Tests set 0.
$Linger = if ($null -ne $Plan.lingerSeconds) { [int]$Plan.lingerSeconds } else { 3 }
$Crashed = $Plan.reason -eq 'crash'
try { $Host.UI.RawUI.WindowTitle = if ($Crashed) { 'HyperAI Studio: the editor crashed' } else { 'HyperAI Studio: restarting the editor' } } catch {}

function Say([string]$Text, [string]$Color = 'Gray') {
  Write-Host $Text -ForegroundColor $Color
  [IO.File]::AppendAllText($HelperLog, ('{0}  {1}' -f (Get-Date).ToString('HH:mm:ss'), $Text) + [Environment]::NewLine, $Utf8)
}

function Save-Result([hashtable]$Result) {
  $Result.token = $Plan.token
  $Result.at = (Get-Date).ToUniversalTime().ToString('o')
  [IO.File]::WriteAllText($ResultPath, ($Result | ConvertTo-Json -Depth 5), $Utf8)
}

# An editor someone opened already (by hand, or after a crash the crash reporter's Send and Restart) keeps the project,
# and takes the chats back itself. For a restart this is checked only once the editor that asked for it is gone.
# ponytail: matched by the project's file name, so two projects sharing one count as one.
function Test-EditorProcess {
  $Name = if ($Plan.projectFile) { [IO.Path]::GetFileName($Plan.projectFile) } else { '' }
  if (-not $Name) { return $false }
  $Open = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike 'UnrealEditor-Cmd*' -and "$($_.CommandLine)".IndexOf($Name, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
  return $Open.Count -gt 0
}

function Test-EditorOpen {
  if (-not (Test-EditorProcess)) { return $false }
  Say 'The editor is open again already, and its chats come back there.' 'Green'
  Save-Result @{ state = 'already_open' }
  Start-Sleep -Seconds $Linger
  return $true
}

# Unreal's crash reporter runs beside the editor with -MONITOR=<its process> and, after a crash, waits in its dialog.
# Nobody chose by now: send the report (unless Send The Crash Report For Me is off) and close it, as Send and Close
# would, so it cannot open a second editor later. Gone already: someone chose, and Send and Restart's editor may still
# be starting, so it gets a few seconds to appear.
function Close-CrashReporter {
  $Id = [int]$Plan.crash.processId
  $Reporter = if ($Id) { @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { "$($_.CommandLine)" -match "-MONITOR=$Id\b" }) } else { @() }
  if ($Reporter.Count -eq 0) {
    for ($Try = 0; $Try -lt 10 -and -not (Test-EditorProcess); $Try++) { Start-Sleep -Milliseconds 500 }
    return
  }
  $Reporter | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  if (-not $Plan.sendCrashReport -or -not $Plan.crash.folder) {
    Say 'Nobody chose in the crash reporter, so HyperAI Studio closed it without sending the report.'
    return
  }
  Say 'Nobody chose in the crash reporter, so HyperAI Studio sends the report and closes it, as Send and Close would.'
  $Sender = if ($Plan.crashReporter) { $Plan.crashReporter } else { Join-Path (Split-Path -Parent $Plan.relaunch.executable) 'CrashReportClientEditor.exe' }
  try {
    $Send = Start-Process -FilePath $Sender -ArgumentList @("`"$($Plan.crash.folder)`"", '-Unattended') -WindowStyle Hidden -PassThru -ErrorAction Stop
    if (-not $Send.WaitForExit(90000)) {
      Stop-Process -Id $Send.Id -Force -ErrorAction SilentlyContinue
      Say 'The crash report was not sent within 90 s.' 'Yellow'
    } elseif ($Send.ExitCode -eq 0) {
      Say 'The crash report was sent.' 'Green'
    } else {
      Say "The crash reporter could not send the report (exit code $($Send.ExitCode))." 'Yellow'
    }
  } catch {
    Say "The crash report was not sent: $_" 'Yellow'
  }
}

if ($Crashed) {
  Say "The editor crashed: $($Plan.crash.error)" 'Yellow'
  Say "The crash report: $($Plan.crash.report)"
  if (-not $Plan.reopen) {
    Say 'It crashed three times in half an hour, so HyperAI Studio leaves it closed.' 'Yellow'
    Save-Result @{ state = 'left_closed' }
    Start-Sleep -Seconds ($Linger * 20)
    exit 1
  }
  $Delay = if ($null -ne $Plan.delaySeconds) { [int]$Plan.delaySeconds } else { 15 }
  Say "HyperAI Studio opens it again in $Delay s, with its chats. Close this window to leave it closed." 'Cyan'
  Say "Your choice in Unreal's crash reporter counts: Send and Restart opens the editor now, and its chats come back there."
  Start-Sleep -Seconds $Delay
  if (Test-EditorOpen) { exit 0 }
  Close-CrashReporter
  if (Test-EditorOpen) { exit 0 }
} else {
  Say "HyperAI Studio is restarting the editor for $($Plan.project)." 'Cyan'
}

# 1. Wait for the editor to close, however long its questions take (save your work, close anyway). A close that is
# turned down calls the restart off: the editor stops this window and withdraws the plan, so a later close builds and
# reopens nothing. closeTimeoutSeconds above 0 also gives up after that long. After a crash there is nothing to wait for.
$Editor = if ($Plan.editorProcessId) { Get-Process -Id $Plan.editorProcessId -ErrorAction SilentlyContinue }
if ($Editor) {
  Say 'Waiting for the editor to close. If it asks first (save your work, close anyway), it waits for your answer.'
  $Limit = [int]$Plan.closeTimeoutSeconds
  $Waited = [Diagnostics.Stopwatch]::StartNew()
  while (-not $Editor.WaitForExit(500)) {
    if (-not (Test-Path -LiteralPath $Request)) {
      Say 'The restart was called off in the editor, which stays open, so nothing was built or opened again.' 'Yellow'
      Start-Sleep -Seconds $Linger
      exit 1
    }
    if ($Limit -gt 0 -and $Waited.Elapsed.TotalSeconds -ge $Limit) {
      Say 'The editor did not close, so nothing was built or opened again.' 'Yellow'
      Save-Result @{ state = 'cancelled' }
      Start-Sleep -Seconds $Linger
      exit 1
    }
  }
  Say 'The editor closed.'
}

# 2. Build, showing and logging the output, and keep the compiler, linker and UBT errors for the agent.
$Build = @{ enabled = [bool]$Plan.build.enabled }
if ($Build.enabled) {
  Say "Building $($Plan.build.target)..." 'Cyan'
  $Watch = [Diagnostics.Stopwatch]::StartNew()
  $Log = New-Object System.IO.StreamWriter($Plan.build.log, $false, $Utf8)
  $global:LASTEXITCODE = $null
  try {
    & $Plan.build.script 2>&1 | ForEach-Object { $Line = "$_"; Write-Host $Line; $Log.WriteLine($Line) }
  } finally {
    $Log.Close()
  }
  $Build.exitCode = if ($null -eq $LASTEXITCODE) { -1 } else { $LASTEXITCODE }
  $Build.seconds = [int]$Watch.Elapsed.TotalSeconds
  $Build.errors = @(Select-String -LiteralPath $Plan.build.log -Pattern '\berror [A-Z]+\d+|: (fatal )?error:|^\s*(ERROR|Error):' |
    Select-Object -First 30 | ForEach-Object { $_.Line.Trim() })
  if ($Build.exitCode -eq 0) { Say "The build succeeded in $($Build.seconds) s." 'Green' }
  else { Say "The build failed (exit code $($Build.exitCode)); the editor opens on the binaries from before it." 'Yellow' }
}

# 3. Open the same project again, with the token that tells HyperAI Studio to give the chats back.
if (Test-EditorOpen) { exit 0 }
Save-Result @{ state = 'relaunching'; build = $Build }
Say 'Opening the project again...' 'Cyan'
try {
  Start-Process -FilePath $Plan.relaunch.executable -ArgumentList $Plan.relaunch.arguments -WorkingDirectory $Plan.relaunch.workingDirectory -ErrorAction Stop
} catch {
  Say "The editor did not start: $_" 'Red'
  Save-Result @{ state = 'relaunch_failed'; build = $Build; error = "$_" }
  Start-Sleep -Seconds ($Linger * 10)
  exit 1
}
Save-Result @{ state = 'relaunched'; build = $Build }
Say 'The editor is starting; HyperAI Studio gives the chats back once it is up.' 'Green'
Start-Sleep -Seconds $Linger
