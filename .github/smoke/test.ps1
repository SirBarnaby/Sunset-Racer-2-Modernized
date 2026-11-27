# Plays the release ZIP on this clean Windows runner (no GPU, no Vulkan
# driver), the way a player's PC without Vulkan would run it:
#   fresh   the ZIP as shipped: the game must start and stay in the menu
#   vulkan  the player picked Vulkan: the game must fall back to DirectX 9,
#           create a race and drive
# Screenshots, video and logs go to $Out. Exits 1 when a check fails.
param([string]$Zip, [string]$Out)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$failed = @()

function Unpack($name) {
    $dir = "C:\smoke\$name"
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Expand-Archive $Zip -DestinationPath $dir
    return "$dir\Sunset Racer 2"
}
function Play($game, $script, $out) {
    $r = @(& "$PSScriptRoot\run.ps1" -Game $game -Out $out -Script "$PSScriptRoot\$script")
    return [bool]$r[-1]
}

# fresh
$game = Unpack "fresh"
if (Test-Path "$game\SunsetRacer2-fx.ini") { $failed += "fresh: the ZIP ships SunsetRacer2-fx.ini" }
if (-not (Play $game "fresh.txt" "$Out\fresh")) { $failed += "fresh: the game exited" }

# vulkan (dev file on, so the graphics layer writes its log)
$game = Unpack "vulkan"
Set-Content "$game\SunsetRacer2-fx.ini" "renderer=1"
New-Item "$game\SunsetRacer2-dev.txt" -ItemType File | Out-Null
$alive = Play $game "vulkan-race.txt" "$Out\vulkan"
foreach ($f in "SunsetRacer2-d3d9.log", "SunsetRacer2_d3d9.log", "SunsetRacer2-audio.log", "SunsetRacer2-fx.ini", "SunsetRacer2-trace.txt") {
    if (Test-Path "$game\$f") { Copy-Item "$game\$f" "$Out\vulkan\$f" }
}
if (-not $alive) { $failed += "vulkan: the game exited" }
$log = Get-Content "$game\SunsetRacer2-d3d9.log" -Raw -ErrorAction SilentlyContinue
if (-not $log) { $failed += "vulkan: no graphics layer log" }
else {
    $asked = ([regex]::Matches($log, 'vulkan: .*using DirectX 9')).Count
    $devices = ([regex]::Matches($log, 'CreateDevice on DirectX 9.*')).Count
    $ok = ([regex]::Matches($log, 'CreateDevice -> 0x0\b')).Count
    Write-Host "graphics layer: $asked Vulkan fallbacks, $devices DirectX 9 devices, $ok created"
    if ($asked -lt 1) { $failed += "vulkan: the log shows no fallback from Vulkan" }
    if ($ok -lt 1) { $failed += "vulkan: no Direct3D device was created" }
}

if ($failed) { $failed | ForEach-Object { Write-Host "FAILED $_" }; exit 1 }
Write-Host "all smoke checks passed"
