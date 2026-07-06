@echo off
REM Build the flying double-pendulum cursor.
REM Tries MSVC (cl) first, then MinGW (g++). Produces pendulum_cursor.exe here.
setlocal

set SRC=src\main.cpp src\Physics.cpp src\SnapMode.cpp src\Renderer.cpp src\CursorController.cpp

where cl >nul 2>nul
if %errorlevel%==0 (
    echo Building with MSVC...
    cl /nologo /EHsc /O2 /std:c++17 /Iinclude %SRC% ^
        /Fe:pendulum_cursor.exe user32.lib gdi32.lib winmm.lib
    if errorlevel 1 goto :fail
    del *.obj >nul 2>nul
    goto :ok
)

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo Building with MinGW g++...
    g++ -O2 -std=c++17 -Iinclude %SRC% -o pendulum_cursor.exe ^
        -luser32 -lgdi32 -lwinmm -static
    if errorlevel 1 goto :fail
    goto :ok
)

echo No C++ compiler found. Install Visual Studio Build Tools or MinGW-w64.
exit /b 1

:ok
echo.
echo Built pendulum_cursor.exe  --  run it, then press Ctrl+C to stop.
exit /b 0

:fail
echo Build failed.
exit /b 1
