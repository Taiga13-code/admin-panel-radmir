cmake_minimum_required(VERSION 3.24)
project(ReportPanel VERSION 0.2.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ReportPanel ASI currently targets Windows / GTA San Andreas.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The RADMIR CRMP ASI must be built for Win32 (32-bit). Run build.bat.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE)

add_library(ReportPanel SHARED
    src/asi/AsiPlugin.cpp
    src/core/ReportManager.cpp
    src/parser/ReportParser.cpp
    src/chatlog/ChatlogReader.cpp
    src/settings/Settings.cpp
    src/themes/Theme.cpp
    src/game/ClipboardGameIntegration.cpp
    src/ui/ReportPanelUi.cpp
)
set_target_properties(ReportPanel PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "ReportPanel"
)
target_include_directories(ReportPanel PRIVATE src)
target_link_libraries(ReportPanel PRIVATE
    imgui
    minhook
    nlohmann_json::nlohmann_json
    d3d9 d3dcompiler dinput8 dxguid dwmapi
    comctl32 comdlg32 shell32 ole32 uuid imm32 user32 gdi32
)
target_compile_definitions(ReportPanel PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(ReportPanel PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(ReportPanel PRIVATE -Wall -Wextra -Wpedantic)
endif()
