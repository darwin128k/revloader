@echo off
setlocal
cd /d %~dp0

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul
if errorlevel 1 (
    echo Failed to init MSVC x86 environment
    exit /b 1
)

if not exist build mkdir build
cd build

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ..
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

set ROOT=..\
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

echo Build OK: revloader cstrike.exe -^> game root
endlocal
