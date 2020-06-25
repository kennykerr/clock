#include "Precompiled.h"

namespace wrl = Microsoft::WRL;
namespace d2d = D2D1;

#define ASSERT(expression) _ASSERTE(expression)

#ifdef _DEBUG

#define VERIFY(expression) ASSERT(expression)
#define HR(expression) ASSERT(S_OK == (expression))

inline void TRACE(WCHAR const* const format, ...)
{
    va_list args;
    va_start(args, format);

    WCHAR output[512];
    vswprintf_s(output, format, args);

    OutputDebugString(output);

    va_end(args);
}

#else

#define VERIFY(expression) (expression)

struct ComException
{
    HRESULT const hr;
    ComException(HRESULT const value) : hr(value) {}
};

inline void HR(HRESULT const hr)
{
    if (S_OK != hr) throw ComException(hr);
}

#define TRACE __noop

#endif

struct AutoCoInitialize
{
    AutoCoInitialize()
    {
        HR(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }

    ~AutoCoInitialize()
    {
        CoUninitialize();
    }
};

template <typename T>
wrl::ComPtr<T> CreateInstance(REFCLSID clsid, DWORD const context = CLSCTX_INPROC_SERVER)
{
    wrl::ComPtr<T> instance;

    HR(CoCreateInstance(clsid,
        nullptr,
        context,
        __uuidof(T),
        reinterpret_cast<void**>(instance.GetAddressOf())));

    return instance;
}

inline wrl::ComPtr<ID2D1Factory1> CreateFactory()
{
    D2D1_FACTORY_OPTIONS fo = {};

#ifdef _DEBUG
    fo.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    wrl::ComPtr<ID2D1Factory1> factory;

    HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        fo,
        factory.GetAddressOf()));

    return factory;
}

inline HRESULT CreateDevice(D3D_DRIVER_TYPE const type, wrl::ComPtr<ID3D11Device>& device)
{
    ASSERT(!device);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    return D3D11CreateDevice(nullptr,
        type,
        nullptr,
        flags,
        nullptr, 0,
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        nullptr,
        nullptr);
}

inline wrl::ComPtr<ID3D11Device> CreateDevice()
{
    wrl::ComPtr<ID3D11Device> device;

    auto hr = CreateDevice(D3D_DRIVER_TYPE_HARDWARE, device);

    if (DXGI_ERROR_UNSUPPORTED == hr)
    {
        hr = CreateDevice(D3D_DRIVER_TYPE_WARP, device);
    }

    HR(hr);

    return device;
}

inline wrl::ComPtr<ID2D1DeviceContext> CreateRenderTarget(wrl::ComPtr<ID2D1Factory1> const& factory,
    wrl::ComPtr<ID3D11Device> const& device)
{
    ASSERT(factory);
    ASSERT(device);

    wrl::ComPtr<IDXGIDevice> dxdevice;
    HR(device.As(&dxdevice));

    wrl::ComPtr<ID2D1Device> d2device;

    HR(factory->CreateDevice(dxdevice.Get(),
        d2device.GetAddressOf()));

    wrl::ComPtr<ID2D1DeviceContext> target;

    HR(d2device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        target.GetAddressOf()));

    return target;
}

inline wrl::ComPtr<IDXGIFactory2> GetDxgiFactory(wrl::ComPtr<ID3D11Device> const& device)
{
    ASSERT(device);

    wrl::ComPtr<IDXGIDevice> dxdevice;
    HR(device.As(&dxdevice));

    wrl::ComPtr<IDXGIAdapter> adapter;
    HR(dxdevice->GetAdapter(adapter.GetAddressOf()));

    wrl::ComPtr<IDXGIFactory2> factory;

    HR(adapter->GetParent(__uuidof(factory),
        reinterpret_cast<void**>(factory.GetAddressOf())));

    return factory;
}

