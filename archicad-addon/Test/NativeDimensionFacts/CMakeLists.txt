cmake_minimum_required (VERSION 3.17)
project (NativeDimensionFactsTests LANGUAGES CXX)

if (NOT DEFINED AC_API_DEVKIT_DIR)
    message (FATAL_ERROR "AC_API_DEVKIT_DIR is required.")
endif ()

add_executable (native-dimension-facts-tests main.cpp)
target_compile_features (native-dimension-facts-tests PRIVATE cxx_std_17)
target_compile_definitions (native-dimension-facts-tests PRIVATE UNICODE _UNICODE _ITERATOR_DEBUG_LEVEL=0)
target_include_directories (native-dimension-facts-tests PRIVATE
    "${AC_API_DEVKIT_DIR}/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../../Sources"
)
file (GLOB ModuleFolders "${AC_API_DEVKIT_DIR}/Modules/*")
target_include_directories (native-dimension-facts-tests SYSTEM PRIVATE ${ModuleFolders})
find_library (GSRootLibrary NAMES GSRootImp PATHS "${AC_API_DEVKIT_DIR}/Modules/GSRoot/Win" "${AC_API_DEVKIT_DIR}/Modules/GSRoot" NO_DEFAULT_PATH REQUIRED)
find_library (JSONConversionLibrary NAMES JSONConversionImp PATHS "${AC_API_DEVKIT_DIR}/Modules/JSONConversion/Win" "${AC_API_DEVKIT_DIR}/Modules/JSONConversion" NO_DEFAULT_PATH REQUIRED)
target_link_libraries (native-dimension-facts-tests PRIVATE "${GSRootLibrary}" "${JSONConversionLibrary}")
set_property (TARGET native-dimension-facts-tests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_compile_options (native-dimension-facts-tests PRIVATE /W4 /WX /FS /Zc:wchar_t- /wd4499 /EHsc -D_CRT_SECURE_NO_WARNINGS)
