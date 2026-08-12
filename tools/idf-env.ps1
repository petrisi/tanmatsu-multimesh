# Activates ESP-IDF v6.0.2 for this PowerShell session.
#
#   . .\tools\idf-env.ps1      <- note the leading dot (dot-source)
#   idf.py --version
#
# Where the SDK lives is set in config.ps1 and overridable with
# $env:TANMATSU_IDF_PATH / $env:TANMATSU_IDF_TOOLS_PATH.

. "$PSScriptRoot\config.ps1"

if (-not (Test-Path "$MM_IdfPath\export.ps1")) {
    Write-Error ("ESP-IDF not found at $MM_IdfPath (expected export.ps1). " +
                 "Install it, or point `$env:TANMATSU_IDF_PATH at your checkout.")
    return
}

$env:IDF_PATH          = $MM_IdfPath
$env:IDF_TOOLS_PATH    = $MM_IdfToolsPath
$env:IDF_GITHUB_ASSETS = "dl.espressif.com/github_assets"

. "$MM_IdfPath\export.ps1"
