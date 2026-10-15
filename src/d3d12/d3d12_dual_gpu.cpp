#include "xrfg/d3d12_dual_gpu.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT busy = DXGI_ERROR_WAS_STILL_DRAWING;
std::atomic<bool> worker_fault_latched{};
std::atomic<bool> worker_context_owned{};
void check(HRESULT hr) { if (FAILED(hr)) throw hr; }
std::uint64_t qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
std::uint64_t qpf() { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }
bool complete(ID3D12Fence* fence, std::uint64_t value) {
    if (!value) return true;
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) throw DXGI_ERROR_DEVICE_REMOVED;
    return completed >= value;
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    void create(ID3D12Device* device) {
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(list->Close());
    }
    void begin() { check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr)); }
    void execute(ID3D12CommandQueue* queue) {
        check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    }
};
ComPtr<ID3D12CommandQueue> queue(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC d{}; d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> result;
    check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&result))); return result;
}
ComPtr<ID3D12Fence> fence(ID3D12Device* device) {
    ComPtr<ID3D12Fence> result; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result))); return result;
}
struct SharedFence {
    ComPtr<ID3D12Fence> primary, worker;
    void create(ID3D12Device* p, ID3D12Device* w) {
        check(p->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&primary)));
        HANDLE handle{}; check(p->CreateSharedHandle(primary.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
        const auto hr = w->OpenSharedHandle(handle, IID_PPV_ARGS(&worker)); CloseHandle(handle); check(hr);
    }
};
struct SharedBuffer {
    ComPtr<ID3D12Heap> primary_heap, worker_heap;
    ComPtr<ID3D12Resource> primary, worker;
    void create(ID3D12Device* p, ID3D12Device* w, std::uint64_t bytes) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        const auto pa = p->GetResourceAllocationInfo(0, 1, &d);
        const auto wa = w->GetResourceAllocationInfo(0, 1, &d);
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = std::max(pa.SizeInBytes, wa.SizeInBytes);
        hd.Alignment = std::max(pa.Alignment, wa.Alignment);
        hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        hd.Properties.CreationNodeMask = hd.Properties.VisibleNodeMask = 1;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        check(p->CreateHeap(&hd, IID_PPV_ARGS(&primary_heap)));
        HANDLE handle{}; check(p->CreateSharedHandle(primary_heap.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
        const auto hr = w->OpenSharedHandle(handle, IID_PPV_ARGS(&worker_heap)); CloseHandle(handle); check(hr);
        check(p->CreatePlacedResource(primary_heap.Get(), 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&primary)));
        check(w->CreatePlacedResource(worker_heap.Get(), 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&worker)));
    }
};
ComPtr<ID3D12Resource> texture(ID3D12Device* device, D3D12_RESOURCE_DESC d) {
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r))); return r;
}
struct GpuSpan {
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    std::uint64_t frequency{}, gpu_origin{}, cpu_origin{};
    void create(ID3D12Device* device, ID3D12CommandQueue* queue) {
        D3D12_QUERY_HEAP_DESC q{}; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count = 2;
        check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)));
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 16;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        check(queue->GetTimestampFrequency(&frequency));
        // Calibration is per queue/device. No raw cross-device timestamp subtraction.
        check(queue->GetClockCalibration(&gpu_origin, &cpu_origin));
    }
    void begin(ID3D12GraphicsCommandList* list) { list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); }
    void end(ID3D12GraphicsCommandList* list) {
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
    }
    std::uint64_t microseconds() {
        const std::uint64_t* p{}; D3D12_RANGE range{0, 16};
        check(readback->Map(0, &range, reinterpret_cast<void**>(const_cast<std::uint64_t**>(&p))));
        const auto a = p[0], b = p[1]; D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
        return b >= a && frequency ? (b - a) * 1'000'000 / frequency : 0;
    }
};
std::uint64_t view_hash(std::span<const D3D12ReprojectionView> views) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto add = [&](std::uint32_t v) { hash ^= v; hash *= 1099511628211ull; };
    for (const auto& v : views) {
        for (float value : {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
            v.pose.position.x, v.pose.position.y, v.pose.position.z, v.fov.angle_left, v.fov.angle_right,
            v.fov.angle_up, v.fov.angle_down}) add(std::bit_cast<std::uint32_t>(value));
        for (auto value : {v.image_rect.offset_x, v.image_rect.offset_y, v.image_rect.width,
            v.image_rect.height, v.array_slice}) add(value);
    }
    return hash;
}
}

