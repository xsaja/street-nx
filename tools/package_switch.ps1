# Builds switch_sd\switch\street-nx\ beside the repo: street.nro, the default settings, LICENSES.txt and an empty game\
# folder for the player's own files. Nothing of the game goes in: the player copies those.
# package_switch.ps1  (run tools/make_licenses.py first when a license changes)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'switch_sd\switch\street-nx'
$nro = Join-Path $root 'build-switch\street.nro'
if (-not (Test-Path $nro)) { throw "No ${nro}: build it first (switch/build.sh)" }
Remove-Item -Recurse -Force (Join-Path $root 'switch_sd') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $out 'game') | Out-Null
Copy-Item $nro $out
Copy-Item (Join-Path $root 'switch\street-settings.toml') $out
Set-Content (Join-Path $out 'game\PUT-THE-GAME-HERE.txt') -Encoding utf8 -Value @'
Copy the files of your FIFA Street disc (Xbox) here: default.xbe with all its folders,
or an .iso image of the disc. They are the same files as the assets folder of the Windows package.
'@

# every license of what the NRO carries (tools/make_licenses.py writes it)
Copy-Item (Join-Path $root 'LICENSES.txt') $out
Get-ChildItem -Recurse $out | Select-Object FullName, Length
