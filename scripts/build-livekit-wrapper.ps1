param([string]$Python = 'C:\Users\Hello\MiniConda3\python.exe', [switch]$DeferEngineInstall)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sources = Join-Path $projectRoot '.deps\sources'
New-Item -ItemType Directory -Force $sources | Out-Null
$archives = @(
    @{ Name='livekit-cpp-80130a2.zip'; Url='https://codeload.github.com/livekit/client-sdk-cpp/zip/80130a2'; Hash='afd41a64247fb6faf76e6d4e4e63e7697eb998061dbe46bfb521796706e45bf4'; Folder='cpp-80130a2' },
    @{ Name='livekit-rust-a6f93a16.zip'; Url='https://codeload.github.com/livekit/rust-sdks/zip/a6f93a16'; Hash='d361ab5562481869f0f1aeaa588bd811ab8a26abeb96fedde5000b9d783c05e0'; Folder='rust-a6f93a16' }
)
foreach ($item in $archives) {
    $archive = Join-Path $sources $item.Name
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $item.Url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.Hash) { throw "Source hash mismatch: $($item.Name)" }
    $destination = Join-Path $sources $item.Folder
    if (-not (Test-Path -LiteralPath $destination)) { Expand-Archive -LiteralPath $archive -DestinationPath $destination }
}
& $Python (Join-Path $PSScriptRoot 'prepare_livekit_wrapper.py')
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the pinned wrapper patch.' }
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC C++ tools were not found.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & cmd.exe /d /s /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize MSVC.' }
foreach ($line in $environmentLines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$cppRoot = Join-Path $sources 'cpp-80130a2\client-sdk-cpp-80130a2'
$buildRoot = Join-Path $projectRoot '.deps\build-livekit-wrapper'
& $cmake -S $cppRoot -B $buildRoot -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" '-DCMAKE_BUILD_TYPE=Release' '-DLIVEKIT_VERSION=1.12.3' '-DLIVEKIT_USE_VCPKG=OFF' '-Dprotobuf_MSVC_STATIC_RUNTIME=OFF' '-DLIVEKIT_BUILD_TESTS=OFF' '-DLIVEKIT_BUILD_EXAMPLES=OFF' "-DCMAKE_INSTALL_PREFIX=$projectRoot\.deps\livekit-patched"
if ($LASTEXITCODE -ne 0) { throw 'Wrapper configuration failed.' }
& $cmake --build $buildRoot --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Wrapper build failed.' }
& $cmake --install $buildRoot
if ($LASTEXITCODE -ne 0) { throw 'Wrapper installation failed.' }
$installed=Join-Path $projectRoot '.deps\livekit-patched'
$wrapperInfo=Get-Content -LiteralPath (Join-Path $cppRoot '.goodsharing-wrapper-patched') -Raw | ConvertFrom-Json
$wrapperFiles=@{}
foreach($relative in @('bin/livekit.dll','lib/livekit.lib','include/livekit/audio_source.h','include/livekit/audio_stream.h','include/livekit/room.h','include/livekit/room_event_types.h','include/livekit/remote_track_publication.h','include/livekit/video_stream.h')) {
    $wrapperFiles[$relative]=(Get-FileHash -LiteralPath (Join-Path $installed $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$wrapperInfo | Add-Member -NotePropertyName files -NotePropertyValue $wrapperFiles
$wrapperInfo | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$installed\wrapper-patch-info.json" -Encoding utf8
if (-not $DeferEngineInstall -and (Test-Path -LiteralPath "$projectRoot\.deps\patched-engine\engine-build-info.json")) {
    & (Join-Path $PSScriptRoot 'install-patched-engine.ps1')
}
