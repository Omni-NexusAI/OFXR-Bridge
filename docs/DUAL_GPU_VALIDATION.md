# Prototype validation receipt — 2026-10-15

Baseline: release `0.2.13.4`, commit
`44cc951691d580e13e6610648b4d8bf01ec125b9`. This receipt covers the custom
default-off branch, not a released or performance-qualified feature.

## Verified

- Full x64 Release build: tray, OpenXR layer, existing Vulkan helper, CPU/WARP
  tests and manual hardware probe compile and link with VS2019 Build Tools
  16.11.6, MSVC 19.29.30159, Windows SDK 10.0.19041 and CMake 4.4.4.
- Seventeen selected CTest cases pass: dual-GPU policy, core, FPS overlay,
  optical flow, D3D12 OpenXR FPS overlay, standalone settings, tray lifecycle,
  flight logger, layer negotiation, three invalid-worker/split-eye WARP cases, stock inline
  disarm/pause, D3D12 history, vertical FOV and synthetic marker.
- The actual layer DLL's WARP invalid-worker fixtures preserve the runtime's
  D3D12 device and graphics-requirements LUID, reject requested 3x offload,
  continue original stereo frames and recreate sessions/swapchains.
- Separate official stock 0.2.13.4 Arm/Disarm/Exit check passed. Its OpenXR and
  Vulkan registration snapshots were identical before/after. No game launched.

The software suite deliberately excludes real-adapter D3D11 tests and all
hardware probe workloads. Passing these tests does not exercise a working
cross-adapter transport or NVIDIA optical flow on the second GPU.

## Still unverified

- No-FG versus stock 2x versus custom 2x hardware timing or image-readback
  comparison; actual cross-adapter heap compatibility and NVIDIA worker behavior.
- Hardware delayed return, saturation, device failure, recreation and publication
  ordering. CPU publication-order coverage is not hardware race coverage.
- Full stock-first/custom-first launch exclusion. The first stock-first attempt
  showed the second tray's refusal dialog, but the test's initial dialog-close
  harness timed out. The harness was corrected; the complete rerun is pending.
- Headset scanout, actual runtime-selected presentation GPU, D3D11 game bridge,
  stereo image quality, controller behavior, sustained gameplay and FPS benefit.

Separate-eye source swapchains are deliberately unsupported in this first
prototype and fall back to originals. Stereo arrays and double-wide images use
one worker context, with process-wide ownership preventing competing engines.
Upstream reports freezes from concurrent NVIDIA optical-flow contexts; the
restriction is a source-level precaution, not a tested driver fix.

Path of Neo was running again during final validation. All remaining hardware
work and tray activation were held to preserve that session. The OpenXR active
runtime remained Virtual Desktop. An enumeration-only invocation of the manual
probe was also rejected by automatic approval review and was not run.

## Reproduce the software checks

Use the configured build, clear optional `XRFG_TEST_*` hardware overrides in the
child process, and run the following selection serially:

```powershell
ctest --test-dir build -C Release -j 1 --timeout 90 --output-on-failure -R '^(xrfg_(core|fps_overlay|optical_flow|standalone_launcher|tray_lifecycle|bridge_flight_logger|dual_gpu_policy|d3d12_history|vertical_fov|synthetic_marker)_tests|xrfg_openxr_fps_overlay_d3d12|xrfg_layer_negotiation|xrfg_offload-negative(-empty|-split)?_warp|xrfg_manual_(disarm|pause)_inline)$'
```

## Next hardware gate

After active games close naturally, verify GPU health and free memory. The
manual probe without `--run` lists current durable IDs. Choose the intended
primary and worker explicitly; do not infer headset ownership from the desktop.
Then run one bounded command at a time and preserve JSON output:

```powershell
.\build\Release\xrfg_dual_gpu_probe.exe --run --primary-id '<primary PCI ID>' --worker-id '<worker PCI ID>' --mode all --layout both --pairs 3 --budget-us 22222 --case normal
```

The common eye dimensions are 256 by 128. The default deadline is 22.222 ms;
50 ms may be used as a separately labelled correctness diagnostic. It is not
proof of a real-time deadline. Fault cases use `--mode custom2x`, with `--case`
`delay`, `saturation`, `recreation`, or `worker-failure`. Run worker-failure in its
own process and one layout per invocation. Stop on a device fault; do not loop
or launch a game to recover. The current probe does not force a two-ready-ticket
hardware publication race.

See [DUAL_GPU_PROTOTYPE.md](DUAL_GPU_PROTOTYPE.md) for ownership and limitations,
including process-lifetime resource quarantine and the driver-hang boundary.
