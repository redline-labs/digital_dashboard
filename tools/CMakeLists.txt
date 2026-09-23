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

# A fake de.pengutronix.rauc on the session bus, so the web console's reflash
# path is developable without a board.
add_subdirectory(rauc_stub)

# A service that answers, so the console's /api/call path is developable
# without hardware behind it.
add_subdirectory(fake_service)

# The state estimator's workstation side: a simulated drive recorded as a bag,
# and the fixed-lag and whole-drive estimates over any bag. After nodes/,
# whose state_estimator pipeline both reuse.
add_subdirectory(estimator_sim)
add_subdirectory(estimator_offline)
