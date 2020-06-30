#include "pch.h"

using namespace winrt;
using namespace D2D1;

com_ptr<ID2D1Factory1> create_factory()
{
    D2D1_FACTORY_OPTIONS fo = {};

#ifdef _DEBUG
    fo.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    com_ptr<ID2D1Factory1> factory;

    check_hresult(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        fo,
        factory.put()));

    return factory;
}

HRESULT create_device(D3D_DRIVER_TYPE const type, com_ptr<ID3D11Device>& device)
{
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
        device.put(),
        nullptr,
        nullptr);
}

com_ptr<ID3D11Device> create_device()
{
    com_ptr<ID3D11Device> device;
    auto hr = create_device(D3D_DRIVER_TYPE_HARDWARE, device);

    if (DXGI_ERROR_UNSUPPORTED == hr)
    {
        hr = create_device(D3D_DRIVER_TYPE_WARP, device);
    }

    check_hresult(hr);
    return device;
}

com_ptr<ID2D1DeviceContext> create_render_target(
    com_ptr<ID2D1Factory1> const& factory,
    com_ptr<ID3D11Device> const& device)
{
    auto dxdevice = device.as<IDXGIDevice>();

    com_ptr<ID2D1Device> d2device;
    check_hresult(factory->CreateDevice(dxdevice.get(), d2device.put()));
    com_ptr<ID2D1DeviceContext> target;

    check_hresult(d2device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        target.put()));

    return target;
}

com_ptr<IDXGIFactory2> get_dxgi_factory(com_ptr<ID3D11Device> const& device)
{
    auto dxdevice = device.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter;
    check_hresult(dxdevice->GetAdapter(adapter.put()));
    return capture<IDXGIFactory2>(adapter, &IDXGIAdapter::GetParent);
}

void create_swapchain_bitmap(
    com_ptr<IDXGISwapChain1> const& swapchain,
    com_ptr<ID2D1DeviceContext> const& target)
{
    auto surface = capture<IDXGISurface>(swapchain, &IDXGISwapChain1::GetBuffer, 0);

    auto props = BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    com_ptr<ID2D1Bitmap1> bitmap;

    check_hresult(target->CreateBitmapFromDxgiSurface(surface.get(),
        props,
        bitmap.put()));

    target->SetTarget(bitmap.get());
}

com_ptr<IDXGISwapChain1> create_swapchain(com_ptr<ID3D11Device> const& device, HWND window)
{
    auto const factory = get_dxgi_factory(device);

    DXGI_SWAP_CHAIN_DESC1 props = {};
    props.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.SampleDesc.Count = 1;
    props.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    props.BufferCount = 2;
    props.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    com_ptr<IDXGISwapChain1> swapChain;

    check_hresult(factory->CreateSwapChainForHwnd(device.get(),
        window,
        &props,
        nullptr,
        nullptr,
        swapChain.put()));

    return swapChain;
}

struct Window
{
    HWND m_window{};

    Window()
    {
        WNDCLASS wc{};
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Sample";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = window_proc;
        RegisterClassW(&wc);

        CreateWindowW(wc.lpszClassName,
            L"Clock",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            nullptr, nullptr, wc.hInstance, this);
    }

