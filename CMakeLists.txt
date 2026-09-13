cmake_minimum_required(VERSION 3.24)

project(ApplicationQuickDial VERSION 0.1.0 LANGUAGES CXX)

# Keep portable builds independent of a separately installed VC++ runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

add_library(quickdial_background STATIC
  src/BackgroundTasks.cpp
  src/IconLoader.cpp
)
target_include_directories(quickdial_background PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(quickdial_background PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(quickdial_background PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(quickdial_background PUBLIC ole32 shell32 user32 gdi32 windowscodecs)

add_library(quickdial_discovery STATIC src/DiscoveryProcess.cpp src/DiscoveryProtocol.cpp)
target_compile_options(quickdial_discovery PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(quickdial_discovery PUBLIC quickdial_core ole32)

add_executable(ApplicationQuickDial WIN32
  src/HookManager.cpp
  src/InstalledAppsWatcher.cpp
  src/LauncherApp.cpp
  src/LauncherVisualStyle.cpp
  src/StartupManager.cpp
  src/SingleInstance.cpp
  src/main.cpp
  resources.rc
)

target_compile_definitions(ApplicationQuickDial PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ApplicationQuickDial PRIVATE /W4 /permissive- /EHsc)
target_link_options(ApplicationQuickDial PRIVATE /MANIFEST:NO)
target_link_libraries(ApplicationQuickDial PRIVATE
  quickdial_core
  quickdial_background
  quickdial_discovery
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

  add_executable(quickdial_background_tests tests/BackgroundTests.cpp)
  target_compile_options(quickdial_background_tests PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_background_tests PRIVATE quickdial_background)
  add_test(NAME quickdial_background_tests COMMAND quickdial_background_tests)

  add_executable(quickdial_discovery_tests tests/DiscoveryTests.cpp)
  target_compile_options(quickdial_discovery_tests PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_discovery_tests PRIVATE quickdial_discovery)
  add_dependencies(quickdial_discovery_tests ApplicationQuickDial)
  add_test(NAME quickdial_discovery_tests COMMAND quickdial_discovery_tests)
  set_tests_properties(quickdial_discovery_tests PROPERTIES TIMEOUT 60)

  # Requires an interactive Windows desktop; run explicitly, outside CTest.
  add_executable(quickdial_launcher_tests
    tests/LauncherTests.cpp
    src/HookManager.cpp
    src/InstalledAppsWatcher.cpp
    src/LauncherApp.cpp
    src/LauncherVisualStyle.cpp
    src/StartupManager.cpp
  )
  target_compile_options(quickdial_launcher_tests PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_launcher_tests PRIVATE
    quickdial_core quickdial_background quickdial_discovery advapi32 comctl32 d2d1 dwrite dwmapi shcore)

  add_executable(quickdial_benchmarks
    benchmarks/PerformanceBenchmarks.cpp
  )
  target_compile_definitions(quickdial_benchmarks PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(quickdial_benchmarks PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_benchmarks PRIVATE quickdial_core psapi user32)

  # Uses private Win32 desktops and fixture processes; run on the normal desktop.
  add_executable(quickdial_single_instance_tests tests/SingleInstanceTests.cpp src/SingleInstance.cpp)
  target_include_directories(quickdial_single_instance_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(quickdial_single_instance_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(quickdial_single_instance_tests PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(quickdial_single_instance_tests PRIVATE user32)
endif()
