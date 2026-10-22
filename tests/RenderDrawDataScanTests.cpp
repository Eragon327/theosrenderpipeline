#include "RenderDrawDataScan.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using TheosRenderPipeline::Compatibility::FindRenderDrawData;
using TheosRenderPipeline::Compatibility::MatchesRenderDrawDataPrologue;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// The large-frame prologue of ImGui's DX11 RenderDrawData as the supported mods
// compile it (Photo Mode 2.0.2's frame and stack-probe sizes).
static const std::vector<std::uint8_t> kPrologue{ 0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xB8, 0xE4, 0xFF, 0xFF,
    0xB8, 0x48, 0x1C, 0x00, 0x00 };

static void Append(std::vector<std::uint8_t>& code, const std::vector<std::uint8_t>& bytes) { code.insert(code.end(), bytes.begin(), bytes.end()); }
static void Dword(std::vector<std::uint8_t>& code, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) { code.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
}
// A function body: filler, then add eax,5000 and add ecx,10000 as MSVC emits them.
static std::vector<std::uint8_t> Function(bool vertices, bool indices, std::size_t gap = 0x40)
{
    std::vector<std::uint8_t> code;
    Append(code, kPrologue);
    code.insert(code.end(), gap, 0x90);
    if (vertices) { code.push_back(0x05); Dword(code, 5000); }
    code.insert(code.end(), 0x20, 0x90);
    if (indices) { code.push_back(0x81); code.push_back(0xC1); Dword(code, 10000); }
    code.insert(code.end(), 0x20, 0xCC);
    return code;
}

int main()
{
    Require(MatchesRenderDrawDataPrologue(kPrologue), "the profiled prologue matches");
    auto rip = kPrologue; rip[6] = 0x05;  // lea rbp,[rip+disp32] would not relocate
    Require(!MatchesRenderDrawDataPrologue(rip), "a RIP-relative lea is refused");
    Require(!MatchesRenderDrawDataPrologue(std::span(kPrologue).first(16)), "a truncated prologue is refused");

    std::vector<std::uint8_t> code(0x300, 0xCC);
    const auto at = code.size();
    Append(code, Function(true, true));
    auto found = FindRenderDrawData(code);
    Require(found.size() == 1 && found[0] == at, "the one RenderDrawData is found at its entry");

    Require(FindRenderDrawData(Function(true, false)).empty(), "both growth constants are required");
    Require(FindRenderDrawData(Function(false, true)).empty(), "both growth constants are required");
    Require(FindRenderDrawData(Function(true, true, 0x600)).empty(), "constants beyond the window do not count");

    auto twice = Function(true, true);
    Append(twice, Function(true, true));
    Require(FindRenderDrawData(twice).size() == 2, "two candidates are both reported, so the caller refuses");

    auto tail = Function(true, true);
    tail.resize(kPrologue.size() + 3);
    Require(FindRenderDrawData(tail).empty(), "a candidate cut off at the end of a section is skipped safely");
    std::puts("RenderDrawData scan: entry, both constants, window, uniqueness and section-end guards passed.");
}
