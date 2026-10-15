# Explicit secondary-GPU prototype

This branch adds an experimental, default-off, 2x transport backend to the pinned
release 0.2.13.4 code. A build or CPU test is not evidence of hardware throughput,
headset image quality, driver stability, or a frame-rate improvement. The hardware
probe is intentionally excluded from CTest. Run it only with the GPUs isolated
from other workloads.

## Ownership and supported paths

The application's original D3D12 device renders both eyes. Its runtime graphics
binding and `xrGetD3D12GraphicsRequirementsKHR` adapter contract are unchanged.
The existing same-adapter D3D11-to-D3D12 bridge can feed this backend after the
bridge has produced primary D3D12 history. Direct legacy D3D11 interop, Vulkan
(including the Vulkan bridge), MSAA, compressed/planar color formats, and 3x are
not supported by offload v1. These cases fail back to original frames. With
offload disabled, the stock paths are retained.

`dual_gpu_enabled=0` is the default. `dual_gpu_worker_id` persists a PCI
bus/device/function plus vendor/device/subsystem/revision tuple. Adapter ordinals,
display names and boot-lifetime LUIDs are not persisted. Every session resolves
the identity again; missing/ambiguous identities, non-NVIDIA workers and selection
of the primary adapter are rejected. The D3DKMT address has no PCI segment, so
colliding tuples across PCI roots are omitted rather than guessed. Moving a card
to a different PCI location requires explicit reselection.

The worker gets a distinct D3D12 device, one NVIDIA optical-flow context and the
existing NVIDIA synthesis shaders. Both optical flow and composition run there.
DLSS motion-vector ingress and FidelityFX fallback are disabled for this path.
Changing worker identity or enabling/disabling offload during a session requires
a new session; generation is bypassed on a conflicting live request.

## Transport and scheduling

V1 admits one projection layer whose two eyes share one stereo array or atlas
swapchain. Separate-eye swapchains, mono views and multiple projection layers
fail back to original frames before deferred worker initialization. The engine
has three bounded transport slots. A nonblocking process-wide lifetime lease
allows only one offload engine: a second engine is refused before worker device
creation, and the lease is released only after the first engine's contexts and
resources are destroyed. Quarantine retains the lease. This conservative limit
addresses the concurrent-OFA-context freezes recorded in `THIRD_PARTY.md`; the
existing single worker context serializes both isolated eye jobs. It does not
coordinate independent processes or other applications using the GPU.

Each slot owns two cross-adapter shared heaps with placed buffers (input and
output), worker input/current/synthetic textures, a primary return texture, and
separate command allocators. Shared heaps use
`D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`, custom L0 system
memory and cross-adapter resources. This is not a peer-to-peer VRAM path. Texture
footprints are checked on both devices. Copies enumerate every array slice, and
all resources cross ownership in `COMMON` with explicit copy-state barriers.

The ordering is:

1. On a private primary queue, wait for the primary history producer, copy the
   current original into its primary output, and signal **original completion**.
2. Copy only the new input texture(s) into shared input buffers, signal the
   cross-adapter **input completion**, and retire the primary history lease
   against that input fence. Previous input remains in worker-local history.
3. A CPU polling thread admits worker processing only after input completion.
   Its private worker queue also waits on the already-completed shared fence,
   unpacks input, captures worker history, runs NVIDIA flow and composition,
   packs the result, and signals **worker/output-buffer completion**.
4. After CPU observation of worker completion, a dedicated primary return queue
   waits on that already-completed cross-adapter fence and copies the result to
   its local return texture. A distinct fence records **return completion**.
5. At the synthetic presentation opportunity, every eye/resource must have a
   matching generation, token, capture serial, view hash, destination, and a
   complete return copy within the deadline. Only then is a primary-local copy
   submitted into the selected output staging image. The runtime queue joins
   that local publication, then the existing handover copier writes its runtime
   image. The application/render queue never waits on unresolved worker work.

