cmake_minimum_required(VERSION 3.20)
project(L3KV VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
# No specific Boost components, just header include
find_package(Threads REQUIRED)

# Lite3++ (now lite3-cpp) is a subdirectory
set(LITE3CPP_DISABLE_OBSERVABILITY OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/lite3-cpp ${CMAKE_CURRENT_SOURCE_DIR}/../lib/lite3-cpp/build)


# libconveyor
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/libconveyor ${CMAKE_CURRENT_SOURCE_DIR}/../lib/libconveyor/build)

# Executable
add_executable(l3svc
    src/main.cpp
    src/http/http_server.cpp
    src/observability/simple_metrics.cpp
)

# Set include directories for l3svc (must be AFTER add_executable)
# Set include directories for l3svc (must be AFTER add_executable)
target_include_directories(l3svc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/lite3-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/libconveyor/include
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/boost_1_89_0" # Explicitly add Boost root for includes
)

target_link_libraries(l3svc PRIVATE
    Threads::Threads
    ws2_32
    bcrypt
    lite3-cpp
    conveyor
)

if(MSVC)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DNOMINMAX)
endif()

# Optimizations for zero-copy speed
if(NOT MSVC)
    target_compile_options(l3svc PRIVATE -O3 -march=native)
endif()
