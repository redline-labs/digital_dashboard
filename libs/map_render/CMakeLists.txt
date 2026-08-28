# The map renderer: vector tiles to pixels.
#
# Hoisted out of dashboard/widgets/map so that a second app can draw a map
# without reaching into the dashboard's include tree -- the same move
# libs/config_codec and libs/qt_helpers were created by, and for the same
# reason. What is here is everything that is NOT "the widget": the projection,
# the tessellator, the GPU renderer, label layout, the tile cache and the
# worker pool.
#
# What is deliberately NOT here, and must stay out:
#
#   * zenoh and capnp. Where tiles come from is the caller's business.
#     dashboard/widgets/map asks nodes/map_server; scope reads an .mbtiles
#     directly. Neither transport is visible from in here.
#   * a QWidget. Nothing here is one. GpuRenderer drives QRhi against an
#     offscreen TEXTURE rather than a platform surface, which is the whole
#     reason a map draws at all under QT_QPA_PLATFORM=offscreen -- and it is
#     what lets two different hosts, a dashboard widget and a scope panel, blit
#     the same frame. (Qt6::Widgets does arrive on the link line anyway, via
#     qt_helpers' INTERFACE deps. That is a fact about that target, not a
#     QWidget in here.)
#   * libs/mbtiles. The archive reader stays with whoever opens a file.
cmake_minimum_required(VERSION 3.16)

set(MAP_RENDER_LIB map_render)

# GuiPrivate is what exposes <rhi/qrhi.h>. QRhi is semi-public in Qt 6 -- a
# stable-ish API shipped behind the private headers -- and it is the only way to
# reach the GPU without binding to a platform surface. See gpu_renderer.h for
# why binding to one is not an option here.
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate ShaderTools)

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
map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/glyph.vert glyphVert GLYPH_VERT_HEADER)
map_bake_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/glyph.frag glyphFrag GLYPH_FRAG_HEADER)

add_custom_target(map_render_shaders
    DEPENDS ${MAP_VERT_HEADER} ${MAP_FRAG_HEADER} ${MAP_HIGHLIGHT_VERT_HEADER}
            ${MAP_HIGHLIGHT_FRAG_HEADER} ${GLYPH_VERT_HEADER} ${GLYPH_FRAG_HEADER})

# ----------------------------------------------------------------- library

add_library(${MAP_RENDER_LIB} STATIC
    src/gpu_renderer.cpp
    src/label_candidates.cpp
    src/labels.cpp
    src/projection.cpp
    src/tessellator.cpp
    src/tile_cache.cpp
    src/tile_workers.cpp
    ${MAP_VERT_HEADER}
    ${MAP_FRAG_HEADER}
    ${MAP_HIGHLIGHT_VERT_HEADER}
    ${MAP_HIGHLIGHT_FRAG_HEADER}
    ${GLYPH_VERT_HEADER}
    ${GLYPH_FRAG_HEADER}
    include/map_render/gpu_renderer.h
    include/map_render/label_candidates.h
    include/map_render/labels.h
    include/map_render/projection.h
    include/map_render/style.h
    include/map_render/tessellator.h
    include/map_render/text_quad.h
    include/map_render/tile_cache.h
    include/map_render/tile_workers.h
)

add_dependencies(${MAP_RENDER_LIB} map_render_shaders)

target_include_directories(${MAP_RENDER_LIB} PUBLIC include)
target_include_directories(${MAP_RENDER_LIB} PRIVATE ${MAP_SHADER_DIR})

target_link_libraries(${MAP_RENDER_LIB}
    PUBLIC
        Qt6::Core
        Qt6::Gui
        helpers
        reflection
        config_codec
        # The vector tile decoder. Everything drawn here arrives as MVT; where
        # those bytes came from is not known in this library.
        mvt
    PRIVATE
        Qt6::GuiPrivate
        # helpers::Color -> QColor, for the label text and halo. The same
        # resolver every other GUI target in the tree uses, so a colour cannot
        # mean two things.
        qt_helpers
        # Polygon triangulation with holes. See third_party/earcut.cmake.
        earcut::earcut
        spdlog::spdlog
)

set_target_properties(${MAP_RENDER_LIB} PROPERTIES AUTOMOC ON)

# --------------------------------------------------------------- benchmark

# What a frame costs, stage by stage. NOT registered as a test: it asserts
# nothing and always exits 0, and add_project_test() on a program that cannot
# fail is how a green run stops meaning anything.
#
# It reads tiles straight from an .mbtiles archive, so it needs no bus:
#   map_bench --tiles ~/Documents/map_data/socal.mbtiles
#
# It lives here rather than beside the widget because it never touches one --
# and because that is what keeps mbtiles off the dashboard's link line.
add_executable(map_bench EXCLUDE_FROM_ALL
    map_bench.cpp
)

target_link_libraries(map_bench
    PRIVATE
        ${MAP_RENDER_LIB}
        mbtiles
        mvt
        spdlog::spdlog
)

set_target_properties(map_bench PROPERTIES AUTOMOC ON)

# ------------------------------------------------------------------- tests
#
# The unit tests below deliberately compile the translation unit they are about
# rather than linking ${MAP_RENDER_LIB}, so each keeps the minimal dependency
# set its comment describes -- linking the library would pull QtGui and
# GuiPrivate into tests that are meant to prove they need neither.

# Web Mercator and the tile arithmetic, on their own. No Qt, no bus, no tiles.
#
# Worth a `unit` test rather than leaving it to the screenshot loop because
# every mistake possible in it draws a perfectly good map in the WRONG PLACE,
# and a screenshot of somewhere you have not been looks exactly like a
# screenshot of somewhere you have. The anchor is Irvine at z14/2828/6562,
# which libs/mvt independently pulls out of the real archive and finds a city in.
add_executable(map_test_projection
    tests/test_projection.cpp
    src/projection.cpp
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
    tests/test_tessellator.cpp
    src/tessellator.cpp
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
    tests/test_tile_cache.cpp
    src/tile_cache.cpp
    src/tile_workers.cpp
    src/tessellator.cpp
)

target_include_directories(map_test_tile_cache PRIVATE include)

target_link_libraries(map_test_tile_cache
    PRIVATE
        # Core only: LabelCandidate carries a QString, and the cache weighs
        # it. Still no QtGui, no zenoh -- the eviction policy stays a unit.
        Qt6::Core
        mvt
        earcut::earcut
        config_codec
        reflection
        helpers
        spdlog::spdlog
)

add_project_test(TARGET map_test_tile_cache LABELS map unit)

# The thread pool that spreads tile decode across cores. Its callers need either
# a zenoh session and a live server or an archive on disk, so it cannot be
# exercised through them -- and a concurrency bug in it presents as a tile that
# occasionally does not arrive.
add_executable(map_test_tile_workers
    tests/test_tile_workers.cpp
    src/tile_workers.cpp
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
    tests/test_gpu_renderer.cpp
)

target_link_libraries(map_test_gpu
    PRIVATE
        ${MAP_RENDER_LIB}
        spdlog::spdlog
)

set_target_properties(map_test_gpu PROPERTIES AUTOMOC ON)

add_project_test(TARGET map_test_gpu LABELS map gui)
