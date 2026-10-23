#include "Ri.h"
#include "envmap.h"
#include "sceneutil.h"

static void RenderOrbitWorkshop(bool depth)
{
    RtFloat fov = 42.0f;
    RtColor midnight = {0.018f, 0.035f, 0.09f};
    RtColor floorColor = {0.58f, 0.62f, 0.70f};
    RtColor ruby = {0.9f, 0.08f, 0.12f};
    RtColor aqua = {0.04f, 0.58f, 0.78f};
    RtColor gold = {1.0f, 0.48f, 0.06f};
    RtColor opaque = {1, 1, 1};
    RtColor glass = {0.24f, 0.64f, 0.95f};
    RtColor glassOpacity = {0.12f, 0.12f, 0.12f};
    CheckerParams floorChecks = {12, 24};
    BumpParams bumps = {0.075f, -1, 3};
    TextureParams earth = {0}, gas = {1};
    EnvMapParams reflections = {2};

    RtPoint backdrop[4] = {
        {-40, -24, 42}, {40, -24, 42}, {40, 24, 42}, {-40, 24, 42}
    };
    const RtInt quad[1] = {4};
    const RtInt indices[4] = {0, 1, 2, 3};
    RtPoint floor[4] = {
        {-30, -2.5f, 3}, {-30, -2.5f, 48},
        {30, -2.5f, 48}, {30, -2.5f, 3}
    };

    RiBegin(RI_NULL);
        RiFormat(640, 480);
        RiPixelSamples(2, 2);
        if (depth)
            RiDisplay("4_OriginalScene/OrbitWorkshop.z", "zfile", "z");
        else
            RiDisplay("4_OriginalScene/OrbitWorkshop.png", "", "");
        RiMakeTexture("earth.jpg", 0);
        RiMakeTexture("gasgiant.png", 1);
        RiMakeTexture("Starry_Night.jpg", 2);
        RiFrameBegin(0);
            RiProjection(RI_PERSPECTIVE, "fov", &fov);
            RiRotate(-9.0f, 1, 0, 0);
            RiWorldBegin();
                RiOpacity(opaque);
                RiSurface(CONSTANT, NULL, 0);
                RiColor(midnight);
                RiPointsPolygons(1, quad, indices, backdrop, NULL, NULL);

                RiSurface(CHECKERBOARD, &floorChecks, sizeof floorChecks);
                RiColor(floorColor);
                RiPointsPolygons(1, quad, indices, floor, NULL, NULL);

                RiSurface(NULL, NULL, 0);
                RiColor(ruby);
                RiTransformBegin();
                    RiTranslate(-2.15f, -2.45f, 9.5f);
                    RiRotate(-90, 1, 0, 0);
                    RiCylinder(0.58f, 0, 1.15f, 360);
                RiTransformEnd();
                RiTransformBegin();
                    RiTranslate(-2.15f, -0.20f, 9.5f);
                    RiRotate(-90, 1, 0, 0);
                    RiSurface(TexturedStandard, &earth, sizeof earth);
                    RiSphere(0.9f, -0.9f, 0.9f, 360);
                RiTransformEnd();

                RiSurface(NULL, NULL, 0);
                RiColor(gold);
                RiTransformBegin();
                    RiTranslate(0, -2.45f, 8.2f);
                    RiRotate(-90, 1, 0, 0);
                    RiCylinder(0.58f, 0, 1.8f, 360);
                RiTransformEnd();
                RiTransformBegin();
                    RiTranslate(0, -0.1f, 8.2f);
                    RiRotate(22, 1, 0, 0);
                    RiTorus(0.95f, 0.27f, 0, 360, 360);
                RiTransformEnd();

                RiColor(aqua);
                RiTransformBegin();
                    RiTranslate(2.15f, -2.45f, 10.0f);
                    RiRotate(-90, 1, 0, 0);
                    RiCylinder(0.58f, 0, 1.15f, 360);
                RiTransformEnd();
                RiDisplacement(BUMPY, &bumps, sizeof bumps);
                RiTransformBegin();
                    RiTranslate(2.15f, -0.2f, 10.0f);
                    RiRotate(-90, 1, 0, 0);
                    RiSurface(TexturedStandard, &gas, sizeof gas);
                    RiSphere(0.9f, -0.9f, 0.9f, 360);
                RiTransformEnd();
                RiDisplacement(NULL, NULL, 0);

                RiSurface(ENVIRONMENT, &reflections, sizeof reflections);
                RiTransformBegin();
                    RiTranslate(0, -1.45f, 5.9f);
                    RiSphere(0.42f, -0.42f, 0.42f, 360);
                RiTransformEnd();

                RiColor(glass);
                RiOpacity(glassOpacity);
                RiTransformBegin();
                    RiTranslate(-2.15f, -0.20f, 9.5f);
                    RiSphere(1.02f, -1.02f, 1.02f, 360);
                RiTransformEnd();
                RiOpacity(opaque);
            RiWorldEnd();
        RiFrameEnd();
    RiEnd();
}

void OrbitWorkshop(void)
{
    RenderOrbitWorkshop(true);
    RenderOrbitWorkshop(false);
}

void AdaptiveDiceDemo(void)
{
    RtFloat fov = 40.0f;
    RtColor blue = {0.15f, 0.48f, 0.8f};
    BumpParams bumps = {0.3f, 2, 5};
    RiBegin(RI_NULL);
        RiFormat(500, 300);
        RiDisplay("4_OriginalScene/AdaptiveDiceDemo.png", "", "");
        RiPixelSamples(2, 2);
        RiFrameBegin(0);
            RiProjection(RI_PERSPECTIVE, "fov", &fov);
            RiWorldBegin();
                RiColor(blue);
                RiDisplacement(BUMPY, &bumps, sizeof bumps);
                RiTranslate(0, 0, 10);
                RiSphere(0.45f, -0.45f, 0.45f, 360);
            RiWorldEnd();
        RiFrameEnd();
    RiEnd();
}
