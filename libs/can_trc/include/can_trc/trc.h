// SPDX-License-Identifier: GPL-3.0-or-later
//
// PCAN .trc trace files, read and written.
//
// The format is PEAK's, and there are seven versions of it. They are not
// variations on a theme: the column *order* changes between them, v2.x lets the
// file declare its own layout in a `;$COLUMNS=` line, and which columns a given
// line even has depends on what kind of record it is. A v2.0 error frame has no
// ID and no length column at all -- the fields are simply missing and every
// later token shifts left -- while the v2.1 spelling of the same record puts a
// '-' placeholder there and keeps the length. Nothing about this can be pinned
// down by a fixed grammar, which is why the parse walks a column list the file
// itself supplied and asks, per record kind, whether each column is present.
//
//     1.0   no header at all; `1)` message numbers; type implied by the ID
//           being FFFFFFFF or the data starting with ERROR/RTR
//     1.1   $FILEVERSION and $STARTTIME appear; a combined type/direction
//           column (Rx/Tx/Warng/Error); time resolution 0.1 ms
//     1.2   adds Bus; time resolution 1 us
//     1.3   adds the J1939 Reserved column
//     2.0   $COLUMNS; type and direction split; DT/FD/FB/FE/BI/RR/ST/ER/EC
//     2.1   adds optional Bus and Reserved back, and the EV text record
//     3.0   CAN XL, and 11-bit IDs shrink from 4 hex digits to 3
//
// This build reads 1.0 through 2.1 and writes 2.1. A 3.0 file is read for
// everything it shares with 2.1; its XL/PE/OF/EN records are counted in
// ReadStats::unsupported and skipped, because helpers::CanFrame has no VCID,
// SDT or AF field and a 64-byte payload, and widening it would reach every CAN
// consumer in the tree for a bus none of them talk to.
//
// Two things the format will catch you out on:
//
//   * The width of the ID token is what says whether the identifier is 11-bit
//     or 29-bit. `0123` and `00000123` are different messages on a real bus,
//     and the only thing distinguishing them in the file is that one is four
//     characters and the other is eight. Parse the token, not the number.
//
//   * A column headed `l` is a byte count and one headed `L` is a CAN FD length
//     *code*, where 15 means 64 bytes. Reading one as the other gives a frame
//     that is the wrong length with nothing to indicate it.
//
// A malformed line is a malformed line, not a malformed file: it is counted in
// ReadStats::badLines and skipped. The previous implementation failed the whole
// file, so one bad line in a 24,000-line trace returned nothing at all.
#ifndef CAN_TRC_TRC_H
#define CAN_TRC_TRC_H

#include "can/error.h"

