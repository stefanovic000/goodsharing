$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qt = Join-Path $projectRoot '.deps\qt\6.8.3\msvc2022_64'
$exe = Join-Path $projectRoot 'build\bin\GoodSharing.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build the app first with scripts\build.ps1.' }
$env:PATH = "$qt\bin;$env:PATH"
$env:QML_IMPORT_PATH = "$qt\qml"
& $exe
