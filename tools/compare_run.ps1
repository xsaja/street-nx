# Runs one PC build through tools/compare_route.txt and keeps its captures and log apart.
# compare_run.ps1 -Exe STREET-vk.exe -Dir <test folder> -Out vk [-Seconds 150]
param([string]$Exe, [string]$Dir, [string]$Out, [int]$Seconds = 150)
$route = Join-Path $PSScriptRoot 'compare_route.txt'
$shots = Join-Path $Dir 'screenshots'
$dest = Join-Path $Dir "compare\$Out"
Remove-Item -Recurse -Force $shots, $dest -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dest | Out-Null
$env:STREET_PAD_SCRIPT = $route
$p = Start-Process (Join-Path $Dir $Exe) -WorkingDirectory $Dir -PassThru
Start-Sleep $Seconds
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } else { "exited early with code $($p.ExitCode)" }
Get-ChildItem $shots -Filter *.png -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | ForEach-Object -Begin { $i = 0 } -Process {
    $i++; Move-Item $_.FullName (Join-Path $dest ('{0:D2}.png' -f $i))
}
$log = Get-ChildItem (Join-Path $Dir 'logs') | Sort-Object LastWriteTime | Select-Object -Last 1
Copy-Item $log.FullName (Join-Path $dest 'run.log')
"$Out : $i captures"
