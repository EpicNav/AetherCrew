# Games by Hyper 2026.
# What the crash watchdog, the restart helper and the editor share when an editor comes back:
#   Get-ProjectEditors   the editors this user has open on one .uproject, so recovery never opens a second one
#   Read-CrashReport     a crash folder's own report (CrashContext.runtime-xml and its log) as a short summary
# Dot-source it for the functions. Run with -ProjectFile, it prints the editors open on that project as JSON (the
# editor asks this before it takes a restart's chats); with -CrashFolder, the crash summary as JSON.
param(
  [string]$ProjectFile,
  [int]$ExcludeId = 0,
  [string]$CrashFolder,
  [int]$CrashProcessId = 0,
  # Tests: a JSON file of processes (ProcessId, Name, CommandLine, Owner, CreationDate) instead of the real ones.
  [string]$ProcessList
)

# The arguments of a Windows command line, as CommandLineToArgvW splits them (quotes grouped, backslashes kept).
function Split-CommandLine([string]$Line) {
  $Parts = New-Object Collections.Generic.List[string]
  $Current = New-Object Text.StringBuilder
  $Quoted = $false
  $Any = $false
  foreach ($Char in "$Line".ToCharArray()) {
    if ($Char -eq '"') { $Quoted = -not $Quoted; $Any = $true; continue }
    if (-not $Quoted -and [char]::IsWhiteSpace($Char)) {
      if ($Any) { $Parts.Add($Current.ToString()); [void]$Current.Clear(); $Any = $false }
      continue
    }
    [void]$Current.Append($Char); $Any = $true
  }
  if ($Any) { $Parts.Add($Current.ToString()) }
  return , $Parts.ToArray()
}

