param([string]$Python = 'C:\Users\Hello\MiniConda3\python.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$go = 'C:\Program Files\Go\bin\go.exe'
$goVersion = & $go version
if ($goVersion -ne 'go version go1.26.1 windows/amd64') { throw 'Use the recorded Go 1.26.1 Windows toolchain.' }
& $Python (Join-Path $PSScriptRoot 'prepare_clock_server.py')
if ($LASTEXITCODE -ne 0) { throw 'SFU source preparation failed.' }
$env:GOTOOLCHAIN = 'local'
$env:GOPATH = Join-Path $projectRoot '.deps\go-clock-cache\path'
$env:GOMODCACHE = Join-Path $projectRoot '.deps\go-clock-cache\modules'
$env:GOCACHE = Join-Path $projectRoot '.deps\go-clock-cache\build'
$env:GOPROXY = 'https://proxy.golang.org'
$env:GOSUMDB = 'sum.golang.org'
$env:GOMAXPROCS = '2'
$env:GOFLAGS = '-mod=readonly'
$env:GOOS = 'windows'
$env:GOARCH = 'amd64'
$env:CGO_ENABLED = '0'
$source = Join-Path $projectRoot '.deps\sources\livekit-server-clock\livekit-1.13.10'
$output = Join-Path $projectRoot '.deps\livekit-server-clock-experiment'
New-Item -ItemType Directory -Force $output | Out-Null
Push-Location $source
try {
    $testOutput = @(& $go test -mod=readonly -p 2 ./pkg/sfu/rtpstats -count=1 -v)
    $testExit = $LASTEXITCODE
    $testOutput | ForEach-Object { Write-Output $_ }
    if ($testExit -ne 0 -or @($testOutput | Select-String '^--- PASS: Test_').Count -ne 3) { throw 'Expected all three upstream RTP statistics tests to pass.' }
    & $go build -mod=readonly -p 2 -trimpath -buildvcs=false -o "$output\livekit-server.exe" ./cmd/server
    if ($LASTEXITCODE -ne 0) { throw 'SFU experiment build failed.' }
} finally { Pop-Location }
& $Python (Join-Path $PSScriptRoot 'verify_clock_server.py') --record-build
if ($LASTEXITCODE -ne 0) { throw 'SFU experiment provenance verification failed.' }
