#include "ScreenshotDiagnostics.h"
#include "CommunityShaderIntegration.h"
#include "FrameGen/D3D11EntryObservers.h"
#include "FrameGen/NvidiaHost.h"
#include <PCH.h>
#include <atomic>

namespace TheosRenderPipeline::ScreenshotDiagnostics
{
    namespace
    {
        std::atomic<std::uint64_t> presents{};
        std::atomic<unsigned> copiesLogged{};

        ID3D11Texture2D* ScreenshotTarget()
        {
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            return renderer ? reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSCREENSHOT].texture) : nullptr;
        }

        bool Queued()
        {
            auto* controls = RE::MenuControls::GetSingleton();
            return controls && controls->screenshotHandler && controls->screenshotHandler->screenshotQueued;
        }

        std::string Describe(ID3D11Resource* resource)
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) { return "non-texture"; }
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            auto* host = NvidiaHost::GetSingleton();
            const char* role = texture.Get() == host->GameFacingTexture() ? "game-facing framebuffer" :
                texture.Get() == host->NativePresentationTexture() ? "TRP presentation" : "other";
            return std::format("{} {}x{} format={} samples={} bind=0x{:X}", role, desc.Width, desc.Height,
                static_cast<unsigned>(desc.Format), desc.SampleDesc.Count, desc.BindFlags);
        }

        void NoDispatch(ID3D11DeviceContext*, UINT, UINT, UINT, D3D11EntryObservers::Dispatch) {}

        void ObserveCopy(ID3D11DeviceContext*, ID3D11Resource* destination, ID3D11Resource* source)
        {
            auto* target = ScreenshotTarget();
            if (!target || destination != target || copiesLogged.fetch_add(1, std::memory_order_relaxed) >= 40) { return; }
            logger::info("[ScreenshotDiagnostics] CopyResource into kSCREENSHOT at present {} queued={} from {}",
                presents.load(std::memory_order_relaxed), Queued(), Describe(source));
        }
    }

    void OnPresent(ID3D11DeviceContext* context, bool enabled)
    {
        if (!enabled || !context || CommunityShaders::Active()) { return; }
        const auto present = presents.fetch_add(1, std::memory_order_relaxed) + 1;
        static bool installed = false, described = false;
        if (!installed) {
            installed = true;
            const bool observed = D3D11EntryObservers::Ensure(context, &NoDispatch, &ObserveCopy);
            logger::info("[ScreenshotDiagnostics] CopyResource observer {}", observed ? "installed" : "unavailable");
        }
        if (auto* target = ScreenshotTarget(); target && !described) {
            described = true;
            logger::info("[ScreenshotDiagnostics] kSCREENSHOT is {}", Describe(target));
        }
        static bool wasQueued = false;
        const bool queued = Queued();
        if (queued != wasQueued) {
            logger::info("[ScreenshotDiagnostics] screenshot {} at present {}", queued ? "queued" : "no longer queued", present);
            wasQueued = queued;
        }
    }
}
