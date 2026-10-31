param([string]$EngineRoot = '')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
if (-not $EngineRoot) { $EngineRoot=Join-Path $projectRoot '.deps\patched-engine' }
$infoPath=Join-Path $EngineRoot 'engine-build-info.json'
if (-not (Test-Path -LiteralPath $infoPath)) { throw 'Build the patched engine first: scripts\build-livekit-engine.ps1' }
$info=Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
$patch=Join-Path $projectRoot 'patches\livekit-rust-a6f93a16-shutdown.patch'
if ((Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant() -ne $info.patch_sha256) { throw 'Engine is stale: rebuild it after changing the source patch.' }
$installed=Join-Path $projectRoot '.deps\livekit-patched'
foreach($file in $info.files.PSObject.Properties) {
    $sourceFile=Join-Path $EngineRoot $file.Name
    if ((Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value) { throw "Engine artifact checksum mismatch: $($file.Name)" }
    $destination=Join-Path $installed $file.Name
    New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
    # An identical loaded DLL does not need replacing during test-only rebuilds.
    if (-not (Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value) {
        Copy-Item -LiteralPath $sourceFile -Destination $destination -Force
    }
}
Copy-Item -LiteralPath $infoPath -Destination "$installed\engine-build-info.json" -Force
