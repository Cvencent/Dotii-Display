param(
    [string]$Version = "",
    [switch]$Clean,
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $ProjectRoot

$CMakeText = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw
$VersionMatch = [regex]::Match($CMakeText, 'set\s*\(\s*PROJECT_VER\s+"?([^"\s\)]+)')
if (-not $VersionMatch.Success) {
    throw "CMakeLists.txt does not define PROJECT_VER."
}
$ProjectVersion = $VersionMatch.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $ProjectVersion
}
if ($Version -ne $ProjectVersion) {
    throw "Package version $Version does not match firmware version $ProjectVersion."
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version must use semantic version format such as 1.0.0."
}

$CodxRoot = Join-Path $ProjectRoot ".codx"
$VenvRoot = Join-Path $CodxRoot "packaging-venv"
$Python = Join-Path $VenvRoot "Scripts\python.exe"
$WorkRoot = Join-Path $CodxRoot "packaging-work"
$DistRoot = Join-Path $WorkRoot "dist"
$BuildRoot = Join-Path $WorkRoot "build"
$ReleaseRoot = Join-Path $ProjectRoot "release\DotiiManagementCenter-$Version"
$Archive = Join-Path $ProjectRoot "release\DotiiManagementCenter-$Version-portable.zip"
$HostVersionFile = Join-Path $CodxRoot "packaging-version-host.txt"
$FirmwareBuildRoot = Join-Path $ProjectRoot "build"
$FirmwareRoot = Join-Path $ProjectRoot "firmware"
$BuildFirmwareManifest = Join-Path $FirmwareBuildRoot "flasher_args.json"
$BundledFirmwareManifest = Join-Path $FirmwareRoot "flasher_args.json"

if (Test-Path -LiteralPath $BuildFirmwareManifest -PathType Leaf) {
    $FirmwareSourceRoot = $FirmwareBuildRoot
    $FirmwareManifest = $BuildFirmwareManifest
    $RefreshFirmwareBundle = $true
} elseif (Test-Path -LiteralPath $BundledFirmwareManifest -PathType Leaf) {
    # Management-center-only releases may reuse the already validated formal
    # firmware bundle. A fresh ESP-IDF build still takes precedence when present.
    $FirmwareSourceRoot = $FirmwareRoot
    $FirmwareManifest = $BundledFirmwareManifest
    $RefreshFirmwareBundle = $false
} else {
    throw "No firmware manifest is available. Run idf.py build or restore firmware\flasher_args.json."
}
$FirmwarePayload = Get-Content -LiteralPath $FirmwareManifest -Raw | ConvertFrom-Json
$FlashFiles = $FirmwarePayload.flash_files.PSObject.Properties
if (@($FlashFiles).Count -eq 0) {
    throw "$FirmwareManifest does not contain flash_files."
}
if ($RefreshFirmwareBundle -and (Test-Path -LiteralPath $FirmwareRoot)) {
    Remove-Item -LiteralPath $FirmwareRoot -Recurse -Force
}
if ($RefreshFirmwareBundle) {
    New-Item -ItemType Directory -Force -Path $FirmwareRoot | Out-Null
    Copy-Item -LiteralPath $FirmwareManifest -Destination $FirmwareRoot -Force
}
foreach ($FlashFile in $FlashFiles) {
    $RelativeName = [string]$FlashFile.Value
    $Source = [IO.Path]::GetFullPath((Join-Path $FirmwareSourceRoot $RelativeName))
    if (-not $Source.StartsWith(([IO.Path]::GetFullPath($FirmwareSourceRoot) + '\'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Firmware image escapes its source directory: $RelativeName"
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Firmware image is missing: $RelativeName"
    }
    if ($RefreshFirmwareBundle) {
        $Destination = Join-Path $FirmwareRoot $RelativeName
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
}

New-Item -ItemType Directory -Force -Path $CodxRoot | Out-Null
$HostVersionText = @"
VSVersionInfo(
  ffi=FixedFileInfo(filevers=($($Version.Split('.')[0]), $($Version.Split('.')[1]), $($Version.Split('.')[2]), 0), prodvers=($($Version.Split('.')[0]), $($Version.Split('.')[1]), $($Version.Split('.')[2]), 0)),
  kids=[StringFileInfo([StringTable('080404B0', [StringStruct('CompanyName', 'Dotii'), StringStruct('FileDescription', 'Dotii 管理中心'), StringStruct('FileVersion', '$Version'), StringStruct('InternalName', 'DotiiManagementCenter'), StringStruct('OriginalFilename', 'DotiiManagementCenter.exe'), StringStruct('ProductName', 'Dotii 管理中心'), StringStruct('ProductVersion', '$Version')])]), VarFileInfo([VarStruct('Translation', [2052, 1200])])]
)
"@
# Keep the PyInstaller data-structure files ASCII-only. This prevents their
# Python parser from decoding Chinese resource strings with the active OEM code page.
[IO.File]::WriteAllText($HostVersionFile, $HostVersionText, [Text.Encoding]::ASCII)
if (-not (Test-Path $Python)) {
    & python -m venv $VenvRoot
}

& $Python -m pip install --disable-pip-version-check --requirement "packaging\requirements-build.txt"

if ($Clean -and (Test-Path $WorkRoot)) {
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $DistRoot, $BuildRoot | Out-Null

$env:DOTII_VERSION_FILE = $HostVersionFile
& $Python -m PyInstaller --noconfirm --clean `
    --distpath $DistRoot --workpath $BuildRoot `
    "packaging\DotiiManagementCenter.spec"
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

$HostExe = Join-Path $DistRoot "DotiiManagementCenter.exe"
if (-not (Test-Path -LiteralPath $HostExe -PathType Leaf)) {
    throw "PyInstaller did not produce DotiiManagementCenter.exe."
}
$Executables = @(Get-ChildItem -LiteralPath $DistRoot -Filter "*.exe" -File -Recurse)
if ($Executables.Count -ne 1 -or $Executables[0].FullName -ne $HostExe) {
    throw "Expected exactly one executable in dist: $HostExe"
}

if (Test-Path -LiteralPath $ReleaseRoot) {
    Remove-Item -LiteralPath $ReleaseRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $ReleaseRoot | Out-Null
Copy-Item -LiteralPath $HostExe -Destination $ReleaseRoot -Force

$Checksums = Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName |
    ForEach-Object {
        $Relative = $_.FullName.Substring($ReleaseRoot.Length + 1).Replace('\', '/')
        "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $Relative
    }
$Checksums | Set-Content -LiteralPath (Join-Path $ReleaseRoot "SHA256SUMS.txt") -Encoding UTF8

Compress-Archive -Path (Join-Path $ReleaseRoot "*") -DestinationPath $Archive -Force
Get-FileHash -Algorithm SHA256 $Archive | Format-List
Write-Host "Portable package created: $Archive"

$Installer = Join-Path $ProjectRoot "release\Dotii-Setup-$Version.exe"
if (-not $SkipInstaller) {
    $IsccCandidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $Iscc = $IsccCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $Iscc) {
        throw "Inno Setup 6 compiler not found. Install Inno Setup or pass -SkipInstaller."
    }
    & $Iscc "/DAppVersion=$Version" "packaging\DotiiManagementCenter.iss"
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $Installer -PathType Leaf)) {
        throw "Inno Setup did not produce $Installer."
    }
    Get-FileHash -Algorithm SHA256 $Installer | Format-List
    Write-Host "Installer created: $Installer"
}
