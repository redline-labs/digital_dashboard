# Offline Map Widget
cmake_minimum_required(VERSION 3.16)

set(MAP_WIDGET_LIB map_widget)

# GuiPrivate is what exposes <rhi/qrhi.h>. QRhi is semi-public in Qt 6 -- a
# stable-ish API shipped behind the private headers -- and it is the only way to
# reach the GPU without binding to a platform surface. See gpu_renderer.h for
# why binding to one is not an option here.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets GuiPrivate ShaderTools)

# ---------------------------------------------------------------- shaders
#
# qsb bakes the GLSL into a QShader package holding every backend's form (MSL
# for Metal, SPIR-V for Vulkan, GLSL for GL). EmbedBinary then turns that into a
# header of bytes.
#
# The obvious alternative is Qt's resource system, but a .qrc inside a STATIC
# library has to survive the link into two different executables, and a resource
# that quietly fails to register presents as a shader that will not load at
# RUNTIME rather than as a build error. A header cannot be dropped by a linker.
set(MAP_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${MAP_SHADER_DIR})

function(map_bake_shader source symbol out_var)
    get_filename_component(name ${source} NAME)
    set(qsb_out ${MAP_SHADER_DIR}/${name}.qsb)
    set(hdr_out ${MAP_SHADER_DIR}/${symbol}_qsb.h)

    add_custom_command(
        OUTPUT ${hdr_out}
        # --glsl 330 is what an OpenGL fallback needs; --msl and --spirv cover
        # Metal and Vulkan. Baking all three keeps one artefact portable across
        # the backends gpu_renderer.cpp tries.
        COMMAND Qt6::qsb --glsl 330 --msl 12 -o ${qsb_out} ${source}
        COMMAND ${CMAKE_COMMAND} -DIN=${qsb_out} -DOUT=${hdr_out} -DSYMBOL=${symbol}
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${source} ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Baking shader ${name}"
        VERBATIM
    )
    set(${out_var} ${hdr_out} PARENT_SCOPE)
endfunction()

map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/map.vert mapVert MAP_VERT_HEADER)
map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/map.frag mapFrag MAP_FRAG_HEADER)
map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/map_highlight.vert mapHighlightVert
                MAP_HIGHLIGHT_VERT_HEADER)
map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/map_highlight.frag mapHighlightFrag
                MAP_HIGHLIGHT_FRAG_HEADER)

add_custom_target(map_widget_shaders
    DEPENDS ${MAP_VERT_HEADER} ${MAP_FRAG_HEADER} ${MAP_HIGHLIGHT_VERT_HEADER}
            ${MAP_HIGHLIGHT_FRAG_HEADER})

# ----------------------------------------------------------------- library

add_library(${MAP_WIDGET_LIB} STATIC
    gpu_renderer.cpp
    labels.cpp
    map_widget.cpp
    projection.cpp
    recentre_button.cpp
    tessellator.cpp
    tile_cache.cpp
    tile_workers.cpp
    tile_source.cpp
    ${MAP_VERT_HEADER}
    ${MAP_FRAG_HEADER}
    ${MAP_HIGHLIGHT_VERT_HEADER}
    ${MAP_HIGHLIGHT_FRAG_HEADER}
    include/map/config.h
    include/map/gpu_renderer.h
    include/map/labels.h
    include/map/map_widget.h
    include/map/projection.h
    include/map/recentre_button.h
    include/map/style.h
    include/map/tessellator.h
    include/map/tile_cache.h
    include/map/tile_workers.h
    include/map/tile_source.h
)

add_dependencies(${MAP_WIDGET_LIB} map_widget_shaders)

target_include_directories(${MAP_WIDGET_LIB} PUBLIC
    ${CMAKE_SOURCE_DIR}/dashboard/include
    include
    ${CMAKE_SOURCE_DIR}/include
)
target_include_directories(${MAP_WIDGET_LIB} PRIVATE ${MAP_SHADER_DIR})

target_link_libraries(${MAP_WIDGET_LIB}
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        helpers
        qt_helpers
        reflection
        config_codec
        schemas
        zenoh_pub_sub
        # The vector tile decoder. Everything this widget draws arrives as MVT
        # over zenoh; nothing about the format is known here.
        mvt
    PRIVATE
        Qt6::GuiPrivate
        # Polygon triangulation with holes. See third_party/earcut.cmake.
        earcut::earcut
        spdlog::spdlog
)

# Registered by hand rather than through add_dashboard_widget(), the way the
# carplay widget is: this one links mvt, earcut and Qt6::GuiPrivate beyond the
# shared defaults, and bakes shaders.
dashboard_widget_register(${MAP_WIDGET_LIB})

set_target_properties(${MAP_WIDGET_LIB} PROPERTIES AUTOMOC ON)

# --------------------------------------------------------------- benchmark

