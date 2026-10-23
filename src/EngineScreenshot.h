#pragma once
#include <string>

// Skyrim saves screenshots (PrintScreen, Photo Mode) from kSCREENSHOT, its own
// target at the render size TRP gives the game: no DLSS output, no NR, no native
// UI. The engine's save is kept as written and its path queued here; the
// presenter replaces the file with the finished frame of the same Present, as
// it does for ReShade's screenshots.
namespace TheosRenderPipeline::EngineScreenshot
{
    struct Request
    {
        std::string path;
        int jpegQuality{ 90 };
    };

    // Hooks BSGraphics::Renderer::SaveRenderTargetToFile. Declines (and logs the
    // entry bytes) when the function's entry cannot be detoured safely; Skyrim's
    // own screenshots are then unchanged.
    void Install();
    // The oldest saved screenshot waiting for the finished frame.
    bool Take(Request& a_request);
}
