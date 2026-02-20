#include "screenshot.h"
#include <wil/com.h>
#include <wil/resource.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <d3d11_4.h>

#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>

namespace lvt {

// {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1}
MIDL_INTERFACE("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")
IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

namespace lvt {

static const Element* find_element_by_id(const Element& root, const std::string& id) {
    if (root.id == id) return &root;
    for (auto& child : root.children) {
        auto* found = find_element_by_id(child, id);
        if (found) return found;
    }
    return nullptr;
}

static void collect_elements(const Element& el, std::vector<const Element*>& out) {
    out.push_back(&el);
    for (auto& child : el.children) {
        collect_elements(child, out);
    }
}

// Get the IDXGISurface from a WinRT IDirect3DSurface via the interop interface.
static wil::com_ptr<IDXGISurface> get_dxgi_surface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
    auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
    wil::com_ptr<IDXGISurface> dxgiSurface;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&dxgiSurface)));
    return dxgiSurface;
}

// Create a WinRT IDirect3DDevice wrapping our D3D11 device.
static winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice create_direct3d_device(
    ID3D11Device* d3dDevice) {
    wil::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(), inspectable.put()));

    return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

// Capture a single frame from the given HWND using Windows.Graphics.Capture.
// Returns pixel data as BGRA, plus dimensions.
static bool capture_frame(HWND hwnd, ID3D11Device* d3dDevice, ID3D11DeviceContext* ctx,
                          std::vector<BYTE>& pixels, int& width, int& height) {
    namespace WGC = winrt::Windows::Graphics::Capture;

    // Create GraphicsCaptureItem from HWND
    auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{nullptr};
    HRESULT hr = interop->CreateForWindow(
        hwnd, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr) || !item) {
        fprintf(stderr, "lvt: failed to create capture item for window (0x%08lx)\n", hr);
        return false;
    }

    auto itemSize = item.Size();
    width = itemSize.Width;
    height = itemSize.Height;
    if (width <= 0 || height <= 0) return false;

    auto device = create_direct3d_device(d3dDevice);
    auto pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        1, itemSize);

    // Synchronization for async frame arrival
    std::mutex mtx;
    std::condition_variable cv;
    bool frameReady = false;
    WGC::Direct3D11CaptureFrame capturedFrame{nullptr};

    pool.FrameArrived([&](auto&& pool, auto&&) {
        auto frame = pool.TryGetNextFrame();
        if (frame) {
            std::lock_guard lock(mtx);
            capturedFrame = frame;
            frameReady = true;
            cv.notify_one();
        }
    });

    auto session = pool.CreateCaptureSession(item);
    session.IsBorderRequired(false);
    session.StartCapture();

    // Wait for one frame
    {
        std::unique_lock lock(mtx);
        if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return frameReady; })) {
            session.Close();
            pool.Close();
            fprintf(stderr, "lvt: timed out waiting for capture frame\n");
            return false;
        }
    }

    session.Close();

    // Get texture from frame
    auto surface = capturedFrame.Surface();
    auto dxgiSurface = get_dxgi_surface(surface);

    wil::com_ptr<ID3D11Texture2D> frameTex;
    dxgiSurface.query_to(&frameTex);

    D3D11_TEXTURE2D_DESC desc{};
    frameTex->GetDesc(&desc);
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);

    // Create a staging texture to read back pixels
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    wil::com_ptr<ID3D11Texture2D> staging;
    hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) return false;

    ctx->CopyResource(staging.get(), frameTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    pixels.resize(width * height * 4);
    auto* src = static_cast<BYTE*>(mapped.pData);
    for (int row = 0; row < height; row++) {
        memcpy(pixels.data() + row * width * 4, src + row * mapped.RowPitch, width * 4);
    }
    ctx->Unmap(staging.get(), 0);

    capturedFrame.Close();
    pool.Close();
    return true;
}

