#include "core/HardwareProbe.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace companion {

HardwareInfo ProbeHardware() {
    HardwareInfo hw;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) hw.ramGiB = static_cast<double>(ms.ullTotalPhys) / 1073741824.0;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return hw;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> best;
    SIZE_T bestVram = 0;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;  // « Microsoft Basic Render »
        if (d.DedicatedVideoMemory > bestVram) {
            bestVram = d.DedicatedVideoMemory;
            best = a;
            hw.gpuName = log::Narrow(d.Description);
        }
    }
    hw.vramGiB = static_cast<double>(bestVram) / 1073741824.0;
    // VRAM disponible maintenant : budget que Windows accorde au processus (réduit quand un jeu en prend).
    Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
    if (best && SUCCEEDED(best.As(&a3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            hw.vramFreeGiB = static_cast<double>(info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0) / 1073741824.0;
    }
    return hw;
}

}  // namespace companion
