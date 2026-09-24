cmake_minimum_required(VERSION 3.24)

project(deflec)

# Deflection of the vertical: how far the plumb line leans off the ellipsoid
# normal, and so the direction of real gravity. Reads NGS's gridded models
# (.bin) exactly as NGS distributes them, memory-mapped, and interpolates them
# with NGS's bicubic. The files are not here: they live, verbatim and in Git
# LFS, under models/ at the root, which also installs them. This library is
# handed a directory and knows nothing about where it came from.
# See docs/libs/deflec.md.
add_library(deflec STATIC
    src/ngs_grid.cpp
    src/model.cpp
)
target_include_directories(deflec PUBLIC include)
target_link_libraries(deflec PUBLIC geodesy)

# The reader and interpolation off the happy path, on synthetic grids written
# in NGS's format: every malformed file (and an LFS pointer) refused with its
# reason; exact for linear fields up to the edges and quadratics inside;
# longitude in any turn; the gravity it makes.
add_executable(deflec_test_grid tests/test_grid.cpp)
target_link_libraries(deflec_test_grid PRIVATE deflec)
add_project_test(TARGET deflec_test_grid LABELS deflec unit)

# The real model, from models/deflec2022, at every one of NGS's published test
# points inside North America: to their last digit. Skips if the checkout
# never pulled the LFS content.
add_executable(deflec_test_ngs tests/test_ngs.cpp)
target_link_libraries(deflec_test_ngs PRIVATE deflec core)
add_project_test(TARGET deflec_test_ngs LABELS deflec unit)
