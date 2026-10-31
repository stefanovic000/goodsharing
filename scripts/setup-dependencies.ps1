param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$downloads = Join-Path $projectRoot '.deps\downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
function Get-VerifiedArchive($dependency, [string]$filename) {
    $archive = Join-Path $downloads $filename
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri ($dependency.url + '?download=1') -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dependency.sha256) {
        throw "Checksum mismatch for $filename. The archive was left in place for inspection."
    }
    return $archive
}
$sdk = Join-Path $projectRoot '.deps\livekit-1.12.3'
if (-not (Test-Path -LiteralPath "$sdk\livekit-sdk-windows-x64-1.12.3\include\livekit\livekit.h")) {
    $archive = Get-VerifiedArchive $manifest.livekit_cpp 'livekit-sdk-windows-x64-1.12.3.zip'
    Expand-Archive -LiteralPath $archive -DestinationPath $sdk
}
$server = Join-Path $projectRoot '.deps\livekit-server-1.13.10'
if (-not (Test-Path -LiteralPath "$server\livekit-server.exe")) {
    $archive = Get-VerifiedArchive $manifest.local_livekit_server 'livekit_1.13.10_windows_amd64.zip'
    Expand-Archive -LiteralPath $archive -DestinationPath $server
}
$qt = Join-Path $projectRoot '.deps\qt'
if (-not (Test-Path -LiteralPath "$qt\6.8.3\msvc2022_64\bin\Qt6Core.dll")) {
    $pythonPackages = Join-Path $projectRoot '.tools\python'
    & $Python -m pip install --disable-pip-version-check --target $pythonPackages 'aqtinstall==3.3.0'
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the local Qt downloader.' }
    $previousPythonPath = $env:PYTHONPATH
    try {
        $env:PYTHONPATH = $pythonPackages
        & $Python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O $qt -m qtmultimedia --archives qtbase qtdeclarative qtshadertools qtsvg qttools
        if ($LASTEXITCODE -ne 0) { throw 'Qt installation failed.' }
    } finally { $env:PYTHONPATH = $previousPythonPath }
}
Write-Output 'Release dependencies are present. Build the patched SDK with scripts\build-patched-sdk.ps1, then the app with scripts\build.ps1.'
