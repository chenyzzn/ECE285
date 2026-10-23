/*
 * ri.cpp: the renderer behind the Ri interface (include/Ri.h).
 *
 * The C++ REYES renderer: graphics state and transforms, quadric and polygon
 * dicing, per-sample visibility, shaders, texture lookup, and image/depth
 * output. The public interface in Ri.h is shared with the supplied scenes.
 *
 * The two functions at the end, writeImage and writeDepth, are complete:
 * they write the file RiDisplay named, in the format its extension says,
 * from the renderer's filtered color and depth buffers.
 *
 * Shaders live in shaders.cpp. Everything the scene files include is in
 * include/, and must not change: Ri.h, shaders.h and scenes.h are the
 * interface; obj.h, shadowmap.h, sceneutil.h, anim.h and models.h are
 * helpers for the scene files that never call back into the renderer
 * except through that interface.
 */
#include "Ri.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <vector>

/* The one translation unit that compiles the stb implementations. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* Everything inside this unnamed namespace is private to this file, the
 * C++ way of marking each one static; only the Ri* functions below are
 * visible to the scenes. */
namespace {

using Matrix = std::array<std::array<RtFloat, 4>, 4>;
using Point3 = std::array<RtFloat, 3>;

struct Texture {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

struct Hit {
    float z;
    std::array<float, 3> color;
    std::array<float, 3> opacity;
};

struct Sample {
    float x, y;
    std::vector<Hit> hits;
};

std::array<Texture, 8> textures;
std::vector<Sample> samples;
constexpr int MAX_DICE_RATE = 256;

struct RasterPoint {
    RtFloat x, y, z;
};

struct Vertex {
    Point3 po{};
    Point3 p{};
    Point3 ng{};
    Point3 n{};
    float u = 0, v = 0;
    std::array<float, 3> ci{};
    std::array<float, 3> oi{};
    RasterPoint raster{};
};

enum class Projection {
    Orthographic,
    Perspective
};

Matrix identityMatrix()
{
    Matrix result{};
    for (int i = 0; i < 4; ++i)
        result[i][i] = 1.0f;
    return result;
}

Matrix multiply(const Matrix &a, const Matrix &b)
{
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                result[row][col] += a[row][k] * b[k][col];
    return result;
}

Point3 transformPoint(const Matrix &matrix, const RtPoint point)
{
    Point3 result{};
    for (int row = 0; row < 3; ++row)
        result[row] = matrix[row][0] * point[0] + matrix[row][1] * point[1] +
                      matrix[row][2] * point[2] + matrix[row][3];
    return result;
}

Point3 sub(const Point3 &a, const Point3 &b)
{ return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Point3 scale(const Point3 &a, float s)
{ return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Point3 &a, const Point3 &b)
{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Point3 cross(const Point3 &a, const Point3 &b)
{ return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Point3 normalized(const Point3 &a)
{
    const float len = std::sqrt(dot(a, a));
    return len > 1e-20f ? scale(a, 1.0f / len) : Point3{0, 0, 0};
}

/* Apply the inverse transpose of the model-view linear transform to a normal. */
Point3 transformNormal(const Matrix &m, const Point3 &n)
{
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-20)
        return normalized(n);
    const double inv = 1.0 / det;
    /* Cofactor matrix is the inverse transpose. */
    Point3 out{
        (float)(((e * i - f * h) * n[0] + (f * g - d * i) * n[1] + (d * h - e * g) * n[2]) * inv),
        (float)(((c * h - b * i) * n[0] + (a * i - c * g) * n[1] + (b * g - a * h) * n[2]) * inv),
        (float)(((b * f - c * e) * n[0] + (c * d - a * f) * n[1] + (a * e - b * d) * n[2]) * inv)
    };
    return normalized(out);
}

/* Graphics state is copied at RiFrameBegin and restored at RiFrameEnd.
 * Matrices use column vectors internally: p' = M p, with translation in
 * the last column. */
struct GraphicsState {
    int xres = 640, yres = 480;
    int xsamples = 2, ysamples = 2;
    std::string display = "out.png";
    bool depthDisplay = false; /* RiDisplay(name, "zfile", "z") */

    Matrix ctm = identityMatrix();
    Matrix cameraTransform = identityMatrix();
    std::vector<Matrix> transformStack;
    Projection projection = Projection::Orthographic;
    RtFloat fov = 90.0f;
    bool inWorld = false;

    RtColor color = {1.0f, 1.0f, 1.0f};
    RtColor opacity = {1.0f, 1.0f, 1.0f};
    RtShaderFn surfaceShader = STANDARD;
    std::vector<unsigned char> surfaceParams;
    RtShaderFn displacementShader = nullptr;
    std::vector<unsigned char> displacementParams;
};

GraphicsState graphics;
std::vector<GraphicsState> frameStack;

/* Project a camera-space point to continuous raster coordinates. Pixel
 * centers are at half-integers; raster y increases down the image. */
bool projectCameraPoint(const Point3 &point, RasterPoint &raster)
{
    if (graphics.xres <= 0 || graphics.yres <= 0 || point[2] <= 0.01f)
        return false;

    RtFloat screenX = point[0];
    RtFloat screenY = point[1];
    if (graphics.projection == Projection::Perspective) {
        const RtFloat halfFov = graphics.fov * (RtFloat)(std::acos(-1.0) / 360.0);
        const RtFloat tanHalfFov = std::tan(halfFov);
        if (!(tanHalfFov > 0.0f) || !std::isfinite(tanHalfFov))
            return false;
        screenX /= point[2] * tanHalfFov;
        screenY /= point[2] * tanHalfFov;
    }

    const RtFloat aspect = (RtFloat)graphics.xres / (RtFloat)graphics.yres;
    const RtFloat left = aspect >= 1.0f ? -aspect : -1.0f;
    const RtFloat right = -left;
    const RtFloat bottom = aspect >= 1.0f ? -1.0f : -1.0f / aspect;
    const RtFloat top = -bottom;

    raster.x = (screenX - left) * (RtFloat)graphics.xres / (right - left);
    raster.y = (top - screenY) * (RtFloat)graphics.yres / (top - bottom);
    raster.z = point[2];
    return std::isfinite(raster.x) && std::isfinite(raster.y);
}

[[maybe_unused]] bool projectObjectPoint(const RtPoint point, RasterPoint &raster)
{
    const Matrix modelView = multiply(graphics.cameraTransform, graphics.ctm);
    return projectCameraPoint(transformPoint(modelView, point), raster);
}

struct GridInput {
    Point3 po;
    Point3 ng;
    Point3 n;
    float u, v;
};

Matrix modelViewMatrix()
{
    return multiply(graphics.cameraTransform, graphics.ctm);
}

std::vector<Point3> boxCorners(const Point3 &low, const Point3 &high)
{
    std::vector<Point3> points;
    points.reserve(8);
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                points.push_back({x ? high[0] : low[0], y ? high[1] : low[1], z ? high[2] : low[2]});
    return points;
}

int diceRate(const std::vector<Point3> &boundPoints, bool periodicU, bool periodicV)
{
    const Matrix mv = modelViewMatrix();
    float minX = std::numeric_limits<float>::infinity();
    float minY = minX, maxX = -minX, maxY = -minX;
    for (const Point3 &p : boundPoints) {
        RtPoint raw = {p[0], p[1], p[2]};
        const Point3 camera = transformPoint(mv, raw);
        if (camera[2] <= 0.01f)
            return MAX_DICE_RATE;
        RasterPoint q;
        if (!projectCameraPoint(camera, q))
            return MAX_DICE_RATE;
        minX = std::min(minX, q.x); maxX = std::max(maxX, q.x);
        minY = std::min(minY, q.y); maxY = std::max(maxY, q.y);
    }
    const float diagonal = std::hypot(maxX - minX, maxY - minY);
    int rate = std::max(2, (int)std::ceil(2.0f * diagonal));
    rate = std::min(MAX_DICE_RATE, rate);
    if (periodicU || periodicV)
        rate = std::max(rate, 4);
    return rate;
}

bool isOpaque(const std::array<float, 3> &o)
{
    return o[0] >= 1.0f && o[1] >= 1.0f && o[2] >= 1.0f;
}

void addHit(Sample &sample, const Hit &hit)
{
    for (const Hit &old : sample.hits)
        if (isOpaque(old.opacity) && hit.z > old.z)
            return;
    auto where = std::lower_bound(sample.hits.begin(), sample.hits.end(), hit.z,
        [](const Hit &a, float z) { return a.z < z; });
    const bool opaque = isOpaque(hit.opacity);
    where = sample.hits.insert(where, hit);
    if (opaque)
        sample.hits.erase(where + 1, sample.hits.end());
}

bool triangleHit(const Vertex *a, const Vertex *b, const Vertex *c,
                 float x, float y, float &w0, float &w1, float &w2)
{
    const float area = (b->raster.x - a->raster.x) * (c->raster.y - a->raster.y) -
                       (b->raster.y - a->raster.y) * (c->raster.x - a->raster.x);
    if (std::abs(area) < 1e-10f)
        return false;
    const float e0 = (b->raster.x - x) * (c->raster.y - y) - (b->raster.y - y) * (c->raster.x - x);
    const float e1 = (c->raster.x - x) * (a->raster.y - y) - (c->raster.y - y) * (a->raster.x - x);
    w0 = e0 / area;
    w1 = e1 / area;
    w2 = 1.0f - w0 - w1;
    constexpr float eps = -1e-6f;
    if (w0 < eps || w1 < eps || w2 < eps)
        return false;
    const auto topLeft = [](float ax, float ay, float bx, float by) {
        const float dy = by - ay, dx = bx - ax;
        return dy < 0 || (dy == 0 && dx > 0);
    };
    const bool ccw = area > 0;
    const auto owns = [&](float w, const Vertex *from, const Vertex *to) {
        if (std::abs(w) > 1e-6f)
            return true;
        return ccw ? topLeft(from->raster.x, from->raster.y, to->raster.x, to->raster.y)
                   : topLeft(to->raster.x, to->raster.y, from->raster.x, from->raster.y);
    };
    return owns(w0, b, c) && owns(w1, c, a) && owns(w2, a, b);
}

void rasterizeCell(Vertex &v00, Vertex &v10, Vertex &v11, Vertex &v01)
{
    if (v00.p[2] <= 0.01f || v10.p[2] <= 0.01f ||
        v11.p[2] <= 0.01f || v01.p[2] <= 0.01f)
        return;
    /* The two triangles share a diagonal. A sample is tested against the
       second only when it missed the first, preventing double coverage. */
    const float minX = std::min({v00.raster.x, v10.raster.x, v11.raster.x, v01.raster.x});
    const float maxX = std::max({v00.raster.x, v10.raster.x, v11.raster.x, v01.raster.x});
    const float minY = std::min({v00.raster.y, v10.raster.y, v11.raster.y, v01.raster.y});
    const float maxY = std::max({v00.raster.y, v10.raster.y, v11.raster.y, v01.raster.y});
    const int x0 = std::max(0, (int)std::floor(minX));
    const int y0 = std::max(0, (int)std::floor(minY));
    const int x1 = std::min(graphics.xres, (int)std::ceil(maxX));
    const int y1 = std::min(graphics.yres, (int)std::ceil(maxY));
    if (x0 >= x1 || y0 >= y1)
        return;

    const int spp = graphics.xsamples * graphics.ysamples;
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const size_t pixel = (size_t)py * graphics.xres + px;
            for (int s = 0; s < spp; ++s) {
                Sample &sample = samples[pixel * spp + s];
                float w0, w1, w2;
                const Vertex *a, *b, *c;
                if (triangleHit(&v00, &v10, &v11, sample.x, sample.y, w0, w1, w2)) {
                    a = &v00; b = &v10; c = &v11;
                } else if (triangleHit(&v00, &v11, &v01, sample.x, sample.y, w0, w1, w2)) {
                    a = &v00; b = &v11; c = &v01;
                } else {
                    continue;
                }
                Hit hit{};
                hit.z = a->raster.z + w1 * (b->raster.z - a->raster.z) + w2 * (c->raster.z - a->raster.z);
                for (int ch = 0; ch < 3; ++ch) {
                    hit.color[ch] = a->ci[ch] + w1 * (b->ci[ch] - a->ci[ch]) + w2 * (c->ci[ch] - a->ci[ch]);
                    hit.opacity[ch] = a->oi[ch] + w1 * (b->oi[ch] - a->oi[ch]) + w2 * (c->oi[ch] - a->oi[ch]);
                }
                addHit(sample, hit);
            }
        }
    }
}

template <typename Evaluate>
void processGrid(int nu, int nv, Evaluate evaluate)
{
    std::vector<Vertex> grid;
    const Matrix mv = modelViewMatrix();
    for (;;) {
    const size_t width = (size_t)nu + 1;
    grid.resize(width * ((size_t)nv + 1));
    for (int j = 0; j <= nv; ++j) {
        const float v = (float)j / nv;
        for (int i = 0; i <= nu; ++i) {
            const float u = (float)i / nu;
            GridInput in = evaluate(u, v);
            Vertex &vert = grid[(size_t)j * width + i];
            vert.po = in.po;
            RtPoint po = {in.po[0], in.po[1], in.po[2]};
            vert.p = transformPoint(mv, po);
            vert.ng = transformNormal(mv, in.ng);
            vert.n = transformNormal(mv, in.n);
            vert.u = in.u;
            vert.v = in.v;
        }
    }

    std::vector<unsigned char> degenerate(grid.size(), 0);
    if (graphics.displacementShader) {
        for (int j = 0; j <= nv; ++j) {
            for (int i = 0; i <= nu; ++i) {
                const Vertex &vert = grid[(size_t)j * width + i];
                const Point3 &left = grid[(size_t)j * width + (i == nu ? i - 1 : i)].p;
                const Point3 &right = grid[(size_t)j * width + (i == nu ? i : i + 1)].p;
                const Point3 &up = grid[(size_t)(j == nv ? j - 1 : j) * width + i].p;
                const Point3 &down = grid[(size_t)(j == nv ? j : j + 1) * width + i].p;
                const Point3 du = scale(i == nu ? sub(vert.p, left) : sub(right, vert.p), (float)nu);
                const Point3 dv = scale(j == nv ? sub(vert.p, up) : sub(down, vert.p), (float)nv);
                const Point3 tangent = cross(du, dv);
                degenerate[(size_t)j * width + i] = dot(tangent, tangent) < 1e-18f;
            }
        }
        for (Vertex &vert : grid) {
            RtShaderContext ctx{};
            for (int k = 0; k < 3; ++k) {
                ctx.P[k] = vert.p[k]; ctx.Po[k] = vert.po[k];
                ctx.N[k] = vert.n[k]; ctx.Cs[k] = graphics.color[k]; ctx.Os[k] = graphics.opacity[k];
            }
            ctx.u = vert.u; ctx.v = vert.v; ctx.params = graphics.displacementParams.empty() ? nullptr : graphics.displacementParams.data();
            graphics.displacementShader(&ctx);
            for (int k = 0; k < 3; ++k)
                vert.p[k] = ctx.P[k];
        }

        for (int j = 0; j <= nv; ++j) {
            for (int i = 0; i <= nu; ++i) {
                Vertex &vert = grid[(size_t)j * width + i];
                const Point3 &left = grid[(size_t)j * width + (i == nu ? i - 1 : i)].p;
                const Point3 &right = grid[(size_t)j * width + (i == nu ? i : i + 1)].p;
                const Point3 &up = grid[(size_t)(j == nv ? j - 1 : j) * width + i].p;
                const Point3 &down = grid[(size_t)(j == nv ? j : j + 1) * width + i].p;
                Point3 du = scale(i == nu ? sub(vert.p, left) : sub(right, vert.p), (float)nu);
                Point3 dv = scale(j == nv ? sub(vert.p, up) : sub(down, vert.p), (float)nv);

                if (degenerate[(size_t)j * width + i])
                    vert.n = vert.ng;
                else
                    vert.n = normalized(cross(du, dv));
            }
        }

        float largest = 0.0f;
        for (int j = 0; j <= nv; ++j)
            for (int i = 0; i <= nu; ++i) {
                const Vertex &a = grid[(size_t)j * width + i];
                if (a.p[2] <= 0.01f)
                    continue;
                RasterPoint ra;
                if (!projectCameraPoint(a.p, ra))
                    continue;
                const auto measure = [&](const Vertex &b) {
                    if (b.p[2] > 0.01f) {
                        RasterPoint rb;
                        if (projectCameraPoint(b.p, rb))
                            largest = std::max(largest, std::hypot(ra.x - rb.x, ra.y - rb.y));
                    }
                };
                if (i < nu) measure(grid[(size_t)j * width + i + 1]);
                if (j < nv) measure(grid[(size_t)(j + 1) * width + i]);
            }
        if (largest > 0.5f && (nu < MAX_DICE_RATE || nv < MAX_DICE_RATE)) {
            const int finer = std::min(MAX_DICE_RATE,
                std::max(std::max(nu, nv) + 1, (int)std::ceil(std::min((float)MAX_DICE_RATE,
                    std::max(nu, nv) * largest / 0.5f))));
            nu = nv = finer;
            continue;
        }
    }
    break;
    }

    const size_t width = (size_t)nu + 1;

    for (Vertex &vert : grid) {
        /* OBJ winding can oppose supplied vertex normals. Orient Ng to the
           shading normal first, then use Ng to choose the eye-facing side. */
        Point3 face = dot(vert.ng, vert.n) < 0 ? scale(vert.ng, -1.0f) : vert.ng;
        if (dot(face, scale(vert.p, -1.0f)) < 0)
            vert.n = scale(vert.n, -1.0f);

        RtShaderContext ctx{};
        for (int k = 0; k < 3; ++k) {
            ctx.P[k] = vert.p[k]; ctx.Po[k] = vert.po[k]; ctx.N[k] = vert.n[k];
            ctx.Cs[k] = graphics.color[k]; ctx.Os[k] = graphics.opacity[k];
        }
        ctx.u = vert.u; ctx.v = vert.v;
        ctx.params = graphics.surfaceParams.empty() ? nullptr : graphics.surfaceParams.data();
        graphics.surfaceShader(&ctx);
        for (int k = 0; k < 3; ++k) {
            vert.ci[k] = ctx.Ci[k]; vert.oi[k] = ctx.Oi[k];
        }
        RasterPoint rp{};
        if (projectCameraPoint(vert.p, rp))
            vert.raster = rp;
        else
            vert.raster = {NAN, NAN, vert.p[2]};
    }

    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            Vertex &v00 = grid[(size_t)j * width + i];
            Vertex &v10 = grid[(size_t)j * width + i + 1];
            Vertex &v11 = grid[(size_t)(j + 1) * width + i + 1];
            Vertex &v01 = grid[(size_t)(j + 1) * width + i];
            rasterizeCell(v00, v10, v11, v01);
        }
}

void writeImage(const float *rgba);
void writeDepth(const float *depth);

} // namespace

