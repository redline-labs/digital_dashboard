#include "dbc_parser/dbc_parser.h"
#include "dbc_parser/generate_h.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <filesystem>

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    try
    {
        cxxopts::Options options("dbc_code_gen", "DBC-based C code generator");
        options.add_options()
            ("i,input", "Path to input DBC file", cxxopts::value<std::string>())
            ("o,output", "Output directory for generated files", cxxopts::value<std::string>())
            ("n,name", "Base name for generated files (mandatory)", cxxopts::value<std::string>())
            ("h,help", "Print usage")
            ("s,silent", "Silent mode", cxxopts::value<bool>()->default_value("false")->implicit_value("true"));

        auto result = options.parse(argc, argv);

        if (result.count("help") || !result.count("input") || !result.count("output") || !result.count("name"))
        {
            SPDLOG_INFO("{}", options.help());
            return result.count("help") ? 0 : 1;
        }

        // Silent mode... Set the log level to warning so we don't spam the build.
        if (result["silent"].as<bool>())
        {
            spdlog::set_level(spdlog::level::warn);
        }

        std::filesystem::path inputPath = result["input"].as<std::string>();
        std::filesystem::path outputDir = result["output"].as<std::string>();

        // Resolve to absolute paths
        std::error_code ec;
        inputPath = std::filesystem::absolute(inputPath, ec);
        if (ec)
        {
            SPDLOG_ERROR("Failed to resolve input path: {}", ec.message());
            return 5;
        }
        outputDir = std::filesystem::absolute(outputDir, ec);
        if (ec)
        {
            SPDLOG_ERROR("Failed to resolve output path: {}", ec.message());
            return 6;
        }

        // Validate input file
        if (!std::filesystem::exists(inputPath))
        {
            SPDLOG_ERROR("Input file does not exist: {}", inputPath.string());
            return 7;
        }
        if (!std::filesystem::is_regular_file(inputPath))
        {
            SPDLOG_ERROR("Input path is not a file: {}", inputPath.string());
            return 8;
        }

        // Ensure output directory exists
        if (!std::filesystem::exists(outputDir))
        {
            if (!std::filesystem::create_directories(outputDir, ec))
            {
                if (ec)
                {
                    SPDLOG_ERROR("Failed to create output directory '{}': {}", outputDir.string(), ec.message());
                    return 9;
                }
            }
        }
        else if (!std::filesystem::is_directory(outputDir))
        {
            SPDLOG_ERROR("Output path exists but is not a directory: {}", outputDir.string());
            return 10;
        }

        std::ifstream in(inputPath);
        if (!in)
        {
            SPDLOG_ERROR("Failed to open input DBC: {}", inputPath.string());
            return 2;
        }

        SPDLOG_INFO("Opening input DBC: {}", inputPath.string());

        std::ostringstream ss;
        ss << in.rdbuf();
        dbc_parser::Parser parser(ss.str());
        dbc_parser::ParseError err;
        auto db = parser.parse(err);
        if (!db)
        {
            SPDLOG_ERROR("Parse error at {}:{}: {}", err.line, err.column, err.message);
            return 3;
        }

        SPDLOG_INFO("Parsed DBC: version='{}' nodes={} messages={}", db->version, db->nodes.size(), db->messages.size());
        SPDLOG_INFO("Ready to generate into: {}", outputDir.string());

        // Determine base name for outputs (mandatory --name)
        std::string base = result["name"].as<std::string>();
        if (base.empty())
        {
            SPDLOG_ERROR("--name must be a non-empty string");
            return 13;
        }

        // Invoke generators
        dbc_codegen::generate_cpp_header(*db, base, outputDir);
        dbc_codegen::generate_cpp_parser_header(*db, base, outputDir);
        dbc_codegen::generate_cpp_parser_source(*db, base, outputDir);

        SPDLOG_INFO("Generation completed.");
        return 0;
    }
    catch (const std::exception &e)
    {
        SPDLOG_ERROR("Argument error: {}", e.what());
        return 4;
    }
}


