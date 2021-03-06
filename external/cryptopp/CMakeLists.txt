cmake_minimum_required(VERSION 2.8.5 FATAL_ERROR)

project(cryptopp)

set(cryptopp_VERSION_MAJOR 5)
set(cryptopp_VERSION_MINOR 6)
set(cryptopp_VERSION_PATCH 3)

include(GNUInstallDirs)
include(TestBigEndian)
include(CheckCXXSymbolExists)

#============================================================================
# Settable options
#============================================================================

option(BUILD_TESTING "Build library tests" OFF)
option(BUILD_DOCUMENTATION "Use Doxygen to create the HTML based API documentation" OFF)

option(DISABLE_ASM "Disable ASM" OFF)
option(DISABLE_SSSE3 "Disable SSSE3" OFF)
option(DISABLE_AESNI "Disable AES-NI" OFF)
set(CRYPTOPP_DATA_DIR "" CACHE PATH "Crypto++ test data directory")

#============================================================================
# Internal compiler options
#============================================================================

set(LIB_VER ${cryptopp_VERSION_MAJOR}${cryptopp_VERSION_MINOR}${cryptopp_VERSION_PATCH})

if(CMAKE_CXX_COMPILER_ID MATCHES "Intel")
    add_definitions(-wd68 -wd186 -wd279 -wd327 -wd161 -wd3180)
endif()

# Endianess
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
    add_definitions(-DIS_BIG_ENDIAN)
endif()

if(DISABLE_ASM)
    add_definitions(-DCRYPTOPP_DISABLE_ASM)
endif()
if(DISABLE_SSSE3)
    add_definitions(-DCRYPTOPP_DISABLE_SSSE3)
endif()
if(DISABLE_AESNI)
    add_definitions(-DCRYPTOPP_DISABLE_AESNI)
endif()
if(NOT CRYPTOPP_DATA_DIR STREQUAL "")
    add_definitions(-DCRYPTOPP_DATA_DIR="${CRYPTOPP_DATA_DIR}")
endif()

#============================================================================
# Sources & headers
#============================================================================

# Library headers
file(GLOB cryptopp_HEADERS *.h)

# Test sources
file(GLOB cryptopp_SOURCES_TEST bench1.cpp bench2.cpp test.cpp validat1.cpp validat2.cpp validat3.cpp adhoc.cpp datatest.cpp regtest.cpp fipsalgt.cpp dlltest.cpp fipstest.cpp)

# Library sources
file(GLOB cryptopp_SOURCES *.cpp)
list(REMOVE_ITEM cryptopp_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/cryptlib.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/simple.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/winpipes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cryptlib_bds.cpp
        ${cryptopp_SOURCES_TEST}
        )
set(cryptopp_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/cryptlib.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cpu.cpp
        ${cryptopp_SOURCES}
        )

if(MINGW OR WIN32)
    list(APPEND cryptopp_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/winpipes.cpp)
endif()

if(MSVC AND NOT DISABLE_ASM)
    if(CMAKE_CL_64)         
        list(APPEND cryptopp_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/x64dll.asm)
        list(APPEND cryptopp_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/x64masm.asm)
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/x64dll.asm PROPERTIES COMPILE_FLAGS "/D_M_X64")
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/x64masm.asm PROPERTIES COMPILE_FLAGS "/D_M_X64")
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/rdrand.asm PROPERTIES COMPILE_FLAGS "/D_M_X64")
    else()    
             set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/x64dll.asm PROPERTIES COMPILE_FLAGS "/D_M_X86 /safeseh")
             set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/x64masm.asm PROPERTIES COMPILE_FLAGS "/D_M_X86 /safeseh")
             set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/rdrand.asm PROPERTIES COMPILE_FLAGS "/D_M_X86 /safeseh")
    endif()
    list(APPEND cryptopp_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/rdrand.asm)
        enable_language(ASM_MASM)
endif()

#============================================================================
# Compile targets
#============================================================================

add_library(cryptopp-static STATIC ${cryptopp_SOURCES})

if(NOT MSVC)
    set(COMPAT_VERSION ${cryptopp_VERSION_MAJOR}.${cryptopp_VERSION_MINOR})

    set_target_properties(cryptopp-static
            PROPERTIES
            OUTPUT_NAME cryptopp)
endif()

#============================================================================
# Third-party libraries
#============================================================================
if(WIN32)
    target_link_libraries(cryptopp-static ws2_32)
endif()

find_package(Threads)
target_link_libraries(cryptopp-static ${CMAKE_THREAD_LIBS_INIT})
