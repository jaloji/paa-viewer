cmake_minimum_required(VERSION 3.20)
project(paa-viewer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "paa-viewer builds Windows components (WIC codec, XnView plugin) and needs Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLLs are loaded into Explorer and XnView, so they must not need a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

# All binaries land in one folder next to each other.
foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG)
        set(CMAKE_${kind}_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endforeach()

# PAA decoder shared by every component.
add_library(paa_core STATIC src/core/paa.cpp src/core/paa.h)
target_include_directories(paa_core PUBLIC src/core)

# Windows Imaging Component codec: Explorer thumbnails, preview pane, photo viewers.
add_library(PaaWic SHARED src/wic/paawic.cpp src/wic/paawic.def)
target_link_libraries(PaaWic PRIVATE paa_core windowscodecs ole32 advapi32 shell32)

# XnView MP read plugin. XnView only loads files named X*.usr from its Plugins folder.
add_library(Xpaa SHARED src/xnview/xpaa.cpp src/xnview/xpaa.def)
target_link_libraries(Xpaa PRIVATE paa_core)
set_target_properties(Xpaa PROPERTIES PREFIX "" SUFFIX ".usr")

# Command-line tools.
add_executable(paa2png tools/paa2png.cpp)
target_link_libraries(paa2png PRIVATE paa_core)

add_executable(wictest tools/wictest.cpp)
target_link_libraries(wictest PRIVATE windowscodecs ole32)

# Release package: binaries plus install scripts and docs (cmake --install build --prefix dist).
install(TARGETS PaaWic Xpaa paa2png wictest RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES scripts/install.bat scripts/uninstall.bat scripts/install-xnview.bat
              scripts/uninstall-xnview.bat scripts/clear-thumbnails.bat README.md LICENSE
        DESTINATION .)
