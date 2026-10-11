[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ApplicationJson,
    [string]$LauncherPath = (Join-Path $PSScriptRoot 'ApplicationQuickDial.exe'),
    [string]$ReportDirectory,
    [switch]$VerifyOnly,
    [switch]$NativeWorker,
    [string]$ExpectedAccount,
    [string]$WorkerReport
)
$ErrorActionPreference = 'Stop'

function Quote-Argument([string]$Value) {
    if ($Value.Contains('"') -or $Value.EndsWith('\')) { throw 'Paths containing quotes or ending in a slash are unsupported.' }
    return '"' + $Value + '"'
}

if ($NativeWorker) {
    try {
        $account = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        if ($account -ne $ExpectedAccount) { throw 'Registration worker is running as a different account.' }
        $mode = if ($VerifyOnly) { '--catalog-status' } else { '--register-app' }
        $arguments = @($mode, (Quote-Argument $ApplicationJson), '--account', $ExpectedAccount,
            '--output', (Quote-Argument $WorkerReport)) -join ' '
        $process = Start-Process -FilePath $LauncherPath -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $nativeHandle = $process.Handle
        if (-not $process.WaitForExit(20000)) { $process.Kill(); throw 'Launcher catalog operation timed out.' }
        if ($process.ExitCode -ne 0) {
            if (Test-Path -LiteralPath $WorkerReport) {
                $failure = Get-Content -LiteralPath $WorkerReport -Raw | ConvertFrom-Json
                throw $failure.error
            }
            throw 'Launcher did not complete catalog registration. Use version 0.2.0 or newer.'
        }
        $report = Get-Content -LiteralPath $WorkerReport -Raw | ConvertFrom-Json
        $report | Add-Member -NotePropertyName catalogSha256 -NotePropertyValue (Get-FileHash -LiteralPath $report.catalogPath -Algorithm SHA256).Hash
        $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $WorkerReport -Encoding UTF8
        exit 0
    } catch {
        @{ success = $false; error = $_.Exception.Message } | ConvertTo-Json | Set-Content -LiteralPath $WorkerReport -Encoding UTF8
        exit 1
    }
}

$ApplicationJson = (Resolve-Path -LiteralPath $ApplicationJson).Path
$LauncherPath = (Resolve-Path -LiteralPath $LauncherPath).Path
$account = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
if (-not $ReportDirectory) {
    $ReportDirectory = Join-Path (Split-Path -Parent $ApplicationJson) ('out/launcher-registration-' + [Guid]::NewGuid().ToString('N'))
}
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
foreach ($name in @('application.json', 'registration.json', 'verification.json')) {
    if (Test-Path -LiteralPath (Join-Path $ReportDirectory $name)) { throw 'Use a fresh report directory; existing registration evidence will not be overwritten.' }
}
$request = Join-Path $ReportDirectory 'application.json'
# Snapshot the intended request once; both ordinary Windows processes read it.
Copy-Item -LiteralPath $ApplicationJson -Destination $request
$entry = Get-Content -LiteralPath $request -Raw | ConvertFrom-Json
if (-not $entry.id -or -not $entry.name -or -not $entry.target) { throw 'Application JSON requires id, name, and target.' }
$worker = Join-Path $ReportDirectory 'registration.json'
$verification = Join-Path $ReportDirectory 'verification.json'

function Invoke-NativeCatalog([string]$Output, [bool]$ReadOnly) {
    $powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $parts = @((Quote-Argument $powershell), '-NoProfile', '-ExecutionPolicy Bypass', '-File',
        (Quote-Argument $PSCommandPath), '-NativeWorker', '-ApplicationJson', (Quote-Argument $request),
        '-LauncherPath', (Quote-Argument $LauncherPath), '-ExpectedAccount', $account,
        '-WorkerReport', (Quote-Argument $Output))
    if ($ReadOnly) { $parts += '-VerifyOnly' }
    $startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = ($parts -join ' '); ProcessStartupInformation = $startup
    }
    if ($created.ReturnValue -ne 0) { throw "Could not create the ordinary Windows catalog process ($($created.ReturnValue))." }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 100
        $running = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($created.ProcessId)" -ErrorAction Stop
    } while ($running -and [DateTime]::UtcNow -lt $deadline)
    if ($running) { throw 'Native catalog process timed out; inspect its report before retrying.' }
    if (-not (Test-Path -LiteralPath $Output)) { throw 'The native process produced no report. No registration success is established.' }
    $result = Get-Content -LiteralPath $Output -Raw | ConvertFrom-Json
    if (-not $result.success) { throw $result.error }
    if ($result.account -ne $account) { throw 'Native report account mismatch.' }
    return $result
}

$registered = Invoke-NativeCatalog $worker ([bool]$VerifyOnly)
$verified = Invoke-NativeCatalog $verification $true
if ($registered.catalogPath -ne $verified.catalogPath -or $registered.catalogSha256 -ne $verified.catalogSha256 -or
    ($registered.entry | ConvertTo-Json -Depth 100 -Compress) -ne ($verified.entry | ConvertTo-Json -Depth 100 -Compress)) {
    throw 'Independent native verification disagrees. Inspect both reports; registration has not been confirmed.'
}
[PSCustomObject]@{
    Verified = $true
    CatalogPath = $verified.catalogPath
    CatalogSha256 = $verified.catalogSha256
    Entry = $verified.entry
    BackupPath = $registered.backupPath
    Reports = $ReportDirectory
}
