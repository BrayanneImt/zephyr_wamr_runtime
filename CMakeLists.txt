cmake_minimum_required(VERSION 3.20)

# ================================================================
# CMakeLists.txt — Zephyr + WAMR UART Runtime
# ================================================================

set(WAMR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../wasm-micro-runtime)

if(NOT EXISTS "${WAMR_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "WAMR introuvable dans ${WAMR_ROOT}\n"
        "  cd ~/zephyrproject\n"
        "  git clone https://github.com/bytecodealliance/wasm-micro-runtime.git")
endif()

# ----------------------------------------------------------------
# Configuration WAMR
#
# WAMR_BUILD_LIBC_BUILTIN = 1  (activé)
#   Fournit les fonctions C de base (memcpy, printf, math) au .wasm.
#   N'a pas de dépendances système problématiques.
# ----------------------------------------------------------------
set(WAMR_BUILD_PLATFORM     "zephyr" CACHE STRING "" FORCE)
set(WAMR_BUILD_TARGET       "XTENSA" CACHE STRING "" FORCE)
set(WAMR_BUILD_INTERP       1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_FAST_INTERP  1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_AOT          0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_JIT          0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_LIBC_WASI    0        CACHE BOOL   "" FORCE)  # désactivé
set(WAMR_BUILD_LIBC_BUILTIN 1        CACHE BOOL   "" FORCE)  # activé
set(WAMR_BUILD_SHARED_MEMORY 0       CACHE BOOL   "" FORCE)
set(WAMR_BUILD_BULK_MEMORY   0       CACHE BOOL   "" FORCE)

# 1. Zephyr EN PREMIER
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_wamr_runtime)

# 2. Charger les sources WAMR
include(${WAMR_ROOT}/build-scripts/runtime_lib.cmake)

# 3. Ajouter toutes les sources à la target app Zephyr
target_sources(app PRIVATE
    src/main.c
    ${WAMR_RUNTIME_LIB_SOURCE}
)

# 4. Includes WAMR
target_include_directories(app PRIVATE
    ${WAMR_ROOT}/core/iwasm/include
    ${WAMR_ROOT}/core/shared/include
    ${WAMR_ROOT}/core/shared/platform/zephyr
    ${WAMR_ROOT}/core/shared/platform/include
    ${WAMR_ROOT}/core/shared/mem-alloc
    ${WAMR_ROOT}/core/shared/utils
    ${WAMR_ROOT}/core/iwasm/common
    ${WAMR_ROOT}/core/iwasm/interpreter
    ${WAMR_ROOT}/core/iwasm/libraries/libc-builtin
)