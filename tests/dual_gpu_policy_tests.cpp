#include "xrfg/dual_gpu_policy.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void geometry() {
    using R = xrfg::DualGpuViewRect;
    require(xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{0,0,100,80,1}},100,80,2), "texture array two slices");
    require(xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{100,0,100,80,0}},200,80,1), "double-wide atlas");
    require(xrfg::valid_dual_gpu_stereo({R{4,3,90,70,1},R{4,3,90,70,0}},100,80,2), "swapped eye slices and bounded rects");
    require(!xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{0,0,100,80,0}},100,80,1), "overlapping eyes rejected");
    require(!xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{0,0,100,80,2}},100,80,2), "invalid slice rejected");
    require(!xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{99,0,2,80,1}},100,80,2), "right bound checked");
    require(!xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{0,79,100,2,1}},100,80,2), "bottom bound checked");
    require(!xrfg::valid_dual_gpu_stereo({R{0,0,100,80,0},R{0xffffffff,0,100,80,1}},100,80,2), "overflow does not wrap");
}
void context_ownership() {
    std::atomic<bool> owned{};
    xrfg::DualGpuExclusiveLease waiting;
    {
        xrfg::DualGpuExclusiveLease first;
        require(first.acquire(owned), "first context owns process lease");
        require(!first.acquire(owned), "duplicate lease acquisition rejected");
        require(!waiting.acquire(owned), "second engine cannot initialize a competing context");
        require(owned, "rejected contender cannot clear live ownership");
    }
    require(waiting.acquire(owned), "ownership released after first context destruction");
    require(xrfg::dual_gpu_supported_projection(1,1,2), "one stereo resource admitted");
    require(!xrfg::dual_gpu_supported_projection(1,2,1), "separate-eye resources rejected before initialization");
    require(!xrfg::dual_gpu_supported_projection(2,1,2), "multiple projection layers rejected");
    require(!xrfg::dual_gpu_supported_projection(1,1,1), "mono resource rejected");
}
void retirement() {
    xrfg::DualGpuSlotPolicy s;
    s.phase=xrfg::DualGpuPhase::discarded; s.original_value=2; s.input_value=3;
    s.worker_value=4; s.return_value=5; s.publish_value=6;
    require(!s.reusable(1,3,4,5,6), "original completion independent");
    require(!s.reusable(2,2,4,5,6), "input must retire");
    require(!s.reusable(2,3,3,5,6), "worker must retire after deadline");
    require(!s.reusable(2,3,4,4,6), "return copy distinct from worker");
    require(!s.reusable(2,3,4,5,5), "publication completion distinct");
    require(s.reusable(2,3,4,5,6), "all submitted accesses retired");
    s.phase=xrfg::DualGpuPhase::ready;
    require(!s.reusable(99,99,99,99,99), "unconsumed ready output owns its slot");
}
void identity_and_deadline() {
    xrfg::DualGpuSlotPolicy s; s.phase=xrfg::DualGpuPhase::ready;
    s.key={100,200,300}; s.token=9; s.deadline=1000;
    require(s.eligible(s.key,9,999,true), "ready matching output");
    require(!s.eligible(s.key,9,999,false), "worker finished but return copy delayed");
    require(!s.eligible(s.key,9,1001,true), "late result dropped");
    require(!s.eligible({101,200,300},9,999,true), "recreated session or swapchain generation");
    require(!s.eligible({100,201,300},9,999,true), "wrong frame");
    require(!s.eligible({100,200,301},9,999,true), "wrong pose FOV slice or rect");
    require(!s.eligible(s.key,8,999,true), "old token cannot reuse slot");
    s.canceled=true;
    require(!s.eligible(s.key,9,999,true), "canceled output cannot reappear");
    s.canceled=false; s.phase=xrfg::DualGpuPhase::worker;
    require(!s.eligible(s.key,9,999,true), "phase also required");
}
void saturation() {
    std::array<xrfg::DualGpuSlotPolicy,3> slots{};
    require(xrfg::dual_gpu_admit(slots)==0, "first slot admission");
    for (auto phase : {xrfg::DualGpuPhase::input,xrfg::DualGpuPhase::worker,xrfg::DualGpuPhase::returning}) {
        slots[0].phase=phase;
        require(!xrfg::dual_gpu_admit(slots), "no worker backlog even with free slots");
    }
    slots[0].phase=xrfg::DualGpuPhase::ready;
    require(xrfg::dual_gpu_admit(slots)==1, "ready result retained while next frame works");
    slots[1].phase=xrfg::DualGpuPhase::ready; slots[2].phase=xrfg::DualGpuPhase::published;
    require(!xrfg::dual_gpu_admit(slots), "three slots saturated");
    slots[2].phase=xrfg::DualGpuPhase::free;
    require(xrfg::dual_gpu_admit(slots)==2, "completed slot reusable");
}
void publication_order() {
    xrfg::DualGpuPublicationOrder order;
    require(order.reserve(20) == 1, "newer token gets first publication fence");
    require(!order.reserve(19), "older ready result rejected after newer publication");
    require(!order.reserve(20), "published token replay rejected");
    require(order.reserve(21) == 2, "fence sequence follows publication order");
    require(order.reserve(100) == 3, "skipped source tokens do not skip publication fence values");
    require(!order.reserve(0), "invalid token rejected");
}
void stereo_transaction() {
    struct Eye { bool ready{}, publish_success{true}; int index{}; };
    std::array<Eye,2> eyes{{{true,true,0},{false,true,1}}};
    std::vector<int> calls;
    auto run = [&] { return xrfg::dual_gpu_publish_transaction(eyes,
        [&](const Eye& e) { calls.push_back(10+e.index); return e.ready; },
        [&](const Eye& e) { calls.push_back(20+e.index); return e.publish_success; },
        [&](const Eye& e) { calls.push_back(30+e.index); }); };
    require(!run(), "delayed right eye rejects whole frame");
    require(calls==std::vector<int>({10,11,30,31}), "zero publish before both-eye readiness");
    eyes[1].ready=true; calls.clear(); require(run(), "both eyes eligible");
    require(calls==std::vector<int>({10,11,20,21}), "all readiness before any local publish");
    eyes[1].publish_success=false; calls.clear(); require(!run(), "worker/local copy error fails transaction");
    require(calls==std::vector<int>({10,11,20,21,30,31}), "partial local publication discarded for both eyes");
    // V1's admitted array/atlas is one transaction entry. Multi-resource tests
    // above exercise defensive transaction behavior, not supported layouts.
    std::array<Eye,1> stereo{{{true,true,0}}};
    require(xrfg::dual_gpu_publish_transaction(stereo,[](auto& e){return e.ready;},
        [](auto& e){return e.publish_success;},[](auto&){}), "single stereo-resource transaction");
}
}
int main() {
    try { geometry(); context_ownership(); retirement(); identity_and_deadline(); saturation(); publication_order(); stereo_transaction();
        std::cout << "dual_gpu_policy_tests: geometry, fences, metadata, deadlines, saturation, stereo transaction PASS\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