function Format-ProjectPath([string]$Path) {
  try { $Full = [IO.Path]::GetFullPath($Path.Trim()) } catch { $Full = $Path.Trim() }
  return $Full.Replace('/', '\').TrimEnd('\').ToLowerInvariant()
}

# This user's processes as name, command line and owner; the owner is asked of Windows for editor processes only.
function Get-EditorCandidates {
  foreach ($P in @(Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue)) {
    $Owner = ''
    try {
      $O = Invoke-CimMethod -InputObject $P -MethodName GetOwner -ErrorAction Stop
      if ($O.User) { $Owner = if ($O.Domain) { "$($O.Domain)\$($O.User)" } else { "$($O.User)" } }
    } catch {}
    [pscustomobject]@{ ProcessId = [int]$P.ProcessId; Name = "$($P.Name)"; CommandLine = "$($P.CommandLine)"; Owner = $Owner
      CreationDate = if ($P.CreationDate) { $P.CreationDate.ToUniversalTime().ToString('o') } else { '' } }
  }
}

# The editors (not UnrealEditor-Cmd, which runs commandlets and tests) that this user has open on ProjectFile: one of
# its arguments is that very file, by its full path (slashes and case aside), or by its name alone when the editor
# was started in its folder. Another user's editor, or one on a project of the same name elsewhere, does not count.
function Get-ProjectEditors([string]$ProjectFile, [int]$ExcludeId = 0, [object[]]$Processes = $null) {
  if (-not $ProjectFile) { return @() }
  $Wanted = Format-ProjectPath $ProjectFile
  $Name = [IO.Path]::GetFileName($ProjectFile.Trim()).ToLowerInvariant()
  $Me = "$([Environment]::UserDomainName)\$([Environment]::UserName)"
  if ($null -eq $Processes) { $Processes = @(Get-EditorCandidates) }
  $Found = @()
  foreach ($P in $Processes) {
    if ([int]$P.ProcessId -eq $ExcludeId -or "$($P.Name)" -notlike 'UnrealEditor*' -or "$($P.Name)" -like 'UnrealEditor-Cmd*') { continue }
    if ($P.Owner -and -not "$($P.Owner)".Equals($Me, [StringComparison]::OrdinalIgnoreCase)) { continue }
    $Parts = Split-CommandLine "$($P.CommandLine)"
    $Match = $false
    foreach ($Arg in ($Parts | Select-Object -Skip 1)) {
      if ($Arg -notlike '*.uproject') { continue }
      if ($Arg.IndexOfAny([char[]]'\/') -lt 0) { $Match = $Arg.ToLowerInvariant() -eq $Name }
      else { $Match = (Format-ProjectPath $Arg) -eq $Wanted }
      if ($Match) { break }
    }
    if ($Match) { $Found += [pscustomobject]@{ processId = [int]$P.ProcessId; startedAt = "$($P.CreationDate)" } }
  }
  # Plain, so @(...) around a call counts the editors (an empty result is no editor, not one empty list).
  return $Found
}

# The last lines the editor logged before it crashed: the crash's own dump (callstack lines, the "Critical error"
# banner and what follows it) is left out, and so are blank lines; at most Count lines of 200 characters, without the
# timestamp prefix.
function Get-LogTail([string]$Log, [int]$Count = 6) {
  if (-not (Test-Path -LiteralPath $Log)) { return @() }
  $Lines = @(Get-Content -LiteralPath $Log -Tail 400 -Encoding UTF8 -ErrorAction SilentlyContinue)
  $Cut = $Lines.Count
  for ($I = 0; $I -lt $Lines.Count; $I++) {
    if ($Lines[$I] -match '=== Critical error: ===|Fatal error!|appError called:|Unhandled Exception:|Script Stack \(') { $Cut = $I; break }
  }
  $Kept = @($Lines[0..([Math]::Max(0, $Cut - 1))] | Where-Object { $Cut -gt 0 -and $_ -and $_.Trim() -and $_ -notmatch '\[Callstack\]' } |
    ForEach-Object { ($_ -replace '^\[[0-9.:\-]+\]\[\s*\d+\]', '').Trim() } | Where-Object { $_ })
  return @($Kept | Select-Object -Last $Count | ForEach-Object { if ($_.Length -gt 200) { $_.Substring(0, 200) + '...' } else { $_ } })
}

# One crash folder as the chats are told about it: its kind, the error, the first frames of the crashed thread, the
# last lines logged before it, and how long the editor had run. Null when the folder holds no crash context, or one of
# another process (ProcessId, when given).
function Read-CrashReport([string]$Folder, [int]$ProcessId = 0) {
  try { $Context = ([xml][IO.File]::ReadAllText((Join-Path $Folder 'CrashContext.runtime-xml'))).FGenericCrashContext.RuntimeProperties } catch { return $null }
  if (-not $Context) { return $null }
  if ($ProcessId -and "$($Context.ProcessId)" -ne "$ProcessId") { return $null }
  $Message = (@("$($Context.ErrorMessage)" -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join ' ')
  if ($Message.Length -gt 400) { $Message = $Message.Substring(0, 400) + '...' }
  $Log = Get-ChildItem -LiteralPath $Folder -Filter '*.log' -File -ErrorAction SilentlyContinue | Sort-Object Length -Descending | Select-Object -First 1
  $Seconds = 0
  [void][int]::TryParse("$($Context.SecondsSinceStart)", [ref]$Seconds)
  return @{
    type = "$($Context.CrashType)"
    error = $Message
    callstack = @("$($Context.CallStack)" -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Select-Object -First 8)
    logTail = @(if ($Log) { Get-LogTail $Log.FullName })
    uptimeSeconds = $Seconds
    report = 'Saved/Crashes/' + (Split-Path -Leaf $Folder)
    requestingExit = "$($Context.IsRequestingExit)" -eq 'true'
    processId = [int]"0$($Context.ProcessId)"
    folder = $Folder
  }
}

if ($MyInvocation.InvocationName -ne '.') {
  if ($ProjectFile) {
    $List = if ($ProcessList) { @([IO.File]::ReadAllText($ProcessList) | ConvertFrom-Json) } else { $null }
    $Editors = Get-ProjectEditors -ProjectFile $ProjectFile -ExcludeId $ExcludeId -Processes $List
    ConvertTo-Json -InputObject @($Editors) -Compress -Depth 3
  } elseif ($CrashFolder) {
    $Crash = Read-CrashReport -Folder $CrashFolder -ProcessId $CrashProcessId
    if ($Crash) { ConvertTo-Json -InputObject $Crash -Compress -Depth 4 } else { 'null' }
  }
}
