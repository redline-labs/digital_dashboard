#include "bag_tool/verbs.h"

#include "bag/metadata.h"
#include "bag/rebuild.h"

#include "cli/output.h"

#include <spdlog/spdlog.h>

#include <string>

namespace bag_tool
{

void addReindexOptions(cxxopts::Options& options)
{
    options.add_options()
        ("bag", "The recording directory.", cxxopts::value<std::string>())
        ("n,dry-run", "Report what would be written without writing it.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"bag"});
}

int runReindex(cli::Context& context)
{
    const auto path = context.requireString("bag");
    if (!path)
    {
        return cli::kUsage;
    }

    // The rebuild itself is bag::rebuildMetadata() so it can be tested without
    // driving this binary -- recovering a crashed recording is too important to
    // be reachable only through a CLI. What is left here is presentation.
    const auto metadata = bag::rebuildMetadata(*path);
    if (!metadata)
    {
        return cli::kFailure;
    }

    for (const bag::bag_part_t& part : metadata->parts)
    {
        cli::out("{:<24} {:>9} msgs{}", part.path, part.message_count,
                 part.complete ? "" : "  [INCOMPLETE]");
    }

    cli::out("");
    cli::out("{} message(s) across {} part(s), {} topic(s).", metadata->message_count,
             metadata->parts.size(), metadata->topics.size());

    // The advertisement set is gone: only the live recorder knew which topics
    // existed without publishing, and nothing in the files records a topic that
    // never produced a message. Said out loud so a rebuilt index is not read as
    // proof that there were no silent topics.
    cli::out("Note: topics that were advertised but never published cannot be recovered by a "
             "rebuild -- only the recorder knew about those.");

    if (context.flag("dry-run"))
    {
        cli::out("");
        cli::out("--dry-run: nothing was written.");
        return cli::kOk;
    }

    if (!bag::saveMetadata(*metadata, *path))
    {
        return cli::kFailure;
    }

    cli::out("Wrote {}.", bag::metadataPath(*path));
    return cli::kOk;
}

}  // namespace bag_tool
