# ECE/CS 285 HW1: a REYES renderer

This C++ implementation provides a REYES-style renderer in `src/ri.cpp` and
the required shaders in `src/shaders.cpp`. `reference_renders/` has every
provided scene as the reference renderer draws it: compare those images
with the outputs under `renders/`.

## Build and run

The renderer is implemented in C++17. The provided scene files use the
unchanged C interface in `include/Ri.h`.

**C++.** You need CMake (3.16 or newer) and a C++17 compiler (gcc, clang or
Visual Studio).

Tested on macOS with:

    cmake -S . -B build
    cmake --build build -j4
    ./build/reyes FirstPrimitive_sphere
    ./build/reyes OrbitWorkshop
    ./build/reyes AdaptiveDiceDemo

`CMakeLists.txt` picks a Release (optimized) build when no build type is
given, so the configure/build commands above are sufficient on Linux and macOS. Visual
Studio builds every configuration from one project, so on Windows name the
configuration when building and running:

    cmake -B build
    cmake --build build --config Release
    .\build\Release\reyes.exe FirstPrimitive_sphere

Either way, run the executable from this folder, the one that contains
`scenes/`, `textures/` and `renders/`: that is where it looks for textures
and models and where it writes the images.

The executable accepts these selectors:

    reyes <scene>        one scene, by name (reyes with no argument lists them)
    reyes test           every test scene
    reyes intermediate   every intermediate scene
    reyes cinematic      every cinematic scene
    reyes stills         everything but the two movies
    reyes all            everything

## What is where

    include/Ri.h          the interface your renderer implements; do not change it
    include/shaders.h     the four shaders and their parameter structs; do not change it
    include/envmap.h      optional latitude-longitude environment-map shader
    include/scenes.h      the table of scenes (filled in by scenes/0_scene_table.cpp)
    include/*.h (others)  helpers the scene files use: obj.h loads OBJ models,
                          shadowmap.h does shadow lookups from your depth output,
                          sceneutil.h, models.h and anim.h are shared scene code.
                          Your renderer never calls them.
    src/main.cpp          picks the scene from the table; nothing to change
    src/ri.cpp            the C++ renderer
    src/depth_fog.cpp     standalone PNG + Rz01 fog compositor
    src/shaders.cpp       CONSTANT, STANDARD, CHECKERBOARD, BUMPY, the
                          environment-map shader and Perlin noise
    rust/                 optional Rust starter; not used by the C++ build
    scenes/               the scene files, in three tiers (see below)
    renders/              where provided images go in three folders, with
                          the original render in 4_OriginalScene/
    reference_renders/    the same images from the reference renderer:
                          compare yours against these
    textures/, models/    what the scenes load
    third_party/          stb_image.h and stb_image_write.h

## The scenes

`scenes/1_TestScenes.cpp` holds the small test scenes, in the order the
Suggested Workflow needs them; each one's comment says what a correct image
looks like, and `reference_renders/` shows it. Start with
`FirstPrimitive_sphere`: one flat-colored sphere, the first picture your
renderer can draw (`_cylinder`, `_cone` and `_torus` are the same test for
the other quadrics). `scenes/2_IntermediateScenes/` and `scenes/3_CinematicScenes/`
hold the bigger ones, one file each. The scene files are the inputs to your
renderer and must not be edited (the handout says what to hand in). To add
a scene of your own, put it in a new file under `scenes/`, add the file to
`CMakeLists.txt` (the Rust build finds it by itself), and add one line to
`scenes/0_scene_table.cpp`.

The registered original still is `OrbitWorkshop` in `scenes/OriginalScene.cpp`.
It uses textured Earth and gas-giant spheres, a transparent atmosphere,
procedural displacement, quadrics, an environment-mapped chrome sphere
(`Starry_Night.jpg`), and a polygonal checkerboard floor.

Extra credit: the `ENVIRONMENT` shader reflects the camera direction about
the surface normal to sample a latitude-longitude image with `RiTexture`.
`OrbitWorkshop` demonstrates it on the small sphere in front of the central torus.

Displaced grids are re-diced when their projected edges exceed half a pixel,
up to the renderer's maximum dice rate. `AdaptiveDiceDemo` shows a small,
strongly displaced sphere that needs a finer grid than its original bound.
The shadow lookup in `include/shadowmap.h` uses the surface's slope relative
to the light for its depth bias when a normal is available; `ShadowTest`
demonstrates this with its cast shadows.

`OrbitWorkshop` writes matching color and `Rz01` depth images. To apply
depth-based fog (start distance, density, then fog RGB in [0,1]):

    ./build/depth_fog renders/4_OriginalScene/OrbitWorkshop.png \
      renders/4_OriginalScene/OrbitWorkshop.z \
      renders/4_OriginalScene/OrbitWorkshop_fog.png 5 0.045 0.28 0.43 0.56

This keeps the PNG's opacity, fogs only finite-depth surfaces, and leaves
pixels without depth untouched. Color and depth must use the same camera and resolution.

The two scenes that render a movie (`SimpleAnimationMovie` and
`WallEMovie`) write numbered frames. Turn them into an MP4 with ffmpeg (the
same command is in the comment at the top of each scene):

    ffmpeg -framerate 24 -i renders/1_TestScenes/SimpleAnimation/SimpleAnimation_%02d.png \
           -c:v libx264 -pix_fmt yuv420p -crf 18 renders/1_TestScenes/SimpleAnimation.mp4
    ffmpeg -framerate 24 -i renders/3_CinematicScenes/WallEMovie/WallEMovie_%02d.jpg \
           -c:v libx264 -pix_fmt yuv420p -crf 18 renders/3_CinematicScenes/WallEMovie.mp4

Both scenes have a background behind everything, so every frame is opaque
and the movie needs nothing more. (An MP4 has no alpha: frames with
transparent parts would need `-vf premultiply=inplace=1` first, or ffmpeg
drops the alpha without applying it and the edges turn jagged.)
