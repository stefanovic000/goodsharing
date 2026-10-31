param([string]$Target = '', [switch]$ConfigureOnly, [switch]$SkipConfigure, [string]$Python = 'C:\Users\Hello\MiniConda3\python.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC C++ tools were not found.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & cmd.exe /d /s /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the MSVC environment.' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$qt = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$sdk = Join-Path $projectRoot '.deps\livekit-patched'
if (-not (Test-Path -LiteralPath "$sdk\include\livekit\audio_stream.h")) { throw 'Build the stereo-capable SDK wrapper first: scripts\build-livekit-wrapper.ps1' }
& $Python (Join-Path $PSScriptRoot 'verify_livekit_sdk.py')
if ($LASTEXITCODE -ne 0) { throw 'SDK verification failed. Rebuild the patched wrapper and engine.' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'prepare-libyuv.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Pinned libyuv preparation failed.' }
$env:PATH = "$qt\bin;$sdk\bin;$env:PATH"
if ($SkipConfigure) {
    if (-not (Test-Path -LiteralPath "$projectRoot\build\CMakeCache.txt")) { throw 'Configure the project before using -SkipConfigure.' }
} else {
    & $cmake -S $projectRoot -B "$projectRoot\build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$qt;$sdk" "-DLiveKit_DIR=$sdk\lib\cmake\LiveKit"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
}
if (-not $ConfigureOnly) {
    $buildArgs = @('--build', "$projectRoot\build", '--parallel', '4')
    if ($Target) { $buildArgs += @('--target', $Target) }
    & $cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    Copy-Item -Path "$sdk\bin\*.dll" -Destination "$projectRoot\build\bin" -Force
}
