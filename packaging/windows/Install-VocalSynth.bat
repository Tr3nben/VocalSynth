@echo off
setlocal EnableExtensions
title VocalSynth installieren

rem ----------------------------------------------------------------------
rem  Installiert VocalSynth (VST3 + CLAP) jeweils in einen eigenen
rem  Unterordner "VocalSynth". Danach in FL Studio nur noch nach Plugins
rem  suchen lassen (Options > Manage plugins > Find installed plugins).
rem
rem  Bewusst ohne "if (...)"-Bloecke: Klammern im Pfad (z. B. ein Ordner
rem  "VocalSynth (1)") wuerden solche Bloecke in cmd zerbrechen.
rem ----------------------------------------------------------------------

rem Administratorrechte holen (noetig fuer C:\Program Files\Common Files)
net session >nul 2>&1
if not errorlevel 1 goto :admin
echo Fordere Administratorrechte an ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:admin

set "SRC=%~dp0"
if exist "%SRC%VST3\" goto :havesource
echo FEHLER: Ordner "VST3" nicht gefunden. Bitte zuerst die ZIP-Datei komplett
echo entpacken und dieses Skript aus dem entpackten Ordner starten.
goto :fail
:havesource

rem Immer den 64-Bit-Ordner "C:\Program Files\Common Files" verwenden
set "COMMON=%CommonProgramW6432%"
if not defined COMMON set "COMMON=%CommonProgramFiles%"
set "VST3_ROOT=%COMMON%\VST3"
set "CLAP_ROOT=%COMMON%\CLAP"

echo.
echo  VocalSynth wird in einen eigenen Unterordner "VocalSynth" installiert.
echo.
echo  Standard-VST3-Ordner:  %VST3_ROOT%
echo  Nutzt FL Studio bei dir einen anderen VST3-Ordner, z. B. E:\VST3,
echo  dann gib ihn jetzt ein. Sonst einfach Enter druecken.
echo.
set "INPUT="
set /p "INPUT=VST3-Ordner [Enter = Standard]: "
if not defined INPUT goto :defaultroot
set "INPUT=%INPUT:"=%"
set "VST3_ROOT=%INPUT%"
:defaultroot

set "VST3_DEST=%VST3_ROOT%\VocalSynth"
set "CLAP_DEST=%CLAP_ROOT%\VocalSynth"

echo.
echo Kopiere VST3-Plugins nach: %VST3_DEST%
robocopy "%SRC%VST3" "%VST3_DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :copyfail

if not exist "%SRC%CLAP\" goto :clapdone
echo Kopiere CLAP-Plugins nach: %CLAP_DEST%
robocopy "%SRC%CLAP" "%CLAP_DEST%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :copyfail
:clapdone

rem "Aus dem Internet heruntergeladen"-Markierung entfernen
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%VST3_DEST%','%CLAP_DEST%' -Recurse -File -ErrorAction SilentlyContinue | Unblock-File" >nul 2>&1

echo.
echo  Fertig! Installiert:
echo    %VST3_DEST%\VocalSynth FX.vst3
echo    %VST3_DEST%\VocalSynth Instrument.vst3
echo    %CLAP_DEST%\VocalSynth FX.clap
echo    %CLAP_DEST%\VocalSynth Instrument.clap
echo.
echo  Naechster Schritt in FL Studio:
echo    Options ^> Manage plugins ^> "Find installed plugins"
echo    Bei einem eigenen VST3-Ordner: Er muss in der Liste der
echo    Plugin-Suchpfade von FL Studio stehen.
echo.
pause
exit /b 0

:copyfail
echo.
echo FEHLER beim Kopieren. Ist FL Studio noch geoeffnet? Bitte FL Studio
echo schliessen und das Skript erneut starten.
:fail
echo.
pause
exit /b 1
