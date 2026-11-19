cmake_minimum_required(VERSION 3.24)
project(CompanionAI VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CompanionAI cible exclusivement Windows 10/11 (Win32, D3D11, WASAPI).")
endif()

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
option(COMPANION_WITH_LLAMA   "Cerveau local via llama.cpp (external/llama.cpp)"     ON)
option(COMPANION_LLM_VULKAN   "llama.cpp sur GPU AMD via Vulkan (nécessite le Vulkan SDK)" ON)
option(COMPANION_WITH_WHISPER "STT via whisper.cpp (external/whisper.cpp)"          ON)
option(COMPANION_WITH_ONNX    "TTS Piper via ONNX Runtime (prebuilt, ONNXRUNTIME_ROOT)" ON)
option(COMPANION_WITH_ESPEAK  "Phonemisation espeak-ng pour Piper (ESPEAK_NG_ROOT)"  ON)
option(COMPANION_WITH_QWENTTS "Voix neuronale Qwen3-TTS via qwentts.cpp (external/qwentts.cpp)" ON)
# Processeur visé par les calculs du cerveau (ggml) :
#   native : la machine qui compile (build perso) ; zen4 : Ryzen 7000/9000 (AVX-512, l'installateur) ;
#   avx2 : n'importe quel PC x64 depuis ~2015.
set(COMPANION_CPU_TARGET "native" CACHE STRING "native | zen4 | avx2")
set_property(CACHE COMPANION_CPU_TARGET PROPERTY STRINGS native zen4 avx2)
set(ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/external/onnxruntime" CACHE PATH "Package onnxruntime-win-x64-*")
set(ESPEAK_NG_ROOT   "${CMAKE_SOURCE_DIR}/external/espeak-ng"   CACHE PATH "Install espeak-ng (include/, lib/, espeak-ng-data/)")

# C++17 pour les dépendances (llama.cpp ne compile pas en C++20 sous MSVC : littéraux u8 -> char8_t) ;
# Omnis lui-même est en C++20 (propriété de la cible plus bas).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ----------------------------------------------------------------------------
# Dépendances
# ----------------------------------------------------------------------------
if(MINGW)
    # MinGW vise Windows XP par défaut : ggml a besoin des API de Windows 10 (THREAD_POWER_THROTTLING…).
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# SQLite3 (amalgamation, FTS5 activé) — MemoryDB
FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3)
add_library(sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_DQS=0
                                           SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION)

# ggml : UNE seule copie, partagée par llama.cpp et whisper.cpp.
# llama.cpp est ajouté en premier ; whisper.cpp réutilise la cible ggml existante.
# Garde des versions de llama.cpp et whisper.cpp proches (même époque de ggml).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(COMPANION_CPU_TARGET STREQUAL "native")
    set(GGML_NATIVE ON CACHE BOOL "" FORCE)
else()
    # Build distribuable : jeu d'instructions fixé (jamais celui de la machine de build, ex. AMX d'un serveur).
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    foreach(f AVX AVX2 FMA F16C BMI2)
        set(GGML_${f} ON CACHE BOOL "" FORCE)
    endforeach()
    set(_zen4 OFF)
    if(COMPANION_CPU_TARGET STREQUAL "zen4")
        set(_zen4 ON)
    endif()
    foreach(f AVX512 AVX512_VBMI AVX512_VNNI)
        set(GGML_${f} ${_zen4} CACHE BOOL "" FORCE)
    endforeach()
    # BF16 : MSVC refuse les conversions __m512 -> __m512bh de ggml (sgemm) ; sans effet sur les modèles Q4_K_M.
    if(MSVC)
        set(GGML_AVX512_BF16 OFF CACHE BOOL "" FORCE)
    else()
        set(GGML_AVX512_BF16 ${_zen4} CACHE BOOL "" FORCE)
    endif()
    set(GGML_AVX_VNNI OFF CACHE BOOL "" FORCE)
    set(GGML_AMX_TILE OFF CACHE BOOL "" FORCE)
    set(GGML_AMX_INT8 OFF CACHE BOOL "" FORCE)
    set(GGML_AMX_BF16 OFF CACHE BOOL "" FORCE)
endif()
set(GGML_CUDA   OFF CACHE BOOL "" FORCE)
set(GGML_HIP    OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN ${COMPANION_LLM_VULKAN} CACHE BOOL "" FORCE)  # RX 9060 XT ; whisper reste sur CPU
if(MINGW)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)  # pool de threads de ggml : pas de libgomp-1.dll à livrer
endif()

