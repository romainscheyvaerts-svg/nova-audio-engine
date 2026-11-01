@echo off
rem NovaAudioEngine - Copyright (C) 2026 Make Music. SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-MakeMusic-NovaPro
rem Construit le moteur audio natif de Nova Studio (SANS JUCE) + tests, avec MSVC + CMake + Ninja.
rem   build.bat pro      NovaProAudio.exe : mode audio pro Nova + Nova Tune natif, SANS ASIO,
rem                      licence proprietaire de Make Music (pro\LICENCE.md). Dossier build-pro.
rem   build.bat asio     NovaAudioEngine.exe avec ASIO (GPLv3, sans Nova Tune ni mode pro) + pilote
rem                      factice de test (SDK ASIO dans %NOVA_ASIO_SDK%). Dossier build.
rem   build.bat          NovaAudioEngine.exe sans ASIO (GPLv3 : WASAPI, fichier, nul). Dossier build.
rem
rem Sources attendues (jamais copiees dans le depot) :
rem   %NOVA_LIBS%\vst3sdk         (defaut D:\1 WORK\CODE\_libs\ara-host-sources) : voir native-host\build.bat
rem   %NOVA_ASIO_SDK%             (defaut D:\1 WORK\CODE\_libs\asiosdk\ASIOSDK) : SDK ASIO 2.3.4 telecharge sur
rem                               https://www.steinberg.net/asiosdk (ASIO-SDK_2.3.4_2025-10-15.zip) ;
rem                               JAMAIS utilise par l'edition pro.
rem LICENCES : editions asio / libre = GNU GPL v3 (LICENSE, LICENCES.md) ; edition pro = licence
rem proprietaire de Make Music (pro\LICENCE.md). Etapes detaillees : README.md.
setlocal
cd /d "%~dp0"
set VSLANG=1033
if "%NOVA_LIBS%"=="" set "NOVA_LIBS=D:/1 WORK/CODE/_libs/ara-host-sources"
if "%NOVA_ASIO_SDK%"=="" set "NOVA_ASIO_SDK=D:/1 WORK/CODE/_libs/asiosdk/ASIOSDK"
set "WITH_ASIO=OFF"
set "WITH_PRO=OFF"
set "BUILD_DIR=build"
set "EXE_NAME=NovaAudioEngine"
if /i "%1"=="asio" set "WITH_ASIO=ON"
if /i "%1"=="pro" set "WITH_PRO=ON"
if /i "%1"=="pro" set "BUILD_DIR=build-pro"
if /i "%1"=="pro" set "EXE_NAME=NovaProAudio"
if not exist "%NOVA_LIBS%\vst3sdk\pluginterfaces\base\funknown.h" (
  echo VST3 SDK introuvable dans %NOVA_LIBS%\vst3sdk : voir native-host\build.bat
  exit /b 2
)
if "%WITH_ASIO%"=="ON" if not exist "%NOVA_ASIO_SDK%\common\iasiodrv.h" (
  echo SDK ASIO introuvable dans %NOVA_ASIO_SDK% : telecharger https://www.steinberg.net/asiosdk
  exit /b 2
)
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" set "VSW=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (
  echo Visual Studio C++ introuvable
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%WITH_PRO%"=="ON" (
  rem Edition pro : le chemin du SDK ASIO n'est meme pas transmis a CMake.
  cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release "-DNOVA_LIBS=%NOVA_LIBS%" -DNOVA_AUDIO_WITH_ASIO=OFF -DNOVA_AUDIO_PRO=ON -DNOVA_AUDIO_FAKE_ASIO=OFF || exit /b 1
) else (
  cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release "-DNOVA_LIBS=%NOVA_LIBS%" "-DNOVA_ASIO_SDK=%NOVA_ASIO_SDK%" -DNOVA_AUDIO_WITH_ASIO=%WITH_ASIO% -DNOVA_AUDIO_PRO=OFF || exit /b 1
)
cmake --build %BUILD_DIR% --config Release || exit /b 1
echo Construit : %~dp0%BUILD_DIR%\NovaAudioEngine_artefacts\Release\%EXE_NAME%.exe (ASIO=%WITH_ASIO% PRO=%WITH_PRO%)
exit /b 0
