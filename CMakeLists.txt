cmake_minimum_required(VERSION 3.28)

project(LockFreeTripleBuffering
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Hot Reload for MSVC if supported
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# Find Threads (required on Linux/POSIX for std::thread and futex)
find_package(Threads REQUIRED)

# 1. LockFreeTripleBuffer: Header-only INTERFACE library
add_library(LockFreeTripleBuffer INTERFACE)
target_include_directories(LockFreeTripleBuffer INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(LockFreeTripleBuffer INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(LockFreeTripleBuffer INTERFACE /wd4324)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(LockFreeTripleBuffer INTERFACE -Wno-interference-size)
endif()

# 2. LockFreeTripleBufferModule: C++20 Module library using FILE_SET CXX_MODULES
# C++20 modules require Ninja or Visual Studio generator, and a compatible compiler
if(CMAKE_GENERATOR MATCHES "Visual Studio|Ninja")
    if(MSVC)
        set(MODULES_SUPPORTED_DEFAULT ON)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0")
        set(MODULES_SUPPORTED_DEFAULT ON)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14.0")
        set(MODULES_SUPPORTED_DEFAULT ON)
    else()
        set(MODULES_SUPPORTED_DEFAULT OFF)
    endif()
else()
    set(MODULES_SUPPORTED_DEFAULT OFF)
endif()

option(BUILD_CXX_MODULES "Build C++20 module library target" ${MODULES_SUPPORTED_DEFAULT})

if(BUILD_CXX_MODULES)
    add_library(LockFreeTripleBufferModule)
    target_sources(LockFreeTripleBufferModule
        PUBLIC
            FILE_SET CXX_MODULES FILES
                modules/LockFreeTripleBuffer.ixx
    )
    target_link_libraries(LockFreeTripleBufferModule PUBLIC LockFreeTripleBuffer)
    target_compile_features(LockFreeTripleBufferModule PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(LockFreeTripleBufferModule PRIVATE -x c++-module)
    endif()
endif()

# 3. GoogleTest Dependency via FetchContent (v1.14.0)
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 4. Tests
enable_testing()

add_executable(LockFreeTripleBufferTests tests/test_main.cpp)
target_link_libraries(LockFreeTripleBufferTests PRIVATE
    LockFreeTripleBuffer
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
target_compile_features(LockFreeTripleBufferTests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(LockFreeTripleBufferTests)

