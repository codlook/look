@echo off
set PATH=C:\Program Files\CMake\bin;%PATH%
cd /d C:\Users\msi\Desktop\look\cpp\build
cmake -G "Visual Studio 18 2026" ..
if errorlevel 1 exit /b 1
cmake --build . --config Release
if errorlevel 1 exit /b 1
echo.
echo Kopyalaniyor...
if exist "C:\xampp\php" (
    copy /Y Release\look.exe C:\xampp\php\look.exe >nul
    echo look.exe kopyalandi.
)
if exist "C:\xampp\cgi-bin" (
    copy /Y Release\look-cgi.exe C:\xampp\cgi-bin\look-cgi.exe >nul 2>&1
    echo look-cgi.exe kopyalandi.
)
echo Derleme tamamlandi.
