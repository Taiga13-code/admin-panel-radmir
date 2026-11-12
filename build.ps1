param(
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$buildDir = Join-Path $root "build-asi"

cmake -S $root -B $buildDir -A Win32
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

cmake --build $buildDir --config $BuildType --parallel 1
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$asiPath = Join-Path $buildDir "$BuildType\ReportPanel.asi"
if (Test-Path -LiteralPath $asiPath) {
    Write-Host "ASI plugin built: $asiPath"
}
