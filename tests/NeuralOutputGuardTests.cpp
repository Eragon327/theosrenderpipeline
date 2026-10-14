#include "FrameGen/NeuralOutputGuard.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace TheosRenderPipeline::SourceDLSSG;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

static std::vector<std::uint8_t> Half(std::uint16_t r, std::uint16_t g, std::uint16_t b, std::uint32_t texels = 16)
{
    std::vector<std::uint8_t> data(texels * 8);
    for (std::uint32_t i = 0; i < texels; ++i) {
        const std::uint16_t values[4]{ r, g, b, 0x3C00 };
        std::memcpy(data.data() + i * 8, values, 8);
    }
    return data;
}

int main()
{
    // 4x4 patches. 0x3800 = 0.5, 0x7E00 = NaN, 0x7C00 = +inf, 0x1400 ~ 0.001.
    const auto grey = Half(0x3800, 0x3800, 0x3800), black = Half(0, 0, 0), nan = Half(0x7E00, 0x7E00, 0x7E00);
    const auto inf = Half(0x7C00, 0, 0), dim = Half(0x1400, 0x1400, 0x1400);
    auto classify = [](const std::vector<std::uint8_t>& data) { return ClassifyPatch(DXGI_FORMAT_R16G16B16A16_FLOAT, data.data(), 4, 4, 32); };
    Require(classify(grey).lit == 16 && classify(grey).dark == 0, "a grey patch is lit");
    Require(classify(black).dark == 16 && classify(black).lit == 0, "a black patch is dark");
    Require(classify(nan).dark == 16, "NaN output counts as dark");
    Require(classify(inf).dark == 16, "infinite output counts as dark");
    Require(classify(dim).dark == 0 && classify(dim).lit == 0, "a very dim image is neither dark nor lit");
    Require(!ClassifyPatch(DXGI_FORMAT_BC1_UNORM, grey.data(), 4, 4, 32).supported, "unsupported formats are not judged");

    // R11G11B10: 0.5 in each channel (exponent 14).
    std::vector<std::uint8_t> packed(16 * 4);
    const std::uint32_t half = (14u << 6) | ((14u << 6) << 11) | ((14u << 5) << 22);
    for (int i = 0; i < 16; ++i) { std::memcpy(packed.data() + i * 4, &half, 4); }
    Require(ClassifyPatch(DXGI_FORMAT_R11G11B10_FLOAT, packed.data(), 4, 4, 16).lit == 16, "R11G11B10 decodes");
    std::vector<std::uint8_t> rgba(16 * 4, 0);
    for (int i = 0; i < 16; ++i) { rgba[i * 4 + 3] = 255; }
    Require(ClassifyPatch(DXGI_FORMAT_R8G8B8A8_UNORM, rgba.data(), 4, 4, 16).dark == 16, "alpha alone does not light a texel");

    // Four lit-input, black-output samples in a row trip the detector once.
    NeuralBlackDetector detector;
    for (int i = 1; i < NeuralBlackDetector::kStrikes; ++i) { Require(!detector.Add(classify(grey), classify(black)), "not before four strikes"); }
    Require(detector.Add(classify(grey), classify(black)), "trips on the fourth black sample");
    detector.Reset();
    for (int i = 1; i < NeuralBlackDetector::kStrikes; ++i) { detector.Add(classify(grey), classify(nan)); }
    Require(!detector.Add(classify(grey), classify(grey)), "one good frame clears the strikes");
    for (int i = 1; i < NeuralBlackDetector::kStrikes; ++i) { Require(!detector.Add(classify(grey), classify(black)), "counting restarts"); }
    Require(!detector.Add(classify(black), classify(black)), "a dark scene is skipped, not counted");
    Require(detector.Add(classify(grey), classify(black)), "and does not reset the strikes either");
    detector.Reset();
    for (int i = 0; i < 10; ++i) { Require(!detector.Add(classify(dim), classify(black)), "dim input never trips"); }
    std::puts("PASS NR output guard: format decoding, dark/lit classification and the black-output detector");
}
