#include "FrameGen/SourceDLSSGInterop.h"
#include <dxgi1_4.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::SourceDLSSG;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

static void Check(HRESULT hr, const char* reason)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: %s HRESULT=0x%08lX\n", reason, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static std::vector<D3D12StageSample> samples;

static std::vector<D3D12StageSample> Taken(D3D12Stage stage)
{
    std::vector<D3D12StageSample> result;
    for (const auto& sample : samples) { if (sample.stage == stage) { result.push_back(sample); } }
    return result;
}

struct Devices
{
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> source, destination;
    Devices()
    {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device11, nullptr, &context11), "create hardware D3D11 device");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        Check(device11.As(&dxgi), "query DXGI device");
        Check(dxgi->GetAdapter(&adapter), "matching adapter");
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12)), "matching D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create presenting queue");
        // Large enough that a copy takes measurable GPU time.
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 4096; desc.Height = 4096; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&source)), "copy source");
        Check(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&destination)), "copy destination");
    }
};

// One frame in the backend's order: early NR list, DLSS-G tag list, output list.
static void Frame(Devices& d, Interop& interop)
{
    ID3D12GraphicsCommandList* list = nullptr;
    Check(interop.Begin(Work::Upscaling, &list), "begin early list");
    auto marks = interop.Marks(Work::Upscaling);
    marks.Begin(D3D12Stage::kNeuralPrepare);
    Check(Interop::RecordCopy(list, d.source.Get(), d.destination.Get()), "record early copy");
    marks.End(D3D12Stage::kNeuralPrepare);
    Check(interop.Submit(Work::Upscaling), "submit early list");
    Check(interop.Begin(Work::FrameGeneration, &list), "begin tag list");
    Check(interop.Submit(Work::FrameGeneration), "submit empty tag list");
    Check(interop.Begin(Work::SwapChain, &list), "begin output list");
    marks = interop.Marks(Work::SwapChain);
    marks.Begin(D3D12Stage::kOutput);
    Check(Interop::RecordCopy(list, d.destination.Get(), d.source.Get()), "record output copy");
    marks.End(D3D12Stage::kOutput);
    // Unclosed and repeated passes are dropped, never misattributed.
    marks.Begin(D3D12Stage::kXeFGInputs);
    marks.Begin(D3D12Stage::kOutput);
    Check(interop.Submit(Work::SwapChain), "submit output list");
}

static void Retire(Interop& interop)
{
    Check(interop.Drain(), "drain");
    interop.Timer().Harvest();
}

