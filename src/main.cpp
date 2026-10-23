/*
 * main.cpp: render one scene by name, or a set of them.
 *
 *   reyes QuadricZoo
 *   reyes all            every scene
 *   reyes stills         every scene but the two that render a movie's frames
 *   reyes test           every scene of one tier (test, intermediate or
 *   reyes intermediate   cinematic: the three folders of scenes/)
 *   reyes cinematic
 *
 * Run from the repo root so textures/ and renders/ are found.
 */
#include "scenes.h" /* RI_SCENES, the table of scenes: scenes/0_scene_table.cpp fills it */

#include <cstdio>
#include <cstring>
#include <filesystem>

/* The scenes that render a movie's numbered frames, which "stills" skips. */
static const char *const MOVIES[] = {"SimpleAnimationMovie", "WallEMovie"};

static bool isMovie(const char *name)
{
    for (const char *movie : MOVIES)
        if (std::strcmp(name, movie) == 0)
            return true;
    return false;
}

/* Whether `what` (the command line argument) selects this scene. */
static bool selected(const char *what, const RtSceneEntry *s)
{
    if (std::strcmp(what, "all") == 0)
        return true;
    if (std::strcmp(what, "stills") == 0)
        return !isMovie(s->name);
    return std::strcmp(what, s->tier) == 0 || std::strcmp(what, s->name) == 0;
}

static void usage()
{
    std::fprintf(stderr, "usage: reyes <scene> | all | stills | test | intermediate | cinematic\n");
    const char *tier = "";
    for (const RtSceneEntry *s = RI_SCENES; s->name; ++s) {
        if (std::strcmp(tier, s->tier) != 0) {
            tier = s->tier;
            std::fprintf(stderr, "%s scenes:\n", tier);
        }
        std::fprintf(stderr, "  %s%s\n", s->name, isMovie(s->name) ? " (movie)" : "");
    }
}

/* The scenes write into renders/ by tier: RiDisplay("1_TestScenes/UVTest.png")
 * lands in renders/1_TestScenes/UVTest.png. The folders are made here, so a
 * renderer only ever has to write a file. */
static void makeRenderFolders()
{
    const char *const folders[] = {"renders/1_TestScenes", "renders/2_IntermediateScenes",
                                   "renders/3_CinematicScenes", "renders/4_OriginalScene"};
    std::error_code ignored;
    for (const char *folder : folders)
        std::filesystem::create_directories(folder, ignored);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage();
        return 1;
    }
    makeRenderFolders();

    int ran = 0;
    /* Loop over RI_SCENES, an array of {name, function, tier} entries, one per
     * scene, ending with an entry whose name is NULL. Set in scenes/0_scene_table.cpp. */
    for (const RtSceneEntry *s = RI_SCENES; s->name; ++s) {
        /* check to see if scene is one of those selected for rendering */
        if (selected(argv[1], s)) {
            std::printf("== %s\n", s->name);
            std::fflush(stdout);
            s->function();
            ++ran;
        }
    }

    if (ran == 0) {
        std::fprintf(stderr, "unknown scene '%s'\n", argv[1]);
        usage();
        return 1;
    }
    return 0;
}
