@echo off
setlocal
set "DC_CONFIGURATION=Release"
if not "%~1"=="" set "DC_CONFIGURATION=%~1"
if /I "%DC_CONFIGURATION%"=="Release" goto find_tools
if /I "%DC_CONFIGURATION%"=="Debug" goto find_tools
echo Usage: build.cmd [Release^|Debug]
exit /b 2

:find_tools
set "DC_CMAKE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined DC_CMAKE set "DC_CMAKE=%%I"
if defined DC_CMAKE goto build
set "DC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%DC_VSWHERE%" goto missing_tools
for /f "usebackq delims=" %%I in (`"%DC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do if not defined DC_CMAKE set "DC_CMAKE=%%I"
if not defined DC_CMAKE goto missing_tools

:build
for %%I in ("%DC_CMAKE%") do set "DC_CTEST=%%~dpIctest.exe"
if not exist "%DC_CTEST%" goto missing_tools
"%DC_CMAKE%" -S "%~dp0." -B "%~dp0build" -A x64
if errorlevel 1 exit /b 1
"%DC_CMAKE%" --build "%~dp0build" --config "%DC_CONFIGURATION%"
if errorlevel 1 exit /b 1
"%DC_CTEST%" --test-dir "%~dp0build" -C "%DC_CONFIGURATION%" --output-on-failure
if errorlevel 1 exit /b 1
echo Executable: %~dp0build\%DC_CONFIGURATION%\DataCopy.exe
exit /b 0

:missing_tools
echo The C++ tools or CMake are missing. See docs/REQUIRED_DOWNLOADS.md.
exit /b 2
