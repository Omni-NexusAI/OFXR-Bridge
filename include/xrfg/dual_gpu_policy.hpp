#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

namespace xrfg {
// CPU policy is also used by the real transport. A slot is never free merely
// because its result missed a deadline: every submitted access must retire.
struct DualGpuFrameKey {
    std::uint64_t generation{}, serial{}, views_hash{};
    bool operator==(const DualGpuFrameKey&) const = default;
};
enum class DualGpuPhase { free, input, worker, returning, ready, published, discarded };
struct DualGpuSlotPolicy {
    DualGpuPhase phase{DualGpuPhase::free};
    DualGpuFrameKey key{};
    std::uint64_t token{}, deadline{}, original_value{}, input_value{}, worker_value{}, return_value{}, publish_value{};
    bool canceled{};
    [[nodiscard]] bool eligible(DualGpuFrameKey expected, std::uint64_t expected_token,
                                std::uint64_t now, bool returned) const noexcept {
        return !canceled && phase == DualGpuPhase::ready && returned && token == expected_token &&
               key == expected && now <= deadline;
    }
    [[nodiscard]] bool reusable(std::uint64_t original, std::uint64_t input,
                                std::uint64_t worker, std::uint64_t returned,
                                std::uint64_t published) const noexcept {
        return (phase == DualGpuPhase::discarded || phase == DualGpuPhase::published) &&
               original >= original_value && input >= input_value && worker >= worker_value &&
               returned >= return_value && published >= publish_value;
    }
};
inline constexpr std::uint32_t dual_gpu_transport_slots = 3;
// Keep one worker context alive per process. A lifetime lease is more
// conservative than a dispatch lock: driver context destruction also finishes
// before another engine can initialize. Declare this before GPU-owned members.
class DualGpuExclusiveLease {
public:
    DualGpuExclusiveLease() = default;
    DualGpuExclusiveLease(const DualGpuExclusiveLease&) = delete;
    DualGpuExclusiveLease& operator=(const DualGpuExclusiveLease&) = delete;
    ~DualGpuExclusiveLease() { if (owner_) owner_->store(false); }
    [[nodiscard]] bool acquire(std::atomic<bool>& owner) noexcept {
        if (owner_) return false;
        bool expected = false;
        if (!owner.compare_exchange_strong(expected, true)) return false;
        owner_ = &owner;
        return true;
    }
private:
    std::atomic<bool>* owner_{};
};
[[nodiscard]] inline bool dual_gpu_supported_projection(std::size_t layers,
    std::size_t resources, std::size_t views) noexcept {
    return layers == 1 && resources == 1 && views == 2;
}
struct DualGpuPublicationOrder {
    std::uint64_t last_token{}, next_fence{1};
    [[nodiscard]] std::optional<std::uint64_t> reserve(std::uint64_t token) noexcept {
        if (!token || token <= last_token) return std::nullopt;
        last_token = token;
        return next_fence++;
    }
};
[[nodiscard]] inline std::optional<std::uint32_t> dual_gpu_admit(std::span<const DualGpuSlotPolicy> slots) noexcept {
    std::optional<std::uint32_t> free;
    for (std::uint32_t i = 0; i < slots.size(); ++i) {
        if (slots[i].phase == DualGpuPhase::input || slots[i].phase == DualGpuPhase::worker ||
            slots[i].phase == DualGpuPhase::returning) return std::nullopt;
        if (!free && slots[i].phase == DualGpuPhase::free) free = i;
    }
    return free;
}
// Transaction admission checks every output before any publication. V1 admits
// one stereo array/atlas; the generic helper also preserves atomic fallback if
// a future scheduler safely supports multiple resources.
template <class Outputs, class Ready, class Publish, class Discard>
[[nodiscard]] bool dual_gpu_publish_transaction(Outputs& outputs, Ready ready, Publish publish, Discard discard) {
    bool accepted = true;
    for (auto& output : outputs) if (!ready(output)) { accepted = false; break; }
    if (accepted) for (auto& output : outputs) if (!publish(output)) { accepted = false; break; }
    if (!accepted) for (auto& output : outputs) discard(output);
    return accepted;
}
// Geometry is deliberately independent from D3D12 so array/atlas accounting is
// exhaustively testable without creating a graphics device.
struct DualGpuViewRect { std::uint32_t x{}, y{}, width{}, height{}, slice{}; };
[[nodiscard]] inline bool valid_dual_gpu_stereo(std::array<DualGpuViewRect, 2> views,
    std::uint64_t width, std::uint32_t height, std::uint32_t slices) noexcept {
    for (const auto& v : views)
        if (!v.width || !v.height || v.slice >= slices || v.x > width ||
            v.width > width - v.x || v.y > height || v.height > height - v.y) return false;
    const auto& a = views[0]; const auto& b = views[1];
    return a.slice != b.slice || a.x + a.width <= b.x || b.x + b.width <= a.x ||
           a.y + a.height <= b.y || b.y + b.height <= a.y;
}
}
