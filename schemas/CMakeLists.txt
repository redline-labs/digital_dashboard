# Schemas CMakeLists.txt

set(SCHEMA_FILES
    vehicle_speed.capnp
    engine_rpm.capnp
    engine_temperature.capnp
    vehicle_warnings.capnp
)

set(GENERATED_SOURCES)
set(GENERATED_HEADERS)

foreach(SCHEMA_FILE ${SCHEMA_FILES})
    get_filename_component(SCHEMA_NAME ${SCHEMA_FILE} NAME_WE)
    
    set(GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/${SCHEMA_NAME}.capnp.h")
    set(GENERATED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/${SCHEMA_NAME}.capnp.c++")
    
    list(APPEND GENERATED_SOURCES ${GENERATED_SOURCE})
    list(APPEND GENERATED_HEADERS ${GENERATED_HEADER})
    
    # Generate header and source using capnp compile with correct plugin path
    add_custom_command(
        OUTPUT ${GENERATED_HEADER} ${GENERATED_SOURCE}
        COMMAND ${CMAKE_COMMAND} -E env 
                "PATH=$<TARGET_FILE_DIR:capnpc_cpp>:$ENV{PATH}"
                $<TARGET_FILE:capnp_tool>
                compile
                -oc++:${CMAKE_CURRENT_BINARY_DIR}
                --src-prefix=${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/${SCHEMA_FILE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SCHEMA_FILE} capnp_tool capnpc_cpp
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating C++ code from ${SCHEMA_FILE}"
        VERBATIM
    )
endforeach()

# Create a library for all schemas
add_library(schemas STATIC
    ${GENERATED_SOURCES}
)

# Include directories
target_include_directories(schemas PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${capnproto_SOURCE_DIR}/c++/src
)

# Link with capnproto
target_link_libraries(schemas PUBLIC
    capnp
    kj
) 