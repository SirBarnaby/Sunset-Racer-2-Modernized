# Downloads the parts the build does not compile, each checked against a
# pinned SHA-256:
#   third_party\dxvk\dxvk-3.1.1\          DXVK 3.1.1 (the Vulkan renderer, zlib licence)
#   third_party\steamaudio\steamaudio\    Steam Audio 4.8.1 SDK (3D audio, Apache 2.0)
#   third_party\payload\Sunset Racer 2\   the v1.0 game folder: projector exe, Xtras,
#                                         assets and settings pictures
# The projector is built on the owner's PC with Adobe Director 11.5, which
# cannot run here, so the game folder comes from the v1.0 release.
param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")))
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$tp = Join-Path $Root "third_party"

$parts = @(
    @{ Url = "https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz"
       Sha = "40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608"; Dir = "dxvk" },
    @{ Url = "https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip"
       Sha = "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449"; Dir = "steamaudio" },
    @{ Url = "https://github.com/SirBarnaby/Sunset-Racer-2-Modernized/releases/download/v1.0/Sunset-Racer-2-Windows.zip"
       Sha = "dad15d9f3222997fba2e482181362353b6f19627644e14dabc6c8748d931c780"; Dir = "payload" }
)
foreach ($p in $parts) {
    $dir = Join-Path $tp $p.Dir
    New-Item -ItemType Directory -Force $dir | Out-Null
    $file = Join-Path $dir (Split-Path $p.Url -Leaf)
    if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA256).Hash -ne $p.Sha) {
        Write-Host "downloading $($p.Url)"
        Invoke-WebRequest $p.Url -OutFile $file
    }
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash
    if ($hash -ne $p.Sha) { throw "$file has SHA-256 $hash, expected $($p.Sha)" }
    if ($file.EndsWith(".tar.gz")) { tar -xzf $file -C $dir } else { Expand-Archive $file -DestinationPath $dir -Force }
    if ($LASTEXITCODE) { throw "unpacking $file failed" }
}
foreach ($f in "dxvk\dxvk-3.1.1\x32\d3d9.dll", "steamaudio\steamaudio\lib\windows-x86\phonon.dll", "payload\Sunset Racer 2\SunsetRacer2.exe") {
    if (-not (Test-Path (Join-Path $tp $f))) { throw "third_party\$f missing after unpacking" }
}
Write-Host "third-party parts ready in $tp"
