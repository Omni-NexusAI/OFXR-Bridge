# Dual-GPU prototype

This branch starts from OFXR Bridge release 0.2.13.4, commit
`44cc951691d580e13e6610648b4d8bf01ec125b9`.

The requested prototype keeps both original eye renders and OpenXR presentation
on the runtime-required adapter. A separately selected NVIDIA adapter performs
optical flow and synthesis. Cross-adapter staging returns complete stereo output
to the presentation adapter. The feature defaults off and initially targets 2x
generation through native D3D12 and the existing D3D11-to-D3D12 bridge.

Implementation acceptance includes completion-safe bounded transport, stale-output
rejection, original-frame fallback, durable adapter selection, stock/custom launch
exclusion, CPU tests, full binary builds, and bounded synthetic GPU checks when the
desktop is available. Headset display and gameplay validation remain separate.

No performance improvement is claimed before measurements. Cross-adapter shared
heaps use system memory; the design does not assume direct peer-to-peer transport.

References:

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getadapterluid

Implementation and validation details will be updated with verified results.
