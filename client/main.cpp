#include "DriverInterface.h"
#include "Esp.h"
#include <tlhelp32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t TargetName[] = L"RustClient.exe";
constexpr wchar_t WindowClass[] = L"DumperOverlayWindow";

DWORD FindPid() {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot && Process32FirstW(snapshot.Get(), &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, TargetName) == 0) return entry.th32ProcessID;
        } while (Process32NextW(snapshot.Get(), &entry));
    }
    return 0;
}

struct WindowSearch { DWORD pid; HWND window = nullptr; };
BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && IsWindowVisible(window) &&
        !GetWindow(window, GW_OWNER) && !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Check(HRESULT result) {
    if (FAILED(result)) {
        char text[80]{};
        std::snprintf(text, sizeof(text), "DX11/DWM error: 0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

class Overlay {
public:
    ~Overlay() {
        if (dxBackend_) ImGui_ImplDX11_Shutdown();
        if (winBackend_) ImGui_ImplWin32_Shutdown();
        if (imgui_) ImGui::DestroyContext();
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(WindowClass, GetModuleHandleW(nullptr));
    }
    void Initialize() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = WindowClass;
        if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassExW failed");
        registered_ = true;
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WindowClass, L"Dumper overlay",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window_) throw std::runtime_error("CreateWindowExW failed");
        if (!SetLayeredWindowAttributes(window_, RGB(0, 0, 0), 0, LWA_COLORKEY))
            throw std::runtime_error("SetLayeredWindowAttributes failed");
        const MARGINS margins{-1, -1, -1, -1};
        Check(DwmExtendFrameIntoClientArea(window_, &margins));
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = desc.BufferDesc.Height = 1;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window_;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        Check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &desc,
            swap_.GetAddressOf(), device_.GetAddressOf(), nullptr, context_.GetAddressOf()));
        CreateTarget();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_ = true;
        ImGui::GetIO().IniFilename = nullptr;
        winBackend_ = ImGui_ImplWin32_Init(window_);
        if (!winBackend_) throw std::runtime_error("ImGui Win32 initialization failed");
        dxBackend_ = ImGui_ImplDX11_Init(device_.Get(), context_.Get());
        if (!dxBackend_) throw std::runtime_error("ImGui DX11 initialization failed");
    }
    bool Align(HWND target) {
        if (!target || !IsWindow(target) || IsIconic(target) || GetForegroundWindow() != target) {
            ShowWindow(window_, SW_HIDE);
            return false;
        }
        RECT rect{};
        POINT origin{};
        if (!GetClientRect(target, &rect) || !ClientToScreen(target, &origin)) {
            ShowWindow(window_, SW_HIDE);
            return false;
        }
        const UINT width = static_cast<UINT>(rect.right - rect.left);
        const UINT height = static_cast<UINT>(rect.bottom - rect.top);
        if (!width || !height) { ShowWindow(window_, SW_HIDE); return false; }
        if (width != width_ || height != height_) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            target_.Reset();
            Check(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
            CreateTarget();
            width_ = width; height_ = height;
        }
        if (!SetWindowPos(window_, HWND_TOPMOST, origin.x, origin.y,
            static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE | SWP_SHOWWINDOW))
            throw std::runtime_error("SetWindowPos failed");
        return true;
    }
    void Render(DriverInterface& driver, DWORD pid) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        std::uint64_t base = 0;
        if (driver.GetModuleBase(pid, TargetName, base)) {
            DrawESP(driver, pid, base);
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(12, 12),
                IM_COL32(255, 255, 255, 255), "Driver connected | ESP stub | END: exit");
        } else {
            char text[96]{};
            std::snprintf(text, sizeof(text), "GetModuleBase failed: %lu | END: exit", driver.LastError());
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(12, 12), IM_COL32(255, 160, 80, 255), text);
        }
        ImGui::Render();
        ID3D11RenderTargetView* view = target_.Get();
        context_->OMSetRenderTargets(1, &view, nullptr);
        const float transparent[]{0.f, 0.f, 0.f, 0.f};
        context_->ClearRenderTargetView(view, transparent);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        Check(swap_->Present(1, 0));
    }
private:
    void CreateTarget() {
        ComPtr<ID3D11Texture2D> buffer;
        Check(swap_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())));
        Check(device_->CreateRenderTargetView(buffer.Get(), nullptr, target_.GetAddressOf()));
    }
    HWND window_ = nullptr;
    bool registered_ = false, imgui_ = false, winBackend_ = false, dxBackend_ = false;
    UINT width_ = 1, height_ = 1;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> target_;
};
}

int main() {
    // Physical coordinates across monitors with different scaling factors.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        DriverInterface driver;
        if (!driver.Open()) {
            std::fprintf(stderr, "CreateFileW(\\\\.\\Dumper) failed: %lu\n", driver.LastError());
            return 1;
        }
        Overlay overlay;
        overlay.Initialize();
        UniqueHandle process;
        DWORD pid = 0;
        HWND game = nullptr;
        ULONGLONG nextSearch = 0;
        std::puts("Waiting for RustClient.exe. Press END to exit.");
        while (!(GetAsyncKeyState(VK_END) & 0x8000)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) return 0;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (process && WaitForSingleObject(process.Get(), 0) != WAIT_TIMEOUT) {
                process.Reset(); pid = 0; game = nullptr; nextSearch = 0;
            }
            if (GetTickCount64() >= nextSearch) {
                nextSearch = GetTickCount64() + 1000;
                if (!process) {
                    pid = FindPid();
                    if (pid) process.Reset(OpenProcess(PROCESS_VM_READ |
                        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                    if (pid && !process)
                        std::fprintf(stderr, "OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
                }
                WindowSearch search{pid};
                if (process) EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
                game = search.window;
            }
            if (overlay.Align(game)) overlay.Render(driver, pid);
            else Sleep(16);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
