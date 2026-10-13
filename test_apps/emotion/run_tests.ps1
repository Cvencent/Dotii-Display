# Host unit-test runner for the Dotii emotion engine.
# Usage: powershell -File test_apps\emotion\run_tests.ps1
# Requires a host C compiler. Set $env:CC to override; zig cc is used by default.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$cc = $env:CC
if (-not $cc) {
    $zig = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\Lib\site-packages\ziglang\zig.exe'
    if (Test-Path $zig) {
        $cc = "$zig cc"
    } else {
        $cc = 'cc'
    }
}

$outDir = Join-Path $root '.codx\emotion-tests'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$inc = 'main\emotion'
$tests = @(
    @{ name = 'emotion_catalog'; src = 'test_apps\emotion\test_emotion_catalog.c'; libs = @('main\emotion\emotion_catalog.c') },
    @{ name = 'animation';       src = 'test_apps\emotion\test_animation.c';       libs = @('main\emotion\animation.c') },
    @{ name = 'emotion_engine';  src = 'test_apps\emotion\test_emotion_engine.c';  libs = @('main\emotion\emotion_engine.c', 'main\emotion\emotion_catalog.c') },
    @{ name = 'character_pack';  src = 'test_apps\emotion\test_character_pack.c';  libs = @('main\emotion\character_pack.c') },
    @{ name = 'emotion_router';  src = 'test_apps\emotion\test_emotion_router.c';  libs = @('main\emotion\emotion_router.c', 'main\emotion\emotion_engine.c', 'main\emotion\emotion_catalog.c') }
    @{ name = 'emotion_pose';    src = 'test_apps\emotion\test_emotion_pose.c';    libs = @('main\emotion\emotion_pose.c', 'main\emotion\emotion_engine.c', 'main\emotion\emotion_catalog.c', 'main\emotion\character_pack.c', 'main\emotion\animation.c') }
)

$failed = 0
foreach ($t in $tests) {
    if (-not (Test-Path $t.src)) { continue }
    $exe = Join-Path $outDir ("{0}.exe" -f $t.name)
    $cmd = "$cc -std=c11 -Wall -Wextra -I $inc $($t.src) $($t.libs -join ' ') -o $exe"
    Write-Host "== $($t.name) =="
    cmd /c "$cmd" 2>&1 | Out-String | Write-Host
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed -ne 0) { Write-Error "$failed test binary/binaries failed"; exit 1 }
Write-Host 'all emotion host tests passed'
