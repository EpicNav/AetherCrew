# Games by Hyper 2026.
# The part of hyper_editor_restart that outlives the editor: wait for it to close, build it, note the result and
# open the same project again. The editor writes the plan (request.json) and starts this in its own window.
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
try { $Host.UI.RawUI.WindowTitle = 'HyperAI Studio: restarting the editor' } catch {}

function Say([string]$Text, [string]$Color = 'Gray') {
  Write-Host $Text -ForegroundColor $Color
  [IO.File]::AppendAllText($HelperLog, ('{0}  {1}' -f (Get-Date).ToString('HH:mm:ss'), $Text) + [Environment]::NewLine, $Utf8)
}

function Save-Result([hashtable]$Result) {
  $Result.token = $Plan.token
  $Result.at = (Get-Date).ToUniversalTime().ToString('o')
  [IO.File]::WriteAllText($ResultPath, ($Result | ConvertTo-Json -Depth 5), $Utf8)
}

Say "HyperAI Studio is restarting the editor for $($Plan.project)." 'Cyan'

# 1. Wait for the editor to close. A cancelled close never comes: give up, rather than build after a later close.
$Editor = Get-Process -Id $Plan.editorProcessId -ErrorAction SilentlyContinue
if ($Editor) {
  Say 'Waiting for the editor to close...'
  if (-not $Editor.WaitForExit([int]$Plan.closeTimeoutSeconds * 1000)) {
    Say 'The editor did not close, so nothing was built or opened again.' 'Yellow'
    Save-Result @{ state = 'cancelled' }
    Start-Sleep -Seconds $Linger
    exit 1
  }
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
