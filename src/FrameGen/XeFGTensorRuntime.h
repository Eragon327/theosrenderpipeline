#pragma once
#include "XeFGTensorAPI.h"
#include <filesystem>
#include <string>

namespace TheosRenderPipeline
{
    // One optional extension per Intel presenter owner. A failed retirement keeps
    // its active flag and all modules/resources so another presenter cannot start.
    class XeFGTensorRuntime {
    public:
        bool Select(ID3D12Device*, const std::filesystem::path& intelDirectory, bool requested);
        HRESULT Start(ID3D12Device*, HMODULE fg, HMODULE ll);
        HRESULT Retire();
        bool Selected() const { return module_ != nullptr; }
        bool ModelReady() const { return (Snapshot().flags & XeFGTensor::ModelReady) != 0; }
        bool Unsafe() const { return (Snapshot().flags & XeFGTensor::Unsafe) != 0; }
        XeFGTensor::Stats Snapshot() const;
        const std::filesystem::path& Directory() const { return directory_; }
        const std::string& Status() const { return status_; }
    private:
        HMODULE module_{};
        XeFGTensor::API api_{};
        std::filesystem::path directory_;
        std::string status_{"Tensor backend off"};
        bool active_{};
        bool unsafe_{};
    };
}
