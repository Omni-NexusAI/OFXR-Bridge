#include "xrfg/d3d12_dual_gpu.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Manual, bounded hardware diagnostic. Never register this executable with CTest.
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT eye_width = 256, height = 128, channels = 4;
constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr HRESULT busy = DXGI_ERROR_WAS_STILL_DRAWING;

void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + ": HRESULT " + std::to_string(static_cast<std::int32_t>(hr)));
}
[[nodiscard]] D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type = type; p.CreationNodeMask = p.VisibleNodeMask = 1; return p;
}
[[nodiscard]] D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after; return b;
}
struct Layout {
    bool atlas{};
    UINT width() const { return atlas ? eye_width * 2 : eye_width; }
    UINT slices() const { return atlas ? 1 : 2; }
    const char* name() const { return atlas ? "atlas" : "array"; }
};
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 next_fence{1};
    HANDLE event{};
    explicit Fixture(const xrfg::DualGpuAdapter& identity) {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapterByLuid(identity.current_luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");
        DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "GetDesc1");
        require(desc.VendorId == identity.vendor_id && desc.DeviceId == identity.device_id &&
                desc.SubSysId == identity.subsystem_id && desc.Revision == identity.revision,
                "primary adapter identity changed since enumeration");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice(primary)");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue(primary)");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence(primary)");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "CreateEventW");
    }
    ~Fixture() { if (event) CloseHandle(event); }
    void wait(ID3D12Fence* target, UINT64 value) {
        require(target && target->GetCompletedValue() != UINT64_MAX, "device removed while waiting");
        if (target->GetCompletedValue() < value) {
            check(target->SetEventOnCompletion(value, event), "SetEventOnCompletion");
            require(WaitForSingleObject(event, 10'000) == WAIT_OBJECT_0, "10-second GPU wait expired");
            require(target->GetCompletedValue() >= value, "fence did not complete");
        }
    }
    template<class F> void execute(F&& record) {
        ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        record(list.Get()); check(list->Close(), "Close command list");
        ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
        const auto value = next_fence++; check(queue->Signal(fence.Get(), value), "Signal primary fence"); wait(fence.Get(), value);
    }
    [[nodiscard]] ComPtr<ID3D12Resource> texture(Layout layout) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = layout.width(); d.Height = height; d.DepthOrArraySize = static_cast<UINT16>(layout.slices());
        d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto p = heap(D3D12_HEAP_TYPE_DEFAULT); ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r)), "Create texture");
        return r;
    }
    [[nodiscard]] ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size;
        d.Height = 1; d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto p = heap(type); ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&r)), "Create transfer buffer"); return r;
    }
    struct Footprints {
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> layouts{};
        std::array<UINT, 2> rows{}; std::array<UINT64, 2> row_bytes{}; UINT64 size{};
    };
    [[nodiscard]] Footprints footprints(ID3D12Resource* r) {
        auto d = r->GetDesc(); Footprints f;
        device->GetCopyableFootprints(&d, 0, d.DepthOrArraySize, 0, f.layouts.data(), f.rows.data(), f.row_bytes.data(), &f.size);
        require(f.size && f.size < 2'000'000, "unexpected transfer footprint"); return f;
    }
    [[nodiscard]] std::vector<std::uint8_t> pattern(Layout layout, UINT frame) {
        const UINT width = layout.width(), slices = layout.slices();
        std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * slices * channels);
        for (UINT slice = 0; slice < slices; ++slice) for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const UINT eye = layout.atlas ? x / eye_width : slice;
            const UINT local_x = layout.atlas ? x % eye_width : x;
            const auto offset = ((static_cast<std::size_t>(slice) * height + y) * width + x) * channels;
            const UINT moving = (local_x + frame * (eye ? 7 : 11)) % eye_width;
            out[offset] = static_cast<std::uint8_t>((moving * 3 + y + eye * 51) % 256);
            out[offset + 1] = static_cast<std::uint8_t>((y * 5 + local_x / 3 + frame * 37) % 256);
            // Blue is a durable eye tag; motion lives in red and green.
            out[offset + 2] = static_cast<std::uint8_t>(eye ? 210 : 40);
            out[offset + 3] = 255;
        }
        return out;
    }
    void upload(ID3D12Resource* texture, Layout layout, const std::vector<std::uint8_t>& pixels) {
        const auto f = footprints(texture); auto staging = buffer(f.size, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{}; D3D12_RANGE none{0,0}; check(staging->Map(0, &none, &mapped), "Map upload");
        for (UINT s = 0; s < layout.slices(); ++s) for (UINT y = 0; y < height; ++y) {
            auto* dest = static_cast<std::uint8_t*>(mapped) + f.layouts[s].Offset + static_cast<std::size_t>(y) * f.layouts[s].Footprint.RowPitch;
            const auto* src = pixels.data() + (static_cast<std::size_t>(s) * height + y) * layout.width() * channels;
            std::memcpy(dest, src, layout.width() * channels);
        }
        staging->Unmap(0, nullptr);
        execute([&](ID3D12GraphicsCommandList* list) {
            const auto to_copy = barrier(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &to_copy);
            for (UINT s = 0; s < layout.slices(); ++s) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = staging.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = f.layouts[s];
                dst.pResource = texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = s;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            const auto back = barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &back);
        });
    }
    [[nodiscard]] std::vector<std::uint8_t> readback(ID3D12Resource* texture, Layout layout) {
        const auto f = footprints(texture); auto staging = buffer(f.size, D3D12_HEAP_TYPE_READBACK);
        execute([&](ID3D12GraphicsCommandList* list) {
            const auto to_copy = barrier(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            for (UINT s = 0; s < layout.slices(); ++s) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = s;
                dst.pResource = staging.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = f.layouts[s];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            const auto back = barrier(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &back);
        });
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(layout.width()) * height * layout.slices() * channels);
        void* mapped{}; D3D12_RANGE all{0, static_cast<SIZE_T>(f.size)}; check(staging->Map(0, &all, &mapped), "Map readback");
        for (UINT s = 0; s < layout.slices(); ++s) for (UINT y = 0; y < height; ++y) {
            const auto* src = static_cast<const std::uint8_t*>(mapped) + f.layouts[s].Offset + static_cast<std::size_t>(y) * f.layouts[s].Footprint.RowPitch;
            auto* dst = pixels.data() + (static_cast<std::size_t>(s) * height + y) * layout.width() * channels;
            std::memcpy(dst, src, layout.width() * channels);
        }
        D3D12_RANGE none{0,0}; staging->Unmap(0, &none); return pixels;
    }
};

