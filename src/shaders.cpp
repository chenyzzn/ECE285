/*
 * shaders.cpp: the four required shaders (handout, Section 5) and the
 * noise function BUMPY is built on.
 *
 * The required shaders and improved Perlin noise used by the renderer and
 * by scene-provided procedural shaders. Keep the public names and parameter
 * structs declared in shaders.h unchanged.
 *
 * A shader is an ordinary function. The renderer fills in the inputs of the
 * RtShaderContext (Ri.h) at every grid vertex and calls the shader; a
 * surface shader writes Ci and Oi, a displacement shader moves P.
 */
#include "shaders.h"
#include "envmap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/* ------------------------------------------------------------- CONSTANT */

/* The surface color, unlit: Ci = Cs and Oi = Os. The simplest shader, and
 * the first to write. FirstPrimitive binds it, so that its picture is the
 * plain RiColor whether or not your renderer runs shaders yet: once you
 * add shading, FirstPrimitive must look exactly as it did before. */
void CONSTANT(RtShaderContext *c)
{
    if (!c)
        return;
    for (int k = 0; k < 3; ++k) {
        c->Ci[k] = c->Cs[k];
        c->Oi[k] = c->Os[k];
    }
}

/* ------------------------------------------------------------- STANDARD */

/* Ambient, plus Lambertian diffuse, plus Phong specular, with the three
 * lights and the coefficients given in the handout (Section 5):
 *   Ci = Cs (ka + kd sum max(N.L, 0) I C) + ks sum max(R.V, 0)^32 I C
 *   Oi = Os
 * Everything is in camera space: the eye is at the origin, so the direction
 * from P toward the eye is -P. Normalize N and the light directions. Write
 * Ci as the color itself, not multiplied by Oi: the hider applies the
 * opacity when it composites (Section 7). */
void STANDARD(RtShaderContext *c)
{
    if (!c)
        return;
    const auto normalize = [](const float a[3], float out[3]) {
        const float len = std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
        if (len > 1e-20f) {
            out[0] = a[0]/len; out[1] = a[1]/len; out[2] = a[2]/len;
        } else {
            out[0] = out[1] = 0; out[2] = 1;
        }
    };
    float n[3], viewRaw[3] = {-c->P[0], -c->P[1], -c->P[2]}, view[3];
    normalize(c->N, n);
    normalize(viewRaw, view);
    const float positions[3][3] = {{-6, 8, -4}, {7, 3, -2}, {0, 5, 18}};
    const float colors[3][3] = {{1.0f, .95f, .85f}, {.6f, .7f, 1.0f}, {1, 1, 1}};
    const float intensity[3] = {1.0f, .4f, .5f};
    float diffuse[3] = {0,0,0}, specular[3] = {0,0,0};
    for (int light = 0; light < 3; ++light) {
        float lraw[3] = {positions[light][0] - c->P[0],
                         positions[light][1] - c->P[1],
                         positions[light][2] - c->P[2]}, l[3];
        normalize(lraw, l);
        const float ndotl = std::max(0.0f, n[0]*l[0] + n[1]*l[1] + n[2]*l[2]);
        float r[3] = {2*ndotl*n[0] - l[0], 2*ndotl*n[1] - l[1], 2*ndotl*n[2] - l[2]};
        normalize(r, r);
        const float rdotv = std::max(0.0f, r[0]*view[0] + r[1]*view[1] + r[2]*view[2]);
        const float phong = ndotl > 0 ? std::pow(rdotv, 32.0f) : 0.0f;
        for (int k = 0; k < 3; ++k) {
            diffuse[k] += ndotl * intensity[light] * colors[light][k];
            specular[k] += phong * intensity[light] * colors[light][k];
        }
    }
    for (int k = 0; k < 3; ++k) {
        c->Ci[k] = c->Cs[k] * (0.15f + 0.8f * diffuse[k]) + 0.3f * specular[k];
        c->Oi[k] = c->Os[k];
    }
}

/* ---------------------------------------------------------- ENVIRONMENT */

/* Reflect the camera ray around the eye-facing surface normal and look up
 * the resulting direction in a latitude-longitude environment texture. */
