cmake_minimum_required (VERSION 3.17)

include (Tools/CMakeCommon.cmake)

option (TAPIR_ENABLE_PERF_TRACE
    "Compile the diagnostic PerfTrace instrumentation into the Tapir add-on."
    OFF
)

set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)
set (AC_API_DEVKIT_DIR ${AC_API_DEVKIT_DIR} CACHE PATH "API DevKit directory.")
set (AC_VERSION ${API_AC_VERSION} CACHE STRING "Archicad version.")
set (TAPIR_BUILD_ID "unconfigured" CACHE STRING "R&D build identity exposed by GetNativeBuildInfo.")

# Keep the value safe for a C/C++ string literal. The build scripts pass a
# source identity; arbitrary shell text must never become a compiler flag.
string (REGEX REPLACE "[^A-Za-z0-9_.-]" "_" TAPIR_BUILD_ID_SAFE "${TAPIR_BUILD_ID}")

SetGlobalCompilerDefinitions (${AC_VERSION})

if (WIN32)
	set (NamePostFix Win)
else ()
	set (NamePostFix Mac)
endif ()
set (AddOnName "TapirAddOn_AC${AC_VERSION}_${NamePostFix}")

project (${AddOnName})

set (AddOnSourcesFolder Sources)
set (AddOnResourcesFolder Sources)
GenerateAddOnProject (AddOn ${AC_VERSION} ${AC_API_DEVKIT_DIR} ${AddOnName} ${AddOnSourcesFolder} ${AddOnResourcesFolder} INT)

# PerfTrace is deliberately compile-time optional.  Normal/customer builds
# keep the call sites (as no-op RAII helpers) but contain no clocks, file I/O,
# maps, or trace strings.  Diagnostic builds opt in with
# -DTAPIR_ENABLE_PERF_TRACE=ON.
if (TAPIR_ENABLE_PERF_TRACE)
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_PERF_TRACE=1)
else ()
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_PERF_TRACE=0)
endif ()
target_compile_definitions (AddOn PRIVATE TAPIR_BUILD_ID="${TAPIR_BUILD_ID_SAFE}")
