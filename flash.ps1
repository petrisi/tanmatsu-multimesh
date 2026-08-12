$oldErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$env:PYTHONIOENCODING="utf-8"
$env:IDF_PATH="C:\Users\JorKsX\esp\esp-idf"
$env:IDF_TOOLS_PATH="C:\Users\JorKsX\.espressif"
. "$env:IDF_PATH\export.ps1"
$ErrorActionPreference = $oldErrorActionPreference
idf.py -B build/tanmatsu flash
