cmake_minimum_required(VERSION 3.16)
project(reyes C CXX)

# The scene files are C++17; the renderer may be C++ or C (add your .c files
# to RENDERER_SOURCES below; everything that crosses between the scenes and
# the renderer already has C linkage).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # plain C++17, no GNU extensions
set(CMAKE_C_STANDARD 11)

# Release unless asked otherwise (multi-config generators like Visual Studio
# pick the configuration at build time instead).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Your renderer. Add files here as you split it up (C++ or C).
set(RENDERER_SOURCES
  src/main.cpp
  src/ri.cpp
  src/shaders.cpp
)

# The scene files are compiled as-is; 0_scene_table.cpp lists their entry
# points. Add your own scenes here and to that table.
set(SCENE_SOURCES
  scenes/0_scene_table.cpp
  scenes/1_TestScenes.cpp
  scenes/2_IntermediateScenes/Rocket.cpp
  scenes/2_IntermediateScenes/Explorer.cpp
  scenes/2_IntermediateScenes/McQueen.cpp
  scenes/2_IntermediateScenes/Buzz.cpp
  scenes/2_IntermediateScenes/WallE.cpp
  scenes/2_IntermediateScenes/Gumbo.cpp
  scenes/3_CinematicScenes/RadiatorSprings.cpp
  scenes/3_CinematicScenes/AndysRoom.cpp
  scenes/3_CinematicScenes/WallEMovie.cpp
  scenes/OriginalScene.cpp
)

add_executable(reyes ${RENDERER_SOURCES} ${SCENE_SOURCES})
target_include_directories(reyes PRIVATE include)
target_include_directories(reyes SYSTEM PRIVATE third_party) # no warnings from stb

add_executable(depth_fog src/depth_fog.cpp)
target_include_directories(depth_fog SYSTEM PRIVATE third_party)

if(MSVC)
  target_compile_options(reyes PRIVATE /W4)
  target_compile_definitions(reyes PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(reyes PRIVATE -Wall -Wextra)
endif()

# The scene files cannot be edited, so do not warn about one thing they do
# on purpose: CheckerParams cp = { 8, 4 }; leaves the sub-shader fields zero
# (NULL means the default checks).
if(NOT MSVC)
  set_source_files_properties(${SCENE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
endif()
