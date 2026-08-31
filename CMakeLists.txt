cmake_minimum_required(VERSION 3.24)

project(ApplicationQuickDial VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Application Quick Dial is a Windows-only application.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(quickdial_core STATIC
  src/Catalog.cpp
  src/HotkeyState.cpp
  src/InstalledApps.cpp
  src/Search.cpp
)

target_include_directories(quickdial_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(quickdial_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(quickdial_core PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(quickdial_core PUBLIC windowsapp shell32)

add_executable(ApplicationQuickDial WIN32
  src/HookManager.cpp
  src/LauncherApp.cpp
  src/LauncherVisualStyle.cpp
  src/StartupManager.cpp
  src/main.cpp
  resources.rc
)

target_compile_definitions(ApplicationQuickDial PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ApplicationQuickDial PRIVATE /W4 /permissive- /EHsc)
target_link_options(ApplicationQuickDial PRIVATE /MANIFEST:NO)
target_link_libraries(ApplicationQuickDial PRIVATE
  quickdial_core
  advapi32
  comctl32
  d2d1
  dwrite
  dwmapi
  gdi32
  ole32
  shell32
  shcore
  user32
  windowscodecs
)

include(CTest)
if(BUILD_TESTING)
  add_executable(quickdial_tests
    tests/CoreTests.cpp
  )
  target_compile_definitions(quickdial_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(quickdial_tests PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_tests PRIVATE quickdial_core)
  add_test(NAME quickdial_core_tests COMMAND quickdial_tests)

  add_executable(quickdial_benchmarks
    benchmarks/PerformanceBenchmarks.cpp
  )
  target_compile_definitions(quickdial_benchmarks PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(quickdial_benchmarks PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_benchmarks PRIVATE quickdial_core psapi user32)
endif()