#include "helpers/can_frame.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace can::trc
{

enum class Version
{
    V1_0,
    V1_1,
    V1_2,
    V1_3,
    V2_0,
    V2_1,
    V3_0,
};

const char* to_string(Version version);

// The `;$FILEVERSION=` text for a version: "1.0", "2.1".
std::string version_text(Version version);

// Parses "2.1" and friends. Fails on anything else rather than guessing, since
// guessing a version picks a column layout and produces plausible wrong frames.
Result<Version> parse_version(std::string_view text);

// One column, identified as the format identifies it: a case-sensitive letter
// in the `;$COLUMNS=` list. The v1.x layouts have no such line, so they are
// described with the same identifiers to keep one code path.
enum class ColumnId : uint8_t
{
    // N -- index of the recorded message. Optional.
    Number,
    // O -- time offset since the start of the trace.
    Offset,
    // T -- record type. In v1.x this column also carries the direction.
    Type,
    // B -- bus, 1..16, or '-' for an event not tied to one.
    Bus,
    // I -- the CAN identifier, in hex, its width carrying 11-bit vs 29-bit.
    Id,
    // d -- direction, Rx or Tx. v2.0 and later only.
    Direction,
    // R -- J1939 destination address, '-' for plain CAN.
    Reserved,
    // l -- payload length in bytes.
    Length,
    // L -- CAN FD length code, 0..15. Mutually exclusive with Length.
    Dlc,
    // D -- the payload, and always last.
    Data,
    // v3.0 CAN XL columns. Recognised so a 3.0 file's layout can be walked;
    // the records that use them are skipped.
    Vcid,
    Sdt,
    Af,
    Rrs,
    Sec,
};

// The column layout of one file, in file order. Built from `;$COLUMNS=` when
// the file has one and from the version's fixed layout when it does not.
struct Columns
{
    std::vector<ColumnId> order;

    bool has(ColumnId id) const;
};

// The layout a version implies when the file does not declare one.
Columns default_columns(Version version);

// Parses the value of a `;$COLUMNS=` line: "N,O,T,B,I,d,R,L,D".
Result<Columns> parse_columns(std::string_view text);

enum class RecordKind : uint8_t
{
    // DT/FD/FB/FE/BI, or a v1.x line that is neither a warning nor an error.
    Data,
    // RR. Carries no payload.
    Remote,
    // ER, or v1.x `Error`. `frame.data` holds the five (v1.x: four) bytes the
    // format defines, untouched.
    ErrorFrame,
    // ST, or v1.x `Warng`. `frame.data` holds the 32-bit status word, most
    // significant byte first.
    HardwareStatus,
    // EC. Two bytes: receive error counter, then transmit error counter.
    ErrorCounter,
    // EV. `event` holds the text; there is no frame.
    Event,
    // A v3.0 record this build cannot represent. Never returned by Reader --
    // these are counted and skipped -- but named so the switch over this enum
    // is total.
    Unsupported,
};

const char* to_string(RecordKind kind);

// One line of a trace.
//
// Deliberately wider than helpers::CanFrame: an event's text, a status word and
// the line a record came from have nowhere to live in a frame, and squeezing
// them in would distort the type every CAN consumer in the tree is built on.
struct Record
{
    RecordKind kind { RecordKind::Data };

    // The N column. Zero when the file has no such column -- it is a label, not
    // an index, and nothing here depends on it being contiguous.
    uint64_t number { 0 };

    // The O column, in microseconds since the trace started. Held as an integer
    // on purpose: mock_data/data/pdm32_log.trc runs from 4294967270.343 ms to
    // 4295008779.456 ms, crossing 2^32 milliseconds inside the file, so a
    // 32-bit accumulator wraps and a float has seven significant digits for a
    // value that needs thirteen.
    uint64_t offsetUs { 0 };

    // The B column, 1..16. Zero when the file has no bus column, or the record
    // carried '-' because it belongs to no particular bus.
    uint8_t bus { 0 };

    bool isTx { false };

    // The J1939 destination address from the R column. Absent for plain CAN,
    // which is what '-' means there.
    std::optional<uint8_t> destinationAddress;

    // Valid for Data, Remote, ErrorFrame, HardwareStatus and ErrorCounter.
    helpers::CanFrame frame {};

    // The text of an EV record.
    std::string event;

    // Which line of the file this came from, so a diagnostic can point at it.
    uint64_t line { 0 };
};

struct FileHeader
{
    Version version { Version::V1_0 };

    // $STARTTIME converted to microseconds since the UNIX epoch. Zero when the
    // file has none, which is the only case where a record's absolute time is
    // unknowable -- v1.0 files, and nothing else.
    uint64_t startTimeUnixUs { 0 };

    Columns columns;

    // Whatever the ";   Generated by ..." comment said, for a log line.
    std::string generatedBy;
};

struct ReadStats
{
    uint64_t lines { 0 };
    uint64_t records { 0 };
    // Lines that did not parse. Reading continues past them; a non-zero count
    // here is the signal that the trace has holes, not that the read failed.
    uint64_t badLines { 0 };
    // v3.0 records this build cannot represent.
    uint64_t unsupported { 0 };
};

// $STARTTIME is an OLE Automation date: days since 1899-12-30, with the
// fraction being the time of day. Exposed because the writer needs the inverse
// and a test needs both.
double unix_us_to_ole_date(uint64_t unixUs);
uint64_t ole_date_to_unix_us(double oleDate);

// Reads a trace one record at a time.
//
// Streaming rather than gathering: a trace is the one CAN artefact that is
// routinely hundreds of megabytes, and the caller almost always wants to do
// something per record rather than hold them all. The previous implementation
// built a vector of every frame before the first callback, which also made its
// stop-early contract meaningless.
class Reader
{
public:
    static Result<std::unique_ptr<Reader>> open(const std::string& path);
    // For tests and for anything that already has the text.
    static std::unique_ptr<Reader> from_string(std::string text);

    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    const FileHeader& header() const { return header_; }
    const ReadStats& stats() const { return stats_; }

    // The next record, or nullopt at end of file. Malformed lines are counted
    // and skipped, so this only fails when the underlying stream does.
    Result<std::optional<Record>> next();

private:
    Reader() = default;

    // Applies a `;$KEYWORD=` comment line. Anything else is ignored.
    void apply_header_line(std::string_view line);
    // Turns one non-comment line into a record, or says why it could not.
    Result<Record> parse_line(std::string_view line) const;
    bool read_line(std::string& out);

    FileHeader header_;
    ReadStats stats_;

    // Exactly one of these is live. A string source keeps the whole text, which
    // is fine because it only ever comes from a literal in a test.
    std::ifstream file_;
    std::string text_;
    size_t textPos_ { 0 };
    bool fromString_ { false };

    // $COLUMNS was seen, so the layout is the file's rather than the default.
    bool columnsDeclared_ { false };
    // Diagnostics are capped: a file that is not a trace at all would otherwise
    // produce one log line per line of it.
    uint64_t warningsLogged_ { 0 };

    std::string lineBuffer_;
};

// One row of the bus table in a v1.3+ header.
struct BusInfo
{
    uint8_t bus { 1 };
    std::string name;
    std::string connection;
    // Zero omits the bit rate from the table.
    uint32_t bitrateBps { 0 };
    uint32_t dataBitrateBps { 0 };
};

struct WriterOptions
{
    // Only V2_1 is implemented. It is the last version before CAN XL and the
    // only one that carries bus, direction and the FD flags at once, so there
    // is no case for writing an older one except to feed a tool that cannot
    // read this -- and PCAN-Explorer 6 and PCAN-View 4 both can.
    Version version { Version::V2_1 };

    // Microseconds since the UNIX epoch. Zero takes the first record's absolute
    // time, and failing that the wall clock when the file was created.
    uint64_t startTimeUnixUs { 0 };

    std::string generatedBy { "Redline" };

    std::vector<BusInfo> buses;
};

// Writes a v2.1 trace.
//
// The columns written are `N,O,T,B,I,d,R,l,D` -- note `l`, a real byte count,
// rather than the length code `L`. Both are legal; `l` means no reader has to
// own the FD length table to get the payload length right, and a file that can
// be read correctly by a simpler tool is worth more than four saved characters
// a line.
class Writer
{
public:
    static Result<std::unique_ptr<Writer>> create(const std::string& path,
                                                  const WriterOptions& options);

    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Records are written in the order given; nothing sorts or checks that
    // offsets are non-decreasing, because a trace of two buses legitimately
    // interleaves and the caller knows what it recorded.
    Result<void> write(const Record& record);

    Result<void> flush();

    uint64_t recordsWritten() const { return recordsWritten_; }

private:
    Writer() = default;

    Result<void> write_header();

    std::ofstream file_;
    WriterOptions options_;
    uint64_t recordsWritten_ { 0 };
    bool headerWritten_ { false };
    std::string scratch_;
};

} // namespace can::trc

#endif // CAN_TRC_TRC_H
