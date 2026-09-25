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

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE
    "Compile the default-off disposable channel read-only probe into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT
    "Compile the default-off disposable permit verifier contract into an isolated test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN
    "Compile the default-off isolated disposable Column executor contract into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE
    "Compile the default-off isolated disposable Line candidate marker into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC
    "Compile the default-off isolated disposable Arc candidate marker into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH
    "Compile the default-off isolated disposable Hatch candidate/memo marker into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE
    "Compile the default-off isolated disposable Zone candidate/readback marker into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR
    "Compile the default-off isolated disposable Door candidate/readback/receipt marker into a test-only APX."
    OFF
)

option (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW
    "Compile the default-off isolated disposable Window candidate/readback/receipt marker into a test-only APX."
    OFF
)

if ((TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT AND TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    OR (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW AND TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR))
    message (FATAL_ERROR "Disposable channel, permit, Column, Line, Arc, Hatch, Zone, Door, Window and fault profiles must use separate isolated APX builds.")
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
if (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT)
    # This target name is intentionally distinct from the runtime APX.  The
    # disposable fault profile must never look deployable by filename alone.
    set (AddOnName "${AddOnName}_NativeCrudTestFault")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
    # The channel probe is a separate compile-only artifact.  It must never
    # overwrite an ordinary or native-CRUD-test APX.
    set (AddOnName "${AddOnName}_NativeDisposableChannelProbe")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    # The permit bridge is a separate compile-only artifact.  It contains no
    # writer or ordinary CRUD registration and must never overwrite an
    # ordinary or zero-write-probe APX.
    set (AddOnName "${AddOnName}_NativeDisposablePermit")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    # The Column executor is a separate compile-only artifact.  It is not an
    # ordinary APX and cannot overwrite the permit, channel or fault profile.
    set (AddOnName "${AddOnName}_NativeDisposableColumn")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    # The Line candidate is a separate unavailable-marker artifact.  It has no
    # writer or broker route and cannot overwrite an ordinary or Column APX.
    set (AddOnName "${AddOnName}_NativeDisposableLine")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    # The Arc candidate is a separate compile-only unavailable marker.  Its
    # APX identity must not collide with ordinary or any other disposable
    # profile.
    set (AddOnName "${AddOnName}_NativeDisposableArc")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    # The Hatch candidate/memo packet is a separate compile-only marker.  Its
    # identity must not collide with ordinary or any other disposable profile.
    set (AddOnName "${AddOnName}_NativeDisposableHatch")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    # The Zone candidate/readback packet is a separate compile-only marker.
    # Its identity must not collide with ordinary or another disposable APX.
    set (AddOnName "${AddOnName}_NativeDisposableZone")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
    # The Door packet is a separate compile-only marker.  Its APX identity
    # must not collide with ordinary or any existing disposable profile.
    set (AddOnName "${AddOnName}_NativeDisposableDoor")
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW)
    # The Window packet is a separate compile-only marker.  Its APX identity
    # must not collide with ordinary or any existing disposable profile.
    set (AddOnName "${AddOnName}_NativeDisposableWindow")
endif ()

project (${AddOnName})

set (AddOnSourcesFolder Sources)
set (AddOnResourcesFolder Sources)
GenerateAddOnProject (AddOn ${AC_VERSION} ${AC_API_DEVKIT_DIR} ${AddOnName} ${AddOnSourcesFolder} ${AddOnResourcesFolder} INT)

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE OR TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT OR TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    # The real interprocess protocol core is shared only by the explicitly
    # isolated channel APX and the separate broker target.  Ordinary APX
    # profiles do not receive this source or its bcrypt linkage.
    set (DisposableChannelBrokerSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_sources (AddOn PRIVATE "${DisposableChannelBrokerSourceDir}/channel.cpp")
    if (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT OR TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
        target_sources (AddOn PRIVATE "${DisposableChannelBrokerSourceDir}/permit.cpp")
    endif ()
    if (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
        target_sources (AddOn PRIVATE "${DisposableChannelBrokerSourceDir}/column.cpp")
    endif ()
    target_include_directories (AddOn PRIVATE "${DisposableChannelBrokerSourceDir}")
    if (WIN32)
        target_compile_definitions (AddOn PRIVATE NOMINMAX)
        target_link_libraries (AddOn bcrypt)
    endif ()
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    # The marker includes only the typed contract declarations.  Do not link
    # channel/permit/broker sources into this unavailable profile.
    set (DisposableLineContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableLineContractSourceDir}")
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    # The marker includes only the typed offline contract declarations.  No
    # channel, permit, broker route or ordinary native writer is linked.
    set (DisposableArcContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableArcContractSourceDir}")
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    # The marker includes only the typed Hatch contract declarations.  No
    # channel, permit or broker source is linked into this unavailable profile.
    set (DisposableHatchContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableHatchContractSourceDir}")
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    # The marker includes only the typed offline Zone contract declarations.
    # No channel, permit, broker source or live writer is linked.
    set (DisposableZoneContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableZoneContractSourceDir}")
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
    # The Door marker includes only the broker-free typed contract.  No
    # channel, permit, broker source or live writer is linked.
    set (DisposableDoorContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableDoorContractSourceDir}")
endif ()

if (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW)
    # The Window marker includes only the broker-free typed contract.  No
    # channel, permit, broker source or live writer is linked.
    set (DisposableWindowContractSourceDir "${CMAKE_CURRENT_LIST_DIR}/../../disposable-channel-broker")
    target_include_directories (AddOn PRIVATE "${DisposableWindowContractSourceDir}")
endif ()

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
if (TAPIR_ENABLE_NATIVE_CRUD_TEST_FAULT)
    # The ordinary profile deliberately does not define this macro.  The
    # source seam is therefore compiled out unless the explicit disposable
    # test profile selected this option in its isolated build directory.
    target_compile_definitions (AddOn PRIVATE TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
    # Ordinary/customer builds deliberately omit this macro and therefore do
    # not register the probe command in AddOnMain.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
    # Ordinary/customer builds deliberately omit this macro and therefore do
    # not register the compile-only permit contract command.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
    # Ordinary/customer builds deliberately omit this macro.  The APX unit
    # remains an explicit unavailable marker until a native context is proven.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains a non-live unavailable marker until a separate safety gate exists.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains an unavailable marker until a separate live safety gate exists.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains an unavailable marker until a separate live safety gate exists.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains an unavailable marker without native context or authority.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains an exact unavailable marker without native context or authority.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
endif ()
if (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW)
    # Ordinary/customer builds deliberately omit this macro.  The APX command
    # remains an exact unavailable marker without native context or authority.
    target_compile_definitions (AddOn PRIVATE TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW)
endif ()
