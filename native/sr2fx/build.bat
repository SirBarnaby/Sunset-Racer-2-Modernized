@echo off
rem Builds the shipped graphics layer (32-bit d3d9.dll) with MSVC 2022 and the
rem Windows SDK's fxc. Output in out\:
rem   d3d9.dll       the layer (goes beside the game exe)
rem   sr2fx-vk.dll   DXVK's d3d9.dll, renamed, for the Vulkan renderer
setlocal
cd /d "%~dp0"
rem MSVC 2022 (Build Tools or any edition), found with vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VCVARS=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvarsall.bat`) do set "VCVARS=%%i"
if not defined VCVARS (echo MSVC 2022 not found & exit /b 1)
call "%VCVARS%" x86 >nul || exit /b 1
if not exist out mkdir out
if not exist out\shaders mkdir out\shaders

set FXC=
for /f "delims=" %%f in ('dir /b /s /o-n "C:\Program Files (x86)\Windows Kits\10\bin\*fxc.exe" 2^>nul ^| findstr /i "\\x86\\fxc.exe"') do if not defined FXC set "FXC=%%f"
if not defined FXC (echo fxc.exe not found & exit /b 1)

for %%e in (VS_Quad VS_ObjVel) do (
  "%FXC%" /nologo /T vs_3_0 /O3 /E %%e /Vn g_%%e /Fh out\shaders\%%e.h shaders\post.hlsl >nul || exit /b 1
)
"%FXC%" /nologo /T ps_3_0 /O3 /E PS_AO /DAO_SLICES=2 /DAO_STEPS=4 /Vn g_PS_AO_Lo /Fh out\shaders\PS_AO_Lo.h shaders\post.hlsl >nul || exit /b 1
"%FXC%" /nologo /T ps_3_0 /O3 /E PS_AO /DAO_SLICES=3 /DAO_STEPS=4 /Vn g_PS_AO_Hi /Fh out\shaders\PS_AO_Hi.h shaders\post.hlsl >nul || exit /b 1
for %%e in (PS_Copy PS_LinZ PS_Debug PS_AOBlur PS_SunMask PS_SunBlur PS_Composite PS_AOMul PS_CamVel PS_ObjVel PS_TileMax PS_NeighborMax PS_Reconstruct PS_FXAA) do (
  "%FXC%" /nologo /T ps_3_0 /O3 /E %%e /Vn g_%%e /Fh out\shaders\%%e.h shaders\post.hlsl >nul || exit /b 1
)

cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /Iout\shaders /LD sr2fx.cpp post.cpp /Foout\ /Feout\d3d9.dll /link /DEF:d3d9.def user32.lib dxguid.lib || exit /b 1

set DXVK=..\..\third_party\dxvk\dxvk-3.1.1\x32\d3d9.dll
if exist "%DXVK%" (copy /y "%DXVK%" out\sr2fx-vk.dll >nul) else (echo warning: %DXVK% missing, Vulkan renderer not packaged)
echo built out\d3d9.dll
