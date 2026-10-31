@echo off
rem NovaAudioEngine - Copyright (C) 2026 Make Music. SPDX-License-Identifier: GPL-3.0-only
rem Construit NovaAudioEngine.exe (moteur audio natif de Nova Studio, SANS JUCE) + tests, avec MSVC + CMake + Ninja.
rem   build.bat          sans ASIO (WASAPI, fichier, nul)
rem   build.bat asio     avec ASIO + pilote factice de test (SDK ASIO dans %NOVA_ASIO_SDK%)
rem
rem Sources attendues (jamais copiees dans le depot) :
rem   %NOVA_LIBS%\vst3sdk         (defaut D:\1 WORK\CODE\_libs\ara-host-sources) : voir native-host\build.bat
rem   %NOVA_ASIO_SDK%             (defaut D:\1 WORK\CODE\_libs\asiosdk\ASIOSDK) : SDK ASIO 2.3.4 telecharge sur
rem                               https://www.steinberg.net/asiosdk (ASIO-SDK_2.3.4_2025-10-15.zip)
rem LICENCE : NovaAudioEngine est un logiciel libre sous GNU GPL v3 (LICENSE, LICENCES.md) ; le SDK ASIO
rem est utilise sous sa licence GPLv3. Etapes detaillees et sources exactes : README.md.
rem Resultat : native-audio\build\NovaAudioEngine_artefacts\Release\NovaAudioEngine.exe
setlocal
cd /d "%~dp0"
set VSLANG=1033
if "%NOVA_LIBS%"=="" set "NOVA_LIBS=D:/1 WORK/CODE/_libs/ara-host-sources"
if "%NOVA_ASIO_SDK%"=="" set "NOVA_ASIO_SDK=D:/1 WORK/CODE/_libs/asiosdk/ASIOSDK"
set "WITH_ASIO=OFF"
if /i "%1"=="asio" set "WITH_ASIO=ON"
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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DNOVA_LIBS=%NOVA_LIBS%" "-DNOVA_ASIO_SDK=%NOVA_ASIO_SDK%" -DNOVA_AUDIO_WITH_ASIO=%WITH_ASIO% || exit /b 1
cmake --build build --config Release || exit /b 1
echo Construit : %~dp0build\NovaAudioEngine_artefacts\Release\NovaAudioEngine.exe (ASIO=%WITH_ASIO%)
exit /b 0
