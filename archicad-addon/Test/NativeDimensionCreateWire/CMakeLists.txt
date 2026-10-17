cmake_minimum_required (VERSION 3.17)
project (NativeDimensionCreateWireTests LANGUAGES CXX)

if (NOT DEFINED AC_API_DEVKIT_DIR)
    message (FATAL_ERROR "AC_API_DEVKIT_DIR is required.")
endif ()
get_filename_component (AC_API_DEVKIT_DIR "${AC_API_DEVKIT_DIR}" ABSOLUTE)

add_executable (native-dimension-create-wire-tests main.cpp)
target_compile_features (native-dimension-create-wire-tests PRIVATE cxx_std_17)
target_compile_definitions (native-dimension-create-wire-tests PRIVATE UNICODE _UNICODE _ITERATOR_DEBUG_LEVEL=0)
option (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE "Compile the producer policy test under the candidate profile." OFF)
option (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT "Compile the producer policy test under the post-create-fault profile." OFF)
if (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT AND NOT TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE)
    message (FATAL_ERROR "The post-create fault policy test requires the candidate profile.")
endif ()
target_compile_definitions (native-dimension-create-wire-tests PRIVATE
    TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE=$<BOOL:${TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE}>
    TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT=$<BOOL:${TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT}>
)
target_include_directories (native-dimension-create-wire-tests PRIVATE
    "${AC_API_DEVKIT_DIR}/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../../Sources"
)
file (GLOB ModuleFolders "${AC_API_DEVKIT_DIR}/Modules/*")
target_include_directories (native-dimension-create-wire-tests SYSTEM PRIVATE ${ModuleFolders})
target_include_directories (native-dimension-create-wire-tests SYSTEM PRIVATE "${AC_API_DEVKIT_DIR}/Modules/GSRoot")
get_filename_component (NativeDimensionRepositoryRoot "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
if (AC_VERSION STREQUAL "25")
    set (NativeDimensionCandidateFixture "${NativeDimensionRepositoryRoot}/src/fixtures/native-dimensions/ac25-t4a2-static-kernel-native-captured-partial.json")
elseif (AC_VERSION STREQUAL "28")
    set (NativeDimensionCandidateFixture "${NativeDimensionRepositoryRoot}/src/fixtures/native-dimensions/ac28-t4a2-static-kernel-native-captured-partial.json")
else ()
    message (FATAL_ERROR "Native dimension candidate facts require a captured AC25 or AC28 fixture.")
endif ()
configure_file ("${NativeDimensionCandidateFixture}"
    "${CMAKE_CURRENT_BINARY_DIR}/captured-dimension-reader-row.json" COPYONLY)
target_compile_definitions (native-dimension-create-wire-tests PRIVATE
    NATIVE_DIMENSION_CANDIDATE_FIXTURE_PATH="${CMAKE_CURRENT_BINARY_DIR}/captured-dimension-reader-row.json")
find_library (GSRootLibrary NAMES GSRootImp PATHS "${AC_API_DEVKIT_DIR}/Modules/GSRoot/Win" "${AC_API_DEVKIT_DIR}/Modules/GSRoot" NO_DEFAULT_PATH REQUIRED)
find_library (JSONConversionLibrary NAMES JSONConversionImp PATHS "${AC_API_DEVKIT_DIR}/Modules/JSONConversion/Win" "${AC_API_DEVKIT_DIR}/Modules/JSONConversion" NO_DEFAULT_PATH REQUIRED)
target_link_libraries (native-dimension-create-wire-tests PRIVATE "${GSRootLibrary}" "${JSONConversionLibrary}")
set_property (TARGET native-dimension-create-wire-tests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_compile_options (native-dimension-create-wire-tests PRIVATE /W4 /WX /FS /Zc:wchar_t- /wd4499 /EHsc -D_CRT_SECURE_NO_WARNINGS)
