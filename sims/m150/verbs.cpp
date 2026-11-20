#include "verbs.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "agent_control/log_ring.h"
#include "agent_control/mcp_option.h"
#include "cli/interrupt.h"
#include "host/server.h"
#include "m150/top/candump.h"
#include "m150/top/served.h"
#include "node_health/reporter.h"
#include "pub_sub/node_identity.h"

namespace m150::cli {

using namespace ::sim;

namespace {

void addImageOption(cxxopts::Options& options) {
    options.add_options()("image", "A raw image of the internal flash (a kernel and an M1 application); default "
                                   "$M150_IMAGE",
                          cxxopts::value<std::string>())(
        "engine", "CPU engine: interp, block or jit (default jit)", cxxopts::value<std::string>());
}

std::string image_or_env(const std::string& image) {
    if (!image.empty()) {
        return image;
    }
    const char* env = std::getenv("M150_IMAGE");
    return env ? env : "";
}

}  // namespace

void addServeOptions(cxxopts::Options& options) {
    options.add_options()("c,config", "The serve config (YAML); see sims/host/serve_config.h",
                          cxxopts::value<std::string>())(
        "stepped", "Time moves only when an agent says (run, step), not with the wall clock")(
        "symbols", "Symbol files for the debugger: the image's ELF files (kernel, application), comma-separated",
        cxxopts::value<std::string>())("allow-write", "Let agents write memory and registers");
    addImageOption(options);
    agent_control::addMcpOption(options, "m150_sim");
}

int runServe(::cli::Context& context) {
    const sys::ServeSchema& schema = ::sim::m150::m150_serve_schema();
    sys::ServeOptions options;
    options.schema = schema;
    options.config = sys::default_serve_config(schema);
    if (context.has("config")) {
        auto config = sys::load_serve_config(schema, context.stringOr("config", ""));
        if (!config) {
            return ::cli::kFailure;
        }
        options.config = std::move(*config);
    }
    for (const char* key : {"image", "symbols", "engine"}) {
        if (context.has(key)) {
            options.config.device[key] = context.stringOr(key, "");
        }
    }
    options.config.stepped = options.config.stepped || context.flag("stepped");
    if (const auto socket = agent_control::mcpSocketPath(context.args(), schema.app)) {
        options.control_socket = *socket;
        agent_control::installLogRing();
    }

    const std::string image = image_or_env(options.config.device_value("image"));
    const std::string engine_text = options.config.device_value("engine");
    const auto engine = ::sim::m150::parse_engine(engine_text.empty() ? "jit" : engine_text);
    if (image.empty() || !engine) {
        SPDLOG_ERROR(image.empty() ? "no image: --image, image: in the config, or $M150_IMAGE"
                                   : "engine: not interp, block or jit");
        return ::cli::kFailure;
    }

    pub_sub::NodeIdentity identity(schema.app);
    node_health::HealthReporter health(schema.app);
    ::cli::installInterruptHandler();

    std::unique_ptr<sys::DeviceServer> server;
    try {
        ::sim::m150::M150Options device;
        device.image = image;
        device.symbols = ::sim::m150::split_paths(options.config.device_value("symbols"));
        device.engine = *engine;
        device.allow_write = context.flag("allow-write");
        server = std::make_unique<sys::DeviceServer>(options, std::make_unique<::sim::m150::M150Served>(device));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("{}", e.what());
        return ::cli::kFailure;
    }
    server->run(
        [&]() {
            SPDLOG_INFO("[sim] m150: {}, {} time", server->describe_ports(),
                        server->options().config.stepped ? "stepped" : "real");
            health.markReady();
            if (!options.control_socket.empty()) {
                agent_control::announceReady(options.control_socket);
            }
        },
        [&]() { health.kick(); });
    SPDLOG_INFO("[sim] stopping");
    return ::cli::kOk;
}

void addRunOptions(cxxopts::Options& options) {
    addImageOption(options);
    options.add_options()("seconds", "Simulated seconds to run (default 2)", cxxopts::value<double>())(
        "candump-in", "Frames to send onto the buses (candump log; canN is bus N), the first at 0.3 s",
        cxxopts::value<std::string>());
    options.parse_positional({"image"});
    options.positional_help("IMAGE");
}

int runRun(::cli::Context& context) {
    const std::string image = image_or_env(context.stringOr("image", ""));
    const auto engine = ::sim::m150::parse_engine(context.stringOr("engine", "jit"));
    if (image.empty() || !engine) {
        std::fprintf(stderr, "m150_sim run: %s\n", image.empty() ? "no image" : "engine: not interp, block or jit");
        return ::cli::kUsage;
    }
    e200::MachineConfig cfg;
    cfg.engine = *engine;
    ::sim::m150::Board board(cfg);
    try {
        board.load_flash_file(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "m150_sim: %s\n", e.what());
        return ::cli::kFailure;
    }
    std::string line;
    board.soc.esci_a->on_byte = [&line](uint8_t b) {
        if (b == '\n') {
            std::fprintf(stderr, "%s\n", line.c_str());
            line.clear();
        } else if (b != '\r') {
            line += char(b);
        }
    };
    const uint64_t hz = board.m.cfg.core_hz;
    auto print = [hz](const sys::CanFrame& f, int bus, uint64_t now) {
        std::printf("%s\n", ::sim::m150::format_candump(f, bus, now, hz).c_str());
    };
    ::sim::m150::CanListener printers[3] = {{0, print}, {1, print}, {2, print}};
    for (int k = 0; k < 3; ++k) {
        board.soc.can_bus[size_t(k)].attach(printers[k]);
    }
    if (!board.reset()) {
        return ::cli::kFailure;
    }
    if (context.has("candump-in")) {
        try {
            board.m.add(std::make_shared<::sim::m150::CanInjector>(
                board, ::sim::m150::read_candump(context.stringOr("candump-in", ""), hz)));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "m150_sim: %s\n", e.what());
            return ::cli::kFailure;
        }
    }

    const double seconds = context.doubleOr("seconds", 2.0);
    const auto t0 = std::chrono::steady_clock::now();
    const e200::Stop stop = board.m.run_until(uint64_t(seconds * double(hz)));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!line.empty()) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    std::fflush(stdout);
    std::fprintf(stderr,
                 "m150_sim: %.3f s simulated in %.3f s; stopped (%d) at pc 0x%08x; frames: can0 %llu, can1 %llu, "
                 "can2 %llu\n",
                 double(board.m.now()) / double(hz), wall, int(stop.kind), board.m.state.pc,
                 static_cast<unsigned long long>(printers[0].frames),
                 static_cast<unsigned long long>(printers[1].frames),
                 static_cast<unsigned long long>(printers[2].frames));
    return stop.kind == e200::StopKind::Limit ? ::cli::kOk : ::cli::kFailure;
}

}  // namespace m150::cli
