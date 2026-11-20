#include "host/debug/session.h"

namespace sim::sys {

nlohmann::json RunResult::to_json() const {
    nlohmann::json j{{"reason", reason}, {"t", t}, {"pc", pc}, {"where", where}};
    if (id) {
        j["id"] = *id;
    }
    return j;
}

RunResult DebugSession::result(const std::string& reason, std::optional<int> id) {
    RunResult r;
    r.reason = reason;
    r.t = target_.seconds();
    r.pc = target_.pc();
    r.where = debugger_.where(r.pc);
    r.id = id;
    return r;
}

RunResult DebugSession::run_to_tick(uint64_t tick) {
    target_.clear_exit();
    for (;;) {
        const TargetStop stop = target_.run_until(tick);
        switch (stop.kind) {
            case TargetStopKind::PcWatch:
                target_.skip_pc_once(stop.pc);  // the next run passes it
                if (debugger_.on_pc(stop.pc)) {
                    return result("break", debugger_.last_breakpoint());
                }
                continue;
            case TargetStopKind::DataWatch:
                return result("watch", debugger_.take_watch_hit());
            case TargetStopKind::Exit:
                return result("exit");
            case TargetStopKind::Fault:
                return result("fault");
            case TargetStopKind::Time:
                return result("time");
        }
    }
}

RunResult DebugSession::run_until(double t_seconds) {
    return run_to_tick(target_.ticks(t_seconds));
}

RunResult DebugSession::run_for(double seconds) {
    return run_to_tick(target_.now() + target_.ticks(seconds));
}

bool DebugSession::step_one(RunResult* stopped) {
    target_.skip_pc_once(target_.pc());
    // a sleeping CPU: time passes until an interrupt wakes it (at most a second)
    const TargetStop stop = target_.step_instruction(target_.ticks(1.0));
    switch (stop.kind) {
        case TargetStopKind::DataWatch:
            *stopped = result("watch", debugger_.take_watch_hit());
            return false;
        case TargetStopKind::Exit:
            *stopped = result("exit");
            return false;
        case TargetStopKind::Fault:
            *stopped = result("fault");
            return false;
        case TargetStopKind::PcWatch:
        case TargetStopKind::Time:
            break;
    }
    if (!target_.stepped()) {
        *stopped = result("time");  // asleep for the whole second
        return false;
    }
    return true;
}

RunResult DebugSession::step(int count, bool over) {
    RunResult stopped;
    for (int k = 0; k < count; k++) {
        const Decoded insn = target_.decode(target_.pc());
        if (over && insn.call) {
            const uint64_t back = target_.pc() + insn.length;
            const uint64_t sp = target_.stack_pointer();
            if (!step_one(&stopped)) {
                return stopped;
            }
            // run to the return address at this stack depth
            while (!(target_.pc() == back && target_.stack_pointer() >= sp)) {
                if (!step_one(&stopped)) {
                    return stopped;
                }
            }
            continue;
        }
        if (!step_one(&stopped)) {
            return stopped;
        }
    }
    return result("step");
}

RunResult DebugSession::finish() {
    // the return that leaves this function: calls made from here and their returns are counted out
    RunResult stopped;
    const uint64_t sp = target_.stack_pointer();
    int depth = 0;
    for (int k = 0; k < 10'000'000; k++) {
        const Decoded insn = target_.decode(target_.pc());
        if (!step_one(&stopped)) {
            return stopped;
        }
        if (insn.call) {
            depth++;
        } else if (insn.ret) {
            if (depth == 0 && target_.stack_pointer() >= sp) {
                return result("finish");
            }
            depth = depth > 0 ? depth - 1 : 0;
        }
    }
    return result("time");
}

void DebugSession::snapshot(const std::string& name) {
    snapshots_[name] = target_.snapshot();
}

bool DebugSession::restore(const std::string& name) {
    const auto it = snapshots_.find(name);
    if (it == snapshots_.end()) {
        return false;
    }
    target_.restore(it->second);
    return true;
}

nlohmann::json DebugSession::snapshot_names() const {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [name, saved] : snapshots_) {
        out.push_back(name);
    }
    return out;
}

}  // namespace sim::sys
