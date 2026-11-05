cmake_minimum_required(VERSION 3.15)
project(op2lua-newmission CXX)

# A small cross-platform host tool (NOT the 32-bit game runtime) - build it natively for whatever
# OS/arch you're on. Windows, Linux, and macOS are all supported (pure C++17 + <filesystem>).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(op2lua-newmission main.cpp)

# Some older libstdc++ need this for <filesystem>.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(op2lua-newmission PRIVATE stdc++fs)
endif()
