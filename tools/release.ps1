# Stage a release into a checkout of the app-repository fork.
#
#   .\tools\release.ps1
#   .\tools\release.ps1 -RepoPath D:\src\app-repository
#
# The Tanmatsu store is a git repository: one folder per app, holding the
# compiled binary, icons, licence and metadata. Nothing else -- no source. So a
# release is a copy, not a branch, and this script produces it.
#
# Publishing is not automated on purpose. This leaves the fork staged and
# reports what to do next; opening the pull request is a deliberate act, and it
# is the point at which the repository's contributor licence agreement applies.

param(
    [string]$RepoPath = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"

# Where the fork is checked out. Defaults to $MM_ReleaseRepo, which follows this
# repository rather than naming a drive.
if (-not $RepoPath) { $RepoPath = $MM_ReleaseRepo }

# --- gather and check ----------------------------------------------------

if (-not (Test-Path $MM_Metadata)) { throw "Missing $MM_Metadata" }
$meta = Get-Content $MM_Metadata -Raw | ConvertFrom-Json

$app = $meta.application | Where-Object { $_.targets -contains $MM_Target } | Select-Object -First 1
if (-not $app) { throw "No application entry targeting '$MM_Target' in $MM_Metadata" }

if (-not $SkipBuild) {
    "Building..."
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path $MM_Bin)) { throw "No build output at $MM_Bin - run tools\build.ps1 first" }

# The store folder must contain a file with exactly the name `executable`
# carries, because that is the URL the launcher fetches.
$exeName = $app.executable
if (-not $exeName) { throw "No 'executable' in the tanmatsu application entry" }

if (-not (Test-Path $RepoPath)) {
    throw "No app-repository checkout at $RepoPath. Fork and clone it first:`n" +
          "  gh repo fork Nicolai-Electronics/app-repository --clone --fork-name app-repository`n" +
          "or set `$env:MULTIMESH_APP_REPO / pass -RepoPath to point at an existing checkout."
}

# A submission built on a stale fork carries other people's outdated apps in the
# diff, which makes the pull request unreviewable.
Push-Location $RepoPath
try {
    $behind = git rev-list --count HEAD..origin/main 2>$null
    if ($LASTEXITCODE -eq 0 -and [int]$behind -gt 0) {
        Write-Warning "$RepoPath is $behind commit(s) behind origin/main. Sync before opening the pull request:"
        Write-Warning "  git fetch upstream; git merge upstream/main"
    }
} finally {
    Pop-Location
}

# --- stage ---------------------------------------------------------------

$dest = Join-Path $RepoPath $MM_Slug
New-Item -ItemType Directory -Force -Path $dest | Out-Null

Copy-Item $MM_Bin (Join-Path $dest $exeName) -Force
Copy-Item $MM_Metadata (Join-Path $dest "metadata.json") -Force
Copy-Item $MM_License (Join-Path $dest "LICENSE") -Force

# The binary statically links MIT and Apache-2.0 components, whose licences
# require their notices to travel with it. Shipping the binary without them
# would be distributing other people's work without the attribution they ask
# for -- which costs one file to avoid.
Copy-Item $MM_Notices (Join-Path $dest "THIRD-PARTY-NOTICES.md") -Force

foreach ($size in @("16", "32", "64")) {
    $icon = "icon$size.png"
    $src  = Join-Path $MM_Assets $icon
    if (-not (Test-Path $src)) { throw "Missing $src - run tools\make-icon.py" }
    Copy-Item $src (Join-Path $dest $icon) -Force
}

# --- verify --------------------------------------------------------------

# Against the store's own schema, not our reading of it. The check is skipped
# rather than faked when the validator is unavailable.
if ((Test-Path (Join-Path $MM_Validator "node_modules")) -and (Get-Command node -ErrorAction SilentlyContinue)) {
    Push-Location $MM_Validator
    try {
        & node validate.js (Join-Path $dest "metadata.json")
        if ($LASTEXITCODE -ne 0) { throw "metadata.json failed the store schema" }
        "metadata.json passes the store schema"
    } finally {
        Pop-Location
    }
} else {
    Write-Warning "Store validator unavailable; metadata not checked. CI will check it on the pull request."
}

# Every file named in the metadata has to exist under the name it is named by.
foreach ($name in @($exeName, "metadata.json", "LICENSE", "THIRD-PARTY-NOTICES.md",
                    "icon16.png", "icon32.png", "icon64.png")) {
    if (-not (Test-Path (Join-Path $dest $name))) { throw "Staged folder is missing $name" }
}

# --- report --------------------------------------------------------------

""
"Staged $MM_Slug v$($meta.version) revision $($app.revision) into $dest"
Get-ChildItem $dest | ForEach-Object { "  {0,10:N0}  {1}" -f $_.Length, $_.Name }
""
"Next:"
"  cd $RepoPath"
"  git switch -c $MM_Slug-v$($meta.version)"
"  git add $MM_Slug"
"  git commit -m ""Add MultiMesh $($meta.version)"""
"  git push -u origin HEAD"
"  gh pr create --repo Nicolai-Electronics/app-repository --fill"