static void InteropChecks()
{
    Devices d;
    Interop interop;
    Check(interop.Initialize(d.device11.Get(), d.device12.Get(), d.queue.Get()), "initialize production interop");
    interop.Timer().SetSink([](void*, const D3D12StageSample& sample) { samples.push_back(sample); }, nullptr);
    ComPtr<ID3D12Fence> input;
    Check(d.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&input)), "create input completion fence");

    Frame(d, interop);
    Retire(interop);
    Require(samples.empty() && interop.Timer().Availability() == S_FALSE, "a disabled timer creates and records nothing");

    interop.Timer().SetEnabled(true);
    std::uint64_t inputValue = 0;
    for (int frame = 0; frame < 4; ++frame) {
        Frame(d, interop);
        // DLSS-G still holds the inputs when the queue waits: an exact sample.
        Check(interop.WaitForInputReaders(input.Get(), ++inputValue, true), "wait for held inputs");
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        Check(input->Signal(inputValue), "release inputs");
    }
    Frame(d, interop);
    // Already released before the queue waits: only an upper bound.
    Check(input->Signal(++inputValue), "release before the wait");
    Check(interop.WaitForInputReaders(input.Get(), inputValue, true), "wait for released inputs");
    // No output list since the last hold: nothing to measure from.
    Check(interop.WaitForInputReaders(input.Get(), inputValue, true), "unpaired wait");
    // No generated frames: DLSS-G's release is not a generation cost.
    Frame(d, interop);
    Check(interop.WaitForInputReaders(input.Get(), inputValue, false), "wait without generation");
    Retire(interop);
    Require(SUCCEEDED(interop.Timer().Availability()), "timer created on first enabled list");

    const auto early = Taken(D3D12Stage::kEarlyNeuralList), tags = Taken(D3D12Stage::kFrameGenTagList);
    const auto output = Taken(D3D12Stage::kOutputList), prepare = Taken(D3D12Stage::kNeuralPrepare);
    const auto copy = Taken(D3D12Stage::kOutput), holds = Taken(D3D12Stage::kFrameGenerationHold);
    std::printf("lists early=%zu tags=%zu output=%zu passes prepare=%zu output=%zu holds=%zu\n",
        early.size(), tags.size(), output.size(), prepare.size(), copy.size(), holds.size());
    Require(early.size() == 6 && tags.size() == 6 && output.size() == 6, "every enabled list delivers its total");
    Require(prepare.size() == 6 && copy.size() == 6, "every closed pass delivers once");
    Require(Taken(D3D12Stage::kXeFGInputs).empty(), "an unclosed pass is dropped");
    for (std::size_t i = 0; i < output.size(); ++i) {
        std::printf("frame %llu early %.3f prepare %.3f output list %.3f copy %.3f\n",
            static_cast<unsigned long long>(output[i].frame), early[i].ms, prepare[i].ms, output[i].ms, copy[i].ms);
        Require(prepare[i].ms > 0.0f && copy[i].ms > 0.0f, "a 128 MiB copy takes measurable GPU time");
        Require(prepare[i].ms <= early[i].ms + 0.001f && copy[i].ms <= output[i].ms + 0.001f, "a pass fits inside its list");
        Require(prepare[i].frame + 1 == output[i].frame && copy[i].frame == output[i].frame,
            "a frame's lists before the output list carry the previous output count");
        Require(i == 0 || output[i].frame == output[i - 1].frame + 1, "output lists count presenting-queue frames");
    }
    Require(holds.size() == 5, "one hold per paired wait; unpaired and non-generating waits measure nothing");
    for (std::size_t i = 0; i < 4; ++i) {
        std::printf("hold %zu %.3f ms bounded=%u\n", i, holds[i].ms, holds[i].bounded);
        Require(!holds[i].bounded && holds[i].ms >= 5.0f, "a held wait measures the hold from the output list's end");
        Require(holds[i].frame == output[i].frame, "a hold belongs to its frame's output list");
    }
    Require(holds[4].bounded, "an input fence completed before the wait gives an upper bound");

    // Steady state: records are reused without ever running out.
    samples.clear();
    for (int frame = 0; frame < 300; ++frame) {
        Frame(d, interop);
        Check(input->Signal(++inputValue), "release");
        Check(interop.WaitForInputReaders(input.Get(), inputValue, true), "steady wait");
    }
    Retire(interop);
    Require(interop.Timer().DroppedLists() == 0, "steady frames never exhaust timer records");
    Require(Taken(D3D12Stage::kOutputList).size() == 300 && Taken(D3D12Stage::kFrameGenerationHold).size() == 300,
        "steady frames deliver every list and hold");

    // Disabling stops new records; lists already in flight still deliver.
    samples.clear();
    Frame(d, interop);
    interop.Timer().SetEnabled(false);
    Frame(d, interop);
    Retire(interop);
    Require(Taken(D3D12Stage::kOutputList).size() == 1, "disabling applies from the next list");
    std::puts("PASS: production interop times lists, passes and frame generation holds; bounded, unpaired, non-generating, unclosed and repeated cases; 300 steady frames without exhaustion; enable/disable");
}

static void ExhaustionChecks()
{
    Devices d;
    D3D12StageTimer timer;
    timer.SetEnabled(true);
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(D3D12StageTimer::kRecords + 1);
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists(D3D12StageTimer::kRecords + 1);
    std::vector<int> records;
    for (std::size_t i = 0; i < lists.size(); ++i) {
        Check(d.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])), "allocator");
        Check(d.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
            IID_PPV_ARGS(&lists[i])), "list");
        records.push_back(timer.Open(d.device12.Get(), d.queue.Get(), lists[i].Get(), D3D12Stage::kOutputList));
    }
    Require(std::count(records.begin(), records.end(), D3D12StageTimer::kNoRecord) == 1 &&
        records.back() == D3D12StageTimer::kNoRecord && timer.DroppedLists() == 1,
        "with every record in flight a list goes untimed instead of waiting");
    timer.Discard(records.front());
    Check(lists.back()->Close(), "close untimed list");
    Check(lists.back()->Reset(allocators.back().Get(), nullptr), "reset untimed list");
    Require(timer.Open(d.device12.Get(), d.queue.Get(), lists.back().Get(), D3D12Stage::kOutputList) == records.front(),
        "a discarded record is reused");
    std::puts("PASS: record exhaustion drops timing, never waits; discard frees a record");
}

int main()
{
    InteropChecks();
    ExhaustionChecks();
    return 0;
}
