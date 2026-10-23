cmake_minimum_required (VERSION 3.17)

include (Tools/CMakeCommon.cmake)

option (TAPIR_ENABLE_PERF_TRACE
    "Compile the diagnostic PerfTrace instrumentation into the Tapir add-on."
    OFF
)

option (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT
    "Compile the disposable native CRUD fault-after-first-applied seam into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    "Enable explicitly guarded developer-only candidate Native CRUD writers for disposable qualification."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
    "Compile the closed, private Dimension CREATE candidate producer into its isolated test APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT
    "Compile the isolated Dimension producer post-CREATE fault seam; requires its candidate profile."
    OFF
)

if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT AND NOT TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE)
    message (FATAL_ERROR "The native Dimension post-CREATE fault profile requires TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE.")
endif ()

if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE AND (TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    OR TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT))
    message (FATAL_ERROR "The native Dimension producer must use a separate isolated APX profile from every other candidate and fault profile.")
endif ()

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
if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE AND TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT)
    set (AddOnName "${AddOnName}_NativeDimensionCreatePostCreateFault")
elseif (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE)
    set (AddOnName "${AddOnName}_NativeDimensionCreateCandidate")
elseif (TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS)
    # Candidate writers retain one shared developer-only identity, including
    # when combined with the first-applied rollback fault seam.
    set (AddOnName "${AddOnName}_NativeCandidateWriters")
elseif (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT)
    # This target name is intentionally distinct from the runtime APX.  The
    # disposable fault profile must never look deployable by filename alone.
    set (AddOnName "${AddOnName}_NativeCrudTestFault")
endif ()

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
target_compile_definitions (AddOn PRIVATE TAPIR_AC_VERSION=${AC_VERSION})
if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE)
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE=1)
else ()
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE=0)
endif ()
if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT)
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT=1)
else ()
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT=0)
endif ()
if (TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS)
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS=1)
else ()
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS=0)
endif ()
if (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT)
    # The ordinary profile deliberately does not define this macro.  The
    # source seam is therefore compiled out unless the explicit disposable
    # test profile selected this option in its isolated build directory.
    target_compile_definitions (AddOn PRIVATE TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
endif ()