inline void CreateDeviceSwapChainBitmap(wrl::ComPtr<IDXGISwapChain1> const& swapchain,
    wrl::ComPtr<ID2D1DeviceContext> const& target)
{
    ASSERT(swapchain);
    ASSERT(target);

    wrl::ComPtr<IDXGISurface> surface;

    HR(swapchain->GetBuffer(0,
        __uuidof(surface),
        reinterpret_cast<void**>(surface.GetAddressOf())));

    auto const props = d2d::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        d2d::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    wrl::ComPtr<ID2D1Bitmap1> bitmap;

    HR(target->CreateBitmapFromDxgiSurface(surface.Get(),
        props,
        bitmap.GetAddressOf()));

    target->SetTarget(bitmap.Get());
}

template <typename T>
struct DesktopWindow :
    CWindowImpl<DesktopWindow<T>, CWindow, CWinTraits<WS_OVERLAPPEDWINDOW | WS_VISIBLE>>
{
    DECLARE_WND_CLASS_EX(nullptr, 0, -1);

    BEGIN_MSG_MAP(c)
        MESSAGE_HANDLER(WM_PAINT, PaintHandler)
        MESSAGE_HANDLER(WM_SIZE, SizeHandler)
        MESSAGE_HANDLER(WM_DISPLAYCHANGE, DisplayChangeHandler)
        MESSAGE_HANDLER(WM_USER, OcclusionHandler)
        MESSAGE_HANDLER(WM_POWERBROADCAST, PowerHandler)
        MESSAGE_HANDLER(WM_ACTIVATE, ActivateHandler)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, GetMinMaxInfoHandler)
        MESSAGE_HANDLER(WM_DESTROY, DestroyHandler)
    END_MSG_MAP()

    LRESULT PaintHandler(UINT, WPARAM, LPARAM, BOOL&)
    {
        PAINTSTRUCT ps;
        VERIFY(BeginPaint(&ps));

        Render();

        EndPaint(&ps);

        return 0;
    }

    LRESULT DisplayChangeHandler(UINT, WPARAM, LPARAM, BOOL&)
    {
        Render();
        return 0;
    }

    LRESULT DestroyHandler(UINT, WPARAM, LPARAM, BOOL&)
    {
        PostQuitMessage(0);
        return 0;
    }

    LRESULT SizeHandler(UINT, WPARAM wparam, LPARAM, BOOL&)
    {
        if (m_target && SIZE_MINIMIZED != wparam)
        {
            ResizeSwapChainBitmap();
            Render();
        }

        return 0;
    }

    LRESULT OcclusionHandler(UINT, WPARAM, LPARAM, BOOL&)
    {
        ASSERT(m_occlusion);

        if (S_OK == m_swapChain->Present(0, DXGI_PRESENT_TEST))
        {
            m_dxfactory->UnregisterOcclusionStatus(m_occlusion);
            m_occlusion = 0;
            m_visible = true;
        }

        return 0;
    }

    LRESULT PowerHandler(UINT, WPARAM, LPARAM lparam, BOOL&)
    {
        auto const ps = reinterpret_cast<POWERBROADCAST_SETTING*>(lparam);
        m_visible = 0 != *reinterpret_cast<DWORD const*>(ps->Data);

        if (m_visible)
        {
            PostMessage(WM_NULL);
        }

        return TRUE;
    }

    LRESULT ActivateHandler(UINT, WPARAM wparam, LPARAM, BOOL&)
    {
        m_visible = !HIWORD(wparam);

        return 0;
    }

    LRESULT GetMinMaxInfoHandler(UINT, WPARAM, LPARAM lparam, BOOL&)
    {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.y = 200;

        return 0;
    }

    void ResizeSwapChainBitmap()
    {
        ASSERT(m_target);
        ASSERT(m_swapChain);

        m_target->SetTarget(nullptr);

        if (S_OK == m_swapChain->ResizeBuffers(0,
            0, 0,
            DXGI_FORMAT_UNKNOWN,
            0))
        {
            CreateDeviceSwapChainBitmap(m_swapChain, m_target);
            static_cast<T*>(this)->CreateDeviceSizeResources();
        }
        else
        {
            ReleaseDevice();
        }
    }

    static wrl::ComPtr<IDXGISwapChain1> CreateSwapChainForHwnd(wrl::ComPtr<ID3D11Device> const& device, HWND window)
    {
        ASSERT(device);
        ASSERT(window);

        auto const factory = GetDxgiFactory(device);

        DXGI_SWAP_CHAIN_DESC1 props = {};
        props.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.SampleDesc.Count = 1;
        props.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        props.BufferCount = 2;
        props.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        wrl::ComPtr<IDXGISwapChain1> swapChain;

        HR(factory->CreateSwapChainForHwnd(device.Get(),
            window,
            &props,
            nullptr,
            nullptr,
            swapChain.GetAddressOf()));

        return swapChain;
    }

    void Render()
    {
        if (!m_target)
        {
            auto device = CreateDevice();
            m_target = CreateRenderTarget(m_factory, device);
            m_swapChain = CreateSwapChainForHwnd(device, m_hWnd);
            CreateDeviceSwapChainBitmap(m_swapChain, m_target);

            m_target->SetDpi(m_dpi, m_dpi);

            static_cast<T*>(this)->CreateDeviceResources();
            static_cast<T*>(this)->CreateDeviceSizeResources();
        }

        m_target->BeginDraw();
        static_cast<T*>(this)->Draw();
        m_target->EndDraw();

        auto const hr = m_swapChain->Present(1, 0);

        if (S_OK == hr)
        {
            // Do nothing
        }
        else if (DXGI_STATUS_OCCLUDED == hr)
        {
            HR(m_dxfactory->RegisterOcclusionStatusWindow(m_hWnd, WM_USER, &m_occlusion));
            m_visible = false;
        }
        else
        {
            ReleaseDevice();
        }
    }

    void ReleaseDevice()
    {
        m_target.Reset();
        m_swapChain.Reset();

        static_cast<T*>(this)->ReleaseDeviceResources();
    }

    void Run()
    {
        m_factory = CreateFactory();

        HR(CreateDXGIFactory1(__uuidof(m_dxfactory),
            reinterpret_cast<void**>(m_dxfactory.GetAddressOf())));

        float dpiY;
        m_factory->GetDesktopDpi(&m_dpi, &dpiY);

        static_cast<T*>(this)->CreateDeviceIndependentResources();

        RECT bounds = { 10, 10, 1010, 750 };
        VERIFY(__super::Create(nullptr, bounds, L"Direct2D"));

        VERIFY(RegisterPowerSettingNotification(m_hWnd,
            &GUID_SESSION_DISPLAY_STATUS,
            DEVICE_NOTIFY_WINDOW_HANDLE));

        MSG message = {};

        while (true)
        {
            if (m_visible)
            {
                Render();

                while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    DispatchMessage(&message);
                }
            }
            else
            {
                if (BOOL result = GetMessage(&message, 0, 0, 0))
                {
                    if (-1 != result)
                    {
                        DispatchMessage(&message);
                    }
                }
            }

            if (WM_QUIT == message.message)
            {
                break;
            }
        }
    }

    void CreateDeviceIndependentResources() {}
    void CreateDeviceResources() {}
    void CreateDeviceSizeResources() {}
    void ReleaseDeviceResources() {}

    wrl::ComPtr<ID2D1Factory1> m_factory;
    wrl::ComPtr<IDXGIFactory2> m_dxfactory;
    wrl::ComPtr<ID2D1DeviceContext> m_target;
    wrl::ComPtr<IDXGISwapChain1> m_swapChain;
    float m_dpi;
    bool m_visible;
    DWORD m_occlusion;

    DesktopWindow() :
        m_dpi(0),
        m_visible(true),
        m_occlusion(0)
    {
    }
};

