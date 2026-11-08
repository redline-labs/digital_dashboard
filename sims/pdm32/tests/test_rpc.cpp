// The agent methods through the dispatcher (no socket): what a malformed call gets back, and a break/run/read flow on
// the firmware.
#include "firmware_path.h"
#include "mctest/test.h"
#include "pdm32/top/methods.h"
#include "pdm32/top/scenario.h"
#include "pdm32/top/sim_session.h"

#include <stdexcept>

using namespace sim::sys;

namespace {

struct Rig {
    std::unique_ptr<SimSession> session;
    agent_control::Dispatcher d{"pdm32_sim"};
    int id = 0;

    Rig() {
        Scenario s = load_scenario(std::string(PDM32_SCENARIO_DIR) + "/short_output.json");
        SessionOptions o;
        o.instance = instance_config_for(s, sim::test::firmware_hex());
        o.scenario = s;
        session = std::make_unique<SimSession>(o);
        register_methods(d, *session, ServerHooks{});
    }

    json call(const std::string& method, json params = json::object()) {
        const json request{{"jsonrpc", "2.0"}, {"id", ++id}, {"method", method}, {"params", params}};
        return json::parse(d.handleLine(request.dump()));
    }
};

}  // namespace

TEST(rpc_errors_carry_their_reason) {
    agent_control::Dispatcher d("pdm32_sim");
    ControlMethods methods(d);
    methods.add("ping", "", [](const json&) { return json{{"pong", true}}; });
    methods.add("mode", "", [](const json&) -> json {
        throw ControlError(ControlErrorReason::WrongMode, "stepped");
    });
    methods.add("need", "", [](const json& p) { return json{{"id", param_unsigned(p, "id")}}; });
    methods.add("fails", "", [](const json&) -> json { throw std::runtime_error("cannot open x"); });

    const auto reason = [&d](const std::string& line) {
        return json::parse(d.handleLine(line))["error"]["data"].value("reason", "<none>");
    };
    CHECK(reason("not json") == "BAD_PARAMS");
    CHECK(reason(R"({"jsonrpc":"2.0","id":1,"method":"nope"})") == "NO_SUCH_METHOD");
    CHECK(reason(R"({"id":1,"method":"mode"})") == "WRONG_MODE");
    CHECK(reason(R"({"id":1,"method":"need"})") == "BAD_PARAMS");
    CHECK(reason(R"({"id":1,"method":"need","params":{"id":"zz"}})") == "BAD_PARAMS");
    CHECK(reason(R"({"id":1,"method":"need","params":{"id":-1}})") == "BAD_PARAMS");
    CHECK(reason(R"({"id":1,"method":"fails"})") == "FAILED");
    const json r = json::parse(d.handleLine(R"({"jsonrpc":"2.0","id":2,"method":"need","params":{"id":"0x504"}})"));
    CHECK_EQ(r["result"]["id"].get<int>(), 0x504);
    CHECK_EQ(r["id"].get<int>(), 2);
}

TEST(rpc_can_send_refuses_malformed_frames) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    Rig rig;
    const auto reason = [&rig](json params) {
        return rig.call("can.send", std::move(params))["error"]["data"].value("reason", "<none>");
    };
    CHECK(reason({{"id", 0x123}, {"data", "zz"}}) == "BAD_PARAMS");
    CHECK(reason({{"id", 0x123}, {"data", "0102030405060708090A"}}) == "BAD_PARAMS");
    CHECK(reason({{"id", 0x123}, {"data", "abc"}}) == "BAD_PARAMS");
    CHECK(reason({{"id", 0x800}, {"extended", false}}) == "BAD_PARAMS");
    CHECK(reason({{"id", 0x20000000}}) == "BAD_PARAMS");
    const json ok = rig.call("can.send", {{"id", 0x123}, {"data", "01A0"}});
    CHECK(ok["result"]["ok"] == true);
    const json log = rig.call("can.recv", {{"id", 0x123}});
    CHECK(log["result"]["frames"].size() == 1);
    CHECK(log["result"]["frames"][0]["data"] == "01A0");
}

TEST(rpc_session_flow_break_run_read_short) {
    if (sim::test::firmware_hex().empty()) {
        mctest::skip("no firmware image");
        return;
    }
    Rig rig;
    json r = rig.call("hello");
    const json& methods = r["result"]["methods"];
    CHECK(std::find(methods.begin(), methods.end(), "load.set") != methods.end());

    r = rig.call("run", {{"until", 1.5}});
    CHECK(r["result"]["reason"] == "time");
    r = rig.call("read", {{"names", {"out[1].pin", "g_outputStatus[0]", "can:PDM_Output_Status_1"}}});
    CHECK_EQ(r["result"]["values"]["out[1].pin"].get<double>(), 1.0);
    CHECK_EQ(r["result"]["values"]["can:PDM_Output_Status_1"].get<double>(), 1.0);

    r = rig.call("break", {{"function", "UpdateOutputStateMachines"}});
    const int bp = r["result"]["id"].get<int>();
    r = rig.call("run", {{"seconds", 0.1}});
    CHECK(r["result"]["reason"] == "break");
    CHECK(r["result"]["where"] == "UpdateOutputStateMachines");
    rig.call("clear", {{"id", bp}});

    r = rig.call("load.set", {{"output", 1}, {"load", "short"}});
    CHECK(r["result"]["ok"] == true);
    r = rig.call("watch",
                 {{"name", "g_outputStatus[0]"}, {"on", "change"}, {"condition", "g_outputStatus[0] >= 0xA2"}});
    CHECK(r.contains("result"));
    r = rig.call("run", {{"seconds", 1.0}});
    CHECK(r["result"]["reason"] == "watch");
    r = rig.call("outputs");
    CHECK((r["result"][0]["status"].get<int>() & 0x02) != 0);

    r = rig.call("write", {{"name", "g_runMode"}, {"value", 1}});
    CHECK(r["error"]["data"]["reason"] == "BAD_PARAMS");  // memory writes need --allow-write
    r = rig.call("read", {{"names", {"nonsense_name"}}});
    CHECK(r["result"]["values"]["nonsense_name"].is_null());
    CHECK(r["result"]["errors"].contains("nonsense_name"));
}
