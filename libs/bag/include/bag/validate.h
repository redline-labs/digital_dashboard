#ifndef BAG_VALIDATE_H_
#define BAG_VALIDATE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace bag
{

// Structural validation of an MCAP file, from the spec, using NO mcap code.
//
// WHY THIS EXISTS, and why it deliberately re-implements a parser:
//
// Every other test in this library reads a bag back through BagReader, which is
// a thin layer over mcap's reader -- and that reader is lenient. A file can be
// malformed and still round-trip perfectly through the pair that produced it.
// That is not hypothetical: rolling a part used to emit a summary listing schema
// and channel ids the data section never contained, and the round-trip,
// splitting AND seeking tests all passed on it. Only Foxglove's `mcap doctor`
// disagreed.
//
// Depending on an external Go binary to catch that class of bug is not a plan.
// It has to be installed, it is not in CI, and nobody runs it by habit. So this
// walks the raw bytes against
// build/_deps/mcap-src/website/docs/spec/index.md -- magic, TLV framing, section
// ordering, chunk CRCs, and above all whether the summary describes records that
// actually exist -- with no reference to mcap's own types. A bug in our writer
// and a matching bug in mcap's reader cannot cancel out here, because mcap's
// reader is not involved.
//
// It is NOT a full conformance suite. It checks the invariants that a writer can
// plausibly get wrong; it does not verify every field of every record type.

struct Finding
{
    enum class Severity
    {
        // The file violates the spec. Another implementation may reject it.
        Error,

        // Legal but suspicious, or a property we rely on and did not get.
        Warning,
    };

    Severity severity = Severity::Error;
    std::string message;
};

struct ValidationReport
{
    // What the parse found, useful to report even when the file is fine.
    std::uint64_t messages = 0;
    std::uint32_t chunks = 0;
    std::size_t schemas = 0;
    std::size_t channels = 0;
    std::string compression;
    bool has_summary = false;

    std::vector<Finding> findings;

    bool ok() const { return errorCount() == 0; }

    std::size_t errorCount() const
    {
        std::size_t count = 0;
        for (const Finding& finding : findings)
        {
            if (finding.severity == Finding::Severity::Error)
            {
                ++count;
            }
        }
        return count;
    }
};

// True when the file ends with the MCAP magic bytes -- i.e. its writer closed
// it properly.
//
// Cheap: reads the last 8 bytes. Worth having as its own function because the
// obvious alternative is WRONG in a way that is easy to miss:
// McapReader::readSummary() returns SUCCESS on a truncated file. It scans the
// data section, produces perfectly good ChunkIndex records from what is there,
// and reports ok -- so a `part.complete = summary.ok()` says every torn part is
// complete. (The same trap cost the reader its LogTimeOrder path; see
// reader.cpp.)
//
// The trailing magic is the only thing a killed writer cannot have written.
bool hasCompleteEnding(const std::string& path);

// One .mcap file.
ValidationReport validateMcapFile(const std::string& path);

// A whole bag directory: every part, plus cross-checks against metadata.yaml
// (which is ours, not MCAP's -- a part on disk that the index does not mention,
// or a count that disagrees with the file).
ValidationReport validateBag(const std::string& directory);

}  // namespace bag

#endif  // BAG_VALIDATE_H_
