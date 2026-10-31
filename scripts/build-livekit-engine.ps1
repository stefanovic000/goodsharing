param([string]$Python = 'C:\Users\Hello\MiniConda3\python.exe', [switch]$TestShutdown, [switch]$TestTransport)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$source=Join-Path $projectRoot '.deps\sources\rust-a6f93a16\rust-sdks-a6f93a16'
if (-not (Test-Path -LiteralPath "$source\Cargo.lock")) { throw 'Prepare pinned sources with build-livekit-wrapper.ps1 first.' }
if ((Get-FileHash -LiteralPath "$source\Cargo.lock" -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'dad2c0595fa985573849d2ade041093c58e88edf64abe2721d92a62f42482d3e') { throw 'Pinned Cargo.lock checksum mismatch.' }
& $Python (Join-Path $PSScriptRoot 'prepare_livekit_engine.py')
if ($LASTEXITCODE -ne 0) { throw 'Engine patch preparation failed.' }
$env:RUSTUP_HOME=Join-Path $projectRoot '.tools\rustup'
$env:CARGO_HOME=Join-Path $projectRoot '.tools\cargo'
$env:RUSTUP_TOOLCHAIN='1.97.1'
$cargo=Join-Path $env:CARGO_HOME 'bin\cargo.exe'
if (-not (Test-Path -LiteralPath $cargo)) {
    $installer=Join-Path $projectRoot '.tools\rust-installer\rustup-init.exe'
    New-Item -ItemType Directory -Force (Split-Path -Parent $installer) | Out-Null
    Invoke-WebRequest -Uri 'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe' -OutFile $installer
    & $installer -y --no-modify-path --profile minimal --default-toolchain 1.97.1
    if ($LASTEXITCODE -ne 0) { throw 'Project-local Rust installation failed.' }
}
function Get-CheckedBuildArchive([string]$Name,[string]$Url,[string]$Hash,[string]$Destination) {
    $archive=Join-Path $projectRoot ".deps\sources\$Name"
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $Url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) { throw "Build dependency hash mismatch: $Name" }
    if (-not (Test-Path -LiteralPath $Destination)) { Expand-Archive -LiteralPath $archive -DestinationPath $Destination }
}
Get-CheckedBuildArchive 'protoc-25.3-win64.zip' 'https://github.com/protocolbuffers/protobuf/releases/download/v25.3/protoc-25.3-win64.zip' 'd6b336b852726364313330631656b7f395dde5b1141b169f5c4b8d43cdf01482' (Join-Path $projectRoot '.tools\protoc')
Get-CheckedBuildArchive 'webrtc-win-x64-release.zip' 'https://github.com/livekit/rust-sdks/releases/download/webrtc-89d790b/webrtc-win-x64-release.zip' '5c2349c960bae4f06f71c102f58552d0d09c912a142811acfcadfb7c883cbf58' (Join-Path $projectRoot '.deps\webrtc-89d790b')
$libyuvArchive=Join-Path $projectRoot '.deps\sources\libyuv-91727608.tar.gz'
if (-not (Test-Path -LiteralPath $libyuvArchive)) { Invoke-WebRequest -Uri 'https://chromium.googlesource.com/libyuv/libyuv/+archive/917276084a49be726c90292ff0a6b0a3d571a6af.tar.gz' -OutFile $libyuvArchive }
if ((Get-FileHash -LiteralPath $libyuvArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'f49388bdc38853e32c2f4d8b3f67339faf2f722a653ae5701181a83eeeaad057') { throw 'Pinned libyuv checksum mismatch.' }
if (-not (Test-Path -LiteralPath "$source\yuv-sys\libyuv\include\libyuv.h")) {
    & tar -xzf $libyuvArchive -C "$source\yuv-sys\libyuv"
    if ($LASTEXITCODE -ne 0) { throw 'Pinned libyuv extraction failed.' }
}
$webrtc=Get-ChildItem (Join-Path $projectRoot '.deps\webrtc-89d790b') -Filter 'LICENSE.md' -Recurse | Select-Object -First 1
if (-not $webrtc) { throw 'Pinned WebRTC build is incomplete.' }
$env:LK_CUSTOM_WEBRTC=$webrtc.DirectoryName
$env:PROTOC=Join-Path $projectRoot '.tools\protoc\bin\protoc.exe'
$clangWheel=Join-Path $projectRoot '.deps\sources\libclang-18.1.1-py2.py3-none-win_amd64.whl'
if (-not (Test-Path -LiteralPath $clangWheel)) {
    & $Python -m pip download --only-binary=:all: --no-deps libclang==18.1.1 --dest (Join-Path $projectRoot '.deps\sources')
    if ($LASTEXITCODE -ne 0) { throw 'Could not download pinned libclang.' }
}
if ((Get-FileHash -LiteralPath $clangWheel -Algorithm SHA256).Hash.ToLowerInvariant() -ne '4dd2d3b82fab35e2bf9ca717d7b63ac990a3519c7e312f19fa8e86dcc712f7fb') { throw 'libclang checksum mismatch.' }
$clangRoot=Join-Path $projectRoot '.tools\libclang'
if (-not (Test-Path -LiteralPath "$clangRoot\clang\native\libclang.dll")) {
    & $Python -m pip install --no-deps --no-index --target $clangRoot $clangWheel
    if ($LASTEXITCODE -ne 0) { throw 'Project-local libclang installation failed.' }
}
$env:LIBCLANG_PATH=Join-Path $clangRoot 'clang\native'
$env:CARGO_TARGET_DIR=Join-Path $projectRoot '.deps\build-livekit-engine'
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines=& cmd.exe /d /s /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize MSVC.' }
foreach ($line in $environmentLines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
$env:PATH="$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:CARGO_HOME\bin;$env:PATH"
# Rust bundles the complete static WebRTC archive into its rlib. Replace the
# original cryptor object explicitly; never link two competing implementations.
$pristineWebrtc=$webrtc.DirectoryName
$pristineLibrary=Join-Path $pristineWebrtc 'lib\webrtc.lib'
if ((Get-FileHash -LiteralPath $pristineLibrary -Algorithm SHA256).Hash.ToLowerInvariant() -ne '58352400f57b0a16b45fc5c734d45988205074ffe4e61b5a2afb9f4cb223a10a') { throw 'Pinned WebRTC library checksum mismatch.' }
$patchedWebrtc=Join-Path $projectRoot '.deps\webrtc-89d790b-cryptor'
New-Item -ItemType Directory -Force "$patchedWebrtc\lib" | Out-Null
if (-not (Test-Path -LiteralPath "$patchedWebrtc\include")) { New-Item -ItemType Junction -Path "$patchedWebrtc\include" -Target "$pristineWebrtc\include" | Out-Null }
$resolvedIncludes=Get-Item -LiteralPath "$patchedWebrtc\include"
if ($resolvedIncludes.LinkType -ne 'Junction' -or [IO.Path]::GetFullPath($resolvedIncludes.Target).TrimEnd('\') -ne [IO.Path]::GetFullPath("$pristineWebrtc\include").TrimEnd('\')) { throw 'Unexpected custom WebRTC include target.' }
foreach ($file in @('webrtc.ninja','desktop_capture.ninja','LICENSE.md')) { Copy-Item -LiteralPath "$pristineWebrtc\$file" -Destination "$patchedWebrtc\$file" -Force }
$members=@(& lib.exe /nologo /list $pristineLibrary)
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the pinned WebRTC archive.' }
$cryptorMembers=@($members | Where-Object { $_ -match '(^|[\\/])frame_crypto_transformer\.obj$' })
if ($cryptorMembers.Count -ne 1) { throw 'Expected exactly one original WebRTC cryptor object.' }
$customLibrary=Join-Path $patchedWebrtc 'lib\webrtc.lib'
$clockMembers=@('obj/audio/audio/channel_receive.obj','obj/video/video/rtp_video_stream_receiver2.obj')
foreach($clockMember in $clockMembers) { if (@($members | Where-Object { $_ -eq $clockMember }).Count -ne 1) { throw "Expected one original receiver object: $clockMember" } }
& lib.exe /nologo "/OUT:$customLibrary" "/REMOVE:$($cryptorMembers[0])" "/REMOVE:$($clockMembers[0])" "/REMOVE:$($clockMembers[1])" $pristineLibrary
if ($LASTEXITCODE -ne 0) { throw 'Could not exclude the original WebRTC cryptor object.' }
$remainingMembers=@(& lib.exe /nologo /list $customLibrary)
if ($LASTEXITCODE -ne 0 -or $remainingMembers.Count -ne $members.Count-3 -or @($remainingMembers | Where-Object { $_ -match '(^|[\\/])frame_crypto_transformer\.obj$' -or $_ -in $clockMembers }).Count -ne 0) { throw 'Unexpected WebRTC archive replacement result.' }
$env:LK_CUSTOM_WEBRTC=$patchedWebrtc
Push-Location $source
try {
    if ($TestTransport) {
        & $cargo test --locked --release --package livekit --lib stereo --jobs 2 -- --test-threads=1 --nocapture
        if ($LASTEXITCODE -ne 0) { throw 'Native stereo negotiation regression failed.' }
    }
    if ($TestShutdown) {
        & $cargo test --locked --release --package livekit --lib shutdown_cancels_offer_and_does_not_wait_for_transport_lock --jobs 2 -- --nocapture
        if ($LASTEXITCODE -ne 0) { throw 'Native shutdown regression failed.' }
    }
    & $cargo build --locked --release --package livekit-ffi --jobs 2
    if ($LASTEXITCODE -ne 0) { throw 'Patched engine build failed.' }
} finally { Pop-Location }
$staged=Join-Path $projectRoot '.deps\patched-engine'
New-Item -ItemType Directory -Force "$staged\bin","$staged\lib" | Out-Null
Copy-Item -LiteralPath "$env:CARGO_TARGET_DIR\release\livekit_ffi.dll" -Destination "$staged\bin\livekit_ffi.dll" -Force
Copy-Item -LiteralPath "$env:CARGO_TARGET_DIR\release\livekit_ffi.dll.lib" -Destination "$staged\lib\livekit_ffi.dll.lib" -Force
$info=[ordered]@{
    rust_source='a6f93a16'; ffi_version='0.12.82'; rust_toolchain='1.97.1'; profile='release'; target='x86_64-pc-windows-msvc'; cargo_locked=$true
    patch_sha256=(Get-FileHash -LiteralPath "$projectRoot\patches\livekit-rust-a6f93a16-shutdown.patch" -Algorithm SHA256).Hash.ToLowerInvariant()
    patch_inputs_sha256=@{
        'patches/receiver_media_clock.h'=(Get-FileHash -LiteralPath "$projectRoot\patches\receiver_media_clock.h" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/webrtc-89d790b-receiver-clock.patch'=(Get-FileHash -LiteralPath "$projectRoot\patches\webrtc-89d790b-receiver-clock.patch" -Algorithm SHA256).Hash.ToLowerInvariant()
        'scripts/receiver_timing_patch.py'=(Get-FileHash -LiteralPath "$projectRoot\scripts\receiver_timing_patch.py" -Algorithm SHA256).Hash.ToLowerInvariant()
        'scripts/prepare_receiver_clock.py'=(Get-FileHash -LiteralPath "$projectRoot\scripts\prepare_receiver_clock.py" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/source_audio_clock.h'=(Get-FileHash -LiteralPath "$projectRoot\patches\source_audio_clock.h" -Algorithm SHA256).Hash.ToLowerInvariant()
        'scripts/audio_timing_patch.py'=(Get-FileHash -LiteralPath "$projectRoot\scripts\audio_timing_patch.py" -Algorithm SHA256).Hash.ToLowerInvariant()
        'scripts/sender_audio_timing_patch.py'=(Get-FileHash -LiteralPath "$projectRoot\scripts\sender_audio_timing_patch.py" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/preencoded_chain.h'=(Get-FileHash -LiteralPath "$projectRoot\patches\preencoded_chain.h" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/strict_frame_cryptor.h'=(Get-FileHash -LiteralPath "$projectRoot\patches\strict_frame_cryptor.h" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/closed_frame_transformer.h'=(Get-FileHash -LiteralPath "$projectRoot\patches\closed_frame_transformer.h" -Algorithm SHA256).Hash.ToLowerInvariant()
        'patches/webrtc-89d790b-frame-crypto.patch'=(Get-FileHash -LiteralPath "$projectRoot\patches\webrtc-89d790b-frame-crypto.patch" -Algorithm SHA256).Hash.ToLowerInvariant()
        'scripts/prepare_frame_cryptor.py'=(Get-FileHash -LiteralPath "$projectRoot\scripts\prepare_frame_cryptor.py" -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    cargo_lock_sha256=(Get-FileHash -LiteralPath "$source\Cargo.lock" -Algorithm SHA256).Hash.ToLowerInvariant()
    native_shutdown_test_run=[bool]$TestShutdown
    webrtc_original_library_sha256=(Get-FileHash -LiteralPath $pristineLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
    webrtc_without_original_cryptor_sha256=(Get-FileHash -LiteralPath $customLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
    webrtc_removed_cryptor_object=$cryptorMembers[0]
    webrtc_removed_clock_objects=$clockMembers
    repaired_cryptor_source_sha256=(Get-FileHash -LiteralPath "$source\webrtc-sys\src\goodsharing_frame_crypto_transformer.cpp" -Algorithm SHA256).Hash.ToLowerInvariant()
    files=@{
        'bin/livekit_ffi.dll'=(Get-FileHash -LiteralPath "$staged\bin\livekit_ffi.dll" -Algorithm SHA256).Hash.ToLowerInvariant()
        'lib/livekit_ffi.dll.lib'=(Get-FileHash -LiteralPath "$staged\lib\livekit_ffi.dll.lib" -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$info | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$staged\engine-build-info.json" -Encoding utf8
& (Join-Path $PSScriptRoot 'install-patched-engine.ps1') -EngineRoot $staged
