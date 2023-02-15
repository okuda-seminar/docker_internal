cmake_minimum_required(VERSION 3.13)
project(unittest)
find_package(GTest)

add_executable(nsexec_test nsexec_test.cpp)
target_link_libraries(nsexec_test ${GTEST_BOTH_LIBRARIES} pthread)
target_include_directories(nsexec_test PUBLIC ${GTEST_INCLUDE_DIRS}, ../src)