/* ---------------------------------------------------------- graphics state */

/* RiBegin starts a rendering session and resets the graphics state to
 * RenderMan's defaults: color and opacity (1, 1, 1), surface shader
 * STANDARD, no displacement, orthographic projection, 640 x 480 pixels,
 * 2 x 2 samples. Scenes rely on these (FirstPrimitive never calls
 * RiOpacity). RiEnd ends the session (free textures and buffers here).
 * The name is always RI_NULL. */
RtVoid RiBegin(RtToken /*name*/)
{
    graphics = GraphicsState{};
    frameStack.clear();
    for (Texture &texture : textures)
        texture = Texture{};
    samples.clear();
}
RtVoid RiEnd(void)
{
    for (Texture &texture : textures)
        texture = Texture{};
    samples.clear();
}

/* The image size in pixels. The screen window is [-a, a] x [-1, 1] for
 * a = xres / yres >= 1 and [-1, 1] x [-1/a, 1/a] for a < 1, so the field of
 * view spans the shorter side (Section 2). */
RtVoid RiFormat(RtInt xres, RtInt yres)
{
    graphics.xres = xres;
    graphics.yres = yres;
}

/* xsamples x ysamples jittered samples per pixel (Section 4). */
RtVoid RiPixelSamples(RtInt xsamples, RtInt ysamples)
{
    graphics.xsamples = std::max(1, xsamples);
    graphics.ysamples = std::max(1, ysamples);
}

