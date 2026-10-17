cmake_minimum_required (VERSION 3.17)
project (NativeDimensionLayerControlTests LANGUAGES CXX)

add_executable (native-dimension-layer-control-tests main.cpp)
target_compile_features (native-dimension-layer-control-tests PRIVATE cxx_std_17)
target_include_directories (native-dimension-layer-control-tests PRIVATE
	"${CMAKE_CURRENT_LIST_DIR}/../../Sources"
)
if (MSVC)
	target_compile_options (native-dimension-layer-control-tests PRIVATE /W4 /WX /EHsc)
endif ()
