$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$dependency = $manifest.active_livekit_sdk.native_dependencies.libyuv
$revision = $dependency.revision
$sources = Join-Path $projectRoot '.deps\sources'
$archive = Join-Path $sources "libyuv-$($revision.Substring(0,8)).tar.gz"
$target = Join-Path $sources "libyuv-$($revision.Substring(0,8))"
$marker = Join-Path $target 'build-info.json'
New-Item -ItemType Directory -Force $sources | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $dependency.url -OutFile $archive
}
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne $dependency.sha256) {
    throw 'Pinned libyuv checksum mismatch. The archive was left in place for inspection.'
}
# The engine build extracts the same pinned archive into the Rust source tree;
# this copy keeps the application build independent of that intermediate tree.
if (-not (Test-Path -LiteralPath (Join-Path $target 'include\libyuv\scale.h'))) {
    New-Item -ItemType Directory -Force $target | Out-Null
    & tar -xzf $archive -C $target
    if ($LASTEXITCODE -ne 0) { throw 'Pinned libyuv extraction failed.' }
}
foreach ($required in @('include\libyuv\scale.h', 'include\libyuv\scale_argb.h', 'include\libyuv\planar_functions.h',
                        'include\libyuv\convert_from_argb.h', 'source\scale.cc', 'source\row_any.cc',
                        'source\convert_from_argb.cc', 'LICENSE', 'PATENTS')) {
    if (-not (Test-Path -LiteralPath (Join-Path $target $required))) {
        throw "Pinned libyuv extraction is incomplete: $required is missing."
    }
}
$info = [ordered]@{ revision = $revision; url = $dependency.url; archive_sha256 = $hash; extracted = (Get-Date).ToString('s'); consumer = 'GoodSharing capture conversion (single-plane I420 scaling and NV12 canvases)' }
$info | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
Write-Output "Pinned libyuv $revision is present at $target"
