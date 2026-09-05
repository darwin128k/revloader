@echo off
setlocal
cd /d %~dp0

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul
if errorlevel 1 (
    echo Failed to init MSVC x86 environment
    exit /b 1
)

set STANDALONE=OFF
set "HL_DIR="
if /I "%~1"=="standalone" (
    set STANDALONE=ON
    if "%~2"=="" (
        set "HL_DIR=%~dp0..\hl"
    ) else (
        set "HL_DIR=%~2"
    )
)

if not exist build mkdir build
cd build

if "%STANDALONE%"=="ON" (
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=ON -DHL_DIR="%HL_DIR%" ..
) else (
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=OFF ..
)
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)

cmake --build .
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

cd ..

set ROOT=.\
if exist ..\hw.dll (
    set ROOT=..\
) else if exist ..\..\hw.dll (
    set ROOT=..\..\
)
set CSTRIKE_ORIG=orig\cstrike.exe
set CSTRIKE_DST=%ROOT%cstrike.exe

if not exist orig mkdir orig
if not exist "%CSTRIKE_ORIG%" (
    if exist "%CSTRIKE_DST%" (
        echo Backing up current cstrike.exe to %CSTRIKE_ORIG%
        copy /Y "%CSTRIKE_DST%" "%CSTRIKE_ORIG%" >nul
    )
)

copy /Y build\cstrike.exe "%CSTRIKE_DST%" >nul
if errorlevel 1 (
    echo Built OK, but could not copy cstrike.exe to game root -- is the game running?
    exit /b 1
)

if "%STANDALONE%"=="ON" (
    echo Build OK: standalone cstrike.exe ^(GoldSrc in-process, hl from %HL_DIR%^)
) else (
    echo Build OK: revloader cstrike.exe -^> game root
)
endlocal
