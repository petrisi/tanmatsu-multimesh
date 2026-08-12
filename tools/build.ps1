# Build the app for Tanmatsu (ESP32-P4).
#
#   .\tools\build.ps1
#   .\tools\build.ps1 -Target reconfigure
#
# The upstream badge.team Makefiles are bash-only; this is the same idf.py
# invocation they wrap, expressed for PowerShell.

param(
    [string]$Target = "build"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"

. "$PSScriptRoot\idf-env.ps1" *>$null
if (-not $env:IDF_PATH) { throw "ESP-IDF activation failed" }

Push-Location $MM_Root
try {
    idf.py `
        -B "build/$MM_Target" `
        -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/$MM_Target" `
        -DSDKCONFIG="sdkconfig_$MM_Target" `
        -DIDF_TARGET=esp32p4 `
        $Target
    if ($LASTEXITCODE -ne 0) { throw "idf.py $Target failed with exit code $LASTEXITCODE" }

    if ($Target -eq "build") {
        if (Test-Path $MM_Bin) {
            "`nBuilt {0} ({1:N0} bytes)" -f $MM_Bin, (Get-Item $MM_Bin).Length
        }
    }
} finally {
    Pop-Location
}
