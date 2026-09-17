#ifndef WEB_CONSOLE_HTTPLIB_INCLUDE_H
#define WEB_CONSOLE_HTTPLIB_INCLUDE_H

// The one place httplib.h is included, and the only reason this file exists.
//
// cpp-httplib is header-only, so its 778 KB compile inside OUR translation
// unit -- which means it meets this project's -Werror with -Wconversion,
// -Wsign-conversion, -Wold-style-cast and -Wshadow on GCC 15. Vendored
// dependencies normally escape that block by POSITION (they are added before it
// in the root CMakeLists), and a header-only one cannot.
//
// The alternative was marking the include directory SYSTEM, which
// third_party/earcut.cmake and the root CMakeLists explain the cost of, and
// which would also have disabled those warnings for OUR code in whatever file
// did the including. Suppressing around one include keeps the warnings on
// everything we write.
//
// Anything that needs the HTTP types includes this, never <httplib.h>.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"

#include <httplib.h>

#pragma GCC diagnostic pop

#endif // WEB_CONSOLE_HTTPLIB_INCLUDE_H
