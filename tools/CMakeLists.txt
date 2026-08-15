# Things that run on the WORKSTATION and never ship.
#
# The distinction that makes this directory coherent rather than a junk drawer:
#
#     nodes/ runs on the vehicle. tools/ runs on the workstation.
#
# It earns its keep immediately -- map_build holds tens of gigabytes of scratch
# state at continental scale and links a compression stack the vehicle image has
# no use for, and this boundary is what keeps it out of a deployment manifest.
#
# NOT globbed: tools/mcp_dashboard is a Python project with no CMakeLists.
add_subdirectory(map_build)