Original completion, worker completion, return completion and local publication
have distinct fences. Publication values increment in actual submission order,
not source-token order. A discarded slot cannot be reused until every submitted
GPU access has retired. A published slot is retained until the runtime queue has
joined its publication. Only one processing job is admitted per engine; ready
outputs may occupy the other slots, but there is no waiting worker backlog.
Saturation immediately leaves the original path available.

A successful original prime starts the established presenter handoff; immediate
inline submission cannot wait for asynchronous offload. At the actual
presentation opportunity the layer checks all resources before publishing any.
If one eye is late, canceled, stale, or fails publication, the entire synthetic
frame is replaced with the same immutable current-original snapshot, including
both eyes' poses/FOVs. No older synthetic is substituted. The deeper stock
readiness hold is not allowed to extend an offload job past its opportunity.

Continuity resets advance the engine generation and cancel outstanding output
eligibility. They do not free in-flight transport resources. A detected worker
or queue error disables offload for the remainder of the process. Destruction
stops the CPU polling thread without waiting on a worker GPU fence. The thread
join can still block if a driver API itself hangs while submitting commands;
bounded teardown under a driver-wide hang is not claimed. If
GPU retirement cannot be proven, the resource graph is quarantined until process
exit; repeated reinitialization is blocked by the fault latch. This does not
promise recovery from a driver-wide GPU fault.

## Timing and diagnostics

`DualGpuTiming`, exposed through `consume_dual_gpu_timing`, contains input GPU
copy duration (primary pack plus worker unpack), worker GPU span excluding those
transport copies, output GPU copy duration (worker pack plus primary return),
result age, ready/deadline QPC and publish/drop outcome. Individual NVIDIA pack,
per-eye optical flow and synthesis durations remain available in the nested
record. Each GPU interval is measured on its own queue's timestamp clock and
frequency. Queues obtain `GetClockCalibration`; the existing synthesis timing
maps worker timestamps to QPC. No timestamp from one GPU is subtracted from a
timestamp on the other GPU.

Flight recording emits `dual_gpu_transport`, `dual_gpu_deadline`, and the
existing NVIDIA timing records. The small `presentation_gpu.txt` diagnostic
reports the actual finalized runtime-binding adapter, PID and session handle.
The session removes only its own report at teardown. It is not an adapter
selection mechanism.

## Validation boundaries and commands

CPU production-policy tests cover array and atlas rectangles, reversed slices,
invalid bounds/overflow, split-eye admission rejection, exclusive context
ownership, defensive multi-output transaction ordering, delayed return,
independent completion fences, late/canceled output, token reuse, session and
swapchain recreation identity, pose/view metadata mismatch, saturation and
partial-publication failure. Publication ordering tests exercise the actual
helper used by the engine: newer-then-older, replay and skipped source tokens
must not rewind a GPU fence. The presenter calls the same transaction helper
these tests exercise.

The WARP fake-runtime modes `offload-negative`, `offload-negative-empty` and
`offload-negative-split` load
the actual DLL and check missing-worker rejection, original stereo continuation,
unchanged runtime D3D12 device/requirements, 3x rejection, diagnostic ownership,
and session/swapchain recreation. The split-eye case also verifies zero private
generation allocations, proving rejection before worker initialization. Their deliberate invalid identity cannot
reach worker device creation. Existing stock tests remain applicable with the
feature off.

```
ctest --test-dir build -C Release -R "dual_gpu_policy|offload-negative" --output-on-failure
```

`xrfg_dual_gpu_probe` is a manual executable for equal-dimension no-FG, stock 2x
and custom 2x workloads, CPU-generated input, GPU readback validation and explicit
fault/delay cases. Its help lists the current arguments. It requires no OpenXR
runtime or headset. It must not be mistaken for an OpenXR application or headset
acceptance test. Hardware execution and comparative performance remain separate
validation gates; an active game must be left undisturbed.

## API references

- [D3D12 shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps)
- [OpenXR D3D12 graphics requirements](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html)
- [D3D12 device adapter LUID](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getadapterluid)

See [DUAL_GPU_VALIDATION.md](DUAL_GPU_VALIDATION.md) for the verified build/test
receipt and remaining hardware gates.
