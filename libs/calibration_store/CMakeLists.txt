cmake_minimum_required(VERSION 3.24)

project(calibration_store)

# What the state estimator learned about the installation, kept between
# sessions as an append-only SQLite history. Generic rows -- a group, a prior
# hash, numbers -- so it links nothing of the estimator; see
# docs/libs/calibration_store.md.
#
# SQLite through the amalgamation in third_party/sqlite3.cmake, kept out of the
# GUI apps for the reason libs/track_store gives.
add_library(calibration_store STATIC
    src/store.cpp
)

target_include_directories(calibration_store PUBLIC include)

target_link_libraries(calibration_store
    PRIVATE
        sqlite3::sqlite3
        nlohmann_json::nlohmann_json
)

function(calibration_store_test name)
    add_executable(calibration_store_test_${name} tests/test_${name}.cpp)
    target_include_directories(calibration_store_test_${name} PRIVATE tests)
    target_link_libraries(calibration_store_test_${name} PRIVATE calibration_store sqlite3::sqlite3 spdlog::spdlog)
    add_project_test(TARGET calibration_store_test_${name} LABELS calibration_store unit)
endfunction()

# Round trips, the newest matching row, reopening, history.
calibration_store_test(store)

# Rows and files that are not what they should be: skipped or refused, and a
# file that is not ours left byte for byte as it was.
calibration_store_test(malformed)
