@echo off
setlocal
rem Build workshop_fastload.dll (x86, static CRT) for the Left 4 Dead 2 l4n plugin loader.
rem Adjust VCVARS if your Build Tools live somewhere else.

if not exist "%~dp0src\l4n_plugin.h" (
    echo [error] src\l4n_plugin.h not found. Copy it from ^<game^>\bin\neko\plugins\ first.
    exit /b 1
)

set "VCVARS=E:\VS\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo [error] vcvars32.bat not found: %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [error] failed to initialise the x86 toolchain
    exit /b 1
)

cd /d "%~dp0"
if not exist dist mkdir dist
if not exist obj mkdir obj

cl /nologo /LD /O2 /MT /EHsc /W3 /utf-8 /Fo:obj\ /Fd:obj\workshop_fastload.pdb ^
   src\workshop_fastload.cpp ^
   /link /DLL /OUT:dist\workshop_fastload.dll /PDB:dist\workshop_fastload.pdb /SUBSYSTEM:WINDOWS,6.0

if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)

echo.
echo [ok] dist\workshop_fastload.dll
