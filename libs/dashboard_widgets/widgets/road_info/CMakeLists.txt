# The road the vehicle is on, from nodes/map_match's horizon.
#
# schemas is PRIVATE: the horizon is decoded inside road_info.cpp and none of it
# reaches the widget's own header, which carries a plain State struct instead.
# That keeps the capnp generated headers out of app_config.h's include graph,
# which every consumer of every widget pulls in.
#
# The header is named in SOURCES on purpose. AUTOMOC otherwise has to find it by
# parsing the source's includes, and when that misses, the failure is a missing
# vtable at LINK time with no mention of moc anywhere in it. Naming the header
# puts it in the target's HEADERS and removes the guesswork.
add_dashboard_widget(road_info
    SOURCES road_info.cpp include/road_info/road_info.h
    PRIVATE_LIBS schemas capnp
)
