# NovaAudioEngine : moteur audio natif (serveur audio autonome) utilisé par Nova Studio. SANS JUCE.
# Copyright (C) 2026 Make Music. SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-MakeMusic-NovaPro
#
# TROIS ÉDITIONS, construites dans des dossiers séparés (build.bat) :
#   pro    NovaProAudio.exe : « Mode audio pro Nova » (WASAPI exclusif / faible latence, agrégation,
#          assistant) + Nova Tune natif. AUCUN code du SDK ASIO. Licence PROPRIÉTAIRE de Make Music
#          (pro/LICENCE.md) ; SDK VST3 sous MIT (notice gardée). Dossier build-pro.
#   asio   NovaAudioEngine.exe avec le SDK ASIO de Steinberg : GPLv3 (LICENSE, LICENCES.md), SANS Nova
#          Tune ni mode pro. Dossier build.
#   libre  NovaAudioEngine.exe sans ASIO (GPLv3, essais). Dossier build.
# Make Music est seul auteur du code du moteur : le code commun aux éditions (Source/*.cpp, sauf
# NovaTune.*) est publié sous GPLv3 dans les éditions asio / libre et sous licence Make Music dans
# l'édition pro. Source/pro/* et Source/NovaTune.* ne sont JAMAIS compilés dans une édition GPL.
# Protocole avec le client : PROTOCOLE.md. Construction pas à pas : README.md.
#
# Sources externes (jamais copiées dans le dépôt, voir README.md pour les obtenir) :
#   VST3 SDK  https://github.com/steinbergmedia/vst3sdk (tag v3.8.1_build_84, MIT)   -> NOVA_LIBS/vst3sdk
#   ASIO SDK  https://www.steinberg.net/asiosdk (2.3.4 du 15/10/2025, utilisé sous GPLv3) -> NOVA_ASIO_SDK
#
# ASIO : option NOVA_AUDIO_WITH_ASIO (OFF par défaut pour un essai rapide ; la version livrée dans
# l'installateur de Nova Studio est construite avec, par « build.bat asio », appelé par desktop/build.py).
cmake_minimum_required(VERSION 3.22)
project(NovaAudioEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(NOVA_LIBS "D:/1 WORK/CODE/_libs/ara-host-sources" CACHE PATH "Dossier des sources du SDK VST3")
set(NOVA_VST3_SDK_DIR "${NOVA_LIBS}/vst3sdk" CACHE PATH "VST3 SDK")
set(NOVA_ASIO_SDK "D:/1 WORK/CODE/_libs/asiosdk/ASIOSDK" CACHE PATH "ASIO SDK de Steinberg (dossier contenant common/asio.h)")
option(NOVA_AUDIO_WITH_ASIO "Pilotes ASIO (SDK ASIO de Steinberg, sous GPLv3)" OFF)
option(NOVA_AUDIO_FAKE_ASIO "Pilote ASIO factice pour les tests (jamais livré)" ON)
option(NOVA_AUDIO_PRO "Édition Nova Pro Audio (propriétaire, sans ASIO, avec Nova Tune natif)" OFF)
if(NOVA_AUDIO_PRO AND NOVA_AUDIO_WITH_ASIO)
    message(FATAL_ERROR "L'édition pro ne contient aucun code du SDK ASIO : NOVA_AUDIO_WITH_ASIO doit être OFF")
endif()
# Nova Tune natif : uniquement dans l'édition pro (Romain refuse qu'il parte sous GPLv3, 30/10/2026).
set(NOVA_AUDIO_WITH_NOVATUNE ${NOVA_AUDIO_PRO})

include("${CMAKE_CURRENT_SOURCE_DIR}/../native-common/vst3_hosting.cmake")

set(NOVA_VSTHOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../native-host/Source")

# Coeur du moteur (sans ASIO) : partagé par l'exe et les tests.
add_library(nova_audio_core STATIC
    Source/Resampler.cpp
    Source/WavFile.cpp
    Source/Devices.cpp
    Source/DeviceWasapi.cpp
    Source/Engine.cpp
    Source/MonitorChain.cpp
    "${NOVA_VSTHOST_DIR}/VstInstance.cpp"
    "${NOVA_VSTHOST_DIR}/Guard.cpp")
if(NOVA_AUDIO_PRO)
    # Fichiers PROPRIÉTAIRES (jamais dans une édition GPL, jamais publiés) : Nova Tune natif et mode pro.
    target_sources(nova_audio_core PRIVATE
        Source/NovaTune.cpp
        Source/pro/AdaptiveResampler.cpp
        Source/pro/DeviceProWasapi.cpp
        Source/pro/ProCalibrate.cpp)
    target_compile_definitions(nova_audio_core PUBLIC NOVA_AUDIO_PRO=1 NOVA_AUDIO_WITH_NOVATUNE=1)
endif()
target_include_directories(nova_audio_core PUBLIC Source "${NOVA_COMMON_DIR}" "${NOVA_VSTHOST_DIR}")
target_compile_options(nova_audio_core PRIVATE /W3 /utf-8 /permissive- /EHsc)
target_link_libraries(nova_audio_core PUBLIC nova_vst3_hosting ole32 oleaut32 uuid user32 gdi32 shell32 avrt advapi32 winmm)

if(NOVA_AUDIO_WITH_ASIO)
    foreach(f "${NOVA_ASIO_SDK}/common/asio.h" "${NOVA_ASIO_SDK}/common/iasiodrv.h")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "SDK ASIO introuvable : ${f} (voir build.bat)")
        endif()
    endforeach()
    # Tout le code issu du SDK ASIO est dans cette bibliothèque (en-têtes du SDK inclus tels quels).
    add_library(nova_audio_asio STATIC Source/DeviceAsio.cpp)
    target_include_directories(nova_audio_asio PRIVATE "${NOVA_ASIO_SDK}/common")
    target_link_libraries(nova_audio_asio PUBLIC nova_audio_core)
    target_compile_options(nova_audio_asio PRIVATE /W3 /utf-8 /EHsc)
    target_compile_definitions(nova_audio_core PUBLIC NOVA_AUDIO_WITH_ASIO=1)
endif()

if(NOVA_AUDIO_PRO)
    set(NOVA_AUDIO_RC Source/pro/NovaProAudio.rc)
else()
    set(NOVA_AUDIO_RC Source/NovaAudioEngine.rc)
endif()
add_executable(NovaAudioEngine WIN32
    Source/Main.cpp
    ${NOVA_EDITOR_SOURCES}
    ${NOVA_AUDIO_RC})
target_compile_options(NovaAudioEngine PRIVATE /W3 /utf-8 /permissive- /EHsc)
target_link_libraries(NovaAudioEngine PRIVATE nova_audio_core)
if(NOVA_AUDIO_WITH_ASIO)
    target_link_libraries(NovaAudioEngine PRIVATE nova_audio_asio)
endif()
set_target_properties(NovaAudioEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/NovaAudioEngine_artefacts/$<CONFIG>")
if(NOVA_AUDIO_PRO)
    set_target_properties(NovaAudioEngine PROPERTIES OUTPUT_NAME NovaProAudio)
endif()

# Tests unitaires (console) : anneaux, rééchantillonneur, WAV, moteur sur périphériques nul / fichier.
set(NOVA_TEST_SOURCES tests/Tests.cpp)
if(NOVA_AUDIO_PRO)
    list(APPEND NOVA_TEST_SOURCES tests/pro/ProTests.cpp)   # faux WASAPI (tests/pro/FakeWasapi.h), propriétaire
endif()
add_executable(NovaAudioTests ${NOVA_TEST_SOURCES} ${NOVA_EDITOR_SOURCES})
target_compile_options(NovaAudioTests PRIVATE /W3 /utf-8 /permissive- /EHsc)
target_link_libraries(NovaAudioTests PRIVATE nova_audio_core)
if(NOVA_AUDIO_WITH_ASIO)
    target_link_libraries(NovaAudioTests PRIVATE nova_audio_asio)
endif()
set_target_properties(NovaAudioTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")

# Banc de Nova Tune natif (fichiers -> fichiers) : comparé à la version web par tests/novaTuneNatif.test.ts.
if(NOVA_AUDIO_WITH_NOVATUNE)
    add_executable(NovaTuneBench tests/NovaTuneBench.cpp ${NOVA_EDITOR_SOURCES})
    target_compile_options(NovaTuneBench PRIVATE /W3 /utf-8 /permissive- /EHsc)
    target_link_libraries(NovaTuneBench PRIVATE nova_audio_core)
    set_target_properties(NovaTuneBench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()

# Pilote ASIO factice (DLL COM chargée directement par le moteur, sans inscription système).
if(NOVA_AUDIO_FAKE_ASIO AND NOVA_AUDIO_WITH_ASIO)
    add_library(NovaFakeAsio SHARED fake-asio/FakeAsio.cpp fake-asio/FakeAsio.def)
    target_include_directories(NovaFakeAsio PRIVATE "${NOVA_ASIO_SDK}/common")
    target_compile_options(NovaFakeAsio PRIVATE /W3 /utf-8 /EHsc)
    target_compile_definitions(NovaFakeAsio PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(NovaFakeAsio PRIVATE avrt ole32 winmm)
    set_target_properties(NovaFakeAsio PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()
