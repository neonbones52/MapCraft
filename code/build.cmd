@echo off
rem Configures (first time) and builds mapcraft.exe and mapcraft_editor.exe into build\release.
rem The compiler is found with vswhere: the latest Visual Studio or Build Tools instance with the x64 MSVC toolset.
setlocal
if not defined VCPKG_ROOT (
  echo build.cmd: set VCPKG_ROOT to the vcpkg checkout. 1>&2
  exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo build.cmd: vswhere.exe not found, install Visual Studio Build Tools with the MSVC x64 toolset. 1>&2
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo build.cmd: vswhere found no Visual Studio or Build Tools instance with the MSVC x64 toolset. 1>&2
  exit /b 1
)
rem vcvars64 points VCPKG_ROOT at the vcpkg bundled with Visual Studio; keep the user's checkout.
set "USER_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VCPKG_ROOT=%USER_VCPKG_ROOT%"
cd /d "%~dp0"
if not exist build\release\build.ninja cmake --preset release || exit /b 1
cmake --build --preset release %*
