param([string]$Python = 'C:\Users\Hello\MiniConda3\python.exe', [switch]$Smoke)
$ErrorActionPreference = 'Stop'
$demoRoot = Split-Path -Parent $PSScriptRoot
$demoRunner = Join-Path $PSScriptRoot 'run_local_demo.py'
if (-not (Test-Path -LiteralPath (Join-Path $demoRoot 'build\bin\GoodSharing.exe'))) { throw 'Build GoodSharing with scripts/build.ps1 first.' }
if ($Smoke) {
    & $Python $demoRunner --smoke
    exit $LASTEXITCODE
}
$demoLogName = 'local-demo-launch-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
$demoOutput = Join-Path $demoRoot ('artifacts\' + $demoLogName + '.log')
$demoErrors = Join-Path $demoRoot ('artifacts\' + $demoLogName + '-errors.log')
$demoHelper = Start-Process -FilePath $Python -ArgumentList @('"' + $demoRunner + '"') -WorkingDirectory $demoRoot -WindowStyle Hidden -RedirectStandardOutput $demoOutput -RedirectStandardError $demoErrors -PassThru
Start-Sleep -Milliseconds 800
$demoHelper.Refresh()
if ($demoHelper.HasExited -and $demoHelper.ExitCode -ne 0) { throw "Local demo could not start. Read $demoOutput and $demoErrors" }
Write-Output "Opening two local clients. No capture starts automatically."
Write-Output "Close both windows to stop this demo's local server. Log: $demoOutput"
