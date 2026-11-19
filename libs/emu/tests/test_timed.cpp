// emu::Timed / emu::Schedule: the order devices are synced in, repeated passes, and the earliest event.
#include <string>
#include <vector>

#include "emu/timed.h"
#include "mctest/test.h"

using namespace sim;
using namespace sim::sys;

namespace {

// A device with one pending event; records each sync into a shared log.
class Probe final : public Timed {
   public:
    Probe(std::string name, Timing timing, std::vector<std::string>& log) : Timed(std::move(name), timing), log_(log) {
    }
    uint64_t next = kNever;
    Probe* makes_due = nullptr;  // when synced, schedule this one at the same tick

    uint64_t next_event() const override {
        return next;
    }
    void sync(uint64_t now) override {
        log_.push_back(name() + "@" + std::to_string(now));
        if (next <= now) {
            next = kNever;
        }
        if (makes_due != nullptr) {
            makes_due->next = now;
            makes_due = nullptr;
        }
    }

   private:
    std::vector<std::string>& log_;
};

}  // namespace

TEST(schedule_inputs_before_timed_in_add_order) {
    std::vector<std::string> log;
    Probe t1("t1", Timing::Timed, log), in1("in1", Timing::Input, log), t2("t2", Timing::Timed, log),
        in2("in2", Timing::Input, log), passive("passive", Timing::Passive, log);
    Schedule s;
    for (Timed* d : std::vector<Timed*>{&t1, &in1, &t2, &passive, &in2}) {
        s.add(*d);
    }
    CHECK_EQ(s.order().size(), 4);  // the Passive device is never scheduled
    t1.next = t2.next = in1.next = in2.next = passive.next = 10;
    s.sync_due(10);
    CHECK(log == (std::vector<std::string>{"in1@10", "in2@10", "t1@10", "t2@10"}));
}

TEST(schedule_earliest_and_nothing_due) {
    std::vector<std::string> log;
    Probe a("a", Timing::Timed, log), b("b", Timing::Timed, log);
    Schedule s;
    CHECK_EQ(s.earliest(), kNever);  // empty
    s.add(a);
    s.add(b);
    CHECK_EQ(s.earliest(), kNever);
    a.next = 50;
    b.next = 20;
    CHECK_EQ(s.earliest(), 20);
    s.sync_due(19);  // nothing due yet
    CHECK(log.empty());
    s.sync_due(20);
    CHECK(log == (std::vector<std::string>{"b@20"}));
    CHECK_EQ(s.earliest(), 50);
}

TEST(schedule_a_sync_that_makes_an_earlier_device_due_runs_it_in_a_second_pass) {
    std::vector<std::string> log;
    Probe adc("adc", Timing::Timed, log), board("board", Timing::Timed, log);
    Schedule s;
    s.add(adc);
    s.add(board);
    board.next = 5;
    board.makes_due = &adc;  // adc was already passed over in this pass: only a second pass reaches it
    s.sync_due(5);
    CHECK(log == (std::vector<std::string>{"board@5", "adc@5"}));
    CHECK_EQ(s.earliest(), kNever);
}
