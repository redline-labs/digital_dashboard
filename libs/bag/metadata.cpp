#include "bag/metadata.h"

#include "config_codec/config_yaml.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace bag
{

std::string metadataPath(const std::string& directory)
{
    return (std::filesystem::path(directory) / "metadata.yaml").string();
}

std::optional<bag_metadata_t> loadMetadata(const std::string& directory, bool quiet)
{
    const std::string path = metadataPath(directory);

    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        if (!quiet)
        {
            SPDLOG_ERROR("No metadata.yaml in '{}'. Is that a bag directory? `bag reindex` can "
                         "rebuild one from the parts.", directory);
        }
        return std::nullopt;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(path);
        bag_metadata_t metadata = root.as<bag_metadata_t>();

        // A version we do not know means fields we cannot interpret, not fields
        // we can ignore -- the parts are still readable, but whatever the newer
        // recorder recorded about them is not.
        if (metadata.version > 1)
        {
            SPDLOG_WARN("'{}' was written by a newer recorder (version {}); reading it with "
                        "version 1 rules.", path, metadata.version);
        }

        return metadata;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Could not read '{}': {}", path, e.what());
        return std::nullopt;
    }
}

bool saveMetadata(const bag_metadata_t& metadata, const std::string& directory)
{
    const std::string path = metadataPath(directory);
    const std::string temporary = path + ".tmp";

    try
    {
        {
            std::ofstream file(temporary, std::ios::trunc);
            if (!file)
            {
                SPDLOG_ERROR("Could not open '{}' for writing.", temporary);
                return false;
            }

            YAML::Node root;
            root = metadata;

            file << "# Written by redline bag. Do not edit by hand while a recording is "
                    "running.\n";
            file << root << "\n";

            if (!file)
            {
                SPDLOG_ERROR("Write to '{}' failed.", temporary);
                return false;
            }
        }

        // Rename over the old one. The recorder rewrites this after every roll,
        // so a crash during the write is a real possibility -- and a truncated
        // metadata.yaml would lose the index for parts that are on disk and
        // perfectly readable.
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            SPDLOG_ERROR("Could not replace '{}': {}", path, error.message());
            return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Could not write '{}': {}", path, e.what());
        return false;
    }
}

}  // namespace bag