# What a frame costs, stage by stage. NOT registered as a test: it asserts
# nothing and always exits 0, and add_project_test() on a program that cannot
# fail is how a green run stops meaning anything.
#
# It reads tiles straight from an .mbtiles archive, so it needs no bus:
#   map_bench --tiles ~/Documents/map_data/socal.mbtiles
add_executable(map_bench EXCLUDE_FROM_ALL
    map_bench.cpp
)

target_link_libraries(map_bench
    PRIVATE
        ${MAP_WIDGET_LIB}
        mbtiles
        mvt
        spdlog::spdlog
)

set_target_properties(map_bench PROPERTIES AUTOMOC ON)

# ------------------------------------------------------------------- tests

# Web Mercator and the tile arithmetic, on their own. No Qt, no bus, no tiles.
#
# Worth a `unit` test rather than leaving it to the screenshot loop because
# every mistake possible in it draws a perfectly good map in the WRONG PLACE,
# and a screenshot of somewhere you have not been looks exactly like a
# screenshot of somewhere you have. The anchor is Irvine at z14/2828/6562,
# which libs/mvt independently pulls out of the real archive and finds a city in.
add_executable(map_test_projection
    test_projection.cpp
    projection.cpp
)

target_include_directories(map_test_projection PRIVATE include)

target_link_libraries(map_test_projection
    PRIVATE
        spdlog::spdlog
)

add_project_test(TARGET map_test_projection LABELS map unit)

# The tessellator: vector tiles to triangles. Qt-free, so it runs with no
# display and no GPU -- which matters, because this is where the geometry is
# actually decided. The GPU only draws what this produces.
add_executable(map_test_tessellator
    test_tessellator.cpp
    tessellator.cpp
)

target_include_directories(map_test_tessellator PRIVATE include)

target_link_libraries(map_test_tessellator
    PRIVATE
        mvt
        earcut::earcut
        # For MapStyle_t only. The style is a reflected struct, so the colours
        # and the clamping come with it; none of it pulls in Qt.
        config_codec
        reflection
        helpers
        spdlog::spdlog
)

add_project_test(TARGET map_test_tessellator LABELS map unit)

# The decoded-tile cache and its eviction policy. Split out of TileSource so it
# can be tested at all: TileSource cannot be built without a zenoh session, and
# the policy is the part worth testing -- dropping the wrong tile does not draw
# anything wrong, it re-fetches, and the map reads as a slow server.
add_executable(map_test_tile_cache
    test_tile_cache.cpp
    tile_cache.cpp
    tile_workers.cpp
    tessellator.cpp
)

target_include_directories(map_test_tile_cache PRIVATE include)

target_link_libraries(map_test_tile_cache
    PRIVATE
        mvt
        earcut::earcut
        config_codec
        reflection
        helpers
        spdlog::spdlog
)

add_project_test(TARGET map_test_tile_cache LABELS map unit)

# The thread pool that spreads tile decode across cores. Its one caller needs a
# zenoh session and a live server, so it cannot be exercised there -- and a
# concurrency bug in it presents as a tile that occasionally does not arrive.
add_executable(map_test_tile_workers
    test_tile_workers.cpp
    tile_workers.cpp
)

target_include_directories(map_test_tile_workers PRIVATE include)

target_link_libraries(map_test_tile_workers
    PRIVATE
        spdlog::spdlog
)

add_project_test(TARGET map_test_tile_workers LABELS map unit)

# The GPU renderer, headless. Asserts that known geometry lands on known pixels
# under QT_QPA_PLATFORM=offscreen -- the property the whole design exists to
# preserve.
add_executable(map_test_gpu
    test_gpu_renderer.cpp
)

target_link_libraries(map_test_gpu
    PRIVATE
        ${MAP_WIDGET_LIB}
        spdlog::spdlog
)

set_target_properties(map_test_gpu PROPERTIES AUTOMOC ON)

add_project_test(TARGET map_test_gpu LABELS dashboard map gui)

# The widget through the paths the apps and the agent interface use: the config
# surface, and that it paints offscreen.
add_executable(map_test_widget
    test_map_widget.cpp
)

target_link_libraries(map_test_widget
    PRIVATE
        ${MAP_WIDGET_LIB}
        config_codec
        spdlog::spdlog
)

set_target_properties(map_test_widget PROPERTIES AUTOMOC ON)

add_project_test(TARGET map_test_widget LABELS dashboard map gui)

# The same assertions on a 2x screen. QT_SCALE_FACTOR is what gives an offscreen
# widget a device pixel ratio -- there is no display to take one from -- and it
# is the only way to reach the paint path the map renders its geometry through
# on a HiDPI panel. Without this the ratio is 1 in every test on the machine and
# the map could go back to rendering at half resolution unnoticed.
add_project_test(TARGET map_test_widget NAME map_test_widget_hidpi
                 LABELS dashboard map gui ENVIRONMENT QT_SCALE_FACTOR=2)
