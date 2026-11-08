# TEST(name) { CHECK(...); CHECK_EQ(a, b); } and a runner, for test programs
# with many small cases. Header-only. A program defines no main(): link
# mctest_main, which runs every TEST (or those whose name contains argv[1]) and
# exits non-zero on any failure, or PROJECT_TEST_SKIP_CODE when every test that
# ran skipped.
add_library(mctest INTERFACE)
target_include_directories(mctest INTERFACE include)
target_sources(mctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(mctest INTERFACE spdlog::spdlog)