[[nodiscard]] std::array<xrfg::D3D12ReprojectionView, 2> views(Layout layout) {
    std::array<xrfg::D3D12ReprojectionView, 2> v{};
    for (UINT eye = 0; eye < 2; ++eye) {
        v[eye].pose.orientation.w = 1.0f;
        v[eye].pose.position.x = eye ? 0.032f : -0.032f;
        v[eye].fov = {-0.78f, 0.78f, 0.61f, -0.61f};
        v[eye].image_rect = {layout.atlas ? eye * eye_width : 0, 0, eye_width, height};
        v[eye].array_slice = layout.atlas ? 0 : eye;
    }
    return v;
}
[[nodiscard]] bool wait_ready(xrfg::D3D12FrameSynthesizer& synth, const xrfg::D3D12FrameSynthesisTicket& ticket, UINT milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        if (synth.offload_ready(ticket)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
    return false;
}
[[nodiscard]] bool wait_idle(xrfg::D3D12FrameSynthesizer& synth, UINT milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        const auto hr = synth.wait_for_idle();
        if (hr == S_OK) return true;
        if (hr != busy) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
    return false;
}
void check_synthetic(const std::vector<std::uint8_t>& pixels,
                     const std::vector<std::uint8_t>& current,
                     const std::vector<std::uint8_t>& previous, Layout layout) {
    require(pixels.size() == current.size() && pixels.size() == previous.size(),
            "synthetic size differs from captured frame");
    for (UINT eye = 0; eye < 2; ++eye) {
        std::size_t colored = 0, changed_current = 0, changed_previous = 0;
        std::uint64_t blue_sum = 0;
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < eye_width; ++x) {
            const UINT slice = layout.atlas ? 0 : eye;
            const UINT physical_x = layout.atlas ? eye * eye_width + x : x;
            const auto at = ((static_cast<std::size_t>(slice) * height + y) * layout.width() + physical_x) * channels;
            colored += (pixels[at] || pixels[at + 1] || pixels[at + 2]) ? 1 : 0;
            blue_sum += pixels[at + 2];
            changed_current += (pixels[at] != current[at] || pixels[at + 1] != current[at + 1] ||
                        pixels[at + 2] != current[at + 2]) ? 1 : 0;
            changed_previous += (pixels[at] != previous[at] || pixels[at + 1] != previous[at + 1] ||
                        pixels[at + 2] != previous[at + 2]) ? 1 : 0;
        }
        require(colored > 0, "one synthetic eye has no RGB content");
        require(changed_current > 0, "one synthetic eye is an unchanged current copy");
        require(changed_previous > 0, "one synthetic eye is an unchanged previous copy");
        const auto mean_blue = blue_sum / (static_cast<std::uint64_t>(eye_width) * height);
        require(eye ? mean_blue > 150 : mean_blue < 100,
                "synthetic eye tag indicates a swapped or contaminated eye");
    }
}
struct Args {
    std::wstring primary, worker;
    std::string mode{"all"}, layout{"both"}, case_name{"normal"};
    UINT pairs{3}, budget_us{22'000}; bool run{};
};
[[nodiscard]] Args parse(int argc, wchar_t** argv) {
    Args a;
    const auto ascii = [](const wchar_t* value) {
        std::string result;
        for (const wchar_t* p = value; *p; ++p) {
            if (*p < 0x20 || *p > 0x7e) throw std::runtime_error("option value must be ASCII");
            result.push_back(static_cast<char>(*p));
        }
        return result;
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view v = argv[i];
        if (v == L"--list") { a.run = false; return a; }
        if (v == L"--run") a.run = true;
        else if (i + 1 < argc && v == L"--primary-id") a.primary = argv[++i];
        else if (i + 1 < argc && v == L"--worker-id") a.worker = argv[++i];
        else if (i + 1 < argc && v == L"--mode") a.mode = ascii(argv[++i]);
        else if (i + 1 < argc && v == L"--layout") a.layout = ascii(argv[++i]);
        else if (i + 1 < argc && v == L"--case") a.case_name = ascii(argv[++i]);
        else if (i + 1 < argc && v == L"--pairs") a.pairs = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        else if (i + 1 < argc && v == L"--budget-us") a.budget_us = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        else throw std::runtime_error("unknown or incomplete argument");
    }
    require(a.pairs >= 1 && a.pairs <= 6, "pairs must be 1..6");
    require(a.budget_us >= 1'000 && a.budget_us <= 50'000, "budget-us must be 1000..50000");
    require(a.mode == "all" || a.mode == "nofg" || a.mode == "stock2x" || a.mode == "custom2x", "invalid mode");
    require(a.layout == "both" || a.layout == "array" || a.layout == "atlas", "invalid layout");
    require(a.case_name == "normal" || a.case_name == "delay" || a.case_name == "worker-failure" ||
            a.case_name == "saturation" || a.case_name == "recreation", "invalid case");
    return a;
}

[[nodiscard]] xrfg::D3D12FrameSynthesisTicket run_case(
              const xrfg::DualGpuAdapter& primary, const xrfg::DualGpuAdapter& worker,
              const std::string& mode, Layout layout, const Args& args,
              const xrfg::D3D12FrameSynthesisTicket* stale_ticket = nullptr) {
    Fixture gpu(primary);
    std::array<ComPtr<ID3D12Resource>, 3> sources{gpu.texture(layout), gpu.texture(layout), gpu.texture(layout)};
    std::array<ComPtr<ID3D12Resource>, 3> current{gpu.texture(layout), gpu.texture(layout), gpu.texture(layout)};
    std::array<ComPtr<ID3D12Resource>, 3> synthetic{gpu.texture(layout), gpu.texture(layout), gpu.texture(layout)};
    std::array<ID3D12Resource*, 3> source_ptr{}, current_ptr{}, synthetic_ptr{};
    for (UINT i = 0; i < 3; ++i) { source_ptr[i] = sources[i].Get(); current_ptr[i] = current[i].Get(); synthetic_ptr[i] = synthetic[i].Get(); }
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(history->initialize(gpu.device.Get(), gpu.queue.Get(), source_ptr, D3D12_RESOURCE_STATE_RENDER_TARGET), "history.initialize");
    xrfg::D3D12FrameSynthesizer synth;
    xrfg::D3D12NvidiaOpticalFlowOptions options{};
    options.preset = xrfg::D3D12NvidiaPerformancePreset::medium;
    options.input_scale = xrfg::D3D12OpticalFlowInputScale::half;
    if (mode == "stock2x") {
        check(synth.initialize(gpu.device.Get(), gpu.queue.Get(), history, current_ptr, synthetic_ptr,
            format, D3D12_RESOURCE_STATE_RENDER_TARGET, xrfg::D3D12OpticalFlowBackend::nvidia, options, true), "stock initialize");
    } else if (mode == "custom2x") {
        check(synth.initialize_dual_gpu(gpu.device.Get(), gpu.queue.Get(), history, current_ptr, synthetic_ptr,
            format, D3D12_RESOURCE_STATE_RENDER_TARGET, worker.durable_id, options), "dual GPU initialize");
        synth.offload_budget_us((args.case_name == "normal" || args.case_name == "recreation") ? args.budget_us : 1'000);
        if (args.case_name == "delay" || args.case_name == "saturation") synth.offload_test_controls(100, false);
        if (args.case_name == "worker-failure") synth.offload_test_controls(0, true);
        if (stale_ticket) {
            require(!synth.offload_ready(*stale_ticket) &&
                    synth.publish_offload(*stale_ticket) == busy,
                    "prior engine lifetime ticket accepted after recreation");
        }
    }
    const auto v = views(layout);
    UINT published = 0, dropped = 0, missed = 0, verified_current = 0, verified_synthetic = 0;
    bool saturation_rejected = false, failure_observed = false;
    std::vector<xrfg::DualGpuTiming> timings;
    std::vector<xrfg::D3D12NvidiaGpuTiming> stock_timings;
    xrfg::D3D12FrameSynthesisTicket last_ticket{};
    double render_upload_us = 0;
    for (UINT frame = 0; frame <= args.pairs; ++frame) {
        const UINT index = frame % 3;
        // Reuse only after the preceding pair has completed. This keeps source,
        // output, and history leases finite even when a worker misses its deadline.
        if (mode == "custom2x") require(wait_idle(synth, 10'000), "offload idle timeout before source reuse");
        const auto expected = gpu.pattern(layout, frame);
        const auto start = std::chrono::steady_clock::now();
        gpu.upload(sources[index].Get(), layout, expected);
        render_upload_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        xrfg::D3D12HistoryCaptureTicket capture{};
        check(history->capture(index, &capture), "history.capture"); check(history->commit(capture), "history.commit");
        xrfg::D3D12FrameSynthesisTicket ticket{};
        if (mode == "nofg") {
            gpu.execute([&](ID3D12GraphicsCommandList* list) {
                auto a = barrier(sources[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                auto b = barrier(current[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                std::array<D3D12_RESOURCE_BARRIER, 2> bs{a,b}; list->ResourceBarrier(2,bs.data());
                list->CopyResource(current[index].Get(), sources[index].Get());
                a = barrier(sources[index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                b = barrier(current[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                bs = {a,b}; list->ResourceBarrier(2,bs.data());
            });
        } else if (frame == 0) {
            check(synth.submit_prime(capture, v, index, &ticket), "submit_prime");
        } else {
            check(synth.submit_pair(capture, v, v, index, index, &ticket), "submit_pair");
        }
        last_ticket = ticket;
        if (mode == "custom2x" && frame == 0 && args.case_name == "saturation") {
            xrfg::D3D12FrameSynthesisTicket rejected{};
            saturation_rejected = synth.submit_pair(capture, v, v, 1, 1, &rejected) == busy;
            require(saturation_rejected, "second worker job was admitted while first job active");
        }
        if (mode == "custom2x") {
            auto original = ticket; original.synthetic_fence_value = 0;
            check(synth.synchronize_consumer_queue(gpu.queue.Get(), original), "join original");
        }
        if (mode == "stock2x") {
            ComPtr<ID3D12Fence> completion;
            check(synth.completion_fence(&completion), "stock completion_fence");
            gpu.wait(completion.Get(), ticket.fence_value);
        }
        const auto original_pixels = gpu.readback(current[index].Get(), layout);
        require(original_pixels == expected, "current original differs from source bytes"); ++verified_current;
        if (mode == "custom2x" && args.case_name == "worker-failure") {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                if (synth.wait_for_idle() == DXGI_ERROR_DEVICE_REMOVED) { failure_observed = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < until);
            require(failure_observed, "injected worker failure was not observed");
            break;
        }
        if (mode == "custom2x" && frame > 0) {
            const bool ready = wait_ready(synth, ticket,
                (args.case_name == "normal" || args.case_name == "recreation") ? (args.budget_us / 1000 + 2) : 5);
            if (ready) {
                auto wrong = ticket; ++wrong.current_serial;
                require(!synth.offload_ready(wrong) && synth.publish_offload(wrong) == busy,
                        "mismatched serial accepted after valid ticket became ready");
                wrong = ticket; ++wrong.offload_views_hash;
                require(!synth.offload_ready(wrong) && synth.publish_offload(wrong) == busy,
                        "mismatched views hash accepted after valid ticket became ready");
                wrong = ticket; ++wrong.offload_generation;
                require(!synth.offload_ready(wrong) && synth.publish_offload(wrong) == busy,
                        "mismatched generation accepted after valid ticket became ready");
                wrong = ticket; ++wrong.offload_token;
                require(!synth.offload_ready(wrong) && synth.publish_offload(wrong) == busy,
                        "mismatched token accepted after valid ticket became ready");
                const auto hr = synth.publish_offload(ticket);
                if (hr == S_OK) {
                    require(synth.publish_offload(ticket) == busy,
                            "published ticket replay was accepted");
                    check(synth.synchronize_consumer_queue(gpu.queue.Get(), ticket), "join published synthetic");
                    const auto pixels = gpu.readback(synthetic[index].Get(), layout);
                    check_synthetic(pixels, original_pixels, gpu.pattern(layout, frame - 1), layout);
                    ++published; ++verified_synthetic;
                } else { ++dropped; ++missed; synth.discard_offload(ticket); }
            } else { ++dropped; ++missed; synth.discard_offload(ticket); }
        } else if (mode == "stock2x" && frame > 0) {
            const auto pixels = gpu.readback(synthetic[index].Get(), layout);
            check_synthetic(pixels, original_pixels, gpu.pattern(layout, frame - 1), layout);
            ++published; ++verified_synthetic;
        }
        if (mode == "stock2x") {
            xrfg::D3D12NvidiaGpuTiming t{};
            while (synth.consume_nvidia_gpu_timing(&t) == S_OK) stock_timings.push_back(t);
        }
        if (mode == "custom2x") {
            xrfg::DualGpuTiming t{};
            while (synth.consume_dual_gpu_timing(&t) == S_OK) timings.push_back(t);
        }
    }
    if (mode == "custom2x" && args.case_name != "worker-failure") {
        require(wait_idle(synth, 10'000), "offload did not drain");
        xrfg::DualGpuTiming t{}; while (synth.consume_dual_gpu_timing(&t) == S_OK) timings.push_back(t);
    }
    if (args.case_name == "delay") require(dropped > 0, "delay did not exercise deadline drop");
    if ((args.case_name == "normal" || args.case_name == "recreation") && mode == "custom2x")
        require(published > 0, "no dual GPU synthetic published");
    std::cout << "{\"mode\":\"" << mode << "\",\"layout\":\"" << layout.name()
              << "\",\"case\":\"" << args.case_name << "\",\"pairs\":" << args.pairs
              << ",\"current_verified\":" << verified_current << ",\"synthetic_verified\":" << verified_synthetic
              << ",\"published\":" << published << ",\"dropped\":" << dropped << ",\"missed_deadlines\":" << missed
              << ",\"saturation_rejected\":" << (saturation_rejected ? "true" : "false")
              << ",\"worker_failure_observed\":" << (failure_observed ? "true" : "false")
              << ",\"recreation_rejected\":" << (stale_ticket ? "true" : "false")
              << ",\"budget_us\":" << ((args.case_name == "normal" || args.case_name == "recreation") ? args.budget_us : 1'000)
              << ",\"render_upload_cpu_avg_us\":" << std::fixed << std::setprecision(1) << render_upload_us / (args.pairs + 1)
              << ",\"timings\":[";
    for (std::size_t i = 0; i < timings.size(); ++i) {
        const auto& t = timings[i]; if (i) std::cout << ',';
        std::cout << "{\"serial\":" << t.serial << ",\"input_us\":" << t.input_us
                  << ",\"worker_us\":" << t.worker_us << ",\"output_us\":" << t.output_us
                  << ",\"age_us\":" << t.age_us << ",\"flow_us\":" << t.flow_and_synthesis.total_microseconds
                  << ",\"published\":" << (t.published ? "true" : "false")
                  << ",\"dropped\":" << (t.dropped ? "true" : "false") << '}';
    }
    std::cout << "],\"stock_timings\":[";
    for (std::size_t i = 0; i < stock_timings.size(); ++i) {
        const auto& t = stock_timings[i]; if (i) std::cout << ',';
        std::cout << "{\"previous_serial\":" << t.previous_serial << ",\"current_serial\":" << t.current_serial
                  << ",\"pack_us\":" << t.pack_microseconds << ",\"flow_eye0_us\":" << t.eye0_microseconds
                  << ",\"flow_eye1_us\":" << t.eye1_microseconds << ",\"compose_us\":" << t.composition_microseconds
                  << ",\"total_us\":" << t.total_microseconds << '}';
    }
    std::cout << "]}\n";
    return last_ticket;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto args = parse(argc, argv);
        const auto adapters = xrfg::enumerate_dual_gpu_adapters();
        if (!args.run) {
            for (const auto& a : adapters) std::wcout << a.durable_id << L" | " << a.description
                << L" | vendor=" << a.vendor_id << L" device=" << a.device_id << L'\n';
            std::cout << "Use --run --primary-id <durable-id> --worker-id <durable-id> [--mode all|nofg|stock2x|custom2x] [--layout both|array|atlas] [--pairs 1..6] [--budget-us 1000..50000] [--case normal|delay|saturation|recreation|worker-failure]\n";
            return 0;
        }
        require(!args.primary.empty() && !args.worker.empty(), "both durable adapter IDs are required");
        const xrfg::DualGpuAdapter *primary = nullptr, *worker = nullptr;
        for (const auto& a : adapters) { if (a.durable_id == args.primary) primary = &a; if (a.durable_id == args.worker) worker = &a; }
        require(primary && worker && primary != worker, "unique primary and worker durable IDs required");
        require(primary->vendor_id == 0x10de && worker->vendor_id == 0x10de, "both adapters must be NVIDIA");
        require(args.case_name == "normal" || args.mode == "custom2x", "fault cases apply only to custom2x");
        require(args.case_name != "worker-failure" || args.layout != "both",
                "worker failure latches for this process; run one layout per invocation");
        const std::vector<std::string> modes = args.mode == "all" ? std::vector<std::string>{"nofg", "stock2x", "custom2x"} : std::vector<std::string>{args.mode};
        const std::vector<Layout> layouts = args.layout == "both" ? std::vector<Layout>{{false},{true}} : std::vector<Layout>{{args.layout == "atlas"}};
        for (const auto& mode : modes) for (const auto layout : layouts) {
            const auto prior = run_case(*primary, *worker, mode, layout, args);
            if (args.case_name == "recreation") (void)run_case(*primary, *worker, mode, layout, args, &prior);
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "dual GPU probe failed: " << e.what() << '\n'; return 1; }
}
