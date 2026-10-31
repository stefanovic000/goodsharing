param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$source=Join-Path $root '.deps\sources\rust-a6f93a16\rust-sdks-a6f93a16'
$destination=Join-Path $root '.deps\audio-delay-experiment'
if (Test-Path -LiteralPath $destination) { throw 'Refusing to overwrite an experimental runtime.' }
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$sdk=Get-Content -LiteralPath "$root\.deps\livekit-patched\engine-build-info.json" -Raw | ConvertFrom-Json
$marker=Get-Content -LiteralPath "$source\.goodsharing-close-patched" -Raw | ConvertFrom-Json
$native=Join-Path $source 'webrtc-sys\src\goodsharing_channel_receive.cpp'
$original=[IO.File]::ReadAllBytes($native)
$originalHash=Hash $native
if ($originalHash -ne $marker.source_sha256.'webrtc-sys/src/goodsharing_channel_receive.cpp') { throw 'Unexpected receiver source.' }
if ((Hash "$root\.deps\webrtc-89d790b-cryptor\lib\webrtc.lib") -ne $sdk.webrtc_without_original_cryptor_sha256) { throw 'Unexpected WebRTC archive.' }
if ((Hash "$root\.deps\livekit-patched\bin\livekit_ffi.dll") -ne $sdk.files.'bin/livekit_ffi.dll') { throw 'Unexpected installed engine.' }
$before=[Text.Encoding]::UTF8.GetString($original)
$needle='  config.min_delay_ms = jitter_buffer_min_delay_ms;'
if (($before.Split($needle)).Count -ne 2) { throw 'Unexpected NetEq configuration context.' }
$addition=@'
  // Isolated local diagnostic runtime only; never installed into the SDK.
  if (std::getenv("GOODSHARING_TEST_AV_TRACE")) {
    const char* value = std::getenv("GOODSHARING_TEST_NETEQ_MAX_DELAY_MS");
    if (value && std::string(value) == "40") config.max_delay_ms = 40;
    std::fprintf(stderr, "GoodSharing audio delay experiment: max_ms=%d\n", config.max_delay_ms);
  }
'@
$after=$before.Replace('#include <algorithm>',"#include <algorithm>`n#include <cstdio>`n#include <cstdlib>").Replace($needle,"$needle`n$addition")
New-Item -ItemType Directory -Path "$destination\client" -Force | Out-Null
[IO.File]::WriteAllBytes("$destination\original-channel-receive.cpp",$original)
[IO.File]::WriteAllText("$destination\experimental-channel-receive.cpp",$after,[Text.UTF8Encoding]::new($false))
$env:RUSTUP_HOME=Join-Path $root '.tools\rustup'
$env:CARGO_HOME=Join-Path $root '.tools\cargo'
$env:RUSTUP_TOOLCHAIN='1.97.1'
$env:CARGO_TARGET_DIR=Join-Path $root '.deps\build-livekit-engine'
$env:LK_CUSTOM_WEBRTC=Join-Path $root '.deps\webrtc-89d790b-cryptor'
$env:PROTOC=Join-Path $root '.tools\protoc\bin\protoc.exe'
$env:LIBCLANG_PATH=Join-Path $root '.tools\libclang\clang\native'
$vsRoot=& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$lines=& cmd.exe /d /s /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed.' }
foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
$env:PATH="$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:CARGO_HOME\bin;$env:PATH"
try {
    [IO.File]::WriteAllText($native,$after,[Text.UTF8Encoding]::new($false))
    Push-Location $source
    try {
        & "$env:CARGO_HOME\bin\cargo.exe" build --offline --locked --release --package livekit-ffi --jobs 2
        if ($LASTEXITCODE -ne 0) { throw 'Experimental engine build failed.' }
    } finally { Pop-Location }
    Get-ChildItem -LiteralPath "$root\build\bin" | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination "$destination\client" -Recurse }
    Copy-Item -LiteralPath "$env:CARGO_TARGET_DIR\release\livekit_ffi.dll" -Destination "$destination\client\livekit_ffi.dll" -Force
} finally {
    [IO.File]::WriteAllBytes($native,$original)
    if ((Hash $native) -ne $originalHash) { throw 'Receiver source restoration failed.' }
}
if ((Hash "$root\.deps\livekit-patched\bin\livekit_ffi.dll") -ne $sdk.files.'bin/livekit_ffi.dll') { throw 'Installed SDK was modified.' }
$info=[ordered]@{experimental_only=$true; installed_into_sdk=$false; source_restored=$true; original_receiver_sha256=$originalHash;
    experimental_receiver_sha256=(Hash "$destination\experimental-channel-receive.cpp"); script_sha256=(Hash $PSCommandPath);
    baseline_engine_sha256=$sdk.files.'bin/livekit_ffi.dll'; experimental_engine_sha256=(Hash "$destination\client\livekit_ffi.dll");
    app_sha256=(Hash "$destination\client\GoodSharing.exe"); baseline_engine_build=$sdk;
    build_flags=@('--offline','--locked','--release','--package','livekit-ffi','--jobs','2')}
$info | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$destination\build-info.json" -Encoding utf8
Write-Output 'Isolated audio-delay engine built; prepared source restored and installed SDK unchanged.'