// Draw annotation overlays onto raw BGRA pixel buffer
static void annotate_pixels(BYTE* pixels, int bmpWidth, int bmpHeight,
                            HWND hwnd, const Element* tree) {
    if (!tree) return;

    // Create a GDI bitmap backed by the pixel data so we can draw on it
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bmpWidth;
    bmi.bmiHeader.biHeight = -bmpHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HDC memDC = CreateCompatibleDC(nullptr);
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!hBitmap || !dibBits) { DeleteDC(memDC); return; }

    // Copy pixels into the DIB
    memcpy(dibBits, pixels, bmpWidth * bmpHeight * 4);
    HGDIOBJ old = SelectObject(memDC, hBitmap);

    RECT winRect{};
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &winRect, sizeof(winRect)) != S_OK) {
        GetWindowRect(hwnd, &winRect);
    }

    std::vector<const Element*> elements;
    collect_elements(*tree, elements);

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 50, 50));
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(memDC, pen);
    HGDIOBJ oldBrush = SelectObject(memDC, brush);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 50, 50));

    HFONT font = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");
    HGDIOBJ oldFont = SelectObject(memDC, font);

    for (auto* el : elements) {
        if (el->bounds.width <= 0 || el->bounds.height <= 0) continue;
        int x = el->bounds.x - winRect.left;
        int y = el->bounds.y - winRect.top;
        int w = el->bounds.width;
        int h = el->bounds.height;
        if (x + w <= 0 || y + h <= 0 || x >= bmpWidth || y >= bmpHeight) continue;

        Rectangle(memDC, x, y, x + w, y + h);

        if (!el->id.empty()) {
            std::wstring label(el->id.begin(), el->id.end());
            SIZE textSize{};
            GetTextExtentPoint32W(memDC, label.c_str(), static_cast<int>(label.size()), &textSize);
            RECT labelRect = {x, y - textSize.cy - 2, x + textSize.cx + 4, y};
            if (labelRect.top < 0) { labelRect.top = y; labelRect.bottom = y + textSize.cy + 2; }
            HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 220));
            FillRect(memDC, &labelRect, bgBrush);
            DeleteObject(bgBrush);
            TextOutW(memDC, labelRect.left + 2, labelRect.top + 1,
                     label.c_str(), static_cast<int>(label.size()));
        }
    }

    SelectObject(memDC, oldFont);
    SelectObject(memDC, oldBrush);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, old);
    DeleteObject(font);
    DeleteObject(pen);

    // Copy annotated pixels back
    memcpy(pixels, dibBits, bmpWidth * bmpHeight * 4);

    DeleteObject(hBitmap);
    DeleteDC(memDC);
}

// Save BGRA pixels to PNG using WIC
static bool save_pixels_as_png(const BYTE* pixels, int width, int height,
                               const std::string& outputPath,
                               const RECT* cropRect = nullptr) {
    auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hr);

    wil::com_ptr<IWICImagingFactory> factory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    int outX = 0, outY = 0, outW = width, outH = height;
    if (cropRect) {
        outX = (std::max)(0, static_cast<int>(cropRect->left));
        outY = (std::max)(0, static_cast<int>(cropRect->top));
        outW = static_cast<int>(cropRect->right - cropRect->left);
        outH = static_cast<int>(cropRect->bottom - cropRect->top);
        if (outX + outW > width) outW = width - outX;
        if (outY + outH > height) outH = height - outY;
        if (outW <= 0 || outH <= 0) return false;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, outputPath.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, outputPath.c_str(), -1, wpath.data(), wlen);

    wil::com_ptr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;
    hr = stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    wil::com_ptr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;
    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    wil::com_ptr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;
    hr = frame->SetSize(outW, outH);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) return false;

    UINT srcStride = width * 4;
    UINT outStride = outW * 4;
    const BYTE* startPixel = pixels + outY * srcStride + outX * 4;

    if (outX == 0 && outW == width) {
        hr = frame->WritePixels(outH, srcStride, outH * srcStride,
                                const_cast<BYTE*>(startPixel));
    } else {
        for (int row = 0; row < outH; row++) {
            hr = frame->WritePixels(1, outStride, outStride,
                                    const_cast<BYTE*>(startPixel + row * srcStride));
            if (FAILED(hr)) break;
        }
    }
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;
    hr = encoder->Commit();
    if (FAILED(hr)) return false;

    if (needUninit) CoUninitialize();
    return true;
}

bool capture_screenshot(HWND hwnd, const std::string& outputPath,
                        const Element* tree,
                        const std::string& elementId) {
    if (!IsWindow(hwnd)) {
        fprintf(stderr, "lvt: invalid window handle for screenshot\n");
        return false;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Create D3D11 device
    wil::com_ptr<ID3D11Device> d3dDevice;
    wil::com_ptr<ID3D11DeviceContext> d3dCtx;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3dDevice, &featureLevel, &d3dCtx);
    if (FAILED(hr)) {
        fprintf(stderr, "lvt: failed to create D3D11 device (0x%08lx)\n", hr);
        return false;
    }

    std::vector<BYTE> pixels;
    int width = 0, height = 0;
    if (!capture_frame(hwnd, d3dDevice.get(), d3dCtx.get(), pixels, width, height)) {
        return false;
    }

    // Annotate if tree is provided
    if (tree) {
        annotate_pixels(pixels.data(), width, height, hwnd, tree);
    }

    // Determine crop rect if element scoping requested
    RECT cropRect{};
    const RECT* cropPtr = nullptr;
    if (!elementId.empty() && tree) {
        auto* el = find_element_by_id(*tree, elementId);
        if (el && el->bounds.width > 0 && el->bounds.height > 0) {
            RECT winRect{};
            if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &winRect, sizeof(winRect)) != S_OK) {
                GetWindowRect(hwnd, &winRect);
            }
            cropRect.left = el->bounds.x - winRect.left;
            cropRect.top = el->bounds.y - winRect.top;
            cropRect.right = cropRect.left + el->bounds.width;
            cropRect.bottom = cropRect.top + el->bounds.height;
            cropPtr = &cropRect;
        }
    }

    bool ok = save_pixels_as_png(pixels.data(), width, height, outputPath, cropPtr);
    if (!ok) {
        fprintf(stderr, "lvt: failed to save screenshot as PNG\n");
    }
    return ok;
}

} // namespace lvt
