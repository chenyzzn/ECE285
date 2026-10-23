/* Depth-based fog for a PNG and an Rz01 depth image from the same camera. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int main(int argc, char **argv)
{
    if (argc != 9) {
        std::fprintf(stderr, "usage: depth_fog color.png depth.z output.png start density red green blue\n");
        return 1;
    }
    float values[5];
    for (int i = 0; i < 5; ++i) {
        char *end = nullptr;
        values[i] = std::strtof(argv[i + 4], &end);
        if (end == argv[i + 4] || *end || !std::isfinite(values[i]) || values[i] < 0 ||
            (i >= 2 && values[i] > 1)) {
            std::fprintf(stderr, "invalid fog parameter: %s\n", argv[i + 4]);
            return 1;
        }
    }

    int width, height, channels;
    unsigned char *image = stbi_load(argv[1], &width, &height, &channels, 4);
    if (!image) {
        std::fprintf(stderr, "cannot load image %s: %s\n", argv[1], stbi_failure_reason());
        return 1;
    }
    std::FILE *file = std::fopen(argv[2], "rb");
    unsigned char header[12];
    const bool headerOK = file && std::fread(header, 1, sizeof header, file) == sizeof header &&
        std::memcmp(header, "Rz01", 4) == 0;
    const auto little32 = [](const unsigned char *p) {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    };
    if (!headerOK || little32(header + 4) != (uint32_t)width ||
        little32(header + 8) != (uint32_t)height) {
        std::fprintf(stderr, "cannot read matching Rz01 depth image %s\n", argv[2]);
        if (file) std::fclose(file);
        stbi_image_free(image);
        return 1;
    }
    const size_t pixels = (size_t)width * height;
    std::vector<float> depth(pixels);
    const bool depthOK = std::fread(depth.data(), sizeof(float), pixels, file) == pixels;
    std::fclose(file);
    if (!depthOK) {
        std::fprintf(stderr, "truncated depth image %s\n", argv[2]);
        stbi_image_free(image);
        return 1;
    }

    for (size_t p = 0; p < pixels; ++p) {
        if (!std::isfinite(depth[p]) || image[4 * p + 3] == 0)
            continue;
        const float transmission = std::exp(-values[1] * std::max(0.0f, depth[p] - values[0]));
        for (int c = 0; c < 3; ++c) {
            const float original = image[4 * p + c] / 255.0f;
            const float fogged = transmission * original + (1 - transmission) * values[c + 2];
            image[4 * p + c] = (unsigned char)std::lround(255.0f * fogged);
        }
    }
    const int ok = stbi_write_png(argv[3], width, height, 4, image, width * 4);
    stbi_image_free(image);
    if (!ok) {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    std::printf("wrote %s (%d x %d fog)\n", argv[3], width, height);
    return 0;
}
