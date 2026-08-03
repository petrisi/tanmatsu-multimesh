# Activates ESP-IDF v6.0.2 for this PowerShell session.
#
#   . K:\tanmatsu\tools\idf-env.ps1      <- note the leading dot (dot-source)
#   idf.py --version
#
# The IDF lives outside the repo so it is shared across projects and never
# ends up in git. Override with $env:TANMATSU_IDF_PATH / _TOOLS_PATH if needed.

$idf   = if ($env:TANMATSU_IDF_PATH)       { $env:TANMATSU_IDF_PATH }       else { "K:\esp\v6.0.2\esp-idf" }
$tools = if ($env:TANMATSU_IDF_TOOLS_PATH) { $env:TANMATSU_IDF_TOOLS_PATH } else { "K:\esp\tools" }

if (-not (Test-Path "$idf\export.ps1")) {
    Write-Error "ESP-IDF not found at $idf (expected export.ps1). Re-run the SDK install."
    return
}

$env:IDF_PATH        = $idf
$env:IDF_TOOLS_PATH  = $tools
$env:IDF_GITHUB_ASSETS = "dl.espressif.com/github_assets"

. "$idf\export.ps1"
