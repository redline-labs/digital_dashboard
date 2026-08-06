#include "bag_tool/verbs.h"

#include "bag/validate.h"

#include "cli/output.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

namespace bag_tool
{

void addVerifyOptions(cxxopts::Options& options)
{
    options.add_options()
        ("target", "A bag directory, or a single .mcap file.", cxxopts::value<std::string>())
        ("q,quiet", "Print nothing; report only through the exit code.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

    options.parse_positional({"target"});
}

int runVerify(cli::Context& context)
{
    const auto target = context.requireString("target");
    if (!target)
    {
        return cli::kUsage;
    }

    // This is `mcap doctor` without the dependency.
    //
    // The validator behind it walks the raw bytes against the MCAP spec with no
    // reference to mcap's own code -- see libs/bag/include/bag/validate.h for
    // why that independence is the whole point. It exists because a malformed
    // file once round-tripped cleanly through our own writer and reader, and the
    // only thing that noticed was a Go binary nobody had installed.
    std::error_code error;
    const bool is_directory = std::filesystem::is_directory(*target, error);

    const bag::ValidationReport report =
        is_directory ? bag::validateBag(*target) : bag::validateMcapFile(*target);

    if (context.json())
    {
        nlohmann::json out;
        out["target"] = *target;
        out["ok"] = report.ok();
        out["messages"] = report.messages;
        out["chunks"] = report.chunks;
        out["compression"] = report.compression;

        out["findings"] = nlohmann::json::array();
        for (const bag::Finding& finding : report.findings)
        {
            nlohmann::json row;
            row["severity"] =
                finding.severity == bag::Finding::Severity::Error ? "error" : "warning";
            row["message"] = finding.message;
            out["findings"].push_back(std::move(row));
        }

        cli::out("{}", out.dump(2));
        return report.ok() ? cli::kOk : cli::kFailure;
    }

    if (!context.flag("quiet"))
    {
        for (const bag::Finding& finding : report.findings)
        {
            const char* label =
                finding.severity == bag::Finding::Severity::Error ? "error" : "warning";
            cli::out("{:<8} {}", label, finding.message);
        }

        if (report.findings.empty())
        {
            cli::out("{} is valid MCAP.", *target);
        }
        else
        {
            cli::out("");
        }

        cli::out("{} message(s), {} chunk(s), {} compression.", report.messages, report.chunks,
                 report.compression.empty() ? "no" : report.compression);

        if (!report.ok())
        {
            cli::out("{} error(s).", report.errorCount());
        }
    }

    // Exit code is the point: this is meant to be usable from a script and from
    // CI, which is exactly what depending on an uninstalled external binary was
    // not.
    return report.ok() ? cli::kOk : cli::kFailure;
}

}  // namespace bag_tool
