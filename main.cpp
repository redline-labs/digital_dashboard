#include "app_config.h"
#include "command_line_args.h"
#include "main_window.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <csignal>

#include <QApplication>

// Patches to third party:
// LibUSB core for debug messages.
// spdlog tweakme to lower the default log level.

int main(int argc, char** argv)
{
    size_t max_size_bytes = 5u * 1024u * 1024u;  // 5MB
    size_t max_files = 3u;
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/rotating.txt", max_size_bytes, max_files, true);
    spdlog::default_logger()->sinks().push_back(file_sink);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    // Parse command line arguments
    auto args = parse_command_line_args(argc, argv);
    if (!args)
    {
        // Parsing failed or help was shown
        return -1;
    }

    // Set the logging level based on the debug flag
    spdlog::set_level(args->debug_enabled ? spdlog::level::debug : spdlog::level::info);

    // Load the configuration file
    SPDLOG_INFO("Loading configuration file '{}'.", args->config_file_path);
    auto cfg = load_app_config(args->config_file_path);
    if (!cfg)
    {
        SPDLOG_CRITICAL("Failed to load configuration file '{}'.", args->config_file_path);
        return -1;
    }

    QApplication app(argc, argv);

    // Create windows from configuration
    std::vector<std::unique_ptr<MainWindow>> windows;

    // Create configured windows
    for (const auto& window_cfg : cfg.value().windows)
    {
        auto main_window = std::make_unique<MainWindow>(window_cfg);

        main_window->show();
        windows.push_back(std::move(main_window));

        SPDLOG_INFO("Created window '{}' ({}x{}) with {} widget{}.",
                   window_cfg.name, window_cfg.width, window_cfg.height, window_cfg.widgets.size(),
                   window_cfg.widgets.size() == 1 ? "" : "s");
    }

    SPDLOG_INFO("Starting with {} window{}.", windows.size(), windows.size() == 1 ? "" : "s");

    std::signal(SIGINT, [](int /* signum */)
    {
        SPDLOG_WARN("SIGINT received, quitting.");
        QCoreApplication::quit();
    });

    app.exec();  // Blocking.

    SPDLOG_WARN("Exit received, tearing down.");

    return 0;
}