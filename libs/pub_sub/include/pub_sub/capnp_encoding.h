#ifndef PUB_SUB_CAPNP_ENCODING_H_
#define PUB_SUB_CAPNP_ENCODING_H_

#include <string_view>

namespace pub_sub
{

// The zenoh encoding every capnp payload in this tree is published with.
//
// Publishers set the MIME type and then attach the schema name via
// zenoh's Encoding::set_schema(), so what a subscriber reads back from
// Encoding::as_string() is the *combined* form:
//
//     application/capnp;CanFrame
//
// zenoh-cpp has set_schema() but no getter -- the only accessor for the
// schema half alone is `zc_internal_encoding_get_data`, whose name says
// plainly that it is not ours to call. So the split has to happen here,
// which is why this lives next to the publishers that write the string
// rather than in whichever tool happens to read it.
inline constexpr std::string_view kCapnpEncodingMime = "application/capnp";

// The schema name out of an encoding string, i.e. what get_schema() wants.
//
// Returns everything after the first ';', or the whole string when there is
// no separator -- a publisher that set a MIME type but no schema yields ""
// and simply will not match anything in the registry, which is the correct
// outcome rather than a special case.
constexpr std::string_view schemaNameFromEncoding(std::string_view encoding)
{
    const auto separator = encoding.find(';');
    if (separator == std::string_view::npos)
    {
        return encoding;
    }
    return encoding.substr(separator + 1);
}

}  // namespace pub_sub

#endif  // PUB_SUB_CAPNP_ENCODING_H_
