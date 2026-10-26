param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) {
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) { $cmake = Get-Item -LiteralPath $candidate }
        }
    }
}
if (-not $cmake) {
    throw 'CMake and the C++ tools were not found. See docs/REQUIRED_DOWNLOADS.md.'
}
$cmakePath = if ($cmake.Source) { $cmake.Source } else { $cmake.FullName }
& $cmakePath -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmakePath --build (Join-Path $PSScriptRoot 'build') --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$ctest = Join-Path (Split-Path $cmakePath) 'ctest.exe'
& $ctest --test-dir (Join-Path $PSScriptRoot 'build') -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
Write-Host "Executable: $PSScriptRoot\build\$Configuration\DataCopy.exe"