/* The output file, written into renders/ by RiWorldEnd. RiDisplay(name, "",
 * "") names the picture; RiDisplay(name, "zfile", "z") asks for the depth
 * image instead (Section 4, "Depth output"). */
RtVoid RiDisplay(const char *name, RtToken type, RtToken mode)
{
    graphics.display = name ? name : "out.png";
    graphics.depthDisplay =
        (type && std::strcmp(type, "zfile") == 0) || (mode && std::strcmp(mode, "z") == 0);
}

/* RiFrameBegin/End bracket one frame of an animation: save the graphics
 * state at Begin and restore it at End, so that each frame starts afresh. */
RtVoid RiFrameBegin(RtInt /*frame*/)
{
    frameStack.push_back(graphics);
}
RtVoid RiFrameEnd(void)
{
    if (!frameStack.empty()) {
        graphics = frameStack.back();
        frameStack.pop_back();
    }
}

/* RiWorldBegin: the transformation built so far is the camera transform
 * (world to camera); from here on transformations place objects. Set up
 * the sample buffer. RiWorldEnd: all primitives have been drawn, so filter
 * the samples into pixels and write the image (Sections 2 and 4). */
RtVoid RiWorldBegin(void)
{
    graphics.cameraTransform = graphics.ctm;
    graphics.ctm = identityMatrix();
    graphics.transformStack.clear();
    graphics.inWorld = true;

    const size_t pixelCount = (size_t)std::max(0, graphics.xres) * std::max(0, graphics.yres);
    const int xs = std::max(1, graphics.xsamples), ys = std::max(1, graphics.ysamples);
    samples.clear();
    samples.resize(pixelCount * (size_t)xs * ys);
    std::mt19937 rng(0x2852024u);
    std::uniform_real_distribution<float> jitter(0.0f, 1.0f);
    for (int y = 0; y < graphics.yres; ++y)
        for (int x = 0; x < graphics.xres; ++x)
            for (int sy = 0; sy < ys; ++sy)
                for (int sx = 0; sx < xs; ++sx) {
                    const size_t index = ((size_t)y * graphics.xres + x) * xs * ys + sy * xs + sx;
                    samples[index].x = x + (sx + jitter(rng)) / xs;
                    samples[index].y = y + (sy + jitter(rng)) / ys;
                    samples[index].hits.clear();
                }
}

