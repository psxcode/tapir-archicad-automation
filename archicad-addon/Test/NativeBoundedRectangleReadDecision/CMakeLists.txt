cmake_minimum_required (VERSION 3.17)
project (NativeBoundedRectangleReadDecisionTests LANGUAGES CXX)

add_executable (native-bounded-rectangle-read-decision-tests main.cpp)
target_compile_features (native-bounded-rectangle-read-decision-tests PRIVATE cxx_std_17)
target_include_directories (native-bounded-rectangle-read-decision-tests PRIVATE
	"${CMAKE_CURRENT_LIST_DIR}/../../Sources"
)
if (MSVC)
	target_compile_options (native-bounded-rectangle-read-decision-tests PRIVATE /W4 /WX /EHsc)
endif ()