struct __declspec(uuid("C67EA361-1863-4e69-89DB-695D3E9A5B6B")) Direct2DShadow;
D2D1_COLOR_F const COLOR_WHITE = { 1.0f,  1.0f,  1.0f,  1.0f };
D2D1_COLOR_F const COLOR_ORANGE = { 0.92f,  0.38f,  0.208f,  1.0f };

BYTE const* BackgroundImage();
UINT BackgroundImageSize();

template <typename T>
struct ClockSample : T
{
    wrl::ComPtr<ID2D1SolidColorBrush> m_brush;
    wrl::ComPtr<ID2D1StrokeStyle> m_style;
    wrl::ComPtr<ID2D1Effect> m_shadow;
    wrl::ComPtr<ID2D1Bitmap1> m_clock;
    wrl::ComPtr<IWICFormatConverter> m_image;
    wrl::ComPtr<ID2D1Bitmap> m_bitmap;
    wrl::ComPtr<IUIAnimationManager> m_manager;
    wrl::ComPtr<IUIAnimationVariable> m_variable;
    LARGE_INTEGER m_frequency;
    D2D1_MATRIX_3X2_F m_orientation;

    void LoadBackgroundImage()
    {
        auto factory = CreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);

        wrl::ComPtr<IWICStream> stream;
        HR(factory->CreateStream(stream.GetAddressOf()));
        HR(stream->InitializeFromMemory(const_cast<BYTE*>(BackgroundImage()), BackgroundImageSize()));

