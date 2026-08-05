add_subdirectory(megasquirt)
add_subdirectory(motec)
add_subdirectory(msel)

# No tests here on purpose. The parser's tests use synthetic DBCs of their own
# under libs/dbc_parser/tests/dbcs/, so that editing a vendor DBC for product
# reasons cannot break them.
#
# These files are still checked every build: dbc_code_gen parses them, and it
# rejects out-of-frame signals, duplicate ids, missing multiplexors and names
# that are not usable C++ identifiers. A bad DBC here fails the build.
