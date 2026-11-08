@echo off
setlocal
call "E:\VS\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 ( echo [error] vcvars failed & exit /b 1 )

cd /d "%~dp0"
if not exist obj mkdir obj

cl /nologo /LD /O2 /MT /EHsc /W3 /utf-8 /Fo:obj\ /Fd:obj\fake.pdb ^
   fake_steam_api.cpp ^
   /link /DLL /OUT:steam_api.dll /PDB:obj\fake.pdb /SUBSYSTEM:WINDOWS,6.0
if errorlevel 1 ( echo [error] fake_steam_api build failed & exit /b 1 )

cl /nologo /O2 /MT /EHsc /W3 /utf-8 /Fo:obj\ /Fd:obj\runner.pdb ^
   test_runner.cpp ^
   /link /OUT:test_runner.exe /SUBSYSTEM:CONSOLE,6.0
if errorlevel 1 ( echo [error] test_runner build failed & exit /b 1 )

copy /y "..\dist\workshop_fastload.dll" "workshop_fastload.dll" >nul
echo [ok] test artifacts built
