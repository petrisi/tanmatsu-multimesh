# Central settings for every script in this directory.
#
#   . "$PSScriptRoot\config.ps1"      <- note the leading dot (dot-source)
#
# Nothing here is tied to one machine. Paths inside the repository are derived
# from where this file sits, and everything outside it can be overridden with an
# environment variable, so a fresh clone works wherever it is checked out.
#
# Overridable with:
#   TANMATSU_IDF_PATH        where ESP-IDF v6.0.2 is installed
#   TANMATSU_IDF_TOOLS_PATH  where its toolchain lives
#   MULTIMESH_APP_REPO       checkout of the app-repository fork, for releases
#   MULTIMESH_SLUG           app identity, if you fork this under another name

# --- inside the repository, all derived ----------------------------------

$MM_Tools = $PSScriptRoot
$MM_Root  = Split-Path -Parent $PSScriptRoot

$MM_Target   = "tanmatsu"  # build directory name and the device target
$MM_BuildDir = Join-Path $MM_Root "build\$MM_Target"
$MM_Bin      = Join-Path $MM_BuildDir "application.bin"
$MM_Assets   = Join-Path $MM_Root "assets"
$MM_Metadata = Join-Path $MM_Assets "metadata.json"
$MM_License  = Join-Path $MM_Root "LICENSE"
$MM_Notices  = Join-Path $MM_Root "THIRD-PARTY-NOTICES.md"

# BadgeLink is third-party tooling and deliberately not in the repository; see
# tools/README.md for where to get it.
$MM_BadgeLink = Join-Path $MM_Tools "badgelink"
$MM_Python    = Join-Path $MM_BadgeLink ".venv\Scripts\python.exe"

# --- app identity --------------------------------------------------------

$MM_Slug = if ($env:MULTIMESH_SLUG) { $env:MULTIMESH_SLUG } else { "fi.ps.multimesh" }

# On-device data. The launcher mounts the internal partition at /int and the app
# mounts the same partition at /locfd, so the same files have two names
# depending on who is asking. BadgeLink talks to the launcher.
$MM_DeviceDataDir = "/int/multimesh"
$MM_DeviceLog     = "$MM_DeviceDataDir/session.log"

# --- outside the repository ----------------------------------------------

# The SDK lives outside the tree so it is shared between projects and never ends
# up in git. An already-activated IDF_PATH is honoured before the fallback.
$MM_IdfPath =
    if     ($env:TANMATSU_IDF_PATH) { $env:TANMATSU_IDF_PATH }
    elseif ($env:IDF_PATH)          { $env:IDF_PATH }
    else                            { "K:\esp\v6.0.2\esp-idf" }

$MM_IdfToolsPath =
    if     ($env:TANMATSU_IDF_TOOLS_PATH) { $env:TANMATSU_IDF_TOOLS_PATH }
    elseif ($env:IDF_TOOLS_PATH)          { $env:IDF_TOOLS_PATH }
    else                                  { "K:\esp\tools" }

# A checkout of the app-repository fork, for staging releases. Defaults to a
# sibling of this repository, which is where cloning it next to this one puts
# it -- so the default follows the checkout rather than naming a drive.
$MM_ReleaseRepo =
    if ($env:MULTIMESH_APP_REPO) { $env:MULTIMESH_APP_REPO }
    else { Join-Path (Split-Path -Parent $MM_Root) "app-repository" }

# The store's own metadata validator, used by release.ps1 when it is available.
$MM_Validator = Join-Path $MM_Root "reference\app-repository\.validator"
