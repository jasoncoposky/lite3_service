cmake_minimum_required(VERSION 3.20)
project(lite3_service VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
set(Boost_INCLUDE_DIR "C:/Users/jason/playground/lib/boost_1_89_0")
set(Boost_LIBRARY_DIR "C:/Users/jason/playground/lib/boost_1_89_0/lib")
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# Lite3 Configuration
# We assume lite3 is cloned into extern/lite3
include_directories(extern/lite3/include)
# Lite3 is a single C file library, so we just compile it with our project
set(LITE3_SRC extern/lite3/src/lite3.c) 

# Executable
add_executable(l3svc
    src/main.cpp
    ${LITE3_SRC}
)

target_link_libraries(l3svc PRIVATE
    Boost::system
    Threads::Threads
)

# Optimizations for zero-copy speed
if(NOT MSVC)
    target_compile_options(l3svc PRIVATE -O3 -march=native)
endif()
