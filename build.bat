@echo off
setlocal
rem ===================================================================
rem  One-click build: Robot.exe (console) + RobotGUI.exe (Win32 GUI)
rem  Toolchain: MSYS2 MinGW-w64 g++ (static link, no external DLL deps)
rem  Usage:  build.bat          build both
rem          build.bat clean    remove intermediates then build
rem ===================================================================
set "CXX=D:\MSYS2\mingw64\bin\g++.exe"
if not exist "%CXX%" (
  echo [ERROR] compiler not found: %CXX%
  echo         edit CXX at the top of this script to your local g++.
  exit /b 1
)

cd /d "%~dp0"

set "COMMON=robot_common.cpp serial_port.cpp motion.cpp hanzi.cpp polyline.cpp gui_service.cpp"
set "STD=-std=c++17 -g -Wall -Inlohmann -static -static-libgcc -static-libstdc++"

if /i "%~1"=="clean" (
  echo [clean] removing *.o
  if exist *.o del /q *.o
)

echo [1/2] building Robot.exe (console) ...
"%CXX%" %STD% %COMMON% main.cpp -o Robot.exe
if errorlevel 1 (
  echo [FAILED] Robot.exe
  exit /b 1
)

echo [2/2] building RobotGUI.exe (Win32 GUI) ...
"%CXX%" %STD% -municode %COMMON% gui_win32.cpp -o RobotGUI.exe -lcomctl32 -lgdi32 -luser32 -lsetupapi
if errorlevel 1 (
  echo [FAILED] RobotGUI.exe
  exit /b 1
)

echo.
echo [DONE] Robot.exe and RobotGUI.exe generated.
echo        Robot.exe --dryrun     console debug run
echo        RobotGUI.exe --dryrun  GUI debug run
endlocal