RtVoid RiWorldEnd(void)
{
    const size_t pixelCount = (size_t)graphics.xres * graphics.yres;
    const int spp = std::max(1, graphics.xsamples) * std::max(1, graphics.ysamples);
    if (graphics.depthDisplay) {
        std::vector<float> depth(pixelCount, INFINITY);
        for (size_t p = 0; p < pixelCount; ++p)
            for (int s = 0; s < spp; ++s)
                for (const Hit &hit : samples[p * spp + s].hits) {
                    const float meanOpacity = (hit.opacity[0] + hit.opacity[1] + hit.opacity[2]) / 3.0f;
                    if (meanOpacity >= 0.5f) {
                        depth[p] = std::min(depth[p], hit.z);
                        break;
                    }
                }
        writeDepth(depth.data());
    } else {
        std::vector<float> rgba(pixelCount * 4, 0.0f);
        for (size_t p = 0; p < pixelCount; ++p) {
            float color[3] = {0, 0, 0}, opacity[3] = {0, 0, 0};
            for (int s = 0; s < spp; ++s) {
                const Sample &sample = samples[p * spp + s];
                float c[3] = {0, 0, 0}, o[3] = {0, 0, 0};
                for (const Hit &hit : sample.hits) {
                    for (int ch = 0; ch < 3; ++ch) {
                        c[ch] += (1.0f - o[ch]) * hit.color[ch] * hit.opacity[ch];
                        o[ch] += (1.0f - o[ch]) * hit.opacity[ch];
                    }
                    if (o[0] >= 1 && o[1] >= 1 && o[2] >= 1)
                        break;
                }
                for (int ch = 0; ch < 3; ++ch) {
                    color[ch] += c[ch];
                    opacity[ch] += o[ch];
                }
            }
            for (int ch = 0; ch < 3; ++ch)
                rgba[p * 4 + ch] = color[ch] / spp;
            rgba[p * 4 + 3] = (opacity[0] + opacity[1] + opacity[2]) / (3.0f * spp);
        }
        writeImage(rgba.data());
    }
    samples.clear();

    graphics.ctm = graphics.cameraTransform;
    graphics.transformStack.clear();
    graphics.inWorld = false;
}

