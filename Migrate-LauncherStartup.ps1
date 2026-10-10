[CmdletBinding(DefaultParameterSetName = 'Snapshot')]
param(
    [Parameter(Mandatory)][string]$LauncherPath,
    [Parameter(Mandatory)][string]$ExpectedAccount,
    [Parameter(Mandatory)][string]$SnapshotPath,
    [string]$StartupDispatcher,
    [string]$InstallDirectory,
    [Parameter(ParameterSetName = 'Snapshot')][string]$ExpectedCommand,
    [Parameter(ParameterSetName = 'Apply', Mandatory)][switch]$Apply,
    [Parameter(ParameterSetName = 'Verify', Mandatory)][switch]$Verify
)
$ErrorActionPreference = 'Stop'
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
if ($sid -ne $ExpectedAccount) { throw 'Launcher startup migration account mismatch.' }
$valueName = 'ApplicationQuickDial'
$runKey = "$sid\Software\Microsoft\Windows\CurrentVersion\Run"
$approvalKey = "$sid\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"
$file = Get-Item -LiteralPath $LauncherPath
if ($file.PSIsContainer -or $file.Extension -ine '.exe' -or $file.FullName -match '[\r\n"]') { throw 'LauncherPath must be an existing absolute executable path.' }
$launcherHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
$dispatcherPath = $null
$dispatcherHash = $null
$dispatcherModuleHash = $null
if ($StartupDispatcher -or $InstallDirectory) {
    if (-not $StartupDispatcher -or -not $InstallDirectory) { throw 'Stable startup requires both StartupDispatcher and InstallDirectory.' }
    $root = (Get-Item -LiteralPath $InstallDirectory).FullName.TrimEnd('\')
    $dispatcher = Get-Item -LiteralPath $StartupDispatcher
    if ($dispatcher.PSIsContainer -or $dispatcher.Name -cne 'Start-BladeLauncher.ps1' -or
        $dispatcher.DirectoryName -ine $root -or $root -match '[\r\n"]|\\Releases\\[a-fA-F0-9]{32}(?:\\|$)') {
        throw 'StartupDispatcher must be the stable installed-root Start-BladeLauncher.ps1.'
    }
    $dispatcherPath = $dispatcher.FullName
    $dispatcherHash = (Get-FileHash -LiteralPath $dispatcherPath -Algorithm SHA256).Hash
    $dispatcherModuleHash = (Get-FileHash -LiteralPath (Join-Path $root 'BladeLauncherStartup.ps1') -Algorithm SHA256).Hash
} elseif ($file.FullName -match '\\Releases\\[a-fA-F0-9]{32}(?:\\|$)') {
    throw 'Never bind startup to an immutable Blade generation. Supply its stable installed-root StartupDispatcher.'
}

function Get-DesiredCommand($Previous) {
    if ($null -eq $Previous) { return $null }
    if ($Previous -notmatch '^"[^"\r\n]+\.exe"(?<tail>.*)$') { throw 'Existing Run command is not a quoted executable; inspect it before migration.' }
    $tail = $Matches.tail
    if ($dispatcherPath) {
        # Preserve the raw suffix without evaluating it as PowerShell. The stable
        # shell owner decodes it only for a native launcher command line.
        $bytes = [Text.Encoding]::UTF8.GetBytes($tail)
        $hex = if ($bytes.Length) { -join ($bytes | ForEach-Object { $_.ToString('x2') }) } else { '00' }
        $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $desired = '"' + $powershell + '" -NoP -W Hidden -EP Bypass -File "' + $dispatcherPath + '" -LauncherArgumentsHex ' + $hex
    } else { $desired = '"' + $file.FullName + '"' + $tail }
    if ($desired.Length -gt 260) { throw 'New startup command exceeds the Windows Run limit; use a shorter stable installation path.' }
    return $desired
}

function Invoke-Registry([string]$Method, [string]$Key, [hashtable]$Extra = @{}) {
    $arguments = @{ hDefKey = [uint32]2147483651; sSubKeyName = $Key }
    foreach ($name in $Extra.Keys) { $arguments[$name] = $Extra[$name] }
    $result = Invoke-CimMethod -Namespace root/default -ClassName StdRegProv -MethodName $Method -Arguments $arguments
    if ($result.ReturnValue -notin @(0, 2)) { throw "Startup registry $Method failed: $($result.ReturnValue)." }
    return $result
}
function Read-Value([string]$Key, [uint32]$Type, [string]$Method, [string]$Property) {
    $values = Invoke-Registry EnumValues $Key
    if ($values.ReturnValue -eq 2 -or $null -eq $values.sNames) { return $null }
    $names = @($values.sNames)
    for ($index = 0; $index -lt $names.Count; $index++) {
        if ($names[$index] -ine $valueName) { continue }
        if ($values.Types[$index] -ne $Type) { throw 'Unexpected startup registry value type; no migration is allowed.' }
        $value = Invoke-Registry $Method $Key @{ sValueName = $valueName }
        if ($value.ReturnValue -ne 0) { throw 'Startup value changed while reading.' }
        return $value.$Property
    }
    return $null
}
function Read-Command { Read-Value $runKey 1 GetStringValue sValue }
function Read-Approval {
    $bytes = Read-Value $approvalKey 3 GetBinaryValue uValue
    if ($null -eq $bytes) { return $null }
    return [Convert]::ToBase64String([byte[]]$bytes)
}
function Assert-Inventory($Expected) {
    $entries = @(Get-CimInstance Win32_StartupCommand | Where-Object {
        $_.Name -ieq $valueName -and $_.UserSID -eq $sid -and $_.Location -match '\\Run$'
    })
    if ($null -eq $Expected) {
        if ($entries.Count -ne 0) { throw 'Windows inventory still reports a Launcher startup entry.' }
    } elseif ($entries.Count -ne 1 -or $entries[0].Command -cne $Expected) {
        throw 'Windows inventory does not confirm the exact Launcher startup command.'
    }
}
function Write-Command($Value) {
    if ($null -eq $Value) { $null = Invoke-Registry DeleteValue $runKey @{ sValueName = $valueName } }
    else {
        $null = Invoke-Registry CreateKey $runKey
        $written = Invoke-Registry SetStringValue $runKey @{ sValueName = $valueName; sValue = $Value }
        if ($written.ReturnValue -ne 0) { throw 'Launcher startup command could not be written.' }
    }
}

$lock = [Threading.Mutex]::new($false, ('Local\Blade.Launcher.Startup.' + $sid))
$acquired = $false
try {
    try { $acquired = $lock.WaitOne(3000) } catch [Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { throw 'Another Launcher startup operation is busy.' }
    $current = Read-Command
    $approval = Read-Approval
    Assert-Inventory $current
    if (-not $Apply -and -not $Verify) {
        if (Test-Path -LiteralPath $SnapshotPath) { throw 'Use a new snapshot path; existing startup backup will not be overwritten.' }
        if (($null -ne $current -and $current -cne $ExpectedCommand) -or
            ($null -eq $current -and -not [string]::IsNullOrEmpty($ExpectedCommand))) {
            throw 'Current Launcher startup command differs from the reviewed expected value.'
        }
        $desired = Get-DesiredCommand $current
        [ordered]@{ version = 2; account = $sid; previous = $current; desired = $desired;
            startupApproved = $approval; launcherPath = $file.FullName; launcherSha256 = $launcherHash;
            startupDispatcher = $dispatcherPath; dispatcherSha256 = $dispatcherHash;
            dispatcherModuleSha256 = $dispatcherModuleHash } |
            ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $SnapshotPath -Encoding UTF8
        [PSCustomObject]@{ Prepared = $true; Changed = $false; Previous = $current; Desired = $desired; Snapshot = $SnapshotPath }
        return
    }
    $snapshot = Get-Content -LiteralPath $SnapshotPath -Raw | ConvertFrom-Json
    if ($snapshot.version -ne 2 -or $snapshot.account -ne $sid -or $snapshot.launcherPath -cne $file.FullName -or
        $snapshot.launcherSha256 -ne $launcherHash -or $snapshot.startupDispatcher -cne $dispatcherPath -or
        $snapshot.dispatcherSha256 -ne $dispatcherHash -or $snapshot.dispatcherModuleSha256 -ne $dispatcherModuleHash -or
        $snapshot.desired -cne (Get-DesiredCommand $snapshot.previous)) {
        throw 'Startup migration snapshot does not match this account, candidate build, and stable dispatcher.'
    }
    if ($approval -cne $snapshot.startupApproved) { throw 'Startup approval changed; refusing to override the user setting.' }
    if ($Verify) {
        if ($current -cne $snapshot.desired) { throw 'Launcher startup command does not match the selected candidate.' }
        Assert-Inventory $snapshot.desired
        [PSCustomObject]@{ Verified = $true; Command = $current; StartupApprovedPreserved = $true }
        return
    }
    if ($current -cne $snapshot.previous) { throw 'Startup command changed since the reviewed snapshot.' }
    try {
        if ($current -cne $snapshot.desired) { Write-Command $snapshot.desired }
        if ((Read-Command) -cne $snapshot.desired -or (Read-Approval) -cne $snapshot.startupApproved) { throw 'Startup migration readback disagrees.' }
        Assert-Inventory $snapshot.desired
    } catch {
        $failure = $_
        $after = Read-Command
        if ($after -cne $snapshot.desired -and $after -cne $snapshot.previous) {
            throw "Startup migration failed and another writer changed the value; no rollback overwrite was attempted. $failure"
        }
        Write-Command $snapshot.previous
        if ((Read-Command) -cne $snapshot.previous) { throw 'Startup rollback readback failed.' }
        Assert-Inventory $snapshot.previous
        throw "Startup migration failed; original Run command restored. $failure"
    }
    [PSCustomObject]@{ Migrated = $true; Command = $snapshot.desired; StartupApprovedPreserved = $true; Snapshot = $SnapshotPath }
} finally {
    if ($acquired) { $lock.ReleaseMutex() }
    $lock.Dispose()
}
