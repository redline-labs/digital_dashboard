add_subdirectory(widgets)

# Enable Qt's automatic MOC, UIC, and RCC processing
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()


include_directories(
    include
)

qt_add_executable(${PROJECT_NAME}
    main.cpp
    app_config.cpp
    main_window.cpp
    command_line_args.cpp
    resources.qrc
)

#set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    
    # Link common logging interface (wraps spdlog)
    common_logging
    helpers
    
    # Link nlohmann/json (now fetched via FetchContent)
    nlohmann_json::nlohmann_json
    
    # Link cxxopts (now fetched via FetchContent)
    cxxopts::cxxopts
    
    # Link yaml-cpp (now fetched via FetchContent)
    yaml-cpp::yaml-cpp

    zenohcxx::zenohc

    # Link the carplay widget library
    carplay_widget
    mercedes_190e_telltales
    mercedes_190e_speedometer
    mercedes_190e_tachometer
    sparkline
    mercedes_190e_cluster_gauge
    motec_c125_tachometer
    motec_cdl3_tachometer
    static_text
)

# Add comprehensive test data publisher executable
add_executable(test_data_publisher
    test_data_publisher.cpp
)

target_link_libraries(test_data_publisher PRIVATE
    # Link common logging interface (wraps spdlog)
    common_logging
    
    # Link zenoh-cpp
    zenohcxx::zenohc
    
    # Link our schemas library
    schemas
    helpers
)
