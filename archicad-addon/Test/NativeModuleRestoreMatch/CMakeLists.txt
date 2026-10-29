cmake_minimum_required (VERSION 3.17)
project (NativeModuleRestoreMatchTests LANGUAGES CXX)

# Std-only: the matcher needs no DevKit.
add_executable (native-module-restore-match main.cpp)
target_compile_features (native-module-restore-match PRIVATE cxx_std_17)
target_include_directories (native-module-restore-match PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Sources")
if (MSVC)
    target_compile_options (native-module-restore-match PRIVATE /W4 /WX /EHsc -D_CRT_SECURE_NO_WARNINGS)
endif ()
