cmake_minimum_required(VERSION 3.14)
project(
  GroupChat
  VERSION 1.0
  DESCRIPTION
    "Server and client programs for a group chat system which supports sending and receiving messages.")

# Include directories
include_directories(include)

# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# Add -g to debug build flags
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -Wall -Wextra")

# Server executable
add_executable(server src/server.c)
target_link_libraries(server pthread)

# Client executable
add_executable(client src/client.c)
target_link_libraries(client pthread)