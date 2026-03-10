cmake_minimum_required(VERSION 3.16)
project(Ultra VERSION 0.1.0 LANGUAGES CXX)

# ============================================================================
# C++ Standard Configuration
# ============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# MSVC Parallel + PDB Safety Fix
# ============================================================================

if(MSVC)

  # Latest standard
  add_compile_options(/std:c++latest)

  # Warnings
  add_compile_options(/W4 /WX)

  # Enable multi-processor compilation
  add_compile_options(/MP)

  # 🔴 CRITICAL: Fix PDB locking issue
  add_compile_options(/FS)

  # Generate Program Database per target
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

  # Disable incremental linking to avoid .ilk locking
  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} /INCREMENTAL:NO")
  set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} /INCREMENTAL:NO")

else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# ============================================================================
# Optional LibClang flag
# ============================================================================

option(ULTRA_USE_LIBCLANG "Use libclang for AST extraction" OFF)

# ============================================================================
# Source Collection (AUTO-RECONFIGURE ENABLED)
# ============================================================================

file(GLOB_RECURSE ULTRA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# Remove main.cpp from library sources
list(FILTER ULTRA_SOURCES EXCLUDE REGEX "src/main.cpp")

# Remove LibClang source unless explicitly enabled
list(FILTER ULTRA_SOURCES EXCLUDE REGEX "AstExtractorLibClang.cpp")

# ============================================================================
# Ultra Core Library
# ============================================================================

add_library(ultra_core STATIC
    ${ULTRA_SOURCES}
)

# Explicit metrics safety
target_sources(ultra_core PRIVATE
    src/metrics/PerformanceMetrics.cpp
)

target_include_directories(ultra_core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ultra_core PRIVATE cxx_std_20)

# Unique PDB for core (prevents sharing)
if(MSVC)
  set_target_properties(ultra_core PROPERTIES
    COMPILE_PDB_NAME "ultra_core"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
  )
endif()

# ============================================================================
# Platform-specific process executor
# ============================================================================

if(WIN32)
  target_sources(ultra_core PRIVATE
      src/platform/WindowsProcessExecutor.cpp
  )
else()
  target_sources(ultra_core PRIVATE
      src/platform/UnixProcessExecutor.cpp
  )
endif()

# ============================================================================
# Optional LibClang Integration
# ============================================================================

if(ULTRA_USE_LIBCLANG)
  find_package(LibClang QUIET)
  if(LibClang_FOUND)
    target_sources(ultra_core PRIVATE
        src/ast/AstExtractorLibClang.cpp
    )
    target_compile_definitions(ultra_core PRIVATE ULTRA_USE_LIBCLANG=1)
    target_include_directories(ultra_core PRIVATE ${LIBCLANG_INCLUDE_DIRS})
    target_link_libraries(ultra_core PRIVATE ${LIBCLANG_LIBRARIES})
  endif()
endif()

# ============================================================================
# Ultra Executable
# ============================================================================

add_executable(ultra src/main.cpp)
target_link_libraries(ultra PRIVATE ultra_core)

# Unique PDB for executable
if(MSVC)
  set_target_properties(ultra PROPERTIES
    COMPILE_PDB_NAME "ultra_exec"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
  )
endif()

# ============================================================================
# Testing
# ============================================================================

enable_testing()
add_subdirectory(tests)