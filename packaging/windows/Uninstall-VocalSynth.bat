@echo off
setlocal EnableExtensions
title VocalSynth deinstallieren

rem Bewusst ohne "if (...)"-Bloecke, siehe Install-VocalSynth.bat
net session >nul 2>&1
if not errorlevel 1 goto :admin
echo Fordere Administratorrechte an ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:admin

set "COMMON=%CommonProgramW6432%"
if not defined COMMON set "COMMON=%CommonProgramFiles%"
set "VST3_ROOT=%COMMON%\VST3"
set "CLAP_ROOT=%COMMON%\CLAP"

echo.
echo  Entfernt die Ordner "VocalSynth" aus dem VST3- und dem CLAP-Ordner.
echo  Standard-VST3-Ordner:  %VST3_ROOT%
echo.
set "INPUT="
set /p "INPUT=VST3-Ordner, in den du installiert hast [Enter = Standard]: "
if not defined INPUT goto :defaultroot
set "INPUT=%INPUT:"=%"
set "VST3_ROOT=%INPUT%"
:defaultroot

if not exist "%VST3_ROOT%\VocalSynth\" goto :novst3
rmdir /s /q "%VST3_ROOT%\VocalSynth"
echo Entfernt: %VST3_ROOT%\VocalSynth
goto :clap
:novst3
echo Nicht gefunden: %VST3_ROOT%\VocalSynth

:clap
if not exist "%CLAP_ROOT%\VocalSynth\" goto :done
rmdir /s /q "%CLAP_ROOT%\VocalSynth"
echo Entfernt: %CLAP_ROOT%\VocalSynth

:done
echo.
echo Fertig. In FL Studio danach einmal "Find installed plugins" ausfuehren.
echo.
pause
