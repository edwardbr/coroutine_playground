cmake_minimum_required(VERSION 3.18)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/submodule/cppcoro/cmake")
find_package(Coroutines COMPONENTS Experimental Final REQUIRED)

add_executable(double_executor double_executor.cpp)
set_target_properties(double_executor PROPERTIES CXX_STANDARD 20)
target_compile_options(double_executor PUBLIC   /EHsc)
target_link_libraries(double_executor PUBLIC cppcoro WS2_32 synchronization)
target_compile_definitions(double_executor PUBLIC _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING=1)
target_link_libraries(double_executor PUBLIC std::coroutines Ws2_32 Mswsock Synchronization)