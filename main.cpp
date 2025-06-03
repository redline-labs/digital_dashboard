#include "app_config.h"
#include "dongle_driver.h"
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

    DongleDriver driver(cfg, args_result["libusb_debug"].as<bool>());

    std::signal(SIGINT, [](int /* signum */)
    {
        SPDLOG_WARN("SIGINT received.");
        QCoreApplication::quit();
    });


    QApplication app(argc, argv);


    MainWindow main_window(cfg);
    main_window.show();


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

    // Use the integrated decoder in CarPlayWidget instead of separate DecodeThread
    driver.register_frame_ready_callback([&main_window] (const uint8_t* buffer, uint32_t buffer_len){
        main_window.getCarPlayWidget().accept_new_data(buffer, buffer_len);
    });

    driver.register_audio_ready_callback([&audio_buffer] (const uint8_t* buffer, uint32_t buffer_len){
        audio_buffer->write(reinterpret_cast<const char*>(buffer), buffer_len);
    });

    // No longer need the Qt signal/slot connection for frame updates

    QObject::connect(&main_window,  &MainWindow::carplay_touch_event, [&driver] (TouchAction action, uint32_t x, uint32_t y) {
        driver.send_touch_event(action, x, y);
    });

    SPDLOG_INFO("Starting.");
    app.exec();  // Blocking.


    SPDLOG_WARN("Exit received, tearing down.");

    // Stop the integrated decoder
    main_window.getCarPlayWidget().stop_decoder();
    driver.stop();

    return 0u;
}