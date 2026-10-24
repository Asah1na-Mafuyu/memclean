[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$UiTests
)

$ErrorActionPreference = 'Stop'
if ($SkipTests -and $UiTests) {
    throw 'Choose either -SkipTests or -UiTests, not both.'
}
foreach ($commandName in @('cmake', 'ctest')) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Missing $commandName. Install CMake 3.24 or later and add it to PATH."
    }
}

$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$distributionDirectory = Join-Path $projectRoot 'dist'
$builtExecutable = Join-Path $buildDirectory 'Release\memclean.exe'
$testsEnabled = if ($SkipTests) { 'OFF' } else { 'ON' }

& cmake -S $projectRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DBUILD_TESTING=$testsEnabled"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& cmake --build $buildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw 'Tests failed. Run tests in a normal (non-administrator) terminal.'
    }
}
if ($UiTests) {
    # A child shell isolates the test's Add-Type declarations between repeated builds.
    $shellExecutable = (Get-Process -Id $PID).Path
    & $shellExecutable -NoProfile -File (Join-Path $projectRoot 'tests\ui_smoke.ps1') -ExePath $builtExecutable
    if ($LASTEXITCODE -ne 0) { throw 'UI smoke tests failed.' }
}

New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
$destination = Join-Path $distributionDirectory 'memclean.exe'
Copy-Item -LiteralPath $builtExecutable -Destination $destination -Force
Write-Host "Ready: $destination"
