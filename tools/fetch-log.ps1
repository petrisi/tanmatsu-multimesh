# Pull the session log off the device over BadgeLink.
#
# The device must be in BadgeLink mode (violet diamond on the launcher home
# screen). Stop recording first -- fn + yellow square -- so the last writes are
# flushed before the file is read.
#
# Note the path. The app mounts the internal partition at /locfd, but BadgeLink
# talks to the launcher, which mounts the same partition at /int. Same bytes,
# different name, and only the launcher's name works from here.

param(
    [string] $Out = "session.log"
)

$ErrorActionPreference = "Stop"

$device = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_16D0*" }
if (-not $device) {
    throw "Badge not found. Put the device in BadgeLink mode: violet diamond on the launcher home screen."
}

$badgelink = Join-Path $PSScriptRoot "badgelink"
$py        = Join-Path $badgelink ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "BadgeLink venv missing at $py" }

# Absolute, because badgelink runs from its own directory.
$target = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }

Push-Location $badgelink
try {
    & $py badgelink.py fs download "/int/multimesh/session.log" $target
    if ($LASTEXITCODE -ne 0) { throw "fs download failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
$Out = $target

if (Test-Path $Out) {
    $size = (Get-Item $Out).Length
    "Fetched {0:N0} bytes to {1}" -f $size, (Resolve-Path $Out)
}
