param(
    [switch]$SkipBuild,
    [switch]$SkipZip,
    [string]$Python = 'C:\Users\Hello\MiniConda3\python.exe'
)
# Builds a portable GoodSharing package: application, Qt runtime and QML modules,
# LiveKit SDK libraries, notices and a version manifest. No installer is created;
# the folder is self-contained and the optional ZIP is a distribution artifact.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$qt = Join-Path $root '.deps\qt\6.8.3\msvc2022_64'
$sdk = Join-Path $root '.deps\livekit-patched'
$windeployqt = Join-Path $qt 'bin\windeployqt.exe'
$exe = Join-Path $root 'build\bin\GoodSharing.exe'
$version = '0.1.0'

if (-not $SkipBuild) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -SkipConfigure
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing build output: $exe" }

$distRoot = Join-Path $root 'packaging\dist'
$folder = Join-Path $distRoot "GoodSharing-$version-win64"
if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
New-Item -ItemType Directory -Path $folder -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'GoodSharing.exe')

# Deploy Qt runtime, plugins and QML modules next to the executable.
$env:PATH = (Join-Path $qt 'bin') + ';' + (Join-Path $sdk 'bin') + ';' + $env:PATH
& $windeployqt '--release' '--no-translations' '--qmldir' (Join-Path $root 'ui') (Join-Path $folder 'GoodSharing.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }

foreach ($name in 'livekit.dll', 'livekit_ffi.dll') {
    $source = Join-Path $sdk "bin\$name"
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing SDK library: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $folder $name)
}
Copy-Item -LiteralPath (Join-Path $root 'packaging\README.txt') -Destination $folder
Copy-Item -LiteralPath (Join-Path $root 'packaging\THIRD-PARTY-NOTICES.txt') -Destination $folder

$files = Get-ChildItem -LiteralPath $folder -Recurse -File | Sort-Object FullName
$manifest = [ordered]@{
    product = 'GoodSharing'
    version = $version
    platform = 'Windows x64 (MSVC)'
    built_utc = [DateTime]::UtcNow.ToString('o')
    app_sha256 = (Get-FileHash -LiteralPath (Join-Path $folder 'GoodSharing.exe') -Algorithm SHA256).Hash
    livekit_sha256 = (Get-FileHash -LiteralPath (Join-Path $folder 'livekit.dll') -Algorithm SHA256).Hash
    livekit_ffi_sha256 = (Get-FileHash -LiteralPath (Join-Path $folder 'livekit_ffi.dll') -Algorithm SHA256).Hash
    qt_version = (Get-Item (Join-Path $qt 'bin\Qt6Core.dll')).VersionInfo.FileVersion
    files = @($files | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($folder.Length + 1).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    })
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $folder 'version.json') -Encoding utf8

$total = ($files | Measure-Object Length -Sum).Sum
"package: $folder"
"files: {0}, size: {1:N1} MB, app sha256: {2}" -f $files.Count, ($total / 1MB), $manifest.app_sha256

if (-not $SkipZip) {
    $zip = Join-Path $distRoot "GoodSharing-$version-win64.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip -CompressionLevel Optimal
    "zip: $zip ({0:N1} MB)" -f ((Get-Item -LiteralPath $zip).Length / 1MB)
}
