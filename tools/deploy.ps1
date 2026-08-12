# Install the built app into AppFS over BadgeLink and optionally launch it.
#
#   .\tools\deploy.ps1
#   .\tools\deploy.ps1 -Start
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
. "$PSScriptRoot\config.ps1"

if (-not (Test-Path $MM_Python))   { throw "BadgeLink venv missing at $MM_Python - see tools\README.md" }
if (-not (Test-Path $MM_Bin))      { throw "No build output at $MM_Bin - run tools\build.ps1 first" }
if (-not (Test-Path $MM_Metadata)) { throw "Missing $MM_Metadata" }

$meta = Get-Content $MM_Metadata -Raw | ConvertFrom-Json
$app  = $meta.application | Where-Object { $_.targets -contains $MM_Target } | Select-Object -First 1
if (-not $app) { throw "No application entry targeting '$MM_Target' in $MM_Metadata" }

$Title = $meta.name
if ($Revision -eq 0) { $Revision = [int]$app.revision }

"Uploading {0:N0} bytes as '{1}' v{2} rev {3}..." -f (Get-Item $MM_Bin).Length, $MM_Slug, $meta.version, $Revision

Push-Location $MM_BadgeLink
try {
    & $MM_Python badgelink.py appfs upload $MM_Slug $Title $Revision $MM_Bin
    if ($LASTEXITCODE -ne 0) { throw "appfs upload failed with exit code $LASTEXITCODE" }

    if ($Start) {
        & $MM_Python badgelink.py start $MM_Slug
        if ($LASTEXITCODE -ne 0) { throw "start failed with exit code $LASTEXITCODE" }
        "Started $MM_Slug"
    } else {
        "Uploaded. Launch it from the device menu, or re-run with -Start."
    }
} finally {
    Pop-Location
}
