$ErrorActionPreference = "Stop"

$python = "C:\Users\JorKsX\Desktop\Tanmatsu custom\tools\badgelink\.venv\Scripts\python.exe"
$badgelink = "C:\Users\JorKsX\Desktop\Tanmatsu custom\tools\badgelink\badgelink.py"

Write-Host "Creating /int/multimesh directory..."
& $python $badgelink fs mkdir /int/multimesh

Write-Host "Uploading kal_lpc.bin..."
& $python $badgelink fs upload /int/multimesh/kal_lpc.bin kal_lpc.bin

Write-Host "Uploading kal_res.bin..."
& $python $badgelink fs upload /int/multimesh/kal_res.bin kal_res.bin

Write-Host "Uploading kal_resi.bin..."
& $python $badgelink fs upload /int/multimesh/kal_resi.bin kal_resi.bin

Write-Host "Uploading kal_ressize.bin..."
& $python $badgelink fs upload /int/multimesh/kal_ressize.bin kal_ressize.bin

Write-Host "Done!"
