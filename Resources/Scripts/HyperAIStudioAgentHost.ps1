# Games by Hyper 2026.
# HyperAI Studio's agent host: a hidden process, apart from the editor, that runs the native chats' agents so a turn
# keeps going when the editor closes, restarts or crashes. One runs per project, started by the editor the first time a
# chat needs it. Everything goes through files under -Root (.hyperai/runtime/agent-host), which only this user can open:
#   requests/*.json          the editor's asks: start, stop, stop-all, close (one file each, taken in name order)
#   sessions/<id>/meta.json  the editor's note of the chat (agent, tab, its conversation); the host only reads closed
#   sessions/<id>/input.jsonl lines from the editor: "a<TAB>line" goes to the agent's stdin, "u<TAB>json" is a note
#   sessions/<id>/stream.log everything in order: o (agent stdout), e (stderr), i (sent to the agent), u (editor
#                            notes), x (host events: started, exited, stopped, lost)
#   sessions/<id>/state.json the host's view: running, exited, stopped or lost, with the exit code and last activity
#   host.json                this host: process id and a heartbeat
#   editor.json              the editor that is attached: its process id
#   stop                     created by anyone: every agent stops and the host exits (also -Stop)
# The host exits once no agent has run for -LingerSeconds, and stops an agent that did nothing for -IdleMinutes while no
# editor was attached. No network port is opened.
param(
  [Parameter(Mandatory = $true)][string]$Root,
  [double]$IdleMinutes = 30,
  [int]$LingerSeconds = 60,
  [switch]$Stop
)

$ErrorActionPreference = 'Continue'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Root = [IO.Path]::GetFullPath($Root)
$SessionsDir = Join-Path $Root 'sessions'
$RequestsDir = Join-Path $Root 'requests'
$StopFile = Join-Path $Root 'stop'
$HostFile = Join-Path $Root 'host.json'
$EditorFile = Join-Path $Root 'editor.json'
$LogFile = Join-Path $Root 'host.log'

if ($Stop) {
  New-Item -ItemType Directory -Force -Path $Root | Out-Null
  [IO.File]::WriteAllText($StopFile, (Get-Date).ToUniversalTime().ToString('o'), $Utf8)
  exit 0
}