if(COMPANION_WITH_LLAMA)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/llama.cpp/CMakeLists.txt")
        message(FATAL_ERROR "Clone llama.cpp dans external/llama.cpp ou passe -DCOMPANION_WITH_LLAMA=OFF")
    endif()
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)  # vision locale (capture d'écran)
    add_subdirectory(external/llama.cpp EXCLUDE_FROM_ALL)
endif()

if(COMPANION_WITH_WHISPER)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/whisper.cpp/CMakeLists.txt")
        message(FATAL_ERROR "Clone whisper.cpp dans external/whisper.cpp ou passe -DCOMPANION_WITH_WHISPER=OFF")
    endif()
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/whisper.cpp EXCLUDE_FROM_ALL)
endif()

if(COMPANION_WITH_ONNX)
    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH)
    find_library(ORT_LIBRARY onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
        message(FATAL_ERROR "ONNX Runtime introuvable dans ${ONNXRUNTIME_ROOT} (ou -DCOMPANION_WITH_ONNX=OFF)")
    endif()
    add_library(onnxruntime::onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ORT_LIBRARY}"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}")
endif()

if(COMPANION_WITH_ONNX AND COMPANION_WITH_ESPEAK)
    find_path(ESPEAK_INCLUDE_DIR espeak-ng/speak_lib.h PATHS "${ESPEAK_NG_ROOT}/include" NO_DEFAULT_PATH)
    find_library(ESPEAK_LIBRARY NAMES espeak-ng libespeak-ng PATHS "${ESPEAK_NG_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT ESPEAK_INCLUDE_DIR OR NOT ESPEAK_LIBRARY)
        message(FATAL_ERROR "espeak-ng introuvable dans ${ESPEAK_NG_ROOT} (ou -DCOMPANION_WITH_ESPEAK=OFF)")
    endif()
endif()

# Voix neuronale : qwentts.cpp embarque son propre ggml (fork, GGML_MAX_NAME=128), incompatible
# avec celui de llama.cpp dans un même binaire. Construite à part en qwen.dll (seuls les qt_* exportés),
# chargée à l'exécution : DLL absente = repli Piper, sans erreur.
if(COMPANION_WITH_QWENTTS)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/qwentts.cpp/src/qwen.h")
        message(FATAL_ERROR "external/qwentts.cpp absent (git clone --recurse-submodules, ou -DCOMPANION_WITH_QWENTTS=OFF)")
    endif()
    include(ExternalProject)
    ExternalProject_Add(qwentts
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/qwentts.cpp"
        BINARY_DIR "${CMAKE_BINARY_DIR}/qwentts"
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DQWEN_SHARED=ON -DBUILD_SHARED_LIBS=OFF
                   -DGGML_VULKAN=${COMPANION_LLM_VULKAN} -DGGML_NATIVE=${GGML_NATIVE}
                   # Son ggml (fork) ne compile pas en AVX-512 ; la voix tourne sur la carte, AVX2 suffit en repli.
                   -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON
                   -DGGML_AVX512=OFF -DGGML_AVX512_VBMI=OFF -DGGML_AVX512_VNNI=OFF -DGGML_AVX512_BF16=OFF
                   $<$<BOOL:${MINGW}>:-DGGML_OPENMP=OFF>
                   $<$<BOOL:${CMAKE_TOOLCHAIN_FILE}>:-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}>
                   $<$<BOOL:${MINGW}>:-DCMAKE_SHARED_LINKER_FLAGS=-static>
        BUILD_COMMAND ${CMAKE_COMMAND} --build . --config Release --target qwen
        INSTALL_COMMAND ""
        BUILD_ALWAYS OFF)
