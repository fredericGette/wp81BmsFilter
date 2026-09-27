cmake_minimum_required(VERSION 3.20)

# Use the name of the current source directory as the project/target name
get_filename_component(APP_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

project(${APP_NAME} C)

# Kernel-mode KMDF 1.11 driver for Windows Phone 8.1 (ARM), built with LLVM
# (clang-cl + lld-link, see CMakePresets.json).
#   - KMDF 1.11 headers/libs and ARM kernel import libs: Windows Kits 8.1 (WDK)
#   - wdm.h and the shared headers it includes: Windows Phone Kits 8.1 (um)
set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/8.1" CACHE PATH "Windows Kits 8.1 root")
set(PHONE_KIT_ROOT "C:/Program Files (x86)/Windows Phone Kits/8.1" CACHE PATH "Windows Phone Kits 8.1 root")
set(KMDF_MINOR 11)

# Drop CMake's user-mode defaults: /RTC1 needs a runtime, kernel32/user32 do not
# exist for kernel code, and a driver is not a console app with a manifest.
set(CMAKE_C_FLAGS_DEBUG "/Od")
set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CREATE_CONSOLE_EXE "")

file(GLOB SOURCES CONFIGURE_DEPENDS "src/*.c")
add_executable(${APP_NAME} ${SOURCES})

set_target_properties(${APP_NAME} PROPERTIES
    SUFFIX ".sys"
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_compile_definitions(${APP_NAME} PRIVATE
    _ARM_=1
    _KERNEL_MODE=1
    WINNT=1
    NTDDI_VERSION=0x06030000
    _WIN32_WINNT=0x0603
    WINVER=0x0603
    KMDF_VERSION_MAJOR=1
    KMDF_VERSION_MINOR=${KMDF_MINOR}
    DEPRECATE_DDK_FUNCTIONS=1
    WDF_DEVICE_NO_WDMSEC_H   # wdmsec.h is not in the installed kits; no SDDL strings are used
)

# SYSTEM -> passed as /imsvc, so warnings inside the kit headers are not reported.
# um\minwin is only needed for apiset.h; it comes after um so um's ntdef.h wins.
target_include_directories(${APP_NAME} SYSTEM PRIVATE
    "${WDK_ROOT}/Include/wdf/kmdf/1.${KMDF_MINOR}"
    "${WDK_ROOT}/Include/km/crt"
    "${PHONE_KIT_ROOT}/Include/um"
    "${PHONE_KIT_ROOT}/Include/um/minwin"
)

target_compile_options(${APP_NAME} PRIVATE
    /W4
    /Zl                 # no default CRT library references
    /GS-                # no stack cookies (FxDriverEntry does not initialise them)
    /Gy
    /Z7
    /clang:-mno-implicit-float   # no compiler-generated VFP/NEON use in kernel code
)

target_link_directories(${APP_NAME} PRIVATE
    "${WDK_ROOT}/Lib/winv6.3/km/arm"
    "${WDK_ROOT}/Lib/wdf/kmdf/arm/1.${KMDF_MINOR}"
)

target_link_options(${APP_NAME} PRIVATE
    /DRIVER
    /SUBSYSTEM:NATIVE,6.03
    /ENTRY:FxDriverEntry
    /NODEFAULTLIB
    /MACHINE:ARM
    /DEBUG
    /OPT:REF
    /OPT:ICF
    /INCREMENTAL:NO
    /MANIFEST:NO
    /TSAWARE:NO
)

target_link_libraries(${APP_NAME} PRIVATE
    ntoskrnl.lib
    hal.lib
    BufferOverflowFastFailK.lib
    wdfldr.lib
    wdfdriverentry.lib
)