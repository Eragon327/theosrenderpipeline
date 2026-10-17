#include "D3D12StageTimer.h"

#include <limits>

namespace TheosRenderPipeline::SourceDLSSG
{
	namespace
	{
		std::int64_t QueryQpc()
		{
			LARGE_INTEGER now{};
			::QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
	}

	bool D3D12StageTimer::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
	{
		if (availability_ != S_FALSE) { return SUCCEEDED(availability_); }
		auto fail = [&](HRESULT hr) {
			availability_ = FAILED(hr) ? hr : E_FAIL;
			for (auto& list : holdLists_) { list.Reset(); }
			for (auto& allocator : holdAllocators_) { allocator.Reset(); }
			mapped_ = nullptr; readback_.Reset(); heap_.Reset();
			return false;
		};
		if (!device || !queue) { return fail(E_INVALIDARG); }
		UINT64 frequency = 0;
		auto hr = queue->GetTimestampFrequency(&frequency);
		if (FAILED(hr) || !frequency) { return fail(hr); }
		const D3D12_QUERY_HEAP_DESC heapDesc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(kRecords * kQueriesPerRecord), 0 };
		if (FAILED(hr = device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap_)))) { return fail(hr); }
		(void)heap_->SetName(L"TRP stage timestamps");
		D3D12_HEAP_PROPERTIES readback{};
		readback.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = kRecords * kQueriesPerRecord * sizeof(std::uint64_t);
		buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(hr = device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) { return fail(hr); }
		(void)readback_->SetName(L"TRP stage timestamp readback");
		// Mapped for the timer's lifetime. Each range is read only after the
		// fence that follows its resolve has completed.
		const D3D12_RANGE read{ 0, static_cast<SIZE_T>(buffer.Width) };
		void* mapped = nullptr;
		if (FAILED(hr = readback_->Map(0, &read, &mapped))) { return fail(hr); }
		mapped_ = static_cast<const std::uint64_t*>(mapped);
		for (std::size_t i = 0; i < kHoldLists; ++i) {
			if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&holdAllocators_[i]))) ||
				FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, holdAllocators_[i].Get(), nullptr,
					IID_PPV_ARGS(&holdLists_[i]))) ||
				FAILED(hr = holdLists_[i]->Close())) { return fail(hr); }
			(void)holdLists_[i]->SetName(L"TRP frame generation hold timestamp");
		}
		device_ = device;
		frequency_ = frequency;
		availability_ = S_OK;
		return true;
	}

	int D3D12StageTimer::Acquire()
	{
		for (std::size_t i = 0; i < records_.size(); ++i) {
			if (records_[i].state == State::Free) { return static_cast<int>(i); }
		}
		return kNoRecord;
	}

	bool D3D12StageTimer::Writable(int record) const
	{
		return record >= 0 && static_cast<std::size_t>(record) < records_.size() &&
			records_[record].state == State::Open && records_[record].list;
	}

	int D3D12StageTimer::Open(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, D3D12Stage listStage)
	{
		// A hold may only pair with this frame's output list.
		if (listStage == D3D12Stage::kOutputList) { lastOutputEnd_ = UINT_MAX; ++frame_; }
		if (!enabled_ || !list || !Initialize(device, queue)) { return kNoRecord; }
		const auto record = Acquire();
		if (record == kNoRecord) { ++droppedLists_; return kNoRecord; }
		auto& r = records_[record];
		r = {};
		r.state = State::Open; r.listStage = listStage; r.list = list;
		r.frame = frame_; r.qpc = QueryQpc();
		r.queries = 2; // 0 list begin, 1 list end
		list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Query(record, 0));
		return record;
	}

	void D3D12StageTimer::Begin(int record, D3D12Stage stage)
	{
		if (!Writable(record)) { return; }
		auto& r = records_[record];
		if (r.spanCount == r.spans.size() || r.queries + 2 > kQueriesPerRecord) { return; }
		for (std::size_t i = 0; i < r.spanCount; ++i) {
			if (r.spans[i].stage == stage) { return; } // One interval per stage and list.
		}
		r.spans[r.spanCount++] = { stage, r.queries, 0, false };
		r.list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Query(record, r.queries++));
	}

	void D3D12StageTimer::End(int record, D3D12Stage stage)
	{
		if (!Writable(record)) { return; }
		auto& r = records_[record];
		for (std::size_t i = 0; i < r.spanCount; ++i) {
			auto& span = r.spans[i];
			if (span.stage != stage || span.closed) { continue; }
			span.end = r.queries; span.closed = true;
			r.list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Query(record, r.queries++));
			return;
		}
	}

	void D3D12StageTimer::Close(int record)
	{
		if (!Writable(record)) { return; }
		auto& r = records_[record];
		r.list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Query(record, 1));
		r.list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, Query(record, 0), r.queries,
			readback_.Get(), Query(record, 0) * sizeof(std::uint64_t));
		r.list = nullptr;
	}

	void D3D12StageTimer::Submitted(int record, ID3D12Fence* fence, std::uint64_t value)
	{
		if (record < 0 || static_cast<std::size_t>(record) >= records_.size()) { return; }
		auto& r = records_[record];
		// A list submitted without Close has no resolved timestamps.
		if (r.state != State::Open || r.list || !fence) { Discard(record); return; }
		r.state = State::Pending; r.fence = fence; r.value = value; r.sequence = ++sequence_;
		if (r.listStage == D3D12Stage::kOutputList) {
			lastOutputEnd_ = Query(record, 1); lastOutputFrame_ = r.frame; lastOutputQpc_ = r.qpc;
		}
	}

	void D3D12StageTimer::Discard(int record)
	{
		if (record < 0 || static_cast<std::size_t>(record) >= records_.size()) { return; }
		// A discarded output list never ran, so nothing can pair with it.
		if (lastOutputEnd_ == Query(record, 1)) { lastOutputEnd_ = UINT_MAX; }
		records_[record] = {};
	}

	void D3D12StageTimer::SubmitHold(ID3D12CommandQueue* queue, bool bounded, ID3D12Fence* fence, std::uint64_t value)
	{
		const auto outputEnd = lastOutputEnd_;
		lastOutputEnd_ = UINT_MAX;
		if (!enabled_ || outputEnd == UINT_MAX || !queue || !fence || availability_ != S_OK) { return; }
		std::size_t slot = kHoldLists;
		for (std::size_t i = 0; i < kHoldLists; ++i) {
			if (holdRecords_[i] == kNoRecord) { slot = i; break; }
		}
		const auto record = slot == kHoldLists ? kNoRecord : Acquire();
		if (record == kNoRecord) { ++droppedLists_; return; }
		// Free means its previous hold record was harvested: that list retired.
		auto* list = holdLists_[slot].Get();
		if (FAILED(holdAllocators_[slot]->Reset()) || FAILED(list->Reset(holdAllocators_[slot].Get(), nullptr))) {
			++droppedLists_;
			return;
		}
		const auto base = Query(record, 0);
		// The output list ended its query earlier on this queue. Copy that value
		// beside the new timestamp so the pair is read from one record.
		list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, outputEnd, 1,
			readback_.Get(), base * sizeof(std::uint64_t));
		list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
		list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1, 1,
			readback_.Get(), (base + 1) * sizeof(std::uint64_t));
		if (FAILED(list->Close())) { ++droppedLists_; return; }
		ID3D12CommandList* lists[]{ list };
		queue->ExecuteCommandLists(1, lists);
		auto& r = records_[record];
		r = {};
		r.state = State::Pending; r.listStage = D3D12Stage::kFrameGenerationHold;
		r.fence = fence; r.value = value; r.sequence = ++sequence_;
		r.frame = lastOutputFrame_; r.qpc = lastOutputQpc_;
		r.holdList = static_cast<int>(slot); r.hold = true; r.bounded = bounded;
		holdRecords_[slot] = record;
	}

	void D3D12StageTimer::Deliver(const Record& record, D3D12Stage stage, std::uint64_t begin, std::uint64_t end)
	{
		// Zero is a timestamp never written; a reversed pair is not a duration.
		if (!sink_ || !begin || end < begin) { return; }
		const auto ms = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency_));
		sink_(sinkContext_, { stage, ms, record.frame, record.qpc, record.bounded });
	}

	void D3D12StageTimer::Harvest()
	{
		if (!mapped_) { return; }
		constexpr auto removed = (std::numeric_limits<std::uint64_t>::max)();
		std::array<bool, kRecords> retired{};
		for (std::size_t i = 0; i < records_.size(); ++i) {
			const auto& r = records_[i];
			if (r.state != State::Pending) { continue; }
			const auto completed = r.fence->GetCompletedValue();
			// After device removal nothing is retired; leave every record pending.
			if (completed == removed) { return; }
			retired[i] = completed >= r.value;
		}
		// Records reuse slots out of order. Smoothing and the trace expect time order.
		for (;;) {
			std::size_t i = kRecords;
			for (std::size_t j = 0; j < kRecords; ++j) {
				if (retired[j] && (i == kRecords || records_[j].sequence < records_[i].sequence)) { i = j; }
			}
			if (i == kRecords) { return; }
			retired[i] = false;
			auto& r = records_[i];
			const auto* values = mapped_ + i * kQueriesPerRecord;
			if (r.hold) {
				Deliver(r, D3D12Stage::kFrameGenerationHold, values[0], values[1]);
				holdRecords_[r.holdList] = kNoRecord;
			} else {
				Deliver(r, r.listStage, values[0], values[1]);
				for (std::size_t span = 0; span < r.spanCount; ++span) {
					const auto& s = r.spans[span];
					if (s.closed) { Deliver(r, s.stage, values[s.begin], values[s.end]); }
				}
			}
			r = {};
		}
	}

	void D3D12StageTimer::Abandon()
	{
		for (auto& list : holdLists_) { (void)list.Detach(); }
		for (auto& allocator : holdAllocators_) { (void)allocator.Detach(); }
		(void)readback_.Detach();
		(void)heap_.Detach();
		(void)device_.Detach();
		mapped_ = nullptr;
		records_.fill({});
		availability_ = E_ABORT;
	}
}
