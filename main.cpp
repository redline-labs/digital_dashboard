#include "app_config.h"
#include "carplay_widget.h"
#include "carplay_stream.h"
#include "decode_thread.h"
#include "dongle_driver.h"
#include "messages/message.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <condition_variable>
#include <csignal>

#include <QApplication>
#include <QLabel>
#include <QMediaPlayer>
#include <QVideoWidget>

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


    auto cfg = load_app_config("/Users/ryan/src/carplay_cpp/config.yaml");

    DongleDriver driver(cfg, args_result["libusb_debug"].as<bool>());

    DecodeThread decode_thread;

    std::signal(SIGINT, [](int /* signum */)
        {
            SPDLOG_WARN("SIGINT received.");
            QCoreApplication::quit();
        });


    QApplication app(argc, argv);
    CarPlayWidget carplay_widget;
    carplay_widget.setSize(cfg.width_px, cfg.height_px);
    carplay_widget.show();

    /*CarPlayStream video_stream;


    QMediaPlayer player;
    QVideoWidget video_widget;
    player.setVideoOutput(&video_widget);

    player.setSourceDevice(&video_stream);
    video_widget.show();*/



    driver.register_frame_ready_callback([&decode_thread] (const uint8_t* buffer, uint32_t buffer_len){
        decode_thread.accept_new_data(buffer, buffer_len);
    });


    QObject::connect(&decode_thread,   &DecodeThread::imageReady,
                     &carplay_widget,  &CarPlayWidget::setPixmap);

    QObject::connect(&carplay_widget,  &CarPlayWidget::touchEvent, [&driver] (TouchAction action, uint32_t x, uint32_t y) {
        driver.send_touch_event(action, x, y);
    });

    SPDLOG_INFO("Starting.");
    app.exec();  // Blocking.





    SPDLOG_WARN("Exit received, tearing down.");

    decode_thread.stop();
    driver.stop();

    return 0u;
}