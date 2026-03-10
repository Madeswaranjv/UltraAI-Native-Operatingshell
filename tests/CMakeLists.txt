cmake_minimum_required(VERSION 3.20)

# ============================================================================
# GoogleTest Configuration
# ============================================================================

include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Force GoogleTest to use shared CRT on Windows (matches ultra_core)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# ============================================================================
# Collect ALL test files
# ============================================================================

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

# ============================================================================
# Test Executable
# ============================================================================

add_executable(ultra_tests
  ${TEST_SOURCES}
)

# Link against core library and GoogleTest
target_link_libraries(ultra_tests
  ultra_core
  gtest_main
)

# Ensure tests use C++20 (matching ultra_core)
target_compile_features(ultra_tests PRIVATE cxx_std_20)

# Include paths for test access to source headers
target_include_directories(ultra_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
)

# ============================================================================
# Structured binding support for MSVC
# ============================================================================
# MSVC requires explicit flag for C++17+ features like structured bindings
if(MSVC)
  target_compile_options(ultra_tests PRIVATE /std:c++latest)
endif()

# ============================================================================
# Discover and register all Google Tests
# ============================================================================

gtest_discover_tests(ultra_tests)
