#pragma once

#include "PerformanceTuning.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
	using D3D12Stage = PerformanceTuning::D3D12Stage;

	struct D3D12StageSample
	{
		D3D12Stage stage{};
		float ms{};
		// The timer's presenting-queue frame count, and the CPU QPC when the
		// list was opened, for aligning with the frame trace.
		std::uint64_t frame{};
		std::int64_t qpc{};
		bool bounded{};
	};
	using D3D12StageSink = void (*)(void* context, const D3D12StageSample& sample);

	// Timestamps for command lists on the presenting queue. A record is read
	// only after the fence value that follows its list has completed, so timing
	// never adds a CPU or GPU wait: with no free record a list goes untimed.
	// All methods run on the host render thread.
	class D3D12StageTimer
	{
	public:
		static constexpr int kNoRecord = -1;
		// Three slots per work type plus the hold lists stay below this.
		static constexpr std::size_t kRecords = 16;
		// List begin and end, then up to seven passes.
		static constexpr std::size_t kQueriesPerRecord = 16;
		// Interop::Begin keeps the GPU within three frames of the CPU. A fourth
		// hold list has always retired by the time it is reused.
		static constexpr std::size_t kHoldLists = 4;

		D3D12StageTimer() = default;
		D3D12StageTimer(const D3D12StageTimer&) = delete;
		D3D12StageTimer& operator=(const D3D12StageTimer&) = delete;

		void SetSink(D3D12StageSink sink, void* context) { sink_ = sink; sinkContext_ = context; }
		// Applies from the next Open. Lists already in flight still deliver.
		void SetEnabled(bool enabled) { enabled_ = enabled; }
		bool Enabled() const { return enabled_; }
		// S_FALSE until first enabled use; then the creation result.
		HRESULT Availability() const { return availability_; }
		std::uint64_t DroppedLists() const { return droppedLists_; }

		// Starts timing a list just reset for recording.
		int Open(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, D3D12Stage listStage);
		void Begin(int record, D3D12Stage stage);
		void End(int record, D3D12Stage stage);
		// Before the list closes; resolves everything it recorded.
		void Close(int record);
		// After the list executed and fence was signalled to value.
		void Submitted(int record, ID3D12Fence* fence, std::uint64_t value);
		// The list will never execute: free its record.
		void Discard(int record);
		// Call between the queue's Wait for DLSS-G's input fence and the signal
		// of fence to value. Measures from the end of the last output list.
		// bounded: that input fence had already completed before the Wait.
		void SubmitHold(ID3D12CommandQueue* queue, bool bounded, ID3D12Fence* fence, std::uint64_t value);
		// Delivers every record whose fence has completed, in submission order.
		void Harvest();
		// After a failed drain: GPU work may still write these objects.
		void Abandon();

	private:
		enum class State : std::uint8_t { Free, Open, Pending };
		struct Span
		{
			D3D12Stage stage{};
			std::uint8_t begin{}, end{};
			bool closed{};
		};
		struct Record
		{
			State state{ State::Free };
			D3D12Stage listStage{};
			ID3D12GraphicsCommandList* list{};
			ID3D12Fence* fence{};
			std::uint64_t value{}, frame{}, sequence{};
			std::int64_t qpc{};
			std::array<Span, kQueriesPerRecord / 2 - 1> spans{};
			std::uint8_t spanCount{}, queries{};
			int holdList{ -1 };
			bool hold{}, bounded{};
		};

		bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
		int Acquire();
		UINT Query(int record, std::size_t index) const { return static_cast<UINT>(record * kQueriesPerRecord + index); }
		bool Writable(int record) const;
		void Deliver(const Record& record, D3D12Stage stage, std::uint64_t begin, std::uint64_t end);

		Microsoft::WRL::ComPtr<ID3D12Device> device_;
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
		Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
		const std::uint64_t* mapped_{};
		std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kHoldLists> holdAllocators_;
		std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, kHoldLists> holdLists_;
		std::array<int, kHoldLists> holdRecords_{ kNoRecord, kNoRecord, kNoRecord, kNoRecord };
		std::array<Record, kRecords> records_{};
		std::uint64_t frequency_{}, frame_{}, sequence_{}, droppedLists_{};
		// The latest output list's end query and record, until a hold uses them.
		UINT lastOutputEnd_{ UINT_MAX };
		std::uint64_t lastOutputFrame_{};
		std::int64_t lastOutputQpc_{};
		D3D12StageSink sink_{};
		void* sinkContext_{};
		HRESULT availability_{ S_FALSE };
		bool enabled_{};
	};

	// A recorder's view of its list's timer record. The default is inert.
	class D3D12StageMarks
	{
	public:
		D3D12StageMarks() = default;
		D3D12StageMarks(D3D12StageTimer* timer, int record) : timer_(timer), record_(record) {}
		void Begin(D3D12Stage stage) const { if (timer_) { timer_->Begin(record_, stage); } }
		void End(D3D12Stage stage) const { if (timer_) { timer_->End(record_, stage); } }
	private:
		D3D12StageTimer* timer_{};
		int record_{ D3D12StageTimer::kNoRecord };
	};
}