function Now-Iso { (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'") }

function Say([string]$Text) {
  try {
    if ((Test-Path -LiteralPath $LogFile) -and (Get-Item -LiteralPath $LogFile).Length -gt 1MB) { Remove-Item -LiteralPath $LogFile -Force }
    [IO.File]::AppendAllText($LogFile, ('{0}  {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $Text) + [Environment]::NewLine, $Utf8)
  } catch {}
}

# Only this user (and nobody else on the machine) can read the agents' output or ask for a command to run.
function Protect-Folder([string]$Path) {
  try {
    $Me = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $Acl = New-Object Security.AccessControl.DirectorySecurity
    $Acl.SetOwner($Me)
    $Acl.SetAccessRuleProtection($true, $false)
    $Acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($Me, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')))
    [IO.Directory]::SetAccessControl($Path, $Acl)
  } catch {
    Say "Could not limit $Path to this user: $_"
  }
}

New-Item -ItemType Directory -Force -Path $SessionsDir, $RequestsDir | Out-Null
Protect-Folder $Root

# One host per folder and user: a second one started in a race leaves at once.
$Sha = [Security.Cryptography.SHA1]::Create()
$Hash = -join ($Sha.ComputeHash($Utf8.GetBytes($Root.ToLowerInvariant())) | Select-Object -First 8 | ForEach-Object { $_.ToString('x2') })
$Mutex = New-Object Threading.Mutex($false, "Local\HyperAIStudioAgentHost-$Hash")
$Owned = $false
try { $Owned = $Mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $Owned = $true }
if (-not $Owned) { exit 0 }
Remove-Item -LiteralPath $StopFile -Force -ErrorAction SilentlyContinue

$Credentials = $null
function Read-Credential([string]$Target) {
  if (-not $Credentials) {
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class HyperAIStudioAgentHostCredential {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct C { public int Flags; public int Type; public IntPtr Target; public IntPtr Comment; public long Written; public int Size; public IntPtr Blob; public int Persist; public int Count; public IntPtr Attributes; public IntPtr Alias; public IntPtr User; }
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CredReadW(string target, int type, int flags, out IntPtr credential);
  [DllImport("advapi32.dll")] static extern void CredFree(IntPtr credential);
  public static string Read(string target) { IntPtr p; if (!CredReadW(target, 1, 0, out p)) return null; try { C c = (C)Marshal.PtrToStructure(p, typeof(C)); return c.Size > 0 ? Marshal.PtrToStringUni(c.Blob, c.Size / 2) : null; } finally { CredFree(p); } }
}
'@
    $script:Credentials = $true
  }
  [HyperAIStudioAgentHostCredential]::Read($Target)
}

$Sessions = @{}

function Write-Stream($S, [string]$Kind, [string]$Text) {
  if (-not $S.Out) { return }
  $Bytes = $Utf8.GetBytes($Kind + "`t" + $Text + "`n")
  try { $S.Out.Write($Bytes, 0, $Bytes.Length); $S.Out.Flush() } catch { Say "Could not write the stream of $($S.Id): $_" }
}

function Event-Json([hashtable]$Fields) { ($Fields | ConvertTo-Json -Compress) }

function Save-State($S) {
  $State = [ordered]@{
    id = $S.Id; state = $S.State; pid = $S.ProcessId; exitCode = $S.ExitCode; reason = $S.Reason
    startedAt = $S.StartedAt; endedAt = $S.EndedAt; lastActivityAt = $S.LastActivity.ToUniversalTime().ToString('o')
  }
  try { [IO.File]::WriteAllText((Join-Path $S.Folder 'state.json'), ($State | ConvertTo-Json -Compress), $Utf8) } catch {}
}

function Open-Stream([string]$Folder) {
  New-Object IO.FileStream((Join-Path $Folder 'stream.log'), [IO.FileMode]::Append, [IO.FileAccess]::Write, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
}

function Close-Session($S) {
  foreach ($Name in 'Out', 'In') {
    if ($S[$Name]) { try { $S[$Name].Dispose() } catch {}; $S[$Name] = $null }
  }
  if ($S.Proc) { try { $S.Proc.Dispose() } catch {}; $S.Proc = $null }
}

function Start-Agent($Request) {
  $Id = "$($Request.session)"
  if ($Id -notmatch '^[A-Za-z0-9-]{1,64}$') { Say "Refused a session id that is not plain: $Id"; return }
  if ($Sessions.ContainsKey($Id)) { Say "Session $Id runs already."; return }
  $Folder = Join-Path $SessionsDir $Id
  New-Item -ItemType Directory -Force -Path $Folder | Out-Null
  $S = @{ Id = $Id; Folder = $Folder; State = 'starting'; ProcessId = 0; ExitCode = $null; Reason = ''; StartedAt = (Now-Iso); EndedAt = $null
    LastActivity = Get-Date; InPending = New-Object Collections.Generic.List[byte]; ExitedAt = $null; Dirty = $true }
  $S.Out = Open-Stream $Folder
  $S.In = New-Object IO.FileStream((Join-Path $Folder 'input.jsonl'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  $Info = New-Object Diagnostics.ProcessStartInfo
  $Info.FileName = if ($env:ComSpec) { $env:ComSpec } else { 'cmd.exe' }
  # As the editor runs a chat's agent: through cmd so npm's .cmd shims start too, with /s keeping the quotes intact.
  $Info.Arguments = '/d /s /c "' + $Request.command + '"'
  $Info.WorkingDirectory = if ($Request.cwd) { $Request.cwd } else { $Root }
  $Info.UseShellExecute = $false
  $Info.CreateNoWindow = $true
  $Info.RedirectStandardInput = $true
  $Info.RedirectStandardOutput = $true
  $Info.RedirectStandardError = $true
  $Info.StandardOutputEncoding = $Utf8
  $Info.StandardErrorEncoding = $Utf8
  if ($Request.env) {
    foreach ($Pair in $Request.env.PSObject.Properties) { $Info.EnvironmentVariables[$Pair.Name] = "$($Pair.Value)" }
  }
  # The keys stored for this agent come from Credential Manager into its environment alone; no file holds them.
  foreach ($Target in @($Request.targets)) {
    if (-not $Target -or -not $Target.target) { continue }
    try {
      $Value = Read-Credential $Target.target
      if ($Value) { $Info.EnvironmentVariables[$Target.variable] = $Value }
      $Value = $null
    } catch { Say "Could not read a stored key for $Id; the agent starts without it." }
  }
  try {
    $S.Proc = [Diagnostics.Process]::Start($Info)
  } catch {
    $S.State = 'exited'; $S.ExitCode = -1; $S.Reason = "did not start: $_"; $S.EndedAt = (Now-Iso)
    Write-Stream $S 'x' (Event-Json @{ event = 'exited'; code = -1; reason = $S.Reason })
    Save-State $S
    Close-Session $S
    Say "Session $Id did not start: $_"
    return
  }
  $S.ProcessId = $S.Proc.Id
  $S.State = 'running'
  $S.ReadOut = $S.Proc.StandardOutput.ReadLineAsync()
  $S.ReadErr = $S.Proc.StandardError.ReadLineAsync()
  Write-Stream $S 'x' (Event-Json @{ event = 'started'; pid = $S.ProcessId; at = $S.StartedAt })
  Save-State $S
  $Sessions[$Id] = $S
  Say "Session $Id started (process $($S.ProcessId))."
}

function Stop-Agent($S, [string]$Reason) {
  if ($S.State -ne 'running') { return }
  if ($S.Proc -and -not $S.Proc.HasExited) {
    & taskkill.exe /PID $S.ProcessId /T /F 2>&1 | Out-Null
  }
  $S.State = 'stopped'; $S.Reason = $Reason; $S.EndedAt = (Now-Iso)
  Write-Stream $S 'x' (Event-Json @{ event = 'stopped'; reason = $Reason })
  Save-State $S
  Close-Session $S
  Say "Session $($S.Id) stopped: $Reason."
}

# A session nobody needs any more: its process is gone and the editor closed it.
function Remove-Session([string]$Id) {
  $Sessions.Remove($Id)
  Remove-Item -LiteralPath (Join-Path $SessionsDir $Id) -Recurse -Force -ErrorAction SilentlyContinue
}

function Pump-Agent($S) {
  if ($S.State -ne 'running') { return }
  # Its output, line by line, as it comes.
  foreach ($Pair in @(@('ReadOut', 'o', 'StandardOutput'), @('ReadErr', 'e', 'StandardError'))) {
    $Key = $Pair[0]
    while ($S[$Key] -and $S[$Key].IsCompleted) {
      $Line = $null
      try { $Line = $S[$Key].Result } catch { $Line = $null }
      if ($null -eq $Line) { $S[$Key] = $null; break }
      Write-Stream $S $Pair[1] $Line
      $S.LastActivity = Get-Date; $S.Dirty = $true
      $S[$Key] = $S.Proc.($Pair[2]).ReadLineAsync()
    }
  }
  # What the editor sent: whole lines only.
  try {
    $Length = $S.In.Length
    if ($Length -gt $S.In.Position) {
      $Buffer = New-Object byte[] ([int]($Length - $S.In.Position))
      $Read = $S.In.Read($Buffer, 0, $Buffer.Length)
      if ($Read -lt $Buffer.Length) { [Array]::Resize([ref]$Buffer, [Math]::Max(0, $Read)) }
      $S.InPending.AddRange($Buffer)
    }
  } catch { Say "Could not read the input of $($S.Id): $_" }
  while ($true) {
    $End = $S.InPending.IndexOf([byte]10)
    if ($End -lt 0) { break }
    $Line = $Utf8.GetString($S.InPending.GetRange(0, $End).ToArray()).TrimEnd("`r")
    $S.InPending.RemoveRange(0, $End + 1)
    if ($Line.Length -lt 2) { continue }
    $Kind = $Line.Substring(0, 1); $Body = $Line.Substring(2)
    if ($Kind -eq 'a') {
      Write-Stream $S 'i' $Body
      try {
        $Bytes = $Utf8.GetBytes($Body + "`n")
        $S.Proc.StandardInput.BaseStream.Write($Bytes, 0, $Bytes.Length)
        $S.Proc.StandardInput.BaseStream.Flush()
      } catch { Say "Could not write to $($S.Id): $_" }
      $S.LastActivity = Get-Date; $S.Dirty = $true
    } elseif ($Kind -eq 'u') {
      Write-Stream $S 'u' $Body
    }
  }
  # Its end: once it exited and both of its outputs are read to the end (or a moment passed).
  if ($S.Proc.HasExited) {
    if (-not $S.ExitedAt) { $S.ExitedAt = Get-Date }
    if (($S.ReadOut -or $S.ReadErr) -and ((Get-Date) - $S.ExitedAt).TotalSeconds -lt 3) { return }
    $S.ExitCode = $S.Proc.ExitCode
    $S.State = 'exited'; $S.EndedAt = (Now-Iso)
    Write-Stream $S 'x' (Event-Json @{ event = 'exited'; code = $S.ExitCode })
    Save-State $S
    Close-Session $S
    Say "Session $($S.Id) exited with code $($S.ExitCode)."
  }
}

function Read-Json([string]$Path) {
  try { [IO.File]::ReadAllText($Path) | ConvertFrom-Json -ErrorAction Stop } catch { $null }
}

function Test-EditorAttached {
  $Editor = Read-Json $EditorFile
  if (-not $Editor -or -not $Editor.pid) { return $false }
  return [bool](Get-Process -Id ([int]$Editor.pid) -ErrorAction SilentlyContinue)
}

function Take-Requests {
  foreach ($File in @(Get-ChildItem -LiteralPath $RequestsDir -Filter '*.json' -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
    $Request = Read-Json $File.FullName
    Remove-Item -LiteralPath $File.FullName -Force -ErrorAction SilentlyContinue
    if (-not $Request) { continue }
    $Id = "$($Request.session)"
    switch ("$($Request.op)") {
      'start' { Start-Agent $Request }
      'stop' {
        if ($Sessions.ContainsKey($Id)) { Stop-Agent $Sessions[$Id] $(if ($Request.reason) { "$($Request.reason)" } else { 'asked' }) }
        if ($Request.forget) { Remove-Session $Id }
      }
      'close' {
        if ($Id -match '^[A-Za-z0-9-]{1,64}$' -and (-not $Sessions.ContainsKey($Id) -or $Sessions[$Id].State -ne 'running')) { Remove-Session $Id }
      }
      'stop-all' {
        foreach ($S in @($Sessions.Values)) { Stop-Agent $S $(if ($Request.reason) { "$($Request.reason)" } else { 'asked' }) }
      }
    }
  }
}

# Sessions an earlier host ran are over: their agents ended with it.
foreach ($Dir in @(Get-ChildItem -LiteralPath $SessionsDir -Directory -ErrorAction SilentlyContinue)) {
  $Old = Read-Json (Join-Path $Dir.FullName 'state.json')
  if ($Old -and $Old.state -eq 'running') {
    $S = @{ Id = $Dir.Name; Folder = $Dir.FullName; State = 'lost'; ProcessId = $Old.pid; ExitCode = $null; Reason = 'its host ended'
      StartedAt = $Old.startedAt; EndedAt = (Now-Iso); LastActivity = Get-Date }
    $S.Out = Open-Stream $Dir.FullName
    Write-Stream $S 'x' (Event-Json @{ event = 'lost'; reason = 'its host ended' })
    Save-State $S
    Close-Session $S
  } elseif ($Dir.LastWriteTime -lt (Get-Date).AddDays(-7)) {
    Remove-Item -LiteralPath $Dir.FullName -Recurse -Force -ErrorAction SilentlyContinue
  }
}

$Started = Now-Iso
$LastRunning = Get-Date
$LastBeat = [DateTime]::MinValue
$Attached = $true
Say "Host started (process $PID) for $Root."
try {
  while ($true) {
    if (Test-Path -LiteralPath $StopFile) {
      foreach ($S in @($Sessions.Values)) { Stop-Agent $S 'stopped from outside the editor' }
      Remove-Item -LiteralPath $StopFile -Force -ErrorAction SilentlyContinue
      Say 'Asked to stop: every agent stopped.'
      break
    }
    Take-Requests
    $Running = 0
    foreach ($S in @($Sessions.Values)) {
      Pump-Agent $S
      if ($S.State -eq 'running') { $Running++ }
    }
    $Now = Get-Date
    if (($Now - $LastBeat).TotalSeconds -ge 1) {
      $LastBeat = $Now
      $Attached = Test-EditorAttached
      foreach ($S in @($Sessions.Values)) {
        # An agent that did nothing for a long time with no editor to answer it waits for nobody: it stops.
        if ($S.State -eq 'running' -and -not $Attached -and $IdleMinutes -gt 0 -and ($Now - $S.LastActivity).TotalMinutes -ge $IdleMinutes) {
          Stop-Agent $S "idle for $IdleMinutes min with the editor closed"
        }
        if ($S.Dirty -and $S.State -eq 'running') { $S.Dirty = $false; Save-State $S }
        # Done and closed by the editor: nothing left to keep.
        if ($S.State -ne 'running') {
          $Meta = Read-Json (Join-Path $S.Folder 'meta.json')
          if ($Meta -and $Meta.closed) { Remove-Session $S.Id }
        }
      }
      $Beat = [ordered]@{ pid = $PID; startedAt = $Started; heartbeatAt = (Now-Iso); running = $Running; editorAttached = $Attached; idleMinutes = $IdleMinutes; lingerSeconds = $LingerSeconds }
      try { [IO.File]::WriteAllText($HostFile, ($Beat | ConvertTo-Json -Compress), $Utf8) } catch {}
    }
    if ($Running -gt 0) {
      $LastRunning = $Now
    } elseif (($Now - $LastRunning).TotalSeconds -ge $LingerSeconds) {
      Say "No agent ran for $LingerSeconds s, so the host exits."
      break
    }
    Start-Sleep -Milliseconds $(if ($Running -gt 0) { 25 } else { 200 })
  }
} finally {
  foreach ($S in @($Sessions.Values)) {
    if ($S.State -eq 'running') { Stop-Agent $S 'the host ended' }
  }
  Remove-Item -LiteralPath $HostFile -Force -ErrorAction SilentlyContinue
  try { $Mutex.ReleaseMutex() } catch {}
  Say 'Host exited.'
}
