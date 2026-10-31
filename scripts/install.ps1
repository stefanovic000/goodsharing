param(
    [string]$Package = '',
    [string]$InstallDir = '',
    [switch]$NoShortcut
)
# Installs the built portable package for the current user: no administrator
# rights, no machine-wide state. Uninstall is the generated uninstall.ps1.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $Package) {
    $candidates = Get-ChildItem (Join-Path $root 'packaging\dist') -Directory -Filter 'GoodSharing-*-win64' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    if (-not $candidates) { throw 'No package found. Run scripts/package.ps1 first.' }
    $Package = $candidates[0].FullName
}
if (-not (Test-Path -LiteralPath (Join-Path $Package 'GoodSharing.exe'))) { throw "Not a GoodSharing package: $Package" }
if (-not $InstallDir) { $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\GoodSharing' }

$manifest = Get-Content -LiteralPath (Join-Path $Package 'version.json') -Raw | ConvertFrom-Json
$version = $manifest.version

if (Test-Path -LiteralPath $InstallDir) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
}
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Copy-Item -Path (Join-Path $Package '*') -Destination $InstallDir -Recurse -Force

# Uninstaller lives next to the application.
$uninstall = @'
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\GoodSharing.lnk'
if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\GoodSharing'
if (Test-Path $key) { Remove-Item $key -Recurse -Force }
if (-not $Quiet) { Write-Host "Removing $here" }
Get-ChildItem -LiteralPath $here -Force | Where-Object { $_.FullName -ne $MyInvocation.MyCommand.Path } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $here -Recurse -Force -ErrorAction SilentlyContinue
'@
Set-Content -LiteralPath (Join-Path $InstallDir 'uninstall.ps1') -Value $uninstall -Encoding utf8

if (-not $NoShortcut) {
    $programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    New-Item -ItemType Directory -Path $programs -Force | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path $programs 'GoodSharing.lnk'))
    $link.TargetPath = Join-Path $InstallDir 'GoodSharing.exe'
    $link.WorkingDirectory = $InstallDir
    $link.Description = 'GoodSharing screen sharing client'
    $link.Save()
}

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\GoodSharing'
New-Item -Path $uninstallKey -Force | Out-Null
Set-ItemProperty -Path $uninstallKey -Name DisplayName -Value 'GoodSharing'
Set-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value $version
Set-ItemProperty -Path $uninstallKey -Name Publisher -Value 'GoodSharing'
Set-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $InstallDir
Set-ItemProperty -Path $uninstallKey -Name DisplayIcon -Value (Join-Path $InstallDir 'GoodSharing.exe')
Set-ItemProperty -Path $uninstallKey -Name UninstallString -Value ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f (Join-Path $InstallDir 'uninstall.ps1'))
Set-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -Type DWord

$files = (Get-ChildItem -LiteralPath $InstallDir -Recurse -File | Measure-Object).Count
"installed: $InstallDir"
"version: $version, files: $files"
"shortcut: $(if ($NoShortcut) { 'skipped' } else { Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\GoodSharing.lnk' })"
"uninstall entry: HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\GoodSharing"
