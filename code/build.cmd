@echo off
rem Configures (first time) and builds mapcraft.exe and mapcraft_editor.exe into build\release.
call "D:\Dev\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\release\build.ninja cmake --preset release || exit /b 1
cmake --build --preset release %*
