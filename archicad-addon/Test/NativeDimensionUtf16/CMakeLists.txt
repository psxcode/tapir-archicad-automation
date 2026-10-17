cmake_minimum_required (VERSION 3.17)
project (NativeDimensionUtf16Tests LANGUAGES CXX)

if (NOT DEFINED AC_API_DEVKIT_DIR OR NOT DEFINED AC_VERSION)
    message (FATAL_ERROR "AC_API_DEVKIT_DIR and AC_VERSION are required to qualify both DevKit build environments.")
endif ()

add_executable (native-dimension-utf16-tests main.cpp)
target_compile_features (native-dimension-utf16-tests PRIVATE cxx_std_17)
target_compile_definitions (native-dimension-utf16-tests PRIVATE AC_VERSION=${AC_VERSION})
target_include_directories (native-dimension-utf16-tests PRIVATE
    "${AC_API_DEVKIT_DIR}/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../../Sources"
)
set_property (TARGET native-dimension-utf16-tests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_compile_options (native-dimension-utf16-tests PRIVATE /W4 /WX /FS /EHsc -D_CRT_SECURE_NO_WARNINGS)