/* RiProjection(RI_PERSPECTIVE, "fov", &fov), fov in degrees over the
 * shorter side of the image, or RiProjection(RI_ORTHOGRAPHIC). It also
 * resets the current transformation to the identity: the camera transform
 * is built after it (Section 2). */
RtVoid RiProjection(RtToken name, RtToken token, RtPointer value)
{
    if (name && std::strcmp(name, RI_PERSPECTIVE) == 0) {
        graphics.projection = Projection::Perspective;
        if (token && std::strcmp(token, "fov") == 0 && value)
            graphics.fov = *static_cast<const RtFloat *>(value);
    } else if (name && std::strcmp(name, RI_ORTHOGRAPHIC) == 0) {
        graphics.projection = Projection::Orthographic;
    } else {
        return;
    }

    /* Projection starts a new camera transform. Keep the transform stack so
     * an enclosing RiTransformBegin/RiTransformEnd still restores its save. */
    graphics.ctm = identityMatrix();
}

/* ----------------------------------------------------------- transformations */

/* Transform calls post-multiply our column-vector CTM: p' = CTM p. The
 * last transform called is therefore applied to a point first. RtMatrix
 * arguments remain in RenderMan's row-vector form and are transposed when
 * concatenated. Angles are in degrees. */
RtVoid RiTransformBegin(void)
{
    graphics.transformStack.push_back(graphics.ctm);
}

RtVoid RiTransformEnd(void)
{
    if (!graphics.transformStack.empty()) {
        graphics.ctm = graphics.transformStack.back();
        graphics.transformStack.pop_back();
    }
}

