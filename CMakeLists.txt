cmake_minimum_required(VERSION 3.20)
project(FlatPace CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI third_party/imgui)
add_executable(FlatPace WIN32
  main.cpp app.rc
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(FlatPace PRIVATE ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(FlatPace PRIVATE UNICODE _UNICODE NDEBUG)
target_link_libraries(FlatPace PRIVATE d3d11 dxgi d3dcompiler dwmapi dcomp gdi32 shell32 user32 advapi32 version imm32 winmm pdh)

if(MSVC)
  target_compile_options(FlatPace PRIVATE /utf-8 /W4)
  # El manifiesto ya va en app.rc; evitar que el enlazador genere otro
  target_link_options(FlatPace PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)
  set_property(TARGET FlatPace PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Pruebas automaticas de la logica pura (src/core). No necesitan Windows: tambien se pueden
# compilar y ejecutar con tests/run_tests.sh (Linux/macOS) o tests/run_tests.bat.
enable_testing()
add_executable(flatpace_tests tests/test_core.cpp)
add_test(NAME core COMMAND flatpace_tests)
