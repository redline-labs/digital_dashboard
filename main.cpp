#include "app_config.h"
#include "main_window.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cmath>
#include <csignal>

#include <QApplication>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

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
        ("debug", "Enable debug logging.",cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
        ("libusb_debug", "Enable LibUSB debugging logging.",cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
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

    spdlog::set_level(args_result["debug"].as<bool>() ? spdlog::level::debug : spdlog::level::info);


    auto cfg = load_app_config("/Users/ryan/src/mercedes_dashboard/config.yaml");

    std::signal(SIGINT, [](int /* signum */)
    {
        SPDLOG_WARN("SIGINT received.");
        QCoreApplication::quit();
    });


    QApplication app(argc, argv);

    // Create windows from configuration
    std::vector<std::unique_ptr<MainWindow>> windows;
    CarPlayWidget* carplay_widget = nullptr;

    if (cfg.windows.empty()) {
        // Fallback: create a single window with legacy configuration
        window_config_t fallback_window;
        fallback_window.name = "main";
        fallback_window.width = cfg.width_px;
        fallback_window.height = cfg.height_px;
        
        auto main_window = std::make_unique<MainWindow>(cfg, fallback_window, args_result["libusb_debug"].as<bool>());
        carplay_widget = main_window->getCarPlayWidget();
        main_window->show();
        windows.push_back(std::move(main_window));
        
        SPDLOG_INFO("Created fallback window '{}' ({}x{})", fallback_window.name, 
                    fallback_window.width, fallback_window.height);
    } else {
        // Create configured windows
        for (const auto& window_cfg : cfg.windows) {
            auto main_window = std::make_unique<MainWindow>(cfg, window_cfg, args_result["libusb_debug"].as<bool>());
            
            // Check if this window has a CarPlay widget
            if (!carplay_widget) {
                carplay_widget = main_window->getCarPlayWidget();
            }
            
            main_window->show();
            windows.push_back(std::move(main_window));
            
            SPDLOG_INFO("Created window '{}' ({}x{}) with {} widgets", 
                       window_cfg.name, window_cfg.width, window_cfg.height, window_cfg.widgets.size());
        }
    }

    // Only setup CarPlay audio if CarPlay widget exists
    if (carplay_widget) {
        QAudioFormat format;
        format.setSampleRate(44100);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);

        const auto devices = QMediaDevices::audioOutputs();
        for (const QAudioDevice &device : devices)
        {
            SPDLOG_DEBUG("Found audio device: {}",  device.description().toStdString());
        }

        QAudioDevice info(QMediaDevices::defaultAudioOutput());
        if (!info.isFormatSupported(format)) {
            SPDLOG_ERROR("Raw audio format not supported by backend, cannot play audio.");
            // TODO: Should we bomb out?
        }

        SPDLOG_INFO("Using audio output: {}", info.description().toStdString());

        QAudioSink audio(format);
        SPDLOG_DEBUG("Default audio sink buffer size = {}, setting to {}.", audio.bufferSize(), cfg.audio_device_buffer_size);
        audio.setBufferSize(cfg.audio_device_buffer_size);

        auto audio_buffer = audio.start();

        // Register audio callback with the integrated CarPlay widget
        carplay_widget->register_audio_ready_callback([&audio_buffer] (const uint8_t* buffer, uint32_t buffer_len){
            audio_buffer->write(reinterpret_cast<const char*>(buffer), buffer_len);
        });

        // Start the integrated dongle functionality
        carplay_widget->start_dongle();
        
        SPDLOG_INFO("CarPlay widget configured and started.");
    } else {
        SPDLOG_INFO("No CarPlay widget configured in any window.");
    }


    SPDLOG_INFO("Starting with {} windows.", windows.size());
    app.exec();  // Blocking.


    SPDLOG_WARN("Exit received, tearing down.");

    // Stop the integrated decoder and dongle if CarPlay widget exists
    if (carplay_widget) {
        carplay_widget->stop_decoder();
        carplay_widget->stop_dongle();
    }

    return 0u;
}