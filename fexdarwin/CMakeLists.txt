# fexdarwin host: runs freestanding static x86-64 Linux ELF programs through FEXCore on macOS arm64.
# Stage 1 (FEXCore itself) is built from the patched FEX tree, see README.md. This project only links its static libraries.
cmake_minimum_required(VERSION 3.20)
project(fexdarwin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "")

set(FEX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fex" CACHE PATH "FEX source tree (the fex submodule, Zagrios/FEX)")
set(LKD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lkd" CACHE PATH "LKD source tree")
set(LKD_LIB "${LKD_DIR}/build/lib/liblkd.a" CACHE FILEPATH "liblkd.a")
set(FEX_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/fexcore" CACHE PATH "Build directory of FEXCore (stage 1)")
option(FEX_ASSERTIONS "FEXCore (stage 1) was built with ENABLE_ASSERTIONS" OFF)

foreach(L
    FEXCore/Source/libFEXCore.a FEXCore/Source/libFEXCore_Base.a FEXCore/Source/libJemallocDummy.a
    External/fmt/libfmt.a External/xxhash/cmake_unofficial/libxxhash.a External/cephes/libcephes_128bit.a
    External/SoftFloat-3e/libsoftfloat_3e.a)
  if (NOT EXISTS "${FEX_BUILD_DIR}/${L}")
    message(FATAL_ERROR "${FEX_BUILD_DIR}/${L} is missing. Build stage 1 first (README.md).")
  endif()
  list(APPEND FEX_LIBS "${FEX_BUILD_DIR}/${L}")
endforeach()

add_executable(fexdarwin-run
  src/main.cpp
  src/CodeValidation.cpp
  src/CrashReport.cpp
  src/GuestMemory.cpp
  src/ElfLoader.cpp
  src/LinuxAbi.cpp
  src/DarwinHostFeatures.cpp
  src/DarwinSignalDelegator.cpp
  src/DarwinSyscallHandler.cpp
  src/GuestSignals.cpp
  src/GuestThread.cpp
  src/GuardedLoadPatch.cpp
  src/HitchLog.cpp
  src/LkdGlue.cpp
  src/LkdLoader.cpp
  src/LkdShim.c)

target_include_directories(fexdarwin-run PRIVATE
  include
  ${FEX_SOURCE_DIR}/FEXCore/include
  ${FEX_BUILD_DIR}/include
  ${FEX_BUILD_DIR}/generated
  ${FEX_SOURCE_DIR}/External/fmt/include
  ${FEX_SOURCE_DIR}/External/unordered_dense/include
  ${FEX_SOURCE_DIR}/FEXHeaderUtils
  ${FEX_SOURCE_DIR}/CodeEmitter
  ${LKD_DIR}/include)
# LkdShim.c alone uses LKD internal headers.
set_source_files_properties(src/LkdShim.c PROPERTIES INCLUDE_DIRECTORIES ${LKD_DIR}/src/include COMPILE_OPTIONS -std=c11)

# These must match the definitions FEXCore itself was compiled with: several public structs change layout with them.
target_compile_definitions(fexdarwin-run PRIVATE
  ARCHITECTURE_arm64=1 FEX_DARWIN=1 FEX_DISABLE_TELEMETRY=1
  FEX_HAS_PRESERVE_ALL_ATTR=1 "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
  FEXCORE_HAS_PRESERVE_ALL_ATTR=1 "FEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
  JIT_ARM64 _LARGEFILE64_SOURCE)
if (FEX_ASSERTIONS)
  target_compile_definitions(fexdarwin-run PRIVATE ASSERTIONS_ENABLED=1)
endif()
target_compile_options(fexdarwin-run PRIVATE -Wall -Wno-trigraphs -Wno-deprecated-enum-enum-conversion -fno-strict-aliasing -fno-exceptions)
target_link_libraries(fexdarwin-run PRIVATE ${FEX_LIBS} pthread)
# LKD holds weak default hooks and strong module code: force_load keeps the strong definitions.
target_link_options(fexdarwin-run PRIVATE -Wl,-force_load,${LKD_LIB})
# -force_load hides the library from ninja: declare it so a rebuilt liblkd.a relinks the host.
set_property(TARGET fexdarwin-run APPEND PROPERTY LINK_DEPENDS ${LKD_LIB})
target_link_options(fexdarwin-run PRIVATE -Wl,-dead_strip)
