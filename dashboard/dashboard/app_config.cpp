#include "dashboard/app_config.h"

#include <spdlog/spdlog.h>

#include <string>


std::optional<app_config_t> load_app_config(const std::string& config_filepath)
{
    // Default config in case of error.
    std::optional<app_config_t> config = std::nullopt;

    try
    {
        config = YAML::LoadFile(config_filepath).as<app_config_t>();
    }
    catch (const YAML::BadFile& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadFile : {})", e.what());
    }
    catch (const YAML::ParserException& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::ParserException : {})", e.what());
    }
    catch (const YAML::BadConversion& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::BadConversion : {})", e.what());
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (YAML::Exception : {})", e.what());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to load app config: (std::exception : {})", e.what());
    }

    return config;
}