    static LRESULT __stdcall window_proc(HWND const window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
    {
        if (WM_NCCREATE == message)
        {
            auto cs = reinterpret_cast<CREATESTRUCT*>(lparam);
            auto that = static_cast<Window*>(cs->lpCreateParams);
            that->m_window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
        }
        else if (auto that = reinterpret_cast<Window*>(GetWindowLongPtr(window, GWLP_USERDATA)))
        {
            return that->message_handler(message, wparam, lparam);
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT message_handler(UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept
    {
        if (WM_DESTROY == message)
        {
            PostQuitMessage(0);
            return 0;
        }

        if (WM_PAINT == message)
        {
            PAINTSTRUCT ps;
            check_bool(BeginPaint(m_window, &ps));
            render();
            EndPaint(m_window, &ps);
            return 0;
        }

        if (WM_SIZE == message)
        {
            if (m_target && SIZE_MINIMIZED != wparam)
            {
                resize_swapchain_bitmap();
                render();
            }

            return 0;
        }

        if (WM_DISPLAYCHANGE == message)
        {
            render();
            return 0;
        }

        if (WM_USER == message)
        {
            if (S_OK == m_swapChain->Present(0, DXGI_PRESENT_TEST))
            {
                m_dxfactory->UnregisterOcclusionStatus(m_occlusion);
                m_occlusion = 0;
                m_visible = true;
            }

            return 0;
        }

        if (WM_POWERBROADCAST == message)
        {
            auto const ps = reinterpret_cast<POWERBROADCAST_SETTING*>(lparam);
            m_visible = 0 != *reinterpret_cast<DWORD const*>(ps->Data);

            if (m_visible)
            {
                PostMessageW(m_window, WM_NULL, 0, 0);
            }

            return TRUE;
        }

        if (WM_ACTIVATE == message)
        {
            m_visible = !HIWORD(wparam);
            return 0;
        }

        if (WM_GETMINMAXINFO == message)
        {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.y = 200;

            return 0;
        }

        return DefWindowProcW(m_window, message, wparam, lparam);
    }

    void resize_swapchain_bitmap()
    {
        m_target->SetTarget(nullptr);

        if (S_OK == m_swapChain->ResizeBuffers(0,
            0, 0,
            DXGI_FORMAT_UNKNOWN,
            0))
        {
            create_swapchain_bitmap(m_swapChain, m_target);
            create_device_size_resources();
        }
        else
        {
            release_device();
        }
    }

    void render()
    {
        if (!m_target)
        {
            auto device = create_device();
            m_target = create_render_target(m_factory, device);
            m_swapChain = create_swapchain(device, m_window);
            create_swapchain_bitmap(m_swapChain, m_target);

            m_target->SetDpi(m_dpi, m_dpi);

            create_device_resources();
            create_device_size_resources();
        }

        m_target->BeginDraw();
        draw();
        m_target->EndDraw();

        auto const hr = m_swapChain->Present(1, 0);

        if (S_OK == hr)
        {
            // Do nothing
        }
        else if (DXGI_STATUS_OCCLUDED == hr)
        {
            check_hresult(m_dxfactory->RegisterOcclusionStatusWindow(m_window, WM_USER, &m_occlusion));
            m_visible = false;
        }
        else
        {
            release_device();
        }
    }

    void release_device()
    {
        m_target = nullptr;
        m_swapChain = nullptr;

        release_device_resources();
    }

    void run()
    {
        m_factory = create_factory();

        check_hresult(CreateDXGIFactory1(__uuidof(m_dxfactory),
            reinterpret_cast<void**>(m_dxfactory.put())));

        float dpiY;
        m_factory->GetDesktopDpi(&m_dpi, &dpiY);

        create_device_independent_resources();

        check_bool(RegisterPowerSettingNotification(m_window,
            &GUID_SESSION_DISPLAY_STATUS,
            DEVICE_NOTIFY_WINDOW_HANDLE));

        MSG message = {};

        while (true)
        {
            if (m_visible)
            {
                render();

                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    DispatchMessageW(&message);
                }
            }
            else
            {
                if (BOOL result = GetMessageW(&message, 0, 0, 0))
                {
                    if (-1 != result)
                    {
                        DispatchMessageW(&message);
                    }
                }
            }

            if (WM_QUIT == message.message)
            {
                break;
            }
        }
    }

    double get_time() const
    {
        LARGE_INTEGER time;
        check_bool(QueryPerformanceCounter(&time));

        return static_cast<double>(time.QuadPart) / m_frequency.QuadPart;
    }

    void schedule_animation()
    {
        m_manager = create_instance<IUIAnimationManager>(__uuidof(UIAnimationManager));
        auto library = create_instance<IUIAnimationTransitionLibrary>(__uuidof(UIAnimationTransitionLibrary));
        check_bool(QueryPerformanceFrequency(&m_frequency));

        com_ptr<IUIAnimationTransition> transition;

        check_hresult(library->CreateAccelerateDecelerateTransition(
            5.0,
            1.0,
            0.2,
            0.8,
            transition.put()));

        check_hresult(m_manager->CreateAnimationVariable(0.0, m_variable.put()));

        check_hresult(m_manager->ScheduleTransition(m_variable.get(),
            transition.get(),
            get_time()));
    }

    void create_device_independent_resources()
    {
        D2D1_STROKE_STYLE_PROPERTIES style = {};
        style.startCap = D2D1_CAP_STYLE_ROUND;
        style.endCap = D2D1_CAP_STYLE_TRIANGLE;

        check_hresult(m_factory->CreateStrokeStyle(style,
            nullptr, 0,
            m_style.put()));

        schedule_animation();
    }

    void release_device_resources()
    {
        m_brush = nullptr;
        m_clock = nullptr;
        m_shadow = nullptr;
    }

    void create_device_resources()
    {
        constexpr D2D1_COLOR_F color_orange = { 0.92f,  0.38f,  0.208f,  1.0f };

        check_hresult(m_target->CreateSolidColorBrush(color_orange,
            BrushProperties(0.8f),
            m_brush.put()));
    }

    void create_device_size_resources()
    {
        auto sizeF = m_target->GetSize();

        auto sizeU = SizeU(static_cast<UINT>(sizeF.width * m_dpi / 96.0f),
            static_cast<UINT>(sizeF.height * m_dpi / 96.0f));

        auto props = BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            m_dpi, m_dpi);

        m_clock = nullptr;

        check_hresult(m_target->CreateBitmap(sizeU,
            nullptr, 0,
            props,
            m_clock.put()));

        m_shadow = nullptr;

        struct __declspec(uuid("C67EA361-1863-4e69-89DB-695D3E9A5B6B")) Direct2DShadow;

        check_hresult(m_target->CreateEffect(__uuidof(Direct2DShadow),
            m_shadow.put()));

        m_shadow->SetInput(0, m_clock.get());

    }

    void draw_clock()
    {
        auto size = m_target->GetSize();
        auto radius = std::max(200.0f, std::min(size.width, size.height)) / 2.0f - 50.0f;
        auto const offset = SizeF(2.0f, 2.0f);
        auto translation = Matrix3x2F::Translation(size.width / offset.width, size.height / offset.height);

        m_target->SetTransform(translation);

        m_target->DrawEllipse(Ellipse(Point2F(), radius, radius),
            m_brush.get(),
            radius / 20.f);

        SYSTEMTIME time;
        GetLocalTime(&time);

        auto secondAngle = (time.wSecond + time.wMilliseconds / 1000.0f) * 6.0f;
        auto minuteAngle = time.wMinute * 6.0f + secondAngle / 60.0f;
        auto hourAngle = time.wHour % 12 * 30.0f + minuteAngle / 12.0f;

        double swing;
        check_hresult(m_variable->GetValue(&swing));

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

        m_target->SetTransform(Matrix3x2F::Rotation(secondAngle) * m_orientation * translation);

        m_target->DrawLine(Point2F(),
            Point2F(0.0f, -(radius * 0.75f)),
            m_brush.get(),
            radius / 25.f,
            m_style.get());

        m_target->SetTransform(Matrix3x2F::Rotation(minuteAngle) * m_orientation * translation);

        m_target->DrawLine(Point2F(),
            Point2F(0.0f, -(radius * 0.75f)),
            m_brush.get(),
            radius / 15.0f,
            m_style.get());

        m_target->SetTransform(Matrix3x2F::Rotation(hourAngle) * m_orientation * translation);

        m_target->DrawLine(Point2F(),
            Point2F(0.0f, -(radius * 0.5f)),
            m_brush.get(),
            radius / 10.0f,
            m_style.get());
    }

    void draw()
    {
        m_orientation = Matrix3x2F::Identity();
        auto offset = SizeF(5.0f, 5.0f);
        check_hresult(m_manager->Update(get_time()));

        m_target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        constexpr D2D1_COLOR_F color_white = { 1.0f,  1.0f,  1.0f,  1.0f };
        m_target->Clear(color_white);

        m_target->SetUnitMode(D2D1_UNIT_MODE_DIPS);

        com_ptr<ID2D1Image> previous;
        m_target->GetTarget(previous.put());

        m_target->SetTarget(m_clock.get());
        m_target->Clear();
        draw_clock();

        m_target->SetTarget(previous.get());
        m_target->SetTransform(Matrix3x2F::Translation(offset));

        m_target->DrawImage(m_shadow.get(),
            D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_OVER);

        m_target->SetTransform(Matrix3x2F::Identity());

        m_target->DrawImage(m_clock.get());
    }

    float m_dpi{};
    bool m_visible{};
    DWORD m_occlusion{};
    LARGE_INTEGER m_frequency{};
    D2D1_MATRIX_3X2_F m_orientation{};

    com_ptr<ID2D1Factory1> m_factory;
    com_ptr<IDXGIFactory2> m_dxfactory;
    com_ptr<ID2D1DeviceContext> m_target;
    com_ptr<IDXGISwapChain1> m_swapChain;
    com_ptr<ID2D1SolidColorBrush> m_brush;
    com_ptr<ID2D1StrokeStyle> m_style;
    com_ptr<ID2D1Effect> m_shadow;
    com_ptr<ID2D1Bitmap1> m_clock;
    com_ptr<IUIAnimationManager> m_manager;
    com_ptr<IUIAnimationVariable> m_variable;
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment(apartment_type::single_threaded);

    Window window;
    window.run();
}
