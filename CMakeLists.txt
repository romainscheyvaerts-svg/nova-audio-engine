# NovaAudioEngine : moteur audio natif (serveur audio autonome) utilisé par Nova Studio. SANS JUCE.
# Copyright (C) 2026 Make Music. SPDX-License-Identifier: GPL-3.0-only
# Logiciel libre sous GNU GPL version 3 + permission supplémentaire (voir LICENSE, LICENCES.md).
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

add_executable(NovaAudioEngine WIN32
    Source/Main.cpp
    ${NOVA_EDITOR_SOURCES}
    Source/NovaAudioEngine.rc)
target_compile_options(NovaAudioEngine PRIVATE /W3 /utf-8 /permissive- /EHsc)
target_link_libraries(NovaAudioEngine PRIVATE nova_audio_core)
if(NOVA_AUDIO_WITH_ASIO)
    target_link_libraries(NovaAudioEngine PRIVATE nova_audio_asio)
endif()
set_target_properties(NovaAudioEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/NovaAudioEngine_artefacts/$<CONFIG>")

# Tests unitaires (console) : anneaux, rééchantillonneur, WAV, moteur sur périphériques nul / fichier.
add_executable(NovaAudioTests tests/Tests.cpp ${NOVA_EDITOR_SOURCES})
target_compile_options(NovaAudioTests PRIVATE /W3 /utf-8 /permissive- /EHsc)
target_link_libraries(NovaAudioTests PRIVATE nova_audio_core)
if(NOVA_AUDIO_WITH_ASIO)
    target_link_libraries(NovaAudioTests PRIVATE nova_audio_asio)
endif()
set_target_properties(NovaAudioTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")

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
