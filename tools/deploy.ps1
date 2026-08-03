# Install the built app into AppFS over BadgeLink and optionally launch it.
#
#   K:\tanmatsu\tools\deploy.ps1
#   K:\tanmatsu\tools\deploy.ps1 -Start
#
# The device must be in BadgeLink mode: press the purple diamond key (2nd from
# top-right) on the launcher home screen; the top-right icon changes from a bug
# to a USB symbol. In debug mode this enumerates as COM ports instead and the
# tool will report "Badge not found".

param(
    [switch]$Start,
    [string]$Slug    = "fi.simolin.meshpoc",
    [string]$Title   = "MeshComms PoC",
    [int]   $Version = 0
)

$ErrorActionPreference = "Stop"
$root      = Split-Path -Parent $PSScriptRoot
$badgelink = Join-Path $PSScriptRoot "badgelink"
$py        = Join-Path $badgelink ".venv\Scripts\python.exe"
$bin       = Join-Path $root "build\tanmatsu\application.bin"

if (-not (Test-Path $py))  { throw "BadgeLink venv missing at $py" }
if (-not (Test-Path $bin)) { throw "No build output at $bin - run tools\build.ps1 first" }

"Uploading {0:N0} bytes as '{1}' v{2}..." -f (Get-Item $bin).Length, $Slug, $Version

Push-Location $badgelink
try {
    & $py badgelink.py appfs upload $Slug $Title $Version $bin
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
