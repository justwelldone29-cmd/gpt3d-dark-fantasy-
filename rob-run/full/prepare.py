"""Assemble the new internal source from the preserved original ZIP and overlay."""
from pathlib import Path
import re
import shutil
import zipfile
ROOT = Path(__file__).resolve().parent
DEST = ROOT / "generated"
if DEST.exists():
    shutil.rmtree(DEST)
internal = DEST / "internal"
internal.mkdir(parents=True)
archive = ROOT.parent / "original" / "ROB RUN _2_.zip"
prefix = "ROB RUN/client/internal/"
with zipfile.ZipFile(archive) as z:
    for entry in z.infolist():
        if entry.is_dir() or not entry.filename.startswith(prefix):
            continue
        rel = Path(entry.filename[len(prefix):])
        if rel.is_absolute() or ".." in rel.parts:
            raise ValueError("Unsafe archive path")
        if rel.suffix.lower() in (".dll", ".obj", ".log", ".lib", ".exp"):
            continue
        out = internal / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(z.read(entry))
shutil.copytree(ROOT / "overlay", internal, dirs_exist_ok=True)
shutil.copytree(ROOT / "runtime", DEST / "runtime", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
shutil.copy2(ROOT.parent / "silent-aim-runtime" / "aim_math.js", DEST / "runtime/aim_math.js")
shutil.copytree(ROOT.parent / "silent-aim-runtime/tests", DEST / "runtime/tests", dirs_exist_ok=True)
shutil.copy2(ROOT / "runtime/test_runner.py", DEST / "runtime/tests/test_runner.py")
def change(text, old, new, count=1):
    if text.count(old) < count:
        raise ValueError("Source anchor missing: " + old[:100])
    return text.replace(old, new, count)
p = internal / "menu/ui.h"
s = p.read_text(encoding="utf-8")
s = change(s, '#include "imgui.h"', '#include "imgui.h"\n#include <atomic>')
s = change(s, "extern bool g_menu_open;", "extern std::atomic<bool> g_menu_open;")
p.write_text(s, encoding="utf-8")
p = internal / "menu/ui.cpp"
s = p.read_text(encoding="utf-8")
s = change(s, '#include "menu_compat.h"', '#include "menu_compat.h"\n#include "silent_bridge.h"')
s = change(s, "bool g_menu_open = true;", "std::atomic<bool> g_menu_open{true};")
s = change(s, "ImGuiIO& io = ImGui::GetIO();", "ImGuiIO& io = ImGui::GetIO();\n    ImFontConfig cfg;\n    cfg.FontDataOwnedByAtlas = false;")
s, n = re.subn(r"(AddFontFromMemoryTTF\(lexend_bold, sizeof\(lexend_bold\), [0-9.]+f)\);", r"\1, &cfg);", s)
assert n == 5, n
s = change(s, "sizeof(IconFont), 19, nullptr,", "sizeof(IconFont), 19, &cfg,")
start, end = s.index("static void RenderTabs()"), s.index("void DrawWatermark()")
tabs = s[start:end]
tabs = re.sub(r"window->Pos.y \+ (\d+)", lambda m: "window->Pos.y + " + str(int(m[1]) + (36 if int(m[1]) >= 99 else 0)), tabs)
tabs = change(tabs, "ImVec2(0, 110)", "ImVec2(0, 146)")
tabs = change(tabs, "ImVec2(0, 240)", "ImVec2(0, 276)")
tabs = change(tabs, 'Tab("1", "Aim", 1);', 'Tab("1", "Aim", 1);\n    ImGui::Spacing(); ImGui::Spacing();\n    Tab("1", "Silent Aim", 7);')
s = s[:start] + tabs + s[end:]
s = change(s, "} else if (active_tab == 2) {", """} else if (active_tab == 7) {
                auto cfg = silent_bridge::GetConfig();
                bool enabled = cfg.enabled != 0;
                bool changed = Toggle("Enable Silent Aim", &enabled);
                changed |= ImGui::SliderFloat("FOV", &cfg.fov, .1f, 90.f, "%.1f deg");
                changed |= ImGui::SliderFloat("Distance", &cfg.distance, 1.f, 500.f, "%.0f m");
                if (changed) silent_bridge::SetConfig(enabled, cfg.fov, cfg.distance);
                ImGui::Separator();
                ImGui::TextUnformatted("Target: Head");
                ImGui::TextUnformatted("Line check: On");
            } else if (active_tab == 2) {""")
s = change(s, "            if (active_tab == 2) {", """            if (active_tab == 7) {
                const auto status = silent_bridge::GetStatus();
                const char* label = !status.connected ? "Waiting for launcher" : (status.phase == 2 ? "Runtime error" : "Connected");
                ImGui::TextColored(status.connected && status.phase == 1 ? ImVec4(.4f,.9f,.6f,1.f) : ImVec4(1.f,.7f,.3f,1.f), "%s", label);
                ImGui::Separator();
                ImGui::TextWrapped("%s", status.message.c_str());
                ImGui::Text("Candidates: %u", status.candidates);
                ImGui::Text("Redirects: %llu", static_cast<unsigned long long>(status.redirects));
                ImGui::TextDisabled("Redirects are not confirmed hits");
            } else if (active_tab == 2) {""")
p.write_text(s, encoding="utf-8")
p = internal / "hook/present.cpp"
s = p.read_text(encoding="utf-8")
s = change(s, "#include <cstdarg>", "#include <cstdarg>\n#include <mutex>")
s = change(s, "using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);", "using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);\nusing ResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);")
s = change(s, "PresentFn g_original_present = nullptr;", "PresentFn g_original_present = nullptr;\nResizeFn g_original_resize = nullptr;\nstd::mutex g_render_mutex;")
s = change(s, """    if (g_imgui_ready) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;
    }
""", "")
s = change(s, """    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());""", """    if (!g_rtv) CreateRTV(sc);
    if (!g_rtv) return;
    ID3D11RenderTargetView* previous_rtv = nullptr;
    ID3D11DepthStencilView* previous_dsv = nullptr;
    g_context->OMGetRenderTargets(1, &previous_rtv, &previous_dsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &previous_rtv, previous_dsv);
    if (previous_rtv) previous_rtv->Release();
    if (previous_dsv) previous_dsv->Release();""")
s = change(s, """    if (!g_imgui_ready) InitImGui(sc);
    Render(sc);""", """    {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        if (!g_imgui_ready) InitImGui(sc);
        Render(sc);
    }""")
s = change(s, "bool PatchVtable() {", """HRESULT WINAPI HookedResize(IDXGISwapChain* sc, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    std::lock_guard<std::mutex> lock(g_render_mutex);
    if (g_context) g_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    return g_original_resize(sc, count, width, height, format, flags);
}

bool PatchVtable() {""")
s = change(s, "g_original_present = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);", "g_original_present = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);\n    g_original_resize = reinterpret_cast<ResizeFn>(vtable[13]);")
s = change(s, "if (!VirtualProtect(&vtable[kPresentIndex], sizeof(void*), PAGE_READWRITE, &old))", "const SIZE_T patch_bytes = (13 - kPresentIndex + 1) * sizeof(void*);\n    if (!VirtualProtect(&vtable[kPresentIndex], patch_bytes, PAGE_READWRITE, &old))")
s = change(s, "VirtualProtect(&vtable[kPresentIndex], sizeof(void*), old, &old);", "vtable[13] = reinterpret_cast<void*>(&HookedResize);\n    VirtualProtect(&vtable[kPresentIndex], patch_bytes, old, &old);")
p.write_text(s, encoding="utf-8")
print("Prepared full source:", DEST)
