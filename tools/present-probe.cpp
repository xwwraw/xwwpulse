//  Test helper for --etw-test: a D3D11 flip-model window that clears and
//  presents with vsync for a few seconds, like the render loop of a game.
//    present-probe.exe [seconds] [interval]   interval 1 = vsync, 0 = uncapped
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdlib>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD seconds = argc > 1 ? _wtoi(argv[1]) : 5;
    UINT interval = argc > 2 ? _wtoi(argv[2]) : 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"xwwPulsePresentProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"xwwPulse present probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360, nullptr, nullptr, instance, nullptr);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &swap))) return 2;
    ID3D11Texture2D* buffer = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
    device->CreateRenderTargetView(buffer, nullptr, &view);

    ULONGLONG end = GetTickCount64() + seconds * 1000ULL;
    for (unsigned frame = 0; GetTickCount64() < end; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        float color[4] = { (frame % 120) / 120.f, 0.2f, 0.4f, 1.f };
        context->OMSetRenderTargets(1, &view, nullptr);
        context->ClearRenderTargetView(view, color);
        swap->Present(interval, 0);
        if (!interval) Sleep(4);
    }
    return 0;
}
