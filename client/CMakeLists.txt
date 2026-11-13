cmake_minimum_required(VERSION 3.24)
project(RustOverlayClient LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This client requires Windows x64.")
endif()

set(IMGUI_DIR "" CACHE PATH "Optional local Dear ImGui v1.91.9b source directory")
if(NOT IMGUI_DIR)
    include(FetchContent)
    FetchContent_Declare(imgui
        URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.91.9b
        URL_HASH SHA256=8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_executable(rust_overlay main.cpp DriverInterface.cpp DriverInterface.h Esp.h)
target_compile_features(rust_overlay PRIVATE cxx_std_17)
target_compile_definitions(rust_overlay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(rust_overlay PRIVATE imgui user32 gdi32)
if(MSVC)
    target_compile_options(rust_overlay PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