RtVoid RiConcatTransform(RtMatrix transform)
{
    if (!transform)
        return;

    Matrix operation{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            operation[row][col] = transform[col][row];
    graphics.ctm = multiply(graphics.ctm, operation);
}

RtVoid RiTranslate(RtFloat dx, RtFloat dy, RtFloat dz)
{
    Matrix operation = identityMatrix();
    operation[0][3] = dx;
    operation[1][3] = dy;
    operation[2][3] = dz;
    graphics.ctm = multiply(graphics.ctm, operation);
}

RtVoid RiRotate(RtFloat angle, RtFloat dx, RtFloat dy, RtFloat dz)
{
    const RtFloat axisLength = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (axisLength <= 0.0f)
        return;

    const RtFloat x = dx / axisLength;
    const RtFloat y = dy / axisLength;
    const RtFloat z = dz / axisLength;
    const RtFloat radians = angle * (RtFloat)(std::acos(-1.0) / 180.0);
    const RtFloat c = std::cos(radians);
    const RtFloat s = std::sin(radians);
    const RtFloat oneMinusC = 1.0f - c;

    Matrix operation = identityMatrix();
    operation[0][0] = c + x * x * oneMinusC;
    operation[0][1] = x * y * oneMinusC - z * s;
    operation[0][2] = x * z * oneMinusC + y * s;
    operation[1][0] = y * x * oneMinusC + z * s;
    operation[1][1] = c + y * y * oneMinusC;
    operation[1][2] = y * z * oneMinusC - x * s;
    operation[2][0] = z * x * oneMinusC - y * s;
    operation[2][1] = z * y * oneMinusC + x * s;
    operation[2][2] = c + z * z * oneMinusC;
    graphics.ctm = multiply(graphics.ctm, operation);
}

RtVoid RiScale(RtFloat sx, RtFloat sy, RtFloat sz)
{
    Matrix operation = identityMatrix();
    operation[0][0] = sx;
    operation[1][1] = sy;
    operation[2][2] = sz;
    graphics.ctm = multiply(graphics.ctm, operation);
}

/* ---------------------------------------------------------------- primitives */

/* Each primitive goes through the REYES pipeline as soon as it is called:
 * bound it, choose a dice rate from the bound's size on screen, dice it
 * into a grid of micropolygons, shade the grid, and hand the micropolygons
 * to the hider (Section 3). The quadrics use the RenderMan parameterizations
 * (RiSpec section 5.4); thetamax, phimin and phimax are degrees.
 *
 * Shade the grid in two passes (Section 5): the displacement shader at
 * every vertex, then N recomputed from the displaced grid, then the surface
 * shader at every vertex. Each call fills one vertex's RtShaderContext,
 * runs the shader and keeps what it wrote. A shader sees only its own
 * vertex, so the order you visit them in does not matter; only the new N
 * needs the neighbors, which is why it waits for the whole first pass. */
RtVoid RiSphere(RtFloat radius, RtFloat zmin, RtFloat zmax, RtFloat thetamax)
{
    if (radius <= 0)
        return;
    zmin = std::max(-radius, std::min(radius, zmin));
    zmax = std::max(-radius, std::min(radius, zmax));
    if (zmin > zmax)
        return;
    const int n = diceRate(boxCorners({-radius, -radius, zmin}, {radius, radius, zmax}),
                           std::abs(thetamax) >= 359.999f, false);
    const float thetaEnd = thetamax * (float)(std::acos(-1.0) / 180.0);
    processGrid(n, n, [=](float u, float v) {
        const float theta = thetaEnd * u;
        const float z = zmin + (zmax - zmin) * v;
        const float radial = std::sqrt(std::max(0.0f, radius * radius - z * z));
        Point3 p = {radial * std::cos(theta), radial * std::sin(theta), z};
        Point3 normal = normalized(p);
        return GridInput{p, normal, normal, u, v};
    });
}

RtVoid RiCylinder(RtFloat radius, RtFloat zmin, RtFloat zmax, RtFloat thetamax)
{
    if (radius <= 0 || zmin == zmax)
        return;
    const int n = diceRate(boxCorners({-radius, -radius, std::min(zmin, zmax)},
                                      {radius, radius, std::max(zmin, zmax)}),
                           std::abs(thetamax) >= 359.999f, false);
    const float thetaEnd = thetamax * (float)(std::acos(-1.0) / 180.0);
    processGrid(n, n, [=](float u, float v) {
        const float theta = thetaEnd * u;
        Point3 normal = {std::cos(theta), std::sin(theta), 0};
        Point3 p = {radius * normal[0], radius * normal[1], zmin + v * (zmax - zmin)};
        return GridInput{p, normal, normal, u, v};
    });
}

RtVoid RiCone(RtFloat height, RtFloat radius, RtFloat thetamax)
{
    if (height == 0 || radius <= 0)
        return;
    const int n = diceRate(boxCorners({-radius, -radius, std::min(0.0f, height)},
                                      {radius, radius, std::max(0.0f, height)}),
                           std::abs(thetamax) >= 359.999f, false);
    const float thetaEnd = thetamax * (float)(std::acos(-1.0) / 180.0);
    processGrid(n, n, [=](float u, float v) {
        const float theta = thetaEnd * u;
        Point3 normal = normalized({height * std::cos(theta), height * std::sin(theta), radius});
        const float r = radius * (1.0f - v);
        Point3 p = {r * std::cos(theta), r * std::sin(theta), height * v};
        return GridInput{p, normal, normal, u, v};
    });
}

RtVoid RiTorus(RtFloat majorradius, RtFloat minorradius, RtFloat phimin, RtFloat phimax, RtFloat thetamax)
{
    if (majorradius <= 0 || minorradius <= 0 || phimin == phimax)
        return;
    const float extent = majorradius + minorradius;
    const int n = diceRate(boxCorners({-extent, -extent, -minorradius},
                                      {extent, extent, minorradius}),
                           std::abs(thetamax) >= 359.999f,
                           std::abs(phimax - phimin) >= 359.999f);
    const float thetaEnd = thetamax * (float)(std::acos(-1.0) / 180.0);
    const float phiStart = phimin * (float)(std::acos(-1.0) / 180.0);
    const float phiSpan = (phimax - phimin) * (float)(std::acos(-1.0) / 180.0);
    processGrid(n, n, [=](float u, float v) {
        const float theta = thetaEnd * u;
        const float phi = phiStart + phiSpan * v;
        Point3 normal = {std::cos(phi) * std::cos(theta), std::cos(phi) * std::sin(theta), std::sin(phi)};
        const float ring = majorradius + minorradius * std::cos(phi);
        Point3 p = {ring * std::cos(theta), ring * std::sin(theta), minorradius * std::sin(phi)};
        return GridInput{p, normal, normal, u, v};
    });
}

/* A mesh of polygons: polygon i has nverts[i] corners, whose indices into
 * P (and N and st, when they are not NULL) follow one another in verts.
 * Treat every polygon as its own primitive, bounded, diced, shaded and
 * hidden before the next: a quad is a bilinear patch, a triangle the same
 * patch with its last corner repeated, and a larger polygon a fan of
 * triangles from its first vertex (Section 3, "Polygon meshes"). */
RtVoid RiPointsPolygons(RtInt npolys, const RtInt nverts[], const RtInt verts[],
                        const RtPoint P[], const RtNormal N[], const RtFloat st[])
{
    if (npolys <= 0 || !nverts || !verts || !P)
        return;
    int offset = 0;
    for (int poly = 0; poly < npolys; ++poly) {
        const int count = nverts[poly];
        if (count < 3) {
            offset += std::max(0, count);
            continue;
        }
        std::vector<int> face(count);
        Point3 low = {INFINITY, INFINITY, INFINITY};
        Point3 high = {-INFINITY, -INFINITY, -INFINITY};
        Point3 faceNormal{0, 0, 0};
        for (int k = 0; k < count; ++k) {
            face[k] = verts[offset + k];
            const RtPoint &p = P[face[k]];
            for (int c = 0; c < 3; ++c) {
                low[c] = std::min(low[c], p[c]);
                high[c] = std::max(high[c], p[c]);
            }
        }
        for (int k = 0; k < count; ++k) {
            const RtPoint &p = P[face[k]];
            const RtPoint &q = P[face[(k + 1) % count]];
            faceNormal[0] += (p[1] - q[1]) * (p[2] + q[2]);
            faceNormal[1] += (p[2] - q[2]) * (p[0] + q[0]);
            faceNormal[2] += (p[0] - q[0]) * (p[1] + q[1]);
        }
        offset += count;
        faceNormal = normalized(faceNormal);
        if (dot(faceNormal, faceNormal) < 1e-20f)
            continue;
        const int n = diceRate(boxCorners(low, high), false, false);
        const int triangles = count == 4 ? 1 : count - 2;
        for (int tri = 0; tri < triangles; ++tri) {
            const int ids[4] = {face[0], face[1 + tri], face[2 + tri],
                                count == 4 ? face[3] : face[2 + tri]};
            const bool quad = count == 4;
            auto eval = [=](float u, float v) {
                float w[4];
                if (quad) {
                    w[0] = (1-u)*(1-v); w[1] = u*(1-v); w[2] = u*v; w[3] = (1-u)*v;
                } else {
                    w[0] = (1-u)*(1-v); w[1] = u*(1-v); w[2] = v; w[3] = 0;
                }
                Point3 p{0,0,0}, smooth{0,0,0};
                float su = 0, sv = 0;
                for (int a = 0; a < 4; ++a) {
                    const RtPoint &src = P[ids[a]];
                    for (int c = 0; c < 3; ++c)
                        p[c] += w[a] * src[c];
                    if (N) {
                        const RtNormal &srcn = N[ids[a]];
                        for (int c = 0; c < 3; ++c)
                            smooth[c] += w[a] * srcn[c];
                    }
                    if (st) {
                        su += w[a] * st[2 * ids[a]];
                        sv += w[a] * st[2 * ids[a] + 1];
                    }
                }
                if (!N)
                    smooth = faceNormal;
                if (!st) {
                    su = u;
                    sv = v;
                }
                return GridInput{p, faceNormal, smooth, su, sv};
            };
            processGrid(n, n, eval);
        }
    }
}

/* ------------------------------------------------------------------ shading */

/* Attributes: they stay set until changed (RiTransformEnd does not restore
 * them). The color and opacity become Cs and Os in the shader context. */
RtVoid RiColor(RtColor color)
{
    if (color)
        for (int k = 0; k < 3; ++k) graphics.color[k] = color[k];
}
RtVoid RiOpacity(RtColor opacity)
{
    if (opacity)
        for (int k = 0; k < 3; ++k) graphics.opacity[k] = opacity[k];
}

/* Bind a shader to the primitives that follow. Copy nbytes of params now:
 * the scene's struct may be gone by the time the shader runs. fn NULL means
 * STANDARD for RiSurface and no displacement for RiDisplacement
 * (Section 5). */
RtVoid RiSurface(RtShaderFn fn, const void *params, size_t nbytes)
{
    graphics.surfaceShader = fn ? fn : STANDARD;
    graphics.surfaceParams.clear();
    if (params && nbytes) {
        const auto *bytes = static_cast<const unsigned char *>(params);
        graphics.surfaceParams.assign(bytes, bytes + nbytes);
    }
}
RtVoid RiDisplacement(RtShaderFn fn, const void *params, size_t nbytes)
{
    graphics.displacementShader = fn;
    graphics.displacementParams.clear();
    if (fn && params && nbytes) {
        const auto *bytes = static_cast<const unsigned char *>(params);
        graphics.displacementParams.assign(bytes, bytes + nbytes);
    }
}

/* Load an image into one of 8 texture slots with stb_image (look for the
 * file as given, then under textures/), and look it up bilinearly with
 * repeat wrap: s runs left to right, t from the TOP row (t = 0) to the
 * bottom (Section 6). An empty slot reads black. */
RtVoid RiMakeTexture(const char *file, RtInt slot)
{
    if (!file || slot < 0 || slot >= (RtInt)textures.size())
        return;
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = stbi_load(file, &width, &height, &channels, 3);
    if (!pixels) {
        const std::string path = (std::filesystem::path("textures") / file).string();
        pixels = stbi_load(path.c_str(), &width, &height, &channels, 3);
    }
    if (!pixels) {
        std::fprintf(stderr, "RiMakeTexture: could not load %s\n", file);
        textures[slot] = Texture{};
        return;
    }
    Texture loaded;
    loaded.width = width;
    loaded.height = height;
    loaded.rgb.assign(pixels, pixels + (size_t)width * height * 3);
    stbi_image_free(pixels);
    textures[slot] = std::move(loaded);
}

RtVoid RiTexture(RtInt slot, RtFloat s, RtFloat t, RtColor result)
{
    if (!result)
        return;
    result[0] = result[1] = result[2] = 0;
    if (slot < 0 || slot >= (RtInt)textures.size())
        return;
    const Texture &texture = textures[slot];
    if (texture.width <= 0 || texture.height <= 0 || texture.rgb.empty())
        return;
    if (s < 0 || s > 1) s -= std::floor(s);
    if (t < 0 || t > 1) t -= std::floor(t);
    const float x = std::max(0.0f, std::min(1.0f, s)) * (texture.width - 1);
    const float y = std::max(0.0f, std::min(1.0f, t)) * (texture.height - 1);
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const int x1 = std::min(texture.width - 1, x0 + 1), y1 = std::min(texture.height - 1, y0 + 1);
    const float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 3; ++c) {
        const auto at = [&](int ix, int iy) {
            return texture.rgb[((size_t)iy * texture.width + ix) * 3 + c] / 255.0f;
        };
        const float top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
        const float bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
        result[c] = top * (1 - fy) + bottom * fy;
    }
}

