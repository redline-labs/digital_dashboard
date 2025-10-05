# Schemas CMakeLists.txt

set(SCHEMA_FILES
    vehicle_speed.capnp
    vehicle_odometer.capnp
    engine_rpm.capnp
    engine_temperature.capnp
    vehicle_warnings.capnp

    racegrade_tc8_configure.capnp
    racegrade_tc8_signals.capnp
    msel_master_relay.capnp
    megasquirt.capnp
    motec_ltc.capnp
    can_frame.capnp
)

set(GENERATED_SOURCES)
set(GENERATED_HEADERS)
set(ALL_SCHEMA_STRUCTS)
set(ALL_HEADER_FILES)

# Function to extract struct names from a .capnp file
function(extract_structs_from_schema SCHEMA_FILE OUTPUT_VAR)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${SCHEMA_FILE}" SCHEMA_CONTENT)
    
    # Extract struct declarations using regex
    # Matches: struct StructName @0x... {
    string(REGEX MATCHALL "struct[ \t]+([a-zA-Z_][a-zA-Z0-9_]*)[ \t]" STRUCT_MATCHES "${SCHEMA_CONTENT}")
    
    set(STRUCT_NAMES)
    foreach(MATCH ${STRUCT_MATCHES})
        # Extract just the struct name from the match
        string(REGEX REPLACE "struct[ \t]+([a-zA-Z_][a-zA-Z0-9_]*).*" "\\1" STRUCT_NAME "${MATCH}")
        list(APPEND STRUCT_NAMES "${STRUCT_NAME}")
    endforeach()
    
    set(${OUTPUT_VAR} ${STRUCT_NAMES} PARENT_SCOPE)
endfunction()


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
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SCHEMA_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating C++ code from ${SCHEMA_FILE}"
        VERBATIM
    )

    # Also manually extract any structs from the schema file.
    extract_structs_from_schema(${SCHEMA_FILE} STRUCTS_IN_FILE)
    list(APPEND ALL_SCHEMA_STRUCTS ${STRUCTS_IN_FILE})
    message(STATUS "Found structs in ${SCHEMA_FILE}: ${STRUCTS_IN_FILE}")

    # Also generate the expected header file name.
    list(APPEND ALL_HEADER_FILES "${SCHEMA_FILE}.h")
endforeach()

# Create a library for all schemas
add_library(schemas STATIC ${GENERATED_SOURCES})
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES GENERATED TRUE)

# Include directories
target_include_directories(schemas PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    #${capnproto_SOURCE_DIR}/c++/src
)

# Link with capnproto
target_link_libraries(schemas PRIVATE
    capnp
    kj
)

# Store the schema structs as a target property to be picked up by the main project.
set_target_properties(schemas PROPERTIES
    SCHEMA_STRUCTS "${ALL_SCHEMA_STRUCTS}"
    SCHEMA_HEADER_FILES "${ALL_HEADER_FILES}"
)