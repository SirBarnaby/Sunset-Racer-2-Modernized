# Assembles the player's game folder and the release ZIP:
#   dist\Sunset Racer 2\            the game folder
#   dist\Sunset-Racer-2-Windows.zip
# Inputs: packaging\fetch.ps1 (third_party\) and the two native builds
# (native\sr2fx\out, native\sr2audio\out).
#
# A fresh install ships no SunsetRacer2-fx.ini, so the game starts with the
# DirectX 9 renderer and every effect off; the game writes the file from the
# player's own settings. Logs, traces and dev files from test runs never ship.
param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")))
$ErrorActionPreference = "Stop"
$dist = Join-Path $Root "dist"
$game = Join-Path $dist "Sunset Racer 2"
$payload = Join-Path $Root "third_party\payload\Sunset Racer 2"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $game | Out-Null

# The projector and its data, from the payload.
foreach ($item in "SunsetRacer2.exe", "SunsetRacer2.exe.manifest", "Xtras", "assets", "settings") {
    $src = Join-Path $payload $item
    if (-not (Test-Path $src)) { throw "payload is missing $item" }
    Copy-Item $src $game -Recurse
}

# The graphics layer, DXVK, the audio layer and Steam Audio, all built or
# fetched here.
$fx = Join-Path $Root "native\sr2fx\out"
$au = Join-Path $Root "native\sr2audio\out"
Copy-Item (Join-Path $fx "d3d9.dll") $game
Copy-Item (Join-Path $Root "third_party\dxvk\dxvk-3.1.1\x32\d3d9.dll") (Join-Path $game "sr2fx-vk.dll")
Copy-Item (Join-Path $au "dsound.dll") $game
Copy-Item (Join-Path $Root "third_party\steamaudio\steamaudio\lib\windows-x86\phonon.dll") $game

$lic = Join-Path $game "licenses"
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $Root "licenses\*") $lic
Copy-Item (Join-Path $Root "third_party\steamaudio\steamaudio\THIRDPARTY.md") (Join-Path $lic "SteamAudio-THIRDPARTY.md")
Copy-Item (Join-Path $PSScriptRoot "README.txt") $game

$bad = Get-ChildItem $game -Recurse -File | Where-Object { $_.Name -match '\.(ini|log)$|-trace\.txt$|-dev\.txt$|\.bmp$' }
if ($bad) { throw "test leftovers in the game folder: $($bad.FullName -join ', ')" }

$zip = Join-Path $dist "Sunset-Racer-2-Windows.zip"
Compress-Archive -Path $game -DestinationPath $zip -CompressionLevel Optimal
Get-ChildItem $game | Format-Table Name, Length -AutoSize | Out-String | Write-Host
Write-Host ("built {0} ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)