        wrl::ComPtr<IWICBitmapDecoder> decoder;
        HR(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()));

        wrl::ComPtr<IWICBitmapFrameDecode> source;
        HR(decoder->GetFrame(0, source.GetAddressOf()));

        HR(factory->CreateFormatConverter(m_image.GetAddressOf()));
        HR(m_image->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
    }

    double GetTime() const
    {
        LARGE_INTEGER time;
        VERIFY(QueryPerformanceCounter(&time));

        return static_cast<double>(time.QuadPart) / m_frequency.QuadPart;
    }

    void ScheduleAnimation()
    {
        m_manager = CreateInstance<IUIAnimationManager>(__uuidof(UIAnimationManager));
        auto library = CreateInstance<IUIAnimationTransitionLibrary>(__uuidof(UIAnimationTransitionLibrary));
        VERIFY(QueryPerformanceFrequency(&m_frequency));

        wrl::ComPtr<IUIAnimationTransition> transition;

        HR(library->CreateAccelerateDecelerateTransition(
            5.0,
            1.0,
            0.2,
            0.8,
            transition.GetAddressOf()));

        HR(m_manager->CreateAnimationVariable(0.0, m_variable.GetAddressOf()));

        HR(m_manager->ScheduleTransition(m_variable.Get(),
            transition.Get(),
            GetTime()));
    }

    void CreateDeviceIndependentResources()
    {
        D2D1_STROKE_STYLE_PROPERTIES style = {};
        style.startCap = D2D1_CAP_STYLE_ROUND;
        style.endCap = D2D1_CAP_STYLE_TRIANGLE;

        ASSERT(!m_style);

        HR(m_factory->CreateStrokeStyle(style,
            nullptr, 0,
            m_style.GetAddressOf()));

        LoadBackgroundImage();
        ScheduleAnimation();
    }

    void ReleaseDeviceResources()
    {
        m_brush.Reset();
        m_bitmap.Reset();
        m_clock.Reset();
        m_shadow.Reset();
    }

    void CreateDeviceResources()
    {
        HR(m_target->CreateSolidColorBrush(COLOR_ORANGE,
            d2d::BrushProperties(0.8f),
            m_brush.GetAddressOf()));

        HR(m_target->CreateBitmapFromWicBitmap(m_image.Get(),
            m_bitmap.GetAddressOf()));
    }

    void CreateDeviceSizeResources()
    {
        auto sizeF = m_target->GetSize();

        auto sizeU = d2d::SizeU(static_cast<UINT>(sizeF.width * m_dpi / 96.0f),
            static_cast<UINT>(sizeF.height * m_dpi / 96.0f));

        auto props = d2d::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            d2d::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            m_dpi, m_dpi);

        HR(m_target->CreateBitmap(sizeU,
            nullptr, 0,
            props,
            m_clock.ReleaseAndGetAddressOf()));

        HR(m_target->CreateEffect(__uuidof(Direct2DShadow),
            m_shadow.ReleaseAndGetAddressOf()));

        m_shadow->SetInput(0, m_clock.Get());

    }

    void DrawClock()
    {
        auto size = m_target->GetSize();
        auto radius = std::max(200.0f, std::min(size.width, size.height)) / 2.0f - 50.0f;
        auto const offset = d2d::SizeF(2.0f, 2.0f);
        auto translation = d2d::Matrix3x2F::Translation(size.width / offset.width, size.height / offset.height);

        m_target->SetTransform(translation);

        m_target->DrawEllipse(d2d::Ellipse(d2d::Point2F(), radius, radius),
            m_brush.Get(),
            radius / 20.f);

        SYSTEMTIME time;
        GetLocalTime(&time);

        auto secondAngle = (time.wSecond + time.wMilliseconds / 1000.0f) * 6.0f;
        auto minuteAngle = time.wMinute * 6.0f + secondAngle / 60.0f;
        auto hourAngle = time.wHour % 12 * 30.0f + minuteAngle / 12.0f;

        double swing;
        HR(m_variable->GetValue(&swing));

        if (1.0 > swing)
        {
            static float secondPrevious = secondAngle;
            static float minutePrevious = minuteAngle;
            static float hourPrevious = hourAngle;

            if (secondPrevious > secondAngle) secondAngle += 360.0f;
            if (minutePrevious > minuteAngle) minuteAngle += 360.0f;
            if (hourPrevious > hourAngle)   hourAngle += 360.0f;

            secondAngle *= static_cast<float>(swing);
            minuteAngle *= static_cast<float>(swing);
            hourAngle *= static_cast<float>(swing);
        }

        m_target->SetTransform(d2d::Matrix3x2F::Rotation(secondAngle) * m_orientation * translation);

        m_target->DrawLine(d2d::Point2F(),
            d2d::Point2F(0.0f, -(radius * 0.75f)),
            m_brush.Get(),
            radius / 25.f,
            m_style.Get());

        m_target->SetTransform(d2d::Matrix3x2F::Rotation(minuteAngle) * m_orientation * translation);

        m_target->DrawLine(d2d::Point2F(),
            d2d::Point2F(0.0f, -(radius * 0.75f)),
            m_brush.Get(),
            radius / 15.0f,
            m_style.Get());

        m_target->SetTransform(d2d::Matrix3x2F::Rotation(hourAngle) * m_orientation * translation);

        m_target->DrawLine(d2d::Point2F(),
            d2d::Point2F(0.0f, -(radius * 0.5f)),
            m_brush.Get(),
            radius / 10.0f,
            m_style.Get());
    }

    void Draw()
    {
        m_orientation = d2d::Matrix3x2F::Identity();
        auto offset = d2d::SizeF(5.0f, 5.0f);
        HR(m_manager->Update(GetTime()));

        m_target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        m_target->Clear(COLOR_WHITE);
        m_target->DrawBitmap(m_bitmap.Get());
        m_target->SetUnitMode(D2D1_UNIT_MODE_DIPS);

        wrl::ComPtr<ID2D1Image> previous;
        m_target->GetTarget(previous.GetAddressOf());

        m_target->SetTarget(m_clock.Get());
        m_target->Clear();
        DrawClock();

        m_target->SetTarget(previous.Get());
        m_target->SetTransform(d2d::Matrix3x2F::Translation(offset));

        m_target->DrawImage(m_shadow.Get(),
            D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_OVER);

        m_target->SetTransform(d2d::Matrix3x2F::Identity());

        m_target->DrawImage(m_clock.Get());
    }
};

struct SampleWindow : ClockSample<DesktopWindow<SampleWindow>>
{
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AutoCoInitialize oldschool;

    SampleWindow window;
    window.Run();
}
