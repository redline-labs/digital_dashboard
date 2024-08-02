#include "app_config.h"
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
    decode_thread.start();


    std::signal(SIGINT, [](int /* signum */)
        {
            SPDLOG_WARN("SIGINT received.");
            QCoreApplication::quit();
        });


    //std::unique_lock<std::mutex> lk(cv_m);
    //cv.wait(lk);



    QApplication app(argc, argv);
    QLabel label("Waiting....");
    label.show();


    driver.register_frame_ready_callback([&decode_thread] (const uint8_t* buffer, uint32_t buffer_len){
        decode_thread.accept_new_data(buffer, buffer_len);
    });


     QObject::connect(&decode_thread,   &DecodeThread::imageReady,
                     &label,            &QLabel::setPixmap);


    app.exec();  // Blocking.





    SPDLOG_WARN("Tearing down driver.");

    decode_thread.requestInterruption();
    driver.stop();

    decode_thread.wait();

    return 0u;
}