struct D3D12DualGpuOffload::Impl {
    DualGpuExclusiveLease context_lease; // destroyed after all GPU/context members
    struct Slot {
        DualGpuSlotPolicy policy;
        std::atomic<std::uint64_t> canceled_token{};
        SharedBuffer input, output;
        ComPtr<ID3D12Resource> worker_input, worker_current, worker_synthetic, returned;
        Commands original_commands, input_commands, worker_input_commands, worker_output_commands, return_commands, publish_commands;
        GpuSpan input_span, worker_span, worker_input_span, worker_output_span, return_span;
        D3D12HistoryCaptureTicket capture{};
        D3D12FrameSynthesisTicket worker_ticket{};
        std::array<D3D12ReprojectionView, 2> views{};
        std::uint32_t current_index{}, synthetic_index{}, view_count{};
        bool publication_joined{};
        float fraction{0.5F}; bool pair{}, primed{};
        std::uint64_t submitted{}, ready_at{};
        ComPtr<ID3D12Resource> primary_source;
        ComPtr<ID3D12Fence> producer;
        DualGpuTiming timing;
    };
    ComPtr<ID3D12Device> primary, worker;
    ComPtr<ID3D12CommandQueue> primary_input_queue, primary_return_queue, worker_queue;
    ComPtr<ID3D12Fence> original_fence, return_fence, publish_fence, worker_synthesis_fence;
    SharedFence input_fence, worker_fence;
    std::shared_ptr<D3D12SwapchainHistory> primary_history, worker_history;
    std::unique_ptr<D3D12FrameSynthesizer> synth;
    std::vector<ComPtr<ID3D12Resource>> current_destinations, synthetic_destinations;
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES destination_state{};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::array<Slot, dual_gpu_transport_slots> slots;
    std::atomic<std::uint64_t> generation{};
    std::uint64_t next_token{1}, budget_us{20'000}, last_worker_serial{}, frequency{qpf()};
    DualGpuPublicationOrder publication_order;
    bool history_valid{}, reset_history{};
    std::uint32_t test_delay_ms{};
    bool test_failure{};
    std::atomic<bool> stopping{}, failed{};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::deque<DualGpuTiming> timings;
    static std::atomic<std::uint64_t> next_generation;

    void transfer(ID3D12GraphicsCommandList* list, ID3D12Resource* tex, ID3D12Resource* buffer, bool to_buffer) {
        transition(list, tex, D3D12_RESOURCE_STATE_COMMON,
            to_buffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
        transition(list, buffer, D3D12_RESOURCE_STATE_COMMON,
            to_buffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT sub = 0; sub < footprints.size(); ++sub) {
            D3D12_TEXTURE_COPY_LOCATION t{}; t.pResource = tex;
            t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; t.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION b{}; b.pResource = buffer;
            b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; b.PlacedFootprint = footprints[sub];
            list->CopyTextureRegion(to_buffer ? &b : &t, 0, 0, 0, to_buffer ? &t : &b, nullptr);
        }
        transition(list, tex, to_buffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(list, buffer, to_buffer ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    void local_copy(Commands& cmd, ID3D12CommandQueue* queue, ID3D12Resource* src, ID3D12Resource* dst) {
        cmd.begin();
        transition(cmd.list.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd.list.Get(), dst, destination_state, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd.list->CopyResource(dst, src);
        transition(cmd.list.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(cmd.list.Get(), dst, D3D12_RESOURCE_STATE_COPY_DEST, destination_state);
        cmd.execute(queue);
    }
    void initialize(ID3D12Device* p, std::shared_ptr<D3D12SwapchainHistory> history,
        std::span<ID3D12Resource* const> currents, std::span<ID3D12Resource* const> synthetics,
        DXGI_FORMAT format, D3D12_RESOURCE_STATES state, std::wstring_view id, D3D12NvidiaOpticalFlowOptions options) {
        if (!p || !history || currents.empty() || synthetics.empty() || id.empty()) throw E_INVALIDARG;
        if (worker_fault_latched) throw DXGI_ERROR_DEVICE_REMOVED;
        primary = p; primary_history = std::move(history); destination_state = state;
        desc = currents[0]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || desc.DepthOrArraySize > 2 || !desc.DepthOrArraySize) throw E_INVALIDARG;
        // Supported color formats are uncompressed and single-plane.
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM: break;
        default: throw E_INVALIDARG;
        }
        auto adapters = enumerate_dual_gpu_adapters();
        const auto selected = std::find_if(adapters.begin(), adapters.end(), [&](const auto& a) { return a.durable_id == id; });
        if (selected == adapters.end() || selected->vendor_id != 0x10de) throw DXGI_ERROR_NOT_FOUND;
        const auto pl = p->GetAdapterLuid(), wl = selected->current_luid;
        if (pl.LowPart == wl.LowPart && pl.HighPart == wl.HighPart) throw E_INVALIDARG;
        // Upstream observed freezes with concurrent OFA contexts. Refuse an
        // additional engine before device/context creation; never queue it.
        if (!context_lease.acquire(worker_context_owned)) throw busy;
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumAdapterByLuid(wl, IID_PPV_ARGS(&adapter)));
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&worker)));
        primary_input_queue = queue(p); primary_return_queue = queue(p); worker_queue = queue(worker.Get());
        original_fence = fence(p); return_fence = fence(p); publish_fence = fence(p);
        input_fence.create(p, worker.Get()); worker_fence.create(p, worker.Get());
        for (auto* image : currents) current_destinations.emplace_back(image);
        for (auto* image : synthetics) synthetic_destinations.emplace_back(image);
        footprints.resize(desc.DepthOrArraySize);
        std::uint64_t bytes{};
        p->GetCopyableFootprints(&desc, 0, desc.DepthOrArraySize, 0, footprints.data(), nullptr, nullptr, &bytes);
        // Footprints are portable for the same resource description; verify,
        // rather than assuming a driver's packing agrees across adapters.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> wf(footprints.size()); std::uint64_t wb{};
        worker->GetCopyableFootprints(&desc, 0, desc.DepthOrArraySize, 0, wf.data(), nullptr, nullptr, &wb);
        if (wb != bytes) throw E_INVALIDARG;
        for (std::size_t i = 0; i < wf.size(); ++i)
            if (wf[i].Offset != footprints[i].Offset || wf[i].Footprint.RowPitch != footprints[i].Footprint.RowPitch ||
                wf[i].Footprint.Width != footprints[i].Footprint.Width || wf[i].Footprint.Height != footprints[i].Footprint.Height) throw E_INVALIDARG;
        std::vector<ID3D12Resource*> inputs, wc, ws;
        for (auto& slot : slots) {
            slot.input.create(p, worker.Get(), bytes); slot.output.create(p, worker.Get(), bytes);
            slot.worker_input = texture(worker.Get(), desc); slot.worker_current = texture(worker.Get(), desc);
            slot.worker_synthetic = texture(worker.Get(), desc); slot.returned = texture(p, desc);
            slot.original_commands.create(p); slot.input_commands.create(p); slot.return_commands.create(p); slot.publish_commands.create(p);
            slot.worker_input_commands.create(worker.Get()); slot.worker_output_commands.create(worker.Get());
            slot.input_span.create(p, primary_input_queue.Get()); slot.return_span.create(p, primary_return_queue.Get());
            slot.worker_span.create(worker.Get(), worker_queue.Get());
            slot.worker_input_span.create(worker.Get(), worker_queue.Get());
            slot.worker_output_span.create(worker.Get(), worker_queue.Get());
            inputs.push_back(slot.worker_input.Get()); wc.push_back(slot.worker_current.Get()); ws.push_back(slot.worker_synthetic.Get());
        }
        worker_history = std::make_shared<D3D12SwapchainHistory>();
        check(worker_history->initialize(worker.Get(), worker_queue.Get(), inputs, D3D12_RESOURCE_STATE_COMMON));
        synth = std::make_unique<D3D12FrameSynthesizer>();
        // No FidelityFX fallback here: both OFA and composition belong to the selected NVIDIA worker.
        check(synth->initialize(worker.Get(), worker_queue.Get(), worker_history, wc, ws, format,
            D3D12_RESOURCE_STATE_COMMON, D3D12OpticalFlowBackend::nvidia, options, true));
        check(synth->completion_fence(&worker_synthesis_fence));
        generation = ++next_generation;
        thread = std::thread([this] { run(); });
    }
    Slot* find(const D3D12FrameSynthesisTicket& t) {
        if (t.offload_generation != generation || !t.offload_token || t.work_slot >= slots.size()) return nullptr;
        auto& slot = slots[t.work_slot];
        if (slot.canceled_token.load() == t.offload_token) return nullptr;
        return slot.policy.token == t.offload_token && t.synthetic_fence_value == t.offload_token && slot.current_index == t.current_destination_index &&
            slot.synthetic_index == t.synthetic_destination_index && t.fence_value == t.offload_token &&
            slot.policy.key == DualGpuFrameKey{t.offload_generation, t.current_serial, t.offload_views_hash}
            ? &slot : nullptr;
    }
    void collect(Slot& slot, bool published) {
        slot.timing.serial = slot.policy.key.serial; slot.timing.token = slot.policy.token;
        slot.timing.deadline_qpc = slot.policy.deadline; slot.timing.ready_qpc = slot.ready_at;
        slot.timing.age_us = (qpc() - slot.submitted) * 1'000'000 / frequency;
        slot.timing.published = published; slot.timing.dropped = slot.pair && !published;
        if (timings.size() == 32) timings.pop_front(); timings.push_back(slot.timing);
    }
    void worker_start(Slot& slot, std::uint32_t index) {
        check(worker_queue->Wait(input_fence.worker.Get(), slot.policy.input_value));
        if (reset_history || !history_valid ||
            (slot.capture.serial != last_worker_serial + 1 && slot.capture.serial != last_worker_serial)) {
            check(synth->retire_previous()); history_valid = false; reset_history = false;
        }
        slot.worker_input_commands.begin(); slot.worker_span.begin(slot.worker_input_commands.list.Get());
        slot.worker_input_span.begin(slot.worker_input_commands.list.Get());
        transfer(slot.worker_input_commands.list.Get(), slot.worker_input.Get(), slot.input.worker.Get(), false);
        slot.worker_input_span.end(slot.worker_input_commands.list.Get());
        slot.worker_input_commands.execute(worker_queue.Get());
        D3D12HistoryCaptureTicket worker_capture{};
        check(worker_history->capture(index, &worker_capture)); check(worker_history->commit(worker_capture));
        slot.primed = !history_valid || !slot.pair;
        if (slot.primed) {
            check(synth->retire_previous());
            check(synth->submit_prime(worker_capture, std::span(slot.views.data(), slot.view_count), index, &slot.worker_ticket));
        } else {
            check(synth->submit_pair(worker_capture, std::span(slot.views.data(), slot.view_count), std::span(slot.views.data(), slot.view_count), index, index,
                &slot.worker_ticket, std::nullopt, {}, false, slot.fraction));
        }
        history_valid = true; last_worker_serial = slot.capture.serial;
        slot.worker_output_commands.begin();
        slot.worker_output_span.begin(slot.worker_output_commands.list.Get());
        if (!slot.primed) transfer(slot.worker_output_commands.list.Get(), slot.worker_synthetic.Get(), slot.output.worker.Get(), true);
        slot.worker_output_span.end(slot.worker_output_commands.list.Get());
        slot.worker_span.end(slot.worker_output_commands.list.Get());
        slot.worker_output_commands.execute(worker_queue.Get());
        check(worker_queue->Signal(worker_fence.worker.Get(), slot.policy.token));
        slot.policy.worker_value = slot.policy.token; slot.policy.phase = DualGpuPhase::worker;
    }
    void pump() {
        if (FAILED(primary->GetDeviceRemovedReason()) || FAILED(worker->GetDeviceRemovedReason())) throw DXGI_ERROR_DEVICE_REMOVED;
        for (std::uint32_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i]; auto& p = slot.policy;
            if (slot.canceled_token.load() == p.token && p.token) p.canceled = true;
            if (p.phase == DualGpuPhase::published && p.canceled) p.phase = DualGpuPhase::discarded;
            if (p.phase == DualGpuPhase::input && complete(input_fence.primary.Get(), p.input_value) &&
                qpc() - slot.submitted >= test_delay_ms * frequency / 1000) {
                if (test_failure) throw DXGI_ERROR_DEVICE_REMOVED;
                slot.primary_source.Reset(); slot.producer.Reset();
                slot.timing.input_us = slot.input_span.microseconds();
                worker_start(slot, i);
            }
            if (p.phase == DualGpuPhase::worker && complete(worker_fence.primary.Get(), p.worker_value)) {
                const auto unpack_us = slot.worker_input_span.microseconds();
                const auto pack_us = slot.worker_output_span.microseconds();
                const auto whole_worker_us = slot.worker_span.microseconds();
                slot.timing.input_us += unpack_us;
                slot.timing.output_us = pack_us;
                slot.timing.worker_us = whole_worker_us > unpack_us + pack_us ? whole_worker_us - unpack_us - pack_us : 0;
                D3D12NvidiaGpuTiming timing{};
                while (synth->consume_nvidia_gpu_timing(&timing) == S_OK) slot.timing.flow_and_synthesis = timing;
                if (slot.primed || p.canceled || qpc() > p.deadline) {
                    p.phase = DualGpuPhase::discarded; collect(slot, false);
                } else {
                    check(primary_return_queue->Wait(worker_fence.primary.Get(), p.worker_value));
                    slot.return_commands.begin(); slot.return_span.begin(slot.return_commands.list.Get());
                    transfer(slot.return_commands.list.Get(), slot.returned.Get(), slot.output.primary.Get(), false);
                    slot.return_span.end(slot.return_commands.list.Get()); slot.return_commands.execute(primary_return_queue.Get());
                    check(primary_return_queue->Signal(return_fence.Get(), p.token));
                    p.return_value = p.token; p.phase = DualGpuPhase::returning;
                }
            }
            if (p.phase == DualGpuPhase::returning && complete(return_fence.Get(), p.return_value)) {
                slot.timing.output_us += slot.return_span.microseconds(); slot.ready_at = qpc();
                p.phase = p.canceled || slot.ready_at > p.deadline ? DualGpuPhase::discarded : DualGpuPhase::ready;
                if (p.phase == DualGpuPhase::discarded) collect(slot, false);
            }
            if (p.phase == DualGpuPhase::ready && (p.canceled || qpc() > p.deadline)) {
                p.phase = DualGpuPhase::discarded; collect(slot, false);
            }
            if ((p.phase == DualGpuPhase::discarded || (p.phase == DualGpuPhase::published && slot.publication_joined)) &&
                complete(original_fence.Get(), p.original_value) && complete(input_fence.primary.Get(), p.input_value) &&
                complete(worker_fence.primary.Get(), p.worker_value) && complete(return_fence.Get(), p.return_value) &&
                complete(publish_fence.Get(), p.publish_value)) p.phase = DualGpuPhase::free;
        }
    }
    void run() noexcept {
        try {
            std::unique_lock lock(mutex);
            while (!stopping) { pump(); wake.wait_for(lock, std::chrono::milliseconds(1)); }
        } catch (...) { failed = true; worker_fault_latched = true; }
    }
    bool idle_locked() {
        for (const auto& s : slots) if (s.policy.phase != DualGpuPhase::free) return false;
        return true;
    }
};
std::atomic<std::uint64_t> D3D12DualGpuOffload::Impl::next_generation{};

