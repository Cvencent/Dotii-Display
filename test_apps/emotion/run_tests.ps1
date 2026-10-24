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
$cjsonInc = 'managed_components\espressif__cjson\cJSON'
$cjsonSrc = 'managed_components\espressif__cjson\cJSON\cJSON.c'
# The firmware links this module against ESP-IDF; the host build compiles the
# same parsing code against these stubs so the mapping stays covered.
$hostStubs = 'test_apps\emotion\host_stubs'
$tests = @(
    @{ name = 'dotii_catalog';  src = 'test_apps\\emotion\\test_dotii_catalog.c';  libs = @('main\\emotion\\dotii_data.c') },
    @{ name = 'dotii_engine';   src = 'test_apps\\emotion\\test_dotii_engine.c';   libs = @('main\\emotion\\dotii_engine.c', 'main\\emotion\\dotii_data.c'); extra = '-lm' },
    @{ name = 'emotion_router'; src = 'test_apps\\emotion\\test_emotion_router.c'; libs = @('main\\emotion\\emotion_router.c', 'main\\emotion\\dotii_engine.c', 'main\\emotion\\dotii_data.c'); extra = '-lm' },
    @{ name = 'emotion_mood';   src = 'test_apps\\emotion\\test_emotion_mood.c';   libs = @('main\\emotion\\emotion_mood.c') },
    @{ name = 'dotii_frames'; src = 'test_apps\\emotion\\test_dotii_frames.c'; libs = @('main\\emotion\\dotii_draw.c', 'main\\emotion\\dotii_engine.c', 'main\\emotion\\dotii_data.c', "$hostStubs\\host_stubs.c"); inc = @("$hostStubs"); extra = '-lm' },
    @{ name = 'xiaozhi_client'; src = 'test_apps\\emotion\\test_xiaozhi_client.c'; libs = @('main\\xiaozhi_client.c', 'main\\emotion\\dotii_data.c', $cjsonSrc, "$hostStubs\\host_stubs.c", "$hostStubs\\device_config.c"); inc = @($hostStubs, 'main', $cjsonInc); extra = '-lm' }
)

$failed = 0
foreach ($t in $tests) {
    if (-not (Test-Path $t.src)) { continue }
    $exe = Join-Path $outDir ("{0}.exe" -f $t.name)
    $extra = ''
    if ($t.inc) { $extra = ($t.inc | ForEach-Object { "-I $_" }) -join ' ' }
    if ($t.extra) { $extra = "$extra $($t.extra)" }
    $cmd = "$cc -std=c11 -Wall -Wextra -I $inc $extra $($t.src) $($t.libs -join ' ') -o $exe"
    Write-Host "== $($t.name) =="
    # Compiler warnings arrive on stderr; only a non-zero exit is a failure.
    $compileOutput = & cmd /c "$cmd 2>&1"
    $compileStatus = $LASTEXITCODE
    $compileOutput | Write-Host
    if ($compileStatus -ne 0) {
        $failed++
        continue
    }
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed -ne 0) { Write-Error "$failed test binary/binaries failed"; exit 1 }
Write-Host 'all emotion host tests passed'