/* ------------------------------------------------------------ output files */

namespace {

/* Lowercase extension of a file name, without the dot ("" if none). */
std::string extension(const std::string &name)
{
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    std::string ext = name.substr(dot + 1);
    for (char &ch : ext)
        ch = (char)std::tolower((unsigned char)ch);
    return ext;
}

/* A value clamped to [0, 1], as an 8-bit integer. */
unsigned char to8(float v)
{
    return (unsigned char)std::floor(std::max(0.0f, std::min(1.0f, v)) * 255 + 0.5f);
}

/* Write the picture to renders/<RiDisplay name>, in the format its
 * extension names. Complete: call it from RiWorldEnd with your filtered
 * pixels, xres * yres * 4 floats, top row first: each pixel's color as
 * compositing leaves it, premultiplied by alpha (that is, over the black
 * background), then its alpha, the mean of its three opacity channels.
 * Don't clamp or convert them; this does, per format:
 *   .png, .tga  keep alpha. Both store straight color, so each pixel's
 *               color is divided by its alpha: a pixel half covered by a
 *               white surface is stored as white with alpha one half, and
 *               whatever shows the file multiplies the alpha back in.
 *   .jpg, .bmp  are written without alpha: JPEG has none, and most
 *               programs ignore a BMP's. They get the picture over the
 *               black background, which is the premultiplied color itself.
 * Values are clamped to [0, 1] and rounded to 8 bits. */
void writeImage(const float *rgba)
{
    const int w = graphics.xres, h = graphics.yres;
    const std::string path = "renders/" + graphics.display;
    const std::string ext = extension(graphics.display);
    const bool png = ext == "png", tga = ext == "tga", bmp = ext == "bmp";
    const bool jpg = ext == "jpg" || ext == "jpeg";
    if (!png && !tga && !bmp && !jpg) {
        std::fprintf(stderr, "cannot write %s: use .png, .bmp, .tga or .jpg\n", path.c_str());
        return;
    }

    const int comp = (png || tga) ? 4 : 3; /* with alpha, or without */
    std::vector<unsigned char> bytes((size_t)w * h * comp);
    for (size_t p = 0; p < (size_t)w * h; ++p) {
        const float *in = rgba + 4 * p;
        unsigned char *out = &bytes[comp * p];
        float alpha = std::max(0.0f, std::min(1.0f, in[3]));
        for (int c = 0; c < 3; ++c)
            out[c] = to8(comp == 3 ? in[c] : alpha > 0 ? in[c] / alpha : 0.0f);
        if (comp == 4)
            out[3] = to8(alpha);
    }

    int ok;
    if (png)
        ok = stbi_write_png(path.c_str(), w, h, comp, bytes.data(), w * comp);
    else if (tga)
        ok = stbi_write_tga(path.c_str(), w, h, comp, bytes.data());
    else if (bmp)
        ok = stbi_write_bmp(path.c_str(), w, h, comp, bytes.data());
    else
        ok = stbi_write_jpg(path.c_str(), w, h, comp, bytes.data(), 95);
    if (ok)
        std::printf("wrote %s (%d x %d)\n", path.c_str(), w, h);
    else
        std::fprintf(stderr, "could not write %s (run reyes from the repo root)\n", path.c_str());
}

/* Write the depth image to renders/<RiDisplay name> in the handout's
 * format: "Rz01", int32 width and height, then one float per pixel, top
 * row first, all little-endian (the camera-space z of the nearest surface
 * at least half opaque, +infinity where there is none). Complete: call it
 * from RiWorldEnd when RiDisplay asked for depth. The scene that reads the
 * file back knows the camera it was rendered with. */
void writeDepth(const float *depth)
{
    const int w = graphics.xres, h = graphics.yres;
    const std::string path = "renders/" + graphics.display;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "could not write %s (run reyes from the repo root)\n", path.c_str());
        return;
    }
    const int32_t size[2] = {w, h};
    bool ok = std::fwrite("Rz01", 1, 4, f) == 4 && std::fwrite(size, sizeof size[0], 2, f) == 2 &&
              std::fwrite(depth, sizeof depth[0], (size_t)w * h, f) == (size_t)w * h;
    ok = std::fclose(f) == 0 && ok;
    if (ok)
        std::printf("wrote %s (%d x %d depth)\n", path.c_str(), w, h);
    else
        std::fprintf(stderr, "could not write %s\n", path.c_str());
}

} // namespace