D3D12DualGpuOffload::D3D12DualGpuOffload() = default;
D3D12DualGpuOffload::~D3D12DualGpuOffload() {
    if (!impl_) return;
    impl_->stopping = true; impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    // A worker fault is not recoverable here. Do not free allocations still
    // referenced by a GPU, and do not wait on its fence. The thread join above
    // can still block if a driver submission API itself hangs.
    bool pending = impl_->failed;
    try {
        for (const auto& s : impl_->slots) {
            const auto& p = s.policy;
            pending |= !complete(impl_->original_fence.Get(), p.original_value) ||
                !complete(impl_->input_fence.primary.Get(), p.input_value) ||
                !complete(impl_->worker_fence.primary.Get(), p.worker_value) ||
                !complete(impl_->return_fence.Get(), p.return_value) ||
                !complete(impl_->publish_fence.Get(), p.publish_value);
        }
    } catch (...) { pending = true; }
    if (pending) {
        worker_fault_latched = true; // includes incomplete normal teardown; bound repeated quarantine allocation
        static_cast<void>(impl_.release()); // process-lifetime quarantine, no background thread.
    }
}
HRESULT D3D12DualGpuOffload::initialize(ID3D12Device* p, ID3D12CommandQueue*,
    std::shared_ptr<D3D12SwapchainHistory> h, std::span<ID3D12Resource* const> c,
    std::span<ID3D12Resource* const> s, DXGI_FORMAT f, D3D12_RESOURCE_STATES state,
    std::wstring_view id, D3D12NvidiaOpticalFlowOptions options) noexcept {
    if (impl_) return E_UNEXPECTED;
    try { auto candidate = std::make_unique<Impl>(); candidate->initialize(p, std::move(h), c, s, f, state, id, options);
        impl_ = std::move(candidate); return S_OK;
    } catch (HRESULT hr) { return hr; } catch (...) { return E_FAIL; }
}
HRESULT D3D12DualGpuOffload::submit(const D3D12HistoryCaptureTicket& capture,
    std::span<const D3D12ReprojectionView> views, std::span<const D3D12ReprojectionView> target,
    std::uint32_t synthetic_index, std::uint32_t current_index, D3D12FrameSynthesisTicket* ticket,
    bool pair, float fraction) noexcept {
    if (!ticket) return E_POINTER; *ticket = {};
    if (!impl_ || impl_->failed || worker_fault_latched) return DXGI_ERROR_DEVICE_REMOVED;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return busy;
    D3D12HistoryConsumerLease lease{}; bool submitted = false;
    try {
        auto& p = *impl_;
        if (views.size() != 2 || current_index >= p.current_destinations.size() ||
            (pair && (target.size() != views.size() || synthetic_index >= p.synthetic_destinations.size() || view_hash(views) != view_hash(target)))) return E_INVALIDARG;
        std::array<DualGpuViewRect, 2> rects{};
        for (std::size_t i = 0; i < views.size(); ++i) {
            const auto& v = views[i]; rects[i] = {v.image_rect.offset_x, v.image_rect.offset_y,
                v.image_rect.width ? v.image_rect.width : static_cast<UINT>(p.desc.Width),
                v.image_rect.height ? v.image_rect.height : p.desc.Height,
                v.array_slice == UINT_MAX ? static_cast<UINT>(i) : v.array_slice};
        }
        if (!valid_dual_gpu_stereo(rects, p.desc.Width, p.desc.Height, p.desc.DepthOrArraySize)) return E_INVALIDARG;
        // One executing job; readiness/publication slots do not constitute a
        // worker backlog. Saturation immediately leaves the original path.
        std::array<DualGpuSlotPolicy, dual_gpu_transport_slots> policies;
        for (std::size_t i = 0; i < policies.size(); ++i) policies[i] = p.slots[i].policy;
        const auto admission = dual_gpu_admit(policies);
        if (!admission) return busy;
        const auto index = *admission; auto& slot = p.slots[index];
        const auto token = p.next_token++;
        slot.policy = {}; slot.policy.token = token;
        slot.policy.key = {p.generation, capture.serial, view_hash(views)};
        slot.submitted = qpc(); slot.policy.deadline = slot.submitted + p.budget_us * p.frequency / 1'000'000;
        slot.capture = capture; slot.pair = pair; slot.fraction = fraction; slot.timing = {};
        slot.view_count = static_cast<UINT>(views.size()); slot.publication_joined = false;
        slot.current_index = current_index; slot.synthetic_index = synthetic_index;
        std::copy(views.begin(), views.end(), slot.views.begin());
        check(p.primary_history->acquire_consumer(capture, &lease, slot.primary_source.ReleaseAndGetAddressOf(), slot.producer.ReleaseAndGetAddressOf()));
        check(p.primary_input_queue->Wait(slot.producer.Get(), capture.fence_value));
        p.local_copy(slot.original_commands, p.primary_input_queue.Get(), slot.primary_source.Get(), p.current_destinations[current_index].Get());
        submitted = true; slot.policy.original_value = token;
        check(p.primary_input_queue->Signal(p.original_fence.Get(), token));
        slot.input_commands.begin(); slot.input_span.begin(slot.input_commands.list.Get());
        p.transfer(slot.input_commands.list.Get(), slot.primary_source.Get(), slot.input.primary.Get(), true);
        slot.input_span.end(slot.input_commands.list.Get()); slot.input_commands.execute(p.primary_input_queue.Get());
        slot.policy.input_value = token;
        check(p.primary_input_queue->Signal(p.input_fence.primary.Get(), token));
        check(p.primary_history->retire_consumer(lease, p.input_fence.primary.Get(), token)); lease = {};
        slot.policy.phase = DualGpuPhase::input;
        ticket->offload_generation = p.generation; ticket->offload_token = token; ticket->offload_views_hash = slot.policy.key.views_hash;
        ticket->current_serial = capture.serial; ticket->previous_serial = pair ? p.last_worker_serial : 0;
        ticket->fence_value = token; ticket->synthetic_fence_value = pair ? token : 0;
        ticket->work_slot = index; ticket->current_destination_index = current_index;
        ticket->synthetic_destination_index = synthetic_index;
        p.wake.notify_all(); return S_OK;
    } catch (HRESULT hr) {
        if (!submitted && lease.lease_serial) impl_->primary_history->cancel_consumer(lease);
        impl_->failed = true; worker_fault_latched = true; return hr;
    } catch (...) { impl_->failed = true; worker_fault_latched = true; return E_FAIL; }
}
bool D3D12DualGpuOffload::ready(const D3D12FrameSynthesisTicket& t) noexcept {
    if (!impl_ || impl_->failed || worker_fault_latched) return false;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return false;
    try { auto* s = impl_->find(t); return s && s->policy.eligible({t.offload_generation, t.current_serial, t.offload_views_hash},
        t.offload_token, qpc(), complete(impl_->return_fence.Get(), s->policy.return_value)); }
    catch (...) { impl_->failed = true; worker_fault_latched = true; return false; }
}
HRESULT D3D12DualGpuOffload::publish(const D3D12FrameSynthesisTicket& t) noexcept {
    if (!impl_ || impl_->failed || worker_fault_latched) return DXGI_ERROR_DEVICE_REMOVED;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return busy;
    try {
        auto* s = impl_->find(t); if (!s || !s->policy.eligible({t.offload_generation, t.current_serial, t.offload_views_hash},
            t.offload_token, qpc(), complete(impl_->return_fence.Get(), s->policy.return_value))) return busy;
        const auto publication_value = impl_->publication_order.reserve(t.offload_token);
        if (!publication_value) return busy;
        // The only work a runtime queue may join is this primary-local copy.
        impl_->local_copy(s->publish_commands, impl_->primary_return_queue.Get(), s->returned.Get(),
            impl_->synthetic_destinations[s->synthetic_index].Get());
        s->policy.publish_value = *publication_value;
        check(impl_->primary_return_queue->Signal(impl_->publish_fence.Get(), s->policy.publish_value));
        s->policy.phase = DualGpuPhase::published; impl_->collect(*s, true); return S_OK;
    } catch (HRESULT hr) { impl_->failed = true; worker_fault_latched = true; return hr; }
      catch (...) { impl_->failed = true; worker_fault_latched = true; return E_FAIL; }
}
void D3D12DualGpuOffload::discard(const D3D12FrameSynthesisTicket& t) noexcept {
    if (!impl_ || t.offload_generation != impl_->generation.load() ||
        !t.offload_token || t.work_slot >= impl_->slots.size()) return;
    auto& requested = impl_->slots[t.work_slot].canceled_token;
    auto previous = requested.load();
    while (previous < t.offload_token && !requested.compare_exchange_weak(previous, t.offload_token)) {}
    impl_->wake.notify_all();
}
HRESULT D3D12DualGpuOffload::synchronize_consumer(ID3D12CommandQueue* queue, const D3D12FrameSynthesisTicket& t) noexcept {
    if (!impl_ || !queue) return E_FAIL;
    // Original and publication fences are deliberately distinct. Original
    // completion never depends on worker/input/return completion.
    if (!t.synthetic_fence_value) {
        const auto hr = queue->Wait(impl_->original_fence.Get(), t.fence_value);
        if (FAILED(hr)) { impl_->failed = true; worker_fault_latched = true; }
        return hr;
    }
    if (impl_->failed || worker_fault_latched) return E_FAIL;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return busy;
    auto* s = impl_->find(t);
    if (!s || s->policy.phase != DualGpuPhase::published || !s->policy.publish_value) return busy;
    const auto hr = queue->Wait(impl_->publish_fence.Get(), s->policy.publish_value);
    if (SUCCEEDED(hr)) s->publication_joined = true;
    else { impl_->failed = true; worker_fault_latched = true; }
    return hr;
}
HRESULT D3D12DualGpuOffload::completion_fence(ID3D12Fence** output) noexcept {
    if (!output || !impl_) return E_POINTER; *output = impl_->return_fence.Get(); (*output)->AddRef(); return S_OK;
}
HRESULT D3D12DualGpuOffload::retire_previous() noexcept {
    if (!impl_) return S_OK;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return busy;
    impl_->reset_history = true;
    impl_->generation = ++Impl::next_generation;
    for (auto& slot : impl_->slots) slot.policy.canceled = true;
    return S_OK;
}
HRESULT D3D12DualGpuOffload::idle() noexcept {
    if (!impl_) return S_OK;
    if (impl_->failed || worker_fault_latched) return DXGI_ERROR_DEVICE_REMOVED;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock()) return busy;
    return impl_->idle_locked() ? S_OK : busy;
}
HRESULT D3D12DualGpuOffload::transport_timing(DualGpuTiming* out) noexcept {
    if (!out || !impl_) return E_POINTER;
    std::unique_lock lock(impl_->mutex, std::try_to_lock); if (!lock.owns_lock() || impl_->timings.empty()) return S_FALSE;
    *out = impl_->timings.front(); impl_->timings.pop_front(); return S_OK;
}
HRESULT D3D12DualGpuOffload::timing(D3D12NvidiaGpuTiming* out) noexcept {
    DualGpuTiming t{}; const auto hr = transport_timing(&t); if (hr == S_OK && out) *out = t.flow_and_synthesis;
    return out ? hr : E_POINTER;
}
void D3D12DualGpuOffload::set_budget_us(std::uint64_t us) noexcept {
    if (!impl_) return; std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (lock.owns_lock()) impl_->budget_us = std::clamp<std::uint64_t>(us, 1'000, 50'000);
}
void D3D12DualGpuOffload::test_controls(std::uint32_t delay, bool failure) noexcept {
    if (!impl_) return; std::scoped_lock lock(impl_->mutex);
    impl_->test_delay_ms = std::min<std::uint32_t>(delay, 2000); impl_->test_failure = failure;
}
}
