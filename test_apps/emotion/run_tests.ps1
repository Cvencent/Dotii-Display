# Host unit-test runner for the Dotii emotion engine.
# Usage: powershell -File test_apps\emotion\run_tests.ps1
# Requires any C compiler that can target the host. Set $env:CC to override.
# The repo has no bundled host compiler, so zig cc is used by default when present.

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

$tests = @(
    @{ name = 'emotion_catalog'; src = 'test_apps\emotion\test_emotion_catalog.c'; lib = 'main\emotion\emotion_catalog.c' },
    @{ name = 'animation';       src = 'test_apps\emotion\test_animation.c';       lib = 'main\emotion\animation.c' },
    @{ name = 'emotion_engine';  src = 'test_apps\emotion\test_emotion_engine.c';  lib = 'main\emotion\emotion_engine.c' },
    @{ name = 'character_pack';  src = 'test_apps\emotion\test_character_pack.c';  lib = 'main\emotion\character_pack.c' },
    @{ name = 'emotion_router';  src = 'test_apps\emotion\test_emotion_router.c';  lib = 'main\emotion\emotion_router.c' }
)

$failed = 0
foreach ($t in $tests) {
    if (-not (Test-Path $t.src)) { continue }
    $exe = Join-Path $outDir ("{0}.exe" -f $t.name)
    $cmd = "$cc -std=c11 -Wall -Wextra -I main\emotion $($t.src) $($t.lib) -o $exe"
    Write-Host "== $($t.name) =="
    cmd /c "$cmd" 2>&1 | Out-String | Write-Host
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed -ne 0) { Write-Error "$failed test binary/binaries failed"; exit 1 }
Write-Host 'all emotion host tests passed'
