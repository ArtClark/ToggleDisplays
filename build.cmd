@echo off
rem Builds ToggleDisplays.exe into the repository root.
rem
rem Locates Visual Studio two ways, in order:
rem   1. vswhere, with -prerelease
rem   2. a glob for Common7\Tools\VsDevCmd.bat under the install root
rem
rem -prerelease is not optional. vswhere excludes prerelease channels by
rem default, so on a machine whose only toolchain is an Insiders or Preview
rem build, a plain "-latest" query matches nothing and the build fails with a
rem misleading "no C++ tools" message even though cl.exe is right there.

setlocal enabledelayedexpansion
set VSPATH=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
)

if not defined VSPATH (
    rem Fall back to looking for VsDevCmd.bat directly. Take the highest version
    rem directory that has one, so a stable install wins over a preview.
    for /f "delims=" %%d in ('dir /b /ad /o-n "%ProgramFiles%\Microsoft Visual Studio\*" 2^>nul') do (
        if not defined VSPATH (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%d\Common7\Tools\VsDevCmd.bat" (
                set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\%%d"
            )
        )
    )
)

if not defined VSPATH (
    echo Could not find Visual Studio with the C++ tools.
    echo Install it with the "Desktop development with C++" workload, or set
    echo VSPATH manually before running this script.
    exit /b 1
)

echo Using Visual Studio at "%VSPATH%"
if not exist "%VSPATH%\Common7\Tools\VsDevCmd.bat" (
    echo "%VSPATH%" has no Common7\Tools\VsDevCmd.bat - wrong installation picked.
    exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
if errorlevel 1 (
    echo VsDevCmd.bat failed.
    exit /b 1
)

pushd "%~dp0src"
rem /W4 /WX: the code is warning-clean at the strictest practical level, and
rem /WX keeps it that way. A warning here is a real defect, not noise.
cl /nologo /EHsc /std:c++17 /W4 /WX /O2 /Fe:"%~dp0ToggleDisplays.exe" ToggleDisplays.cpp user32.lib
set RC=%ERRORLEVEL%
popd

if not "%RC%"=="0" exit /b %RC%
echo.
echo Built "%~dp0ToggleDisplays.exe"
endlocal
