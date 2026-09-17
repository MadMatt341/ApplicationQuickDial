[CmdletBinding(DefaultParameterSetName = 'Enable')]
param(
    [Parameter(ParameterSetName = 'Disable', Mandatory = $true)][switch]$Disable,
    [Parameter(ParameterSetName = 'Status', Mandatory = $true)][switch]$Status,
    [Parameter(ParameterSetName = 'Enable')][string]$ExecutablePath = (Join-Path $PSScriptRoot 'build\Release\ApplicationQuickDial.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run this script in 64-bit PowerShell.' }
if (-not $Disable -and -not $Status -and -not $PSBoundParameters.ContainsKey('ExecutablePath') -and
    (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.git') -PathType Leaf)) {
    throw 'Enabling from a linked worktree requires -ExecutablePath pointing to your permanent build.'
}
$name = 'ApplicationQuickDial'
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$sessionId = (Get-Process -Id $PID).SessionId
$shells = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" |
    Where-Object SessionId -EQ $sessionId)
if ($shells.Count -eq 0) { throw 'No interactive Explorer in this session; run from your normal Windows desktop.' }
foreach ($shell in $shells) {
    $owner = Invoke-CimMethod -InputObject $shell -MethodName GetOwnerSid
    if ($owner.ReturnValue -ne 0 -or $owner.Sid -ne $sid) {
        throw 'Current account does not match the interactive user. Run from that user''s normal PowerShell; no registration was changed.'
    }
}
$runKey = "$sid\Software\Microsoft\Windows\CurrentVersion\Run"
$approvalKey = "$sid\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"

function Invoke-Registry([string]$Method, [string]$Key, [hashtable]$Extra = @{}) {
    $arguments = @{ hDefKey = [uint32]2147483651; sSubKeyName = $Key }
    foreach ($keyName in $Extra.Keys) { $arguments[$keyName] = $Extra[$keyName] }
    $result = Invoke-CimMethod -Namespace root/default -ClassName StdRegProv -MethodName $Method -Arguments $arguments
    if ($result.ReturnValue -notin @(0, 2)) { throw "StdRegProv $Method failed: $($result.ReturnValue)" }
    return $result
}
function Read-Registration {
    $values = Invoke-Registry EnumValues $runKey
    if ($values.ReturnValue -eq 2 -or $null -eq $values.sNames) { return $null }
    $index = -1
    $names = @($values.sNames)
    for ($i = 0; $i -lt $names.Count; $i++) {
        if ($names[$i] -ieq $name) { $index = $i; break }
    }
    if ($index -lt 0) { return $null }
    if ($values.Types[$index] -ne 1) { throw 'Existing startup value is not REG_SZ; refusing to overwrite it.' }
    $value = Invoke-Registry GetStringValue $runKey @{ sValueName = $name }
    if ($value.ReturnValue -ne 0) { throw 'Startup registration changed while reading it; retry.' }
    return [string]$value.sValue
}
function Assert-Inventory($Expected) {
    $entries = @(Get-CimInstance Win32_StartupCommand | Where-Object {
        $_.Name -eq $name -and $_.UserSID -eq $sid -and $_.Location -match '\\Run$'
    })
    if ($null -eq $Expected) {
        if ($entries.Count -ne 0) { throw 'Windows startup inventory still reports a Run registration.' }
    } elseif ($entries.Count -ne 1 -or $entries[0].Command -cne $Expected) {
        throw 'Win32_StartupCommand does not independently confirm the exact Run command for this user.'
    }
}

$previous = Read-Registration
Assert-Inventory $previous
$approvalState = 'Not recorded'
$approvalValues = Invoke-Registry EnumValues $approvalKey
if ($approvalValues.ReturnValue -eq 0) {
    $approvalNames = @($approvalValues.sNames)
    for ($i = 0; $i -lt $approvalNames.Count; $i++) {
        if ($approvalNames[$i] -ine $name) { continue }
        $approvalState = 'Unknown'
        if ($approvalValues.Types[$i] -eq 3) {
            $approval = Invoke-Registry GetBinaryValue $approvalKey @{ sValueName = $name }
            if ($approval.ReturnValue -ne 0) { throw 'Startup approval changed while reading it; retry.' }
            if ($null -ne $approval.uValue -and @($approval.uValue).Count -ge 12) {
                $state = [BitConverter]::ToUInt32([byte[]]$approval.uValue, 0)
                $approvalState = switch ($state) { 2 { 'Enabled' } 6 { 'Enabled' } 3 { 'Disabled' } 7 { 'Disabled' } default { 'Unknown' } }
            }
        }
        break
    }
}
if ($Status) {
    [pscustomobject]@{ Name = $name; UserSID = $sid; Registered = ($null -ne $previous); Command = $previous; StartupApproved = $approvalState; InventoryVerified = $true }
    return
}
$desired = $null
if (-not $Disable) {
    if ($approvalState -in @('Disabled', 'Unknown')) {
        throw "Windows StartupApproved state is $approvalState. Review this app in Settings > Apps > Startup; the script will not override it."
    }
    $file = Get-Item -LiteralPath $ExecutablePath
    if ($file.PSIsContainer -or $file.Extension -ine '.exe') { throw 'ExecutablePath must identify an existing .exe file.' }
    $desired = '"' + $file.FullName + '"'
    if ($desired.Length -gt 260) { throw 'The quoted Run command exceeds the Windows 260-character limit.' }
    if ($file.FullName -match '[\r\n"]') { throw 'ExecutablePath contains invalid command characters.' }
}
if ($previous -cne $desired) {
    try {
        if ($Disable) { $null = Invoke-Registry DeleteValue $runKey @{ sValueName = $name } }
        else {
            $null = Invoke-Registry CreateKey $runKey
            $write = Invoke-Registry SetStringValue $runKey @{ sValueName = $name; sValue = $desired }
            if ($write.ReturnValue -ne 0) { throw 'Run command could not be written.' }
        }
        if ((Read-Registration) -cne $desired) { throw 'Run registration readback failed.' }
        Assert-Inventory $desired
    } catch {
        $failure = $_
        try {
            if ($null -eq $previous) { $null = Invoke-Registry DeleteValue $runKey @{ sValueName = $name } }
            else { $null = Invoke-Registry SetStringValue $runKey @{ sValueName = $name; sValue = $previous } }
            if ((Read-Registration) -cne $previous) { throw 'Restored Run value differs from the original.' }
            Assert-Inventory $previous
        } catch { throw "Registration failed: $failure. Restoration also failed: $_. Inspect Windows Startup settings before retrying." }
        throw "Registration failed; original Run value restored: $failure"
    }
}
[pscustomobject]@{ Name = $name; UserSID = $sid; Registered = (-not $Disable); Command = $desired; StartupApproved = $approvalState; InventoryVerified = $true }