void ENVIRONMENT(RtShaderContext *c)
{
    if (!c || !c->params)
        return;
    const auto *params = static_cast<const EnvMapParams *>(c->params);
    const float plen = std::sqrt(c->P[0]*c->P[0] + c->P[1]*c->P[1] + c->P[2]*c->P[2]);
    const float nlen = std::sqrt(c->N[0]*c->N[0] + c->N[1]*c->N[1] + c->N[2]*c->N[2]);
    if (plen <= 1e-20f || nlen <= 1e-20f) {
        CONSTANT(c);
        return;
    }
    float view[3], normal[3], reflection[3];
    float ndotv = 0;
    for (int k = 0; k < 3; ++k) {
        view[k] = -c->P[k] / plen;
        normal[k] = c->N[k] / nlen;
        ndotv += normal[k] * view[k];
    }
    for (int k = 0; k < 3; ++k)
        reflection[k] = 2.0f * ndotv * normal[k] - view[k];
    const float pi = (float)std::acos(-1.0);
    const float s = 0.5f + std::atan2(reflection[0], reflection[2]) / (2.0f * pi);
    const float t = std::acos(std::clamp(reflection[1], -1.0f, 1.0f)) / pi;
    RiTexture(params->slot, s, t, c->Ci);
    for (int k = 0; k < 3; ++k)
        c->Oi[k] = c->Os[k];
}

/* --------------------------------------------------------- CHECKERBOARD */

/* The check (u, v) falls in is (floor(u sizex), floor(v sizey)), with u = 1
 * and v = 1 counted in the last check. Checks where i + j is even are
 * "light" and run the light sub-shader, the others the dark one. A NULL
 * light shader means STANDARD; a NULL dark shader means STANDARD with
 * Cs = black, so that only its highlights show.
 *
 * You don't choose the sub-shaders: the scene does, in the CheckerParams it
 * binds (shaders.h), just as it passes any shader its parameters. The
 * model scenes' floors, for example, do
 *     CheckerParams cp = { 2, 2, Material, &MATTE_GREY,
 *                                Material, &POLISHED_BLACK };
 *     RiSurface(CHECKERBOARD, &cp, sizeof cp);
 * and most scenes write { 8, 4 }, leaving both NULL. Your RiSurface copied
 * that struct, so here c->params points at it. A shader is a plain
 * function, so run a check's sub-shader by calling it, with c->params
 * pointed at the sub-shader's own parameters during the call:
 *     const CheckerParams *p = (const CheckerParams *)c->params;
 *     c->params = p->light_params;
 *     p->light(c);
 *     c->params = p;    (put back for whoever called CHECKERBOARD) */
void CHECKERBOARD(RtShaderContext *c)
{
    if (!c)
        return;
    if (!c->params) {
        STANDARD(c);
        return;
    }
    const CheckerParams *p = static_cast<const CheckerParams *>(c->params);
    const int sx = std::max(1, p->sizex), sy = std::max(1, p->sizey);
    int i = (int)std::floor(c->u * sx), j = (int)std::floor(c->v * sy);
    if (c->u == 1.0f) i = sx - 1;
    if (c->v == 1.0f) j = sy - 1;
    const bool lightCell = ((i + j) % 2) == 0;
    RtShaderFn fn = lightCell ? p->light : p->dark;
    const void *params = lightCell ? p->light_params : p->dark_params;
    const void *savedParams = c->params;
    RtColor savedColor;
    for (int k = 0; k < 3; ++k)
        savedColor[k] = c->Cs[k];
    c->params = params;
    if (!lightCell && !fn)
        c->Cs[0] = c->Cs[1] = c->Cs[2] = 0;
    if (fn)
        fn(c);
    else
        STANDARD(c);
    c->params = savedParams;
    for (int k = 0; k < 3; ++k)
        c->Cs[k] = savedColor[k];
}

/* ---------------------------------------------------------- perlinNoise */

