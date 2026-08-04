# Pull the session log off the device over BadgeLink.
#
# The device must be in BadgeLink mode (violet diamond on the launcher home
# screen). Stop recording first -- fn + yellow square -- so the last writes are
# flushed before the file is read.

param(
    [string] $Out = "session.log"
)

$ErrorActionPreference = "Stop"

$device = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_16D0*" }
if (-not $device) {
    throw "Badge not found. Put the device in BadgeLink mode: violet diamond on the launcher home screen."
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$py   = Join-Path $here "..\.venv\Scripts\python.exe"
if (-not (Test-Path $py)) { $py = "python" }

Push-Location (Join-Path $here "badgelink")
try {
    & $py badgelink.py fs download "/locfd/multimesh/session.log" $Out
} finally {
    Pop-Location
}

if (Test-Path $Out) {
    $size = (Get-Item $Out).Length
    "Fetched {0:N0} bytes to {1}" -f $size, (Resolve-Path $Out)
}
