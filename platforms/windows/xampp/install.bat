@echo off
title LOOK Language - XAMPP Kurulum
echo.
echo  ========================================
echo   LOOK Language - XAMPP Kurulum
echo  ========================================
echo.

REM Yonetici yetkisi kontrol
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] Yonetici yetkisi gerekli. Yeniden baslatiliyor...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

REM 1. Kur
echo [1/2] LOOK kuruluyor...
powershell -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if %errorlevel% neq 0 (
    echo.
    echo [HATA] Kurulum basarisiz.
    pause
    exit /b 1
)

REM 2. Apache yeniden baslt
echo.
echo [2/2] Apache yeniden baslatiliyor...
net stop Apache2.4 >nul 2>&1
net start Apache2.4 >nul 2>&1
if %errorlevel% equ 0 goto apache_ok

REM Servis olarak yuklenmemisse dogrudan httpd.exe
taskkill /f /im httpd.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start "" /b "C:\xampp\apache\bin\httpd.exe"
timeout /t 3 /nobreak >nul
tasklist /fi "imagename eq httpd.exe" 2>nul | find /i "httpd.exe" >nul
if %errorlevel% neq 0 (
    echo [HATA] Apache baslatılamadi. XAMPP Control Panel'den manuel baslatın.
    pause
    exit /b 1
)

:apache_ok
echo [OK] Apache calisiyor.

REM Test
echo.
timeout /t 2 /nobreak >nul
powershell -Command "$r=try{(Invoke-WebRequest 'http://localhost/test.lk' -UseBasicParsing -TimeoutSec 5).Content}catch{'HATA'}; if($r -like '*LOOK*'){'[OK] ' + $r}else{'[!] Test basarisiz: ' + $r}"

echo.
echo  ========================================
echo   Kurulum tamamlandi!
echo   Test: http://localhost/test.lk
echo  ========================================
echo.
pause
