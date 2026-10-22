#include "FrameGen/NativeUIComposition.h"
#include "HookSafety.h"
#include "D3D11ContextSlots.h"
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using SetTargets = void(WINAPI*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static SetTargets originalTargets{};
static ID3D11RenderTargetView* gameTarget{};
static ID3D11RenderTargetView* uiTarget{};
static unsigned redirects{};
static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void WINAPI RedirectTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
{
    if (count == 1 && targets && targets[0] == gameTarget) {
        ++redirects;
        originalTargets(context, 1, &uiTarget, depth);
    } else { originalTargets(context, count, targets, depth); }
}

int main()
{
    // No debug-layer facade: exercise the native runtime's mutable dispatch table.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "native WARP device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto texture = [&] {
        ComPtr<ID3D11Texture2D> result;
        Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &result)), "texture");
        return result;
    };
    auto scene = texture(), presentation = texture(), game = texture();
    ComPtr<ID3D11RenderTargetView> sceneRTV, presentationRTV, gameRTV;
    Require(SUCCEEDED(device->CreateRenderTargetView(scene.Get(), nullptr, &sceneRTV)) &&
        SUCCEEDED(device->CreateRenderTargetView(presentation.Get(), nullptr, &presentationRTV)) &&
        SUCCEEDED(device->CreateRenderTargetView(game.Get(), nullptr, &gameRTV)), "render views");
    TheosRenderPipeline::NativeUIComposition composition;
    Require(composition.Initialize(device.Get(), context.Get(), scene.Get(), desc, true), "composition");
    gameTarget = gameRTV.Get();
    uiTarget = composition.RenderRTV();
    TheosRenderPipeline::HookSafety::SlotRegistry hooks;
    auto* table = *reinterpret_cast<std::uintptr_t**>(context.Get());
    auto* slot = table + TheosRenderPipeline::D3D11ContextSlots::kOMSetRenderTargets;
    Require(hooks.Install(slot,
        reinterpret_cast<std::uintptr_t>(&RedirectTargets), originalTargets), "target hook");
    auto readDesc = desc;
    readDesc.BindFlags = 0; readDesc.Usage = D3D11_USAGE_STAGING; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    Require(SUCCEEDED(device->CreateTexture2D(&readDesc, nullptr, &readback)), "readback");
    for (unsigned frame = 0; frame < 3; ++frame) {
        const float sceneColor[]{0, 0, 1, 1};
        const float uiColor[]{float(frame == 0), float(frame != 0), 0, 1};
        context->ClearRenderTargetView(sceneRTV.Get(), sceneColor);
        context->CopyResource(presentation.Get(), scene.Get());
        context->OMSetRenderTargets(1, &gameTarget, nullptr);
        Require(redirects == frame + 1, "UI target hook survives composition into the next frame");
        ComPtr<ID3D11RenderTargetView> bound;
        context->OMGetRenderTargets(1, &bound, nullptr);
        Require(bound.Get() == uiTarget, "producer rebind reaches dedicated UI target");
        context->ClearRenderTargetView(bound.Get(), uiColor);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        Require(composition.Compose(context.Get(), presentation.Get()), "dedicated composition");
        context->CopyResource(readback.Get(), presentation.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "read pixels");
        auto* pixel = static_cast<const unsigned char*>(mapped.pData);
        Require(pixel[frame == 0 ? 0 : 1] == 255 && pixel[2] == 0, "current UI appears over world");
        context->Unmap(readback.Get(), 0);
    }
    context->ClearState();
    composition.ResetAfterRetirement(); // Last readback retired composition.
    Require(*slot == reinterpret_cast<std::uintptr_t>(&RedirectTargets), "hook remains installed");
    DWORD protection{}, ignored{};
    Require(VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &protection) != FALSE, "unprotect hook");
    *slot = reinterpret_cast<std::uintptr_t>(originalTargets);
    Require(VirtualProtect(slot, sizeof(*slot), protection, &ignored) != FALSE, "restore hook protection");
    std::puts("Native dispatch hook continuity and dedicated UI pixels PASS");
}
