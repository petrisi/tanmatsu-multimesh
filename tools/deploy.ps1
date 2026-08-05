# Install the built app into AppFS over BadgeLink and optionally launch it.
#
#   K:\tanmatsu\tools\deploy.ps1
#   K:\tanmatsu\tools\deploy.ps1 -Start
#
# The device must be in BadgeLink mode: press the purple diamond key (2nd from
# top-right) on the launcher home screen; the top-right icon changes from a bug
# to a USB symbol. In debug mode this enumerates as COM ports instead and the
# tool will report "Badge not found".

# The revision comes from assets/metadata.json rather than the command line.
# Passing it by hand let the deployed build and the published metadata drift
# apart, which is invisible until a device declines an update it should have
# taken.
param(
    [switch]$Start,
    [int]   $Revision = 0   # 0 = read it from the metadata
)

$ErrorActionPreference = "Stop"
$root      = Split-Path -Parent $PSScriptRoot
$badgelink = Join-Path $PSScriptRoot "badgelink"
$py        = Join-Path $badgelink ".venv\Scripts\python.exe"
$bin       = Join-Path $root "build\tanmatsu\application.bin"
$metaPath  = Join-Path $root "assets\metadata.json"

if (-not (Test-Path $py))       { throw "BadgeLink venv missing at $py - see tools\README.md" }
if (-not (Test-Path $bin))      { throw "No build output at $bin - run tools\build.ps1 first" }
if (-not (Test-Path $metaPath)) { throw "Missing $metaPath" }

$meta  = Get-Content $metaPath -Raw | ConvertFrom-Json
$app   = $meta.application | Where-Object { $_.targets -contains "tanmatsu" } | Select-Object -First 1
if (-not $app) { throw "No application entry targeting 'tanmatsu' in $metaPath" }

$Slug  = "fi.ps.multimesh"
$Title = $meta.name
if ($Revision -eq 0) { $Revision = [int]$app.revision }

"Uploading {0:N0} bytes as '{1}' v{2} rev {3}..." -f (Get-Item $bin).Length, $Slug, $meta.version, $Revision

Push-Location $badgelink
try {
    & $py badgelink.py appfs upload $Slug $Title $Revision $bin
    if ($LASTEXITCODE -ne 0) { throw "appfs upload failed with exit code $LASTEXITCODE" }

    if ($Start) {
        & $py badgelink.py start $Slug
        if ($LASTEXITCODE -ne 0) { throw "start failed with exit code $LASTEXITCODE" }
        "Started $Slug"
    } else {
        "Uploaded. Launch it from the device menu, or re-run with -Start."
    }
} finally {
    Pop-Location
}
