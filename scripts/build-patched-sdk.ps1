param([string]$Python='python', [switch]$TestShutdown, [switch]$TestTransport)
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'build-livekit-wrapper.ps1') -Python $Python -DeferEngineInstall
& (Join-Path $PSScriptRoot 'build-livekit-engine.ps1') -Python $Python -TestShutdown:$TestShutdown -TestTransport:$TestTransport
& $Python (Join-Path $PSScriptRoot 'verify_livekit_sdk.py')
if ($LASTEXITCODE -ne 0) { throw 'Patched SDK verification failed.' }
