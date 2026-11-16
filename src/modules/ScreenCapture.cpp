// Capture d'écran ponctuelle de GameCoach (DXGI Desktop Duplication + encodage JPEG WIC).
#include "modules/GameCoach.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

std::vector<std::uint8_t> EncodeJpeg(const std::vector<std::uint8_t>& rgb, int w, int h, float quality) {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> bgr(rgb.size());
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        bgr[i] = rgb[i + 2];
        bgr[i + 1] = rgb[i + 1];
        bgr[i + 2] = rgb[i];
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &props)))
        return out;

    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    props->Write(1, &option, &value);

    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))) ||
        FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 3), static_cast<UINT>(bgr.size()), bgr.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return out;

    HGLOBAL global = nullptr;
    STATSTG stat{};
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global)) || FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return out;
    if (const void* p = GlobalLock(global)) {
        const auto* bytes = static_cast<const std::uint8_t*>(p);
        out.assign(bytes, bytes + stat.cbSize.LowPart);
        GlobalUnlock(global);
    }
    return out;
}

}  // namespace

std::optional<ScreenShot> GameCoach::CaptureScreen(int maxHeight) {
    // Écran contenant la fenêtre au premier plan (le jeu).
    const HMONITOR target = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == target) { output = o; break; }
        }
        if (!output) adapter.Reset();
    }
    if (!output) return std::nullopt;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &ctx)))
        return std::nullopt;
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> dupl;
    if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device.Get(), &dupl))) {
        log::Info("Capture: duplication d'écran refusée (plein écran exclusif ?)");
        return std::nullopt;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;
    for (int tries = 0; tries < 10 && hr == DXGI_ERROR_WAIT_TIMEOUT; ++tries) hr = dupl->AcquireNextFrame(100, &info, &resource);
    if (FAILED(hr)) return std::nullopt;

    ComPtr<ID3D11Texture2D> frameTex, staging;
    D3D11_TEXTURE2D_DESC desc{};
    bool copied = false;
    if (SUCCEEDED(resource.As(&frameTex))) {
        frameTex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc = {1, 0};
        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
            ctx->CopyResource(staging.Get(), frameTex.Get());
            copied = true;
        }
    }
    dupl->ReleaseFrame();
    if (!copied) return std::nullopt;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return std::nullopt;

    // Réduction par moyenne de zone (BGRA -> RGB) vers maxHeight en conservant le ratio.
    const int srcW = static_cast<int>(desc.Width), srcH = static_cast<int>(desc.Height);
    ScreenShot shot;
    shot.height = std::min(maxHeight, srcH);
    shot.width = std::max(1, srcW * shot.height / srcH);
    shot.rgb.resize(static_cast<size_t>(shot.width) * shot.height * 3);
    const auto* src = static_cast<const std::uint8_t*>(map.pData);
    for (int y = 0; y < shot.height; ++y) {
        const int y0 = y * srcH / shot.height, y1 = std::max(y0 + 1, (y + 1) * srcH / shot.height);
        for (int x = 0; x < shot.width; ++x) {
            const int x0 = x * srcW / shot.width, x1 = std::max(x0 + 1, (x + 1) * srcW / shot.width);
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = src + static_cast<size_t>(sy) * map.RowPitch;
                for (int sx = x0; sx < x1; ++sx, ++count) {
                    sum[0] += row[sx * 4 + 2];
                    sum[1] += row[sx * 4 + 1];
                    sum[2] += row[sx * 4 + 0];
                }
            }
            auto* dst = &shot.rgb[(static_cast<size_t>(y) * shot.width + x) * 3];
            for (int c = 0; c < 3; ++c) dst[c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    ctx->Unmap(staging.Get(), 0);

    shot.jpeg = EncodeJpeg(shot.rgb, shot.width, shot.height, 0.85f);
    log::Info("Capture: {}x{} -> {}x{}, JPEG {} Ko", srcW, srcH, shot.width, shot.height, shot.jpeg.size() / 1024);
    return shot;
}

}  // namespace companion
