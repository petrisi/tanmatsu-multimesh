# Build the app for Tanmatsu (ESP32-P4).
#
#   K:\tanmatsu\tools\build.ps1
#   K:\tanmatsu\tools\build.ps1 -Target reconfigure
#
# The upstream badge.team Makefiles are bash-only; this is the same idf.py
# invocation they wrap, expressed for PowerShell.

param(
    [string]$Target = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

. "$PSScriptRoot\idf-env.ps1" *>$null
if (-not $env:IDF_PATH) { throw "ESP-IDF activation failed" }

Push-Location $root
try {
    $env:PYTHONIOENCODING="utf-8"
    idf.py `
        -B build/tanmatsu `
        -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" `
        -DSDKCONFIG=sdkconfig_tanmatsu `
        -DIDF_TARGET=esp32p4 `
        $Target
    if ($LASTEXITCODE -ne 0) { throw "idf.py $Target failed with exit code $LASTEXITCODE" }

    if ($Target -eq "build") {
        $bin = Join-Path $root "build\tanmatsu\application.bin"
        if (Test-Path $bin) {
            "`nBuilt {0} ({1:N0} bytes)" -f $bin, (Get-Item $bin).Length
        }
    }
} finally {
    Pop-Location
}
