param([Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$')][string]$Version, [string]$Generator = 'Visual Studio 17 2022')
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build/package'
& cmake --fresh -S $PSScriptRoot -B $build -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
$output = Join-Path $PSScriptRoot 'build/releases'
New-Item -ItemType Directory -Force $output | Out-Null
$name = "ApplicationQuickDial-$Version-win-x64"
$zip = Join-Path $output "$name.zip"
if (Test-Path -LiteralPath $zip) { throw "Release already exists: $zip" }
$stage = Join-Path $output ([Guid]::NewGuid().ToString())
$folder = Join-Path $stage $name
New-Item -ItemType Directory -Force $folder | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Release/ApplicationQuickDial.exe') -Destination $folder
foreach ($file in @('README.md', 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $folder
}
Compress-Archive -LiteralPath $folder -DestinationPath $zip
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $name.zip" -Encoding ascii
Write-Output "Packaged $zip"
