#pragma once
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dual_gpu_policy.hpp"
#include <string>
#include <vector>

namespace xrfg {
struct DualGpuAdapter {
    std::wstring durable_id; // PCI bus/device/function + PCI hardware identity; never an ordinal/LUID/name.
    std::wstring description;
    std::uint32_t vendor_id{}, device_id{}, subsystem_id{}, revision{};
    LUID current_luid{}; // resolved anew on each process/session, never persisted.
};
[[nodiscard]] std::vector<DualGpuAdapter> enumerate_dual_gpu_adapters();

struct DualGpuTiming {
    std::uint64_t serial{}, token{}, input_us{}, worker_us{}, output_us{}, age_us{};
    std::uint64_t deadline_qpc{}, ready_qpc{};
    bool published{}, dropped{};
    D3D12NvidiaGpuTiming flow_and_synthesis{};
};
// Offload owns three completion-safe transport slots, but admits no waiting
// worker backlog. All GPU waits are on its private queues. The runtime/app
// queue only joins original-local work, or a local publication whose return
// transport has already completed on the CPU.
class D3D12DualGpuOffload final {
public:
    D3D12DualGpuOffload();
    ~D3D12DualGpuOffload();
    HRESULT initialize(ID3D12Device*, ID3D12CommandQueue*, std::shared_ptr<D3D12SwapchainHistory>,
        std::span<ID3D12Resource* const>, std::span<ID3D12Resource* const>, DXGI_FORMAT,
        D3D12_RESOURCE_STATES, std::wstring_view, D3D12NvidiaOpticalFlowOptions) noexcept;
    HRESULT submit(const D3D12HistoryCaptureTicket&, std::span<const D3D12ReprojectionView>,
        std::span<const D3D12ReprojectionView>, std::uint32_t, std::uint32_t,
        D3D12FrameSynthesisTicket*, bool, float) noexcept;
    bool ready(const D3D12FrameSynthesisTicket&) noexcept;
    HRESULT publish(const D3D12FrameSynthesisTicket&) noexcept;
    void discard(const D3D12FrameSynthesisTicket&) noexcept;
    HRESULT synchronize_consumer(ID3D12CommandQueue*, const D3D12FrameSynthesisTicket&) noexcept;
    HRESULT completion_fence(ID3D12Fence**) noexcept;
    HRESULT retire_previous() noexcept;
    HRESULT idle() noexcept; // nonblocking, ERROR_BUSY while work is active.
    HRESULT timing(D3D12NvidiaGpuTiming*) noexcept;
    HRESULT transport_timing(DualGpuTiming*) noexcept;
    void set_budget_us(std::uint64_t) noexcept;
    void test_controls(std::uint32_t, bool) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
