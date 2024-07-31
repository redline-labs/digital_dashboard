#include "app_config.h"
#include "dongle_driver.h"
#include "messages/message.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <condition_variable>
#include <csignal>

#include <QApplication>
#include <QLabel>
#include <QGraphicsScene>
#include <QPixmap>
#include <QGraphicsView>
#include <QGraphicsItem>

// Patches to third party:
// LibUSB core for debug messages.
// spdlog tweakme to lower the default log level.

std::condition_variable cv;
std::mutex cv_m;

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


    /*
    auto cmd = SendOpen(cfg);

    auto ret = cmd.serialize();

    for (const auto& byte : ret)
    {
        printf("%d ", byte);
    }*/


    //std::signal(SIGINT, [](int /* signum */)
    //    {
    //        SPDLOG_WARN("SIGINT received.");
    //        cv.notify_one();
    //    });


    //std::unique_lock<std::mutex> lk(cv_m);
    //cv.wait(lk);



    QApplication app(argc, argv);
    int imageHeight = 600;
    int imageWidth = 800;


    QGraphicsScene scene(0, 0, imageWidth, imageHeight);
    scene.addText("Hello, world!");

    QGraphicsView view(&scene);
    view.show();

    QImage image(imageWidth, imageHeight, QImage::Format_RGB32);

    driver.register_frame_ready_callback([&image, &scene, &view](const uint8_t* buffer, uint32_t buffer_len)
    {
        image.loadFromData(&buffer[0], buffer_len);
        scene.clear();
        scene.addPixmap(QPixmap::fromImage(image));
        view.update();
    });



    app.exec();  // Blocking.





    SPDLOG_WARN("Tearing down driver.");
    driver.stop();


    return 0u;
}