# Builds the 32-bit dsound.dll proxy (and the test harness) with MSVC, then
# places phonon.dll (Steam Audio 4.8.1, windows-x86) next to it in out\.
#   powershell -NoProfile -ExecutionPolicy Bypass -File native\sr2audio\build.ps1 [-Install <game folder>]
param([string]$Install = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here "..\..")
$sa = Join-Path $repo "third_party\steamaudio\steamaudio"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Auxiliary\Build\vcvarsall.bat" | Select-Object -First 1
if (-not $vcvars -or -not (Test-Path $vcvars)) { throw "MSVC build tools not found at $vcvars" }
if (-not (Test-Path "$sa\lib\windows-x86\phonon.dll")) { throw "Steam Audio SDK not found in $sa" }
$out = Join-Path $here "out"
New-Item -ItemType Directory -Force $out | Out-Null

$cflags = "/nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I`"$sa\include`""
$dll = "cl $cflags /LD src\proxy.c src\settings.c src\vbuf.c src\mixer.c src\fx.c /Foout\ /Feout\dsound.dll " +
       "/link /DEF:src\dsound.def /DELAYLOAD:phonon.dll `"$sa\lib\windows-x86\phonon.lib`" delayimp.lib user32.lib winmm.lib dxguid.lib"
$test = "cl $cflags test\harness.c /Foout\ /Feout\sr2audio-test.exe /link user32.lib winmm.lib dxguid.lib ole32.lib"
$env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + $env:PATH
$cmd = "call `"$vcvars`" x86 >nul && cd /d `"$here`" && $dll && $test"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Copy-Item "$sa\lib\windows-x86\phonon.dll" $out -Force
Remove-Item "$out\*.obj", "$out\*.exp", "$out\dsound.lib" -ErrorAction SilentlyContinue
Write-Host "built $out\dsound.dll (+ phonon.dll, sr2audio-test.exe)"
if ($Install) {
    Copy-Item "$out\dsound.dll", "$out\phonon.dll" $Install -Force
    Write-Host "installed into $Install"
}
