# All registry and startup inventory operations are owned in-memory fixtures.
$ErrorActionPreference = 'Stop'
$script = Join-Path (Split-Path $PSScriptRoot) 'Migrate-LauncherStartup.ps1'
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$candidate = Join-Path (Split-Path $PSScriptRoot) '../build/Release/ApplicationQuickDial.exe'
if ($args.Count) { $candidate = $args[0] }
$candidate = (Resolve-Path -LiteralPath $candidate).Path
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('LauncherStartupFixtures-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$global:migrationCommand = $null
$global:migrationApproval = [byte[]](3,0,0,0,0,0,0,0,0,0,0,0) # user-disabled approval
$global:migrationHideCandidate = $false
$global:migrationWrites = 0
function Invoke-CimMethod {
    param($Namespace, $ClassName, $MethodName, $Arguments)
    if ($Namespace -ne 'root/default' -or $ClassName -ne 'StdRegProv' -or
        $Arguments.hDefKey -ne [uint32]2147483651 -or -not $Arguments.sSubKeyName.StartsWith($sid + '\') -or
        ($Arguments.ContainsKey('sValueName') -and $Arguments.sValueName -ne 'ApplicationQuickDial')) { throw 'Unexpected registry boundary.' }
    $approval = $Arguments.sSubKeyName.EndsWith('\StartupApproved\Run')
    switch ($MethodName) {
        EnumValues {
            if ($approval) { return @{ReturnValue=0;sNames=@('ApplicationQuickDial');Types=@(3)} }
            if ($null -eq $global:migrationCommand) { return @{ReturnValue=2;sNames=$null} }
            return @{ReturnValue=0;sNames=@('ApplicationQuickDial');Types=@(1)}
        }
        GetStringValue { return @{ReturnValue=0;sValue=$global:migrationCommand} }
        GetBinaryValue { return @{ReturnValue=0;uValue=$global:migrationApproval} }
        CreateKey { return @{ReturnValue=0} }
        SetStringValue { $global:migrationCommand = $Arguments.sValue; $global:migrationWrites++; return @{ReturnValue=0} }
        DeleteValue { $global:migrationCommand=$null; $global:migrationWrites++; return @{ReturnValue=0} }
        default { throw "Unexpected registry method: $MethodName" }
    }
}
function Get-CimInstance {
    param($ClassName)
    if ($ClassName -ne 'Win32_StartupCommand') { throw 'Unexpected inventory provider.' }
    if ($null -ne $global:migrationCommand -and -not ($global:migrationHideCandidate -and $global:migrationCommand.StartsWith('"' + $candidate))) {
        [PSCustomObject]@{Name='ApplicationQuickDial';UserSID=$sid;Location='HKU\fixture\Run';Command=$global:migrationCommand}
    }
}
function Check($Condition, [string]$Description) { if (-not $Condition) { throw "FAILED: $Description" } }
function Throws([scriptblock]$Action, [string]$Description) {
    $failed=$false
    try { & $Action | Out-Null } catch { $failed=$true }
    Check $failed $Description
}
try {
    $old = '"D:\Development\ApplicationQuickDial.exe" --shell-mode'
    $global:migrationCommand = $old
    $snapshot = Join-Path $fixture 'startup.json'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -ExpectedCommand $old -SnapshotPath $snapshot | Out-Null
    Check ($global:migrationWrites -eq 0) 'Preparation never changes the live startup value.'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $snapshot -Apply | Out-Null
    Check ($global:migrationCommand -eq ('"' + $candidate + '" --shell-mode')) 'Migration changes only the executable and preserves existing arguments.'
    Check ([Convert]::ToBase64String($global:migrationApproval) -eq 'AwAAAAAAAAAAAAAA') 'User-disabled StartupApproved bytes remain unchanged.'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $snapshot -Verify | Out-Null
    $global:migrationCommand=$old
    $global:migrationHideCandidate=$true
    Throws { & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $snapshot -Apply } 'Independent inventory failure rejects the migration.'
    Check ($global:migrationCommand -eq $old) 'Verification failure restores the exact previous Run command.'
    $global:migrationHideCandidate=$false
    $global:migrationCommand='"C:\Different\ApplicationQuickDial.exe"'
    Throws { & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $snapshot -Apply } 'An intervening startup edit is never overwritten.'
    $global:migrationCommand=$old
    $global:migrationApproval[0]=2
    Throws { & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $snapshot -Apply } 'An intervening approval change is never overridden.'
    $global:migrationCommand=$null
    $empty = Join-Path $fixture 'absent.json'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $empty | Out-Null
    $before=$global:migrationWrites
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $empty -Apply | Out-Null
    Check ($null -eq $global:migrationCommand -and $global:migrationWrites -eq $before) 'Missing startup preference remains absent.'
    Throws { & $script -LauncherPath $candidate -ExpectedAccount 'wrong-account' -SnapshotPath (Join-Path $fixture 'wrong.json') } 'Account mismatch fails before any mutation.'
    $dispatcher = Join-Path $fixture 'Start-BladeLauncher.ps1'
    $module = Join-Path $fixture 'BladeLauncherStartup.ps1'
    Set-Content -LiteralPath $dispatcher -Value '# owned dispatcher contract fixture'
    Set-Content -LiteralPath $module -Value '# owned module contract fixture'
    $global:migrationCommand = $old
    $stableSnapshot = Join-Path $fixture 'stable.json'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -ExpectedCommand $old -SnapshotPath $stableSnapshot `
        -StartupDispatcher $dispatcher -InstallDirectory $fixture | Out-Null
    $binding = Get-Content -LiteralPath $stableSnapshot -Raw | ConvertFrom-Json
    Check ($binding.desired.Contains(' -File "' + $dispatcher + '" -LauncherArgumentsHex 202d2d7368656c6c2d6d6f6465')) 'Stable binding preserves the exact shell-mode suffix as data.'
    Check (-not $binding.desired.Contains($candidate)) 'Startup command does not pin the candidate executable.'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $stableSnapshot -StartupDispatcher $dispatcher -InstallDirectory $fixture -Apply | Out-Null
    Check ($global:migrationCommand -ceq $binding.desired) 'Stable dispatcher command is independently confirmed after migration.'
    & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $stableSnapshot -StartupDispatcher $dispatcher -InstallDirectory $fixture -Verify | Out-Null
    $global:migrationCommand = $old
    Set-Content -LiteralPath $module -Value '# changed module'
    Throws { & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $stableSnapshot -StartupDispatcher $dispatcher -InstallDirectory $fixture -Apply } 'Changed dispatcher module fails before mutation.'
    Set-Content -LiteralPath $module -Value '# owned module contract fixture'
    Set-Content -LiteralPath $dispatcher -Value '# changed dispatcher'
    Throws { & $script -LauncherPath $candidate -ExpectedAccount $sid -SnapshotPath $stableSnapshot -StartupDispatcher $dispatcher -InstallDirectory $fixture -Apply } 'Changed dispatcher fails before mutation.'
    $generation = Join-Path $fixture ('Releases\' + ('a' * 32))
    New-Item -ItemType Directory -Path $generation -Force | Out-Null
    $pinned = Join-Path $generation 'ApplicationQuickDial.exe'
    Copy-Item -LiteralPath $candidate -Destination $pinned
    Throws { & $script -LauncherPath $pinned -ExpectedAccount $sid -ExpectedCommand $old -SnapshotPath (Join-Path $fixture 'pinned.json') } 'Direct immutable-generation startup bindings are rejected.'
    Write-Output 'Launcher startup migration fixtures passed; no registry or live process was changed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'LauncherStartupFixtures-*') { throw 'Unsafe fixture cleanup target.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
