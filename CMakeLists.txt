cmake_minimum_required(VERSION 3.16)
project(Ultra VERSION 0.1.0 LANGUAGES C CXX)

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

  add_compile_options(/std:c++latest)

  add_compile_options(/W4 /WX)

  add_compile_options(/MP)

  add_compile_options(/FS)

  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

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
# Source Collection
# ============================================================================

file(GLOB_RECURSE ULTRA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

list(FILTER ULTRA_SOURCES EXCLUDE REGEX "src/main.cpp")
list(FILTER ULTRA_SOURCES EXCLUDE REGEX "AstExtractorLibClang.cpp")

# ============================================================================
# Tree-sitter Core
# ============================================================================

add_library(tree_sitter STATIC
    third_party/tree-sitter/lib/src/lib.c
)

target_include_directories(tree_sitter PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/tree-sitter/lib/include
)

if(MSVC)
    target_compile_options(tree_sitter PRIVATE /W0)
endif()

# ============================================================================
# Tree-sitter Grammars
# ============================================================================

add_library(ts_cpp STATIC
    third_party/grammars/tree-sitter-cpp/src/parser.c
    third_party/grammars/tree-sitter-cpp/src/scanner.c
)

add_library(ts_python STATIC
    third_party/grammars/tree-sitter-python/src/parser.c
    third_party/grammars/tree-sitter-python/src/scanner.c
)

add_library(ts_javascript STATIC
    third_party/grammars/tree-sitter-javascript/src/parser.c
    third_party/grammars/tree-sitter-javascript/src/scanner.c
)

add_library(ts_typescript STATIC
    third_party/grammars/tree-sitter-typescript/typescript/src/parser.c
    third_party/grammars/tree-sitter-typescript/typescript/src/scanner.c
)

add_library(ts_java STATIC
    third_party/grammars/tree-sitter-java/src/parser.c
    #third_party/grammars/tree-sitter-java/src/scanner.c
)

add_library(ts_go STATIC
    third_party/grammars/tree-sitter-go/src/parser.c
    #third_party/grammars/tree-sitter-go/src/scanner.c
)

add_library(ts_rust STATIC
    third_party/grammars/tree-sitter-rust/src/parser.c
    third_party/grammars/tree-sitter-rust/src/scanner.c
)

add_library(ts_csharp STATIC
    third_party/grammars/tree-sitter-c-sharp/src/parser.c
    third_party/grammars/tree-sitter-c-sharp/src/scanner.c
)
if(MSVC)
  target_compile_options(ts_cpp PRIVATE /W0)
  target_compile_options(ts_python PRIVATE /W0)
  target_compile_options(ts_javascript PRIVATE /W0)
  target_compile_options(ts_typescript PRIVATE /W0)
  target_compile_options(ts_java PRIVATE /W0)
  target_compile_options(ts_go PRIVATE /W0)
  target_compile_options(ts_rust PRIVATE /W0)
  target_compile_options(ts_csharp PRIVATE /W0)
endif()
# ============================================================================
# Ultra Core
# ============================================================================

add_library(ultra_core STATIC
    ${ULTRA_SOURCES}
)

target_link_libraries(ultra_core PRIVATE
    tree_sitter
    ts_cpp
    ts_python
    ts_javascript
    ts_typescript
    ts_java
    ts_go
    ts_rust
    ts_csharp
)

target_sources(ultra_core PRIVATE
    src/metrics/PerformanceMetrics.cpp
)

target_include_directories(ultra_core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/tree-sitter/lib/include
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ultra_core PRIVATE cxx_std_20)

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
# Optional LibClang
# ============================================================================

if(ULTRA_USE_LIBCLANG)
  message(WARNING "ULTRA_USE_LIBCLANG is deprecated and ignored; Ultra uses the unified Tree-sitter semantic pipeline.")
endif()


# ============================================================================
# Ultra Executable
# ============================================================================

add_executable(ultra src/main.cpp)
target_link_libraries(ultra PRIVATE ultra_core)

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