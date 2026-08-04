# SPDX-License-Identifier: GPL-3.0-or-later
#
# One way to declare a dashboard widget library.
#
#     add_dashboard_widget(value_readout
#         SOURCES value_readout.cpp
#     )
#
#     add_dashboard_widget(mercedes_190e_telltales
#         SOURCES telltale.cpp
#         QT_COMPONENTS Svg
#         PRIVATE_LIBS schemas
#     )
#
# The widget is added to the DASHBOARD_WIDGET_LIBS global property, which
# dashboard/CMakeLists.txt reads after add_subdirectory(widgets) to link both
# executables. Widgets therefore register themselves, rather than being named a
# second time in a list somewhere above them.
#
# Defaults, applied to every widget:
#
#   PUBLIC   reflection, helpers, zenoh_pub_sub, Qt6::Widgets, QT_COMPONENTS
#   PRIVATE  spdlog::spdlog
#   includes dashboard/include and the widget's own include/
#   AUTOMOC  on, per target
#
# The PUBLIC defaults are PUBLIC deliberately, because a widget's headers are
# part of its interface. Every widget is a QWidget subclass, config.h almost
# always names helpers::Color, and the widget header usually holds a
# subscription member. Linking any of those PRIVATE compiles the widget itself
# and breaks anything that includes its headers -- which is every consumer,
# since app_config.h pulls in all of them.
#
# That is not hypothetical, and the failure never points at the cause. The
# helpers case was hit three separate times and reports "helpers/color.h not
# found" against the including file. Several widgets had zenoh_pub_sub PRIVATE
# while declaring subscription members in public headers, which only linked
# because the executables pull the library in themselves. The cluster gauge
# had Qt6::Svg PRIVATE while including <QSvgRenderer> from its public header.
#
# So Qt components go in PUBLIC too. A widget that truly needs something only
# in its .cpp can still pass it as PRIVATE_LIBS.
#
# Anything genuinely unusual should not be forced through here. carplay needs
# ffmpeg via pkg-config and several extra Qt components; it keeps its own
# hand-written CMakeLists and calls dashboard_widget_register() directly.

function(dashboard_widget_register widget_name)
    if(NOT TARGET ${widget_name})
        message(FATAL_ERROR
            "dashboard_widget_register: no such target '${widget_name}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY DASHBOARD_WIDGET_LIBS ${widget_name})
endfunction()

function(add_dashboard_widget widget_name)
    cmake_parse_arguments(DW "" "" "SOURCES;PUBLIC_LIBS;PRIVATE_LIBS;QT_COMPONENTS" ${ARGN})

    if(NOT DW_SOURCES)
        message(FATAL_ERROR "add_dashboard_widget(${widget_name}): SOURCES is required")
    endif()
    if(DW_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "add_dashboard_widget(${widget_name}): unrecognised argument(s): "
            "${DW_UNPARSED_ARGUMENTS}")
    endif()

    # Widgets is needed by every widget; anything else is opt-in. The components
    # are found again here rather than assumed, so a widget still configures
    # when built on its own. dashboard/CMakeLists.txt finds the common set
    # before add_subdirectory(widgets), so in the normal build these are no-ops.
    find_package(Qt6 REQUIRED COMPONENTS Widgets ${DW_QT_COMPONENTS})

    add_library(${widget_name} STATIC ${DW_SOURCES})

    # Per-target rather than the global CMAKE_AUTOMOC, so a widget added later
    # does not depend on where in the tree it happens to be included from.
    set_target_properties(${widget_name} PROPERTIES AUTOMOC ON)

    target_include_directories(${widget_name} PUBLIC
        ${CMAKE_SOURCE_DIR}/dashboard/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    foreach(component IN LISTS DW_QT_COMPONENTS)
        list(APPEND DW_PUBLIC_LIBS Qt6::${component})
    endforeach()

    target_link_libraries(${widget_name}
        PUBLIC
            reflection
            helpers        # config.h is public and normally names helpers::Color
            zenoh_pub_sub  # the widget header usually holds a subscription
            Qt6::Widgets   # the widget header is a QWidget subclass
            ${DW_PUBLIC_LIBS}
        PRIVATE
            spdlog::spdlog
            ${DW_PRIVATE_LIBS}
    )

    dashboard_widget_register(${widget_name})
endfunction()
