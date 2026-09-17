# Provider fixtures: never call the real registry or Windows startup inventory.
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path (Split-Path $PSScriptRoot) 'startup.ps1'
$global:aqdTestsid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$global:aqdTestcommand = $null
$global:aqdTestapproved = $null
$global:aqdTestmismatch = $false
$global:aqdTesthideNew = $false
$global:aqdTestwrites = 0
$global:aqdTestworktree = $false
$global:aqdTestfixturePath = 'C:\Permanent App\ApplicationQuickDial.exe'
function Get-CimInstance {
    param($ClassName, $Filter)
    if ($ClassName -eq 'Win32_Process') { return [pscustomobject]@{ SessionId = (Get-Process -Id $PID).SessionId } }
    if ($ClassName -ne 'Win32_StartupCommand') { throw "Unexpected CIM class $ClassName" }
    if ($null -ne $global:aqdTestcommand -and -not ($global:aqdTesthideNew -and $global:aqdTestcommand -eq ('"' + $global:aqdTestfixturePath + '"'))) {
        [pscustomobject]@{ Name = 'ApplicationQuickDial'; UserSID = $global:aqdTestsid; Location = 'HKU\fixture\Software\Microsoft\Windows\CurrentVersion\Run'; Command = $global:aqdTestcommand }
    }
}
function Invoke-CimMethod {
    param($InputObject, $MethodName, $Namespace, $ClassName, $Arguments)
    if ($MethodName -eq 'GetOwnerSid') {
        return [pscustomobject]@{ ReturnValue = 0; Sid = $(if ($global:aqdTestmismatch) { 'other-user' } else { $global:aqdTestsid }) }
    }
    if ($ClassName -ne 'StdRegProv' -or $Namespace -ne 'root/default' -or
        $Arguments.hDefKey -ne [uint32]2147483651 -or -not $Arguments.sSubKeyName.StartsWith($global:aqdTestsid + '\')) {
        throw 'Provider must address the explicit current-user SID in HKEY_USERS.'
    }
    switch ($MethodName) {
        EnumValues {
            if ($Arguments.sSubKeyName -match '\\StartupApproved\\Run$') {
                return @{ ReturnValue = 0; sNames = $(if ($null -ne $global:aqdTestapproved) { @('ApplicationQuickDial') } else { @() }); Types = @(3) }
            }
            return @{ ReturnValue = 0; sNames = $(if ($null -ne $global:aqdTestcommand) { @('ApplicationQuickDial') } else { @() }); Types = @(1) }
        }
        GetStringValue { return @{ ReturnValue = 0; sValue = $global:aqdTestcommand } }
        GetBinaryValue { return @{ ReturnValue = $(if ($null -eq $global:aqdTestapproved) { 1 } else { 0 }); uValue = $global:aqdTestapproved } }
        CreateKey { return @{ ReturnValue = 0 } }
        SetStringValue { $global:aqdTestcommand = $Arguments.sValue; $global:aqdTestwrites++; return @{ ReturnValue = 0 } }
        DeleteValue { $global:aqdTestcommand = $null; $global:aqdTestwrites++; return @{ ReturnValue = 0 } }
        default { throw "Unexpected method $MethodName" }
    }
}
function Get-Item {
    param($LiteralPath)
    if ($LiteralPath -eq 'missing.exe') { throw 'Missing executable' }
    return [pscustomobject]@{ PSIsContainer = $false; Extension = '.exe'; FullName = $global:aqdTestfixturePath }
}
function Test-Path {
    param($LiteralPath, $PathType)
    if ([IO.Path]::GetFileName($LiteralPath) -ne '.git' -or $PathType -ne 'Leaf') { throw 'Unexpected path probe' }
    return $global:aqdTestworktree
}
function Check($Condition, $Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    try { & $Action; throw 'Expected failure was not raised' }
    catch { if ($_.ToString() -notmatch $Pattern) { throw } }
}
$result = & $scriptPath -Status
Check (-not $result.Registered -and $global:aqdTestwrites -eq 0) 'Status must be read-only.'
Check ($result.StartupApproved -eq 'Not recorded') 'Missing approval must not call GetBinaryValue, which can return error 1.'
$global:aqdTestworktree = $true
Expect-Failure { & $scriptPath } 'linked worktree requires -ExecutablePath'
$null = & $scriptPath -Status
$null = & $scriptPath -ExecutablePath 'C:\Permanent App\ApplicationQuickDial.exe'
$null = & $scriptPath -Disable
$global:aqdTestworktree = $false
$result = & $scriptPath
Check ($global:aqdTestcommand -ceq '"C:\Permanent App\ApplicationQuickDial.exe"') 'Enable must quote the absolute path without arguments.'
$count = $global:aqdTestwrites
$null = & $scriptPath
Check ($global:aqdTestwrites -eq $count) 'Repeated enable must not rewrite registration.'
$null = & $scriptPath -Disable
Check ($null -eq $global:aqdTestcommand) 'Disable must remove registration.'
$global:aqdTestmismatch = $true
Expect-Failure { & $scriptPath } 'does not match'
$global:aqdTestmismatch = $false
Check ($null -eq $global:aqdTestcommand) 'Identity mismatch must not write.'
$global:aqdTestapproved = [byte[]]@(3,0,0,0,0,0,0,0,0,0,0,0)
Expect-Failure { & $scriptPath } 'StartupApproved state is Disabled'
$result = & $scriptPath -Status
Check ($result.StartupApproved -eq 'Disabled') 'Status must expose Windows disable state.'
$global:aqdTestapproved = [byte[]]@(99,0,0,0)
Expect-Failure { & $scriptPath } 'StartupApproved state is Unknown'
$global:aqdTestapproved = [byte[]]@(2,0,0,0)
Expect-Failure { & $scriptPath } 'StartupApproved state is Unknown'
$global:aqdTestapproved = $null
Expect-Failure { & $scriptPath -ExecutablePath missing.exe } 'Missing executable'
$global:aqdTestfixturePath = 'C:\' + ('a' * 254) + '.exe'
Expect-Failure { & $scriptPath } '260-character'
$global:aqdTestfixturePath = 'C:\Permanent App\ApplicationQuickDial.exe'
$global:aqdTestcommand = '"C:\Original\ApplicationQuickDial.exe"'
$global:aqdTesthideNew = $true
Expect-Failure { & $scriptPath } 'original Run value restored'
Check ($global:aqdTestcommand -ceq '"C:\Original\ApplicationQuickDial.exe"') 'Inventory failure must restore original command.'
$global:aqdTestcommand = '"' + $global:aqdTestfixturePath + '"'
$count = $global:aqdTestwrites
Expect-Failure { & $scriptPath -Disable } 'does not independently confirm'
Check ($global:aqdTestwrites -eq $count) 'Pre-existing inventory mismatch must prevent mutation.'
Expect-Failure { & $scriptPath -Status -Disable } 'parameter set'
'Startup script fixture checks passed.'