/* Ken Perlin's improved noise, from his paper "Improving Noise" (SIGGRAPH
 * 2002); his reference implementation, a page of Java, is at
 * https://mrl.cs.nyu.edu/~perlin/noise/. BUMPY sums octaves of it, and so
 * do some scene files' own shaders (Fbm in sceneutil.h), so until it works
 * those surfaces come out smooth and plain.
 *
 * Follow his algorithm exactly: his permutation of 0..255, his fade curve
 * 6t^5 - 15t^4 + 10t^3 and his gradient function. Any other noise looks
 * just as good, but your bumps will not match reference_renders/. The
 * result is smooth and signed, about [-1, 1], and exactly 0 at every
 * integer point. Points may be negative or far from the origin, so make
 * sure the lattice index wraps into 0..255 for those too. */
double perlinNoise(double x, double y, double z)
{
    /* Permutation table from Ken Perlin's improved-noise reference
       implementation: https://mrl.cs.nyu.edu/~perlin/noise/ (SIGGRAPH 2002). */
    static constexpr std::array<int, 256> permutation = {
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
        140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,
        26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,
        20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,
        83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,
        54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,
        130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,
        5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,
        28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,
        43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
        218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,
        235,249,14,239,107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,
        50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,
        78,66,215,61,156,180
    };
    auto wrap = [](double q) {
        double r = std::fmod(q, 256.0);
        return (int)(r < 0 ? r + 256.0 : r);
    };
    auto perm = [&](int i) { return permutation[(unsigned)i & 255u]; };
    auto fade = [](double t) { return t*t*t*(t*(t*6.0 - 15.0) + 10.0); };
    auto lerp = [](double t, double a, double b) { return a + t*(b-a); };
    auto grad = [](int hash, double a, double b, double c) {
        const int h = hash & 15;
        const double u = h < 8 ? a : b;
        const double v = h < 4 ? b : (h == 12 || h == 14 ? a : c);
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    };
    const double fx0 = std::floor(x), fy0 = std::floor(y), fz0 = std::floor(z);
    const int xi = wrap(fx0), yi = wrap(fy0), zi = wrap(fz0);
    const double xf = x - fx0, yf = y - fy0, zf = z - fz0;
    const double u = fade(xf), v = fade(yf), w = fade(zf);
    const int a = perm(xi) + yi, aa = perm(a) + zi, ab = perm(a + 1) + zi;
    const int b = perm(xi + 1) + yi, ba = perm(b) + zi, bb = perm(b + 1) + zi;
    return lerp(w,
        lerp(v, lerp(u, grad(perm(aa), xf, yf, zf), grad(perm(ba), xf-1, yf, zf)),
                   lerp(u, grad(perm(ab), xf, yf-1, zf), grad(perm(bb), xf-1, yf-1, zf))),
        lerp(v, lerp(u, grad(perm(aa+1), xf, yf, zf-1), grad(perm(ba+1), xf-1, yf, zf-1)),
                   lerp(u, grad(perm(ab+1), xf, yf-1, zf-1), grad(perm(bb+1), xf-1, yf-1, zf-1))));
}

/* ---------------------------------------------------------------- BUMPY */

/* Displace P along the unit normal N by
 *   h = amplitude * sum(w_e * perlinNoise(2^e * Po)) / sum(w_e)
 * over e = min_freq_exp .. max_freq_exp, with w_e = 2^(min_freq_exp - e):
 * each octave counts half as much as the one below it, and dividing by the
 * total weight keeps |h| <= amplitude whatever the octaves. Evaluate the
 * noise at Po (object space), so the bumps stick to the object as it
 * moves. The parameters are a BumpParams (shaders.h) at c->params. */
void BUMPY(RtShaderContext *c)
{
    if (!c || !c->params)
        return;
    const BumpParams *p = static_cast<const BumpParams *>(c->params);
    if (p->min_freq_exp > p->max_freq_exp)
        return;
    double sum = 0, weightSum = 0;
    for (int e = p->min_freq_exp; e <= p->max_freq_exp; ++e) {
        const double frequency = std::ldexp(1.0, e);
        const double weight = std::ldexp(1.0, p->min_freq_exp - e);
        sum += weight * perlinNoise(frequency*c->Po[0], frequency*c->Po[1], frequency*c->Po[2]);
        weightSum += weight;
        if (e == std::numeric_limits<int>::max())
            break;
    }
    if (weightSum == 0)
        return;
    const float height = p->amplitude * (float)(sum / weightSum);
    for (int k = 0; k < 3; ++k)
        c->P[k] += height * c->N[k];
}
