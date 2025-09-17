#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join
#include <cxxopts.hpp>
#include <zenoh.hxx>

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include "pub_sub/session_manager.h"
#include "pub_sub/schema_registry.h"

#include <capnp/serialize.h>
#include <capnp/dynamic.h>
#include <capnp/pretty-print.h>

static std::atomic<bool> g_running{true};

static void handle_sigint(int /*signum*/)
{
    g_running = false;
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    cxxopts::Options options("inspect", "Subscribe to a zenoh key and print raw payload bytes");
    options.add_options()
        ("k,key", "Zenoh key-expression to subscribe to", cxxopts::value<std::string>())
        ("h,help", "Print usage");

    cxxopts::ParseResult result;
    try
    {
        result = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        SPDLOG_INFO("{}", options.help());
        return 1;
    }

    if (result.count("help") || !result.count("key"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string keyexpr = result["key"].as<std::string>();
    SPDLOG_INFO("Subscribing to '{}'", keyexpr);

    // Set up SIGINT handler for graceful shutdown
    std::signal(SIGINT, handle_sigint);

    try
    {
        auto session = zenoh_session_manager::SessionManager::getOrCreate();
        if (!session)
        {
            SPDLOG_ERROR("Failed to obtain zenoh session");
            return 1;
        }

        auto key = zenoh::KeyExpr(keyexpr);

        // Keep subscriber in scope until program exits
        // Cache schema once discovered from encoding.
        bool found_schema = false;
        capnp::Schema cached_schema{};
        std::string cached_schema_name{};

        auto pretty_print_handler = [&](const zenoh::Sample& serialized_msg)
        {
            const std::vector<uint8_t> bytes = serialized_msg.get_payload().as_vector();

            // Attempt to lookup the schema if we haven't already.
            if (found_schema == false)
            {
                cached_schema_name = serialized_msg.get_encoding().as_string();
                cached_schema = get_schema(cached_schema_name);

                found_schema = cached_schema.getProto().getId() != 0;
                if (found_schema == true)
                {
                    SPDLOG_INFO("Using schema '{}' for decoding", cached_schema_name);
                }
                else
                {
                    SPDLOG_WARN("Schema '{}' not found in registry; falling back to hex dump", cached_schema_name);
                }
            }

            if (found_schema == true)
            {
                auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.data()), bytes.size() / sizeof(capnp::word));
                capnp::FlatArrayMessageReader reader(words);
                auto root = reader.getRoot<capnp::DynamicStruct>(cached_schema.asStruct());
                auto s = capnp::prettyPrint(root).flatten();
                SPDLOG_INFO("Decoded {} bytes: {}", bytes.size(), s.cStr());
            }
            else
            {
                SPDLOG_INFO("{} bytes: [{:02X}]", bytes.size(), fmt::join(bytes, ", "));
            }
        };

        zenoh::Subscriber<void> sub = session->declare_subscriber(key, pretty_print_handler, zenoh::closures::none);

        while (g_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Subscription error: {}", e.what());
        return 1;
    }

    SPDLOG_INFO("Exiting.");
    return 0;
}