endif()

# ----------------------------------------------------------------------------
# Exécutable
# ----------------------------------------------------------------------------
add_executable(CompanionAI WIN32
    src/main.cpp
    src/audio/OmnisAnalyzer.cpp
    src/audio/VoiceEmotion.cpp
    src/audio/WasapiLoopback.cpp
    src/core/AgentOrchestrator.cpp
    src/core/Config.cpp
    src/core/LocalBrain.cpp
    src/core/MemoryDB.cpp
    src/core/Phrases.cpp
    src/core/MixLexicon.cpp
    src/core/ModelPlanner.cpp
    src/core/HardwareProbe.cpp
    src/core/MemeBank.cpp
    src/core/PermissionGate.cpp
    src/core/ResourceGovernor.cpp
    src/core/GamePerfMonitor.cpp
    src/modules/GameCoach.cpp
    src/modules/ScreenCapture.cpp
    src/modules/SelfTrainer.cpp
    src/modules/SystemControl.cpp
    src/modules/Soundboard.cpp
    src/network/ClaudeClient.cpp
    src/network/HttpClient.cpp
    src/network/WebResearcher.cpp
    src/network/WebSurf.cpp
    src/audio/VoiceEngine.cpp
    src/audio/NeuralVoice.cpp
    src/audio/Expressivity.cpp
    src/audio/VoiceColor.cpp
    src/audio/DiscordBridge.cpp
    src/audio/SpeakerId.cpp
    src/core/CallPeople.cpp
    src/core/RageBait.cpp
    src/core/BaitBank.cpp
    src/modules/DesktopPilot.cpp
    src/modules/DiscordSpeaking.cpp
    src/modules/VideoStudy.cpp
    src/modules/VoiceoverWords.cpp
    src/modules/WatchAlong.cpp
    src/audio/TextNormalizerFr.cpp
    src/render/OverlayWindow.cpp
    src/render/CartoonAvatar.cpp
    src/render/CaptionBar.cpp
    src/ui/ConfigView.cpp
    src/ui/ConfigWindow.cpp
)

target_include_directories(CompanionAI PRIVATE src)
set_target_properties(CompanionAI PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(CompanionAI PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    $<$<BOOL:${COMPANION_WITH_LLAMA}>:COMPANION_WITH_LLAMA=1>
    $<$<BOOL:${COMPANION_WITH_LLAMA}>:COMPANION_WITH_MTMD=1>
    $<$<BOOL:${COMPANION_WITH_WHISPER}>:COMPANION_WITH_WHISPER=1>
    $<$<BOOL:${COMPANION_WITH_ONNX}>:COMPANION_WITH_ONNX=1>
    $<$<AND:$<BOOL:${COMPANION_WITH_ONNX}>,$<BOOL:${COMPANION_WITH_ESPEAK}>>:COMPANION_WITH_ESPEAK=1>
    $<$<BOOL:${COMPANION_WITH_QWENTTS}>:COMPANION_WITH_QWENTTS=1>
    $<$<STREQUAL:${COMPANION_CPU_TARGET},zen4>:COMPANION_REQUIRES_AVX512=1>
)
if(COMPANION_WITH_QWENTTS)
    target_include_directories(CompanionAI PRIVATE "${CMAKE_SOURCE_DIR}/external/qwentts.cpp/src")  # types seulement
    add_dependencies(CompanionAI qwentts)
endif()

if(MSVC)
    target_compile_options(CompanionAI PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    set_property(TARGET CompanionAI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET CompanionAI PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    target_compile_options(CompanionAI PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()
if(MINGW)
    # wWinMain + runtime C++ embarqué (aucune DLL de GCC à livrer).
    target_link_options(CompanionAI PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(CompanionAI PRIVATE
    nlohmann_json::nlohmann_json
    sqlite3
    d3d11 dxgi dcomp d2d1 dwrite
    ole32 oleaut32 uuid avrt pdh shell32 user32 windowscodecs winhttp advapi32
    mfplat mfreadwrite mfuuid bcrypt
    $<$<BOOL:${COMPANION_WITH_LLAMA}>:llama>
    $<$<BOOL:${COMPANION_WITH_LLAMA}>:mtmd>
    $<$<BOOL:${COMPANION_WITH_WHISPER}>:whisper>
    $<$<BOOL:${COMPANION_WITH_ONNX}>:onnxruntime::onnxruntime>
)

if(COMPANION_WITH_ONNX AND COMPANION_WITH_ESPEAK)
    target_include_directories(CompanionAI PRIVATE "${ESPEAK_INCLUDE_DIR}")
    target_link_libraries(CompanionAI PRIVATE "${ESPEAK_LIBRARY}")
endif()

# ----------------------------------------------------------------------------
# Post-build : assets (personnage 2D, modèles) et DLL copiés à côté de l'exécutable.
# ----------------------------------------------------------------------------
add_custom_command(TARGET CompanionAI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:CompanionAI>/assets")

if(COMPANION_WITH_ONNX)
    add_custom_command(TARGET CompanionAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:CompanionAI>")
endif()
if(COMPANION_WITH_ONNX AND COMPANION_WITH_ESPEAK AND EXISTS "${ESPEAK_NG_ROOT}/bin/espeak-ng.dll")
    add_custom_command(TARGET CompanionAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ESPEAK_NG_ROOT}/bin/espeak-ng.dll" "$<TARGET_FILE_DIR:CompanionAI>")
endif()
if(COMPANION_WITH_QWENTTS)
    add_custom_command(TARGET CompanionAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/qwentts/$<IF:$<BOOL:${MSVC}>,Release/,>$<IF:$<BOOL:${MINGW}>,libqwen.dll,qwen.dll>"
                "$<TARGET_FILE_DIR:CompanionAI>")
endif()

# ----------------------------------------------------------------------------
# Installation (cmake --install build --prefix <dossier>) : ce que l'installateur livre.
# Les modèles NE sont PAS livrés : Omnis télécharge au 1er lancement ceux qui conviennent au PC.
# ----------------------------------------------------------------------------
install(TARGETS CompanionAI RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets PATTERN ".gitkeep" EXCLUDE)
install(FILES companion.example.json README.md installer/THIRD_PARTY.txt DESTINATION .)
if(COMPANION_WITH_ONNX)
    install(FILES "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll" DESTINATION .)
endif()
if(COMPANION_WITH_ONNX AND COMPANION_WITH_ESPEAK)
    foreach(dll espeak-ng.dll libespeak-ng.dll)
        if(EXISTS "${ESPEAK_NG_ROOT}/bin/${dll}")
            install(FILES "${ESPEAK_NG_ROOT}/bin/${dll}" DESTINATION .)
        endif()
    endforeach()
    # Piper a besoin des données de phonèmes à côté de l'exe (main.cpp : <exe>/espeak-ng-data).
    if(EXISTS "${ESPEAK_NG_ROOT}/espeak-ng-data")
        install(DIRECTORY "${ESPEAK_NG_ROOT}/espeak-ng-data" DESTINATION .)
    elseif(EXISTS "${ESPEAK_NG_ROOT}/share/espeak-ng-data")
        install(DIRECTORY "${ESPEAK_NG_ROOT}/share/espeak-ng-data" DESTINATION .)
    else()
        message(WARNING "espeak-ng-data introuvable dans ${ESPEAK_NG_ROOT} : la voix Piper ne pourra pas parler.")
    endif()
endif()
if(COMPANION_WITH_QWENTTS)
    install(FILES "${CMAKE_BINARY_DIR}/qwentts/$<IF:$<BOOL:${MSVC}>,Release/,>$<IF:$<BOOL:${MINGW}>,libqwen.dll,qwen.dll>"
            DESTINATION . RENAME qwen.dll)
endif()
