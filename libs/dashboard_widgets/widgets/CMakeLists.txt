# Every widget lives in its own directory here and registers itself through
# add_dashboard_widget() (cmake/DashboardWidget.cmake), so there is no list to
# keep in step: a new directory with a CMakeLists is a new widget library.
# Sorted, so the configure order -- and the link order it feeds -- does not
# depend on the filesystem.

file(GLOB widget_dirs LIST_DIRECTORIES true RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT widget_dirs)
foreach(widget_dir IN LISTS widget_dirs)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${widget_dir}
       AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${widget_dir}/CMakeLists.txt)
        add_subdirectory(${widget_dir})
    endif()
endforeach()
