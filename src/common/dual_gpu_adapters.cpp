#include "xrfg/d3d12_dual_gpu.hpp"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cwchar>
#include <algorithm>

namespace xrfg {
std::vector<DualGpuAdapter> enumerate_dual_gpu_adapters() {
    std::vector<DualGpuAdapter> result;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return {};
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        D3DKMT_OPENADAPTERFROMLUID open{desc.AdapterLuid, 0};
        if (D3DKMTOpenAdapterFromLuid(&open) < 0) continue;
        D3DKMT_ADAPTERADDRESS address{};
        D3DKMT_QUERYADAPTERINFO query{open.hAdapter, KMTQAITYPE_ADAPTERADDRESS, &address, sizeof(address)};
        const auto status = D3DKMTQueryAdapterInfo(&query);
        D3DKMT_CLOSEADAPTER close{open.hAdapter};
        D3DKMTCloseAdapter(&close);
        if (status < 0) continue;
        wchar_t id[192]{};
        swprintf_s(id, L"pci:%08x:%08x:%08x:ven%04x:dev%04x:sub%08x:rev%02x",
            address.BusNumber, address.DeviceNumber, address.FunctionNumber,
            desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision);
        result.push_back({id, desc.Description, desc.VendorId, desc.DeviceId,
            desc.SubSysId, desc.Revision, desc.AdapterLuid});
    }
    // No segment is exposed by ADAPTERADDRESS. If the address/hardware tuple
    // collides across PCI roots, neither device is safe to select persistently.
    auto ambiguous = result;
    std::erase_if(result, [&](const auto& a) {
        return std::count_if(ambiguous.begin(), ambiguous.end(), [&](const auto& b) {
            return a.durable_id == b.durable_id;
        }) != 1;
    });
    return result;
}
}
