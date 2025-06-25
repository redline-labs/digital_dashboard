#include "app_config.h"
#include "main_window.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cmath>
#include <csignal>

#include <QApplication>

// Patches to third party:
// LibUSB core for debug messages.
// spdlog tweakme to lower the default log level.

int main(int argc, char** argv)
{
    auto max_size = 1048576 * 5;
    auto max_files = 3;
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/rotating.txt", max_size, max_files, true);
    spdlog::default_logger()->sinks().push_back(file_sink);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("carplay_app", "Spin up a CarPlay instance.");
    options.add_options()
        ("config", "Configuration file to use.", cxxopts::value<std::string>())
        ("debug", "Enable debug logging.", cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
    cxxopts::ParseResult args_result;

    try
    {
        args_result = options.parse(argc, argv);
    }
    catch (const cxxopts::exceptions::exception& e)
    {
        SPDLOG_CRITICAL(e.what());
        return -1;
    }

    // Set the configuration based on the command line.
    spdlog::set_level(args_result["debug"].as<bool>() ? spdlog::level::debug : spdlog::level::info);

    // Use the configuration specified by the user.
    std::string config_file_path = args_result["config"].as<std::string>();
    SPDLOG_INFO("Loading configuration file '{}'.", config_file_path);
    auto cfg = load_app_config(config_file_path);


    QApplication app(argc, argv);

    // Create windows from configuration
    std::vector<std::unique_ptr<MainWindow>> windows;

    // Create configured windows
    for (const auto& window_cfg : cfg.windows) {
        auto main_window = std::make_unique<MainWindow>(cfg, window_cfg);

        main_window->show();
        windows.push_back(std::move(main_window));

        SPDLOG_INFO("Created window '{}' ({}x{}) with {} widget{}.",
                   window_cfg.name, window_cfg.width, window_cfg.height, window_cfg.widgets.size(),
                   window_cfg.widgets.size() == 1 ? "" : "s");
    }

    SPDLOG_INFO("Starting with {} window{}.", windows.size(), windows.size() == 1 ? "" : "s");

    std::signal(SIGINT, [](int /* signum */)
    {
        SPDLOG_WARN("SIGINT received.");
        QCoreApplication::quit();
    });

    app.exec();  // Blocking.


    SPDLOG_WARN("Exit received, tearing down.");

    return 0u;
}