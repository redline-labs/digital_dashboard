// SPDX-License-Identifier: GPL-3.0-or-later
//
// How checks fold into one state. The rule worth pinning is that a check nobody
// could evaluate never makes a node look healthy.
#include "node_health/state.h"

#include "check.h"

#include <array>
#include <set>
#include <string>
#include <vector>

using node_health::Check;
using node_health::State;

namespace
{

constexpr std::array kEvery{State::unknown, State::starting, State::ok,
                            State::degraded, State::fault, State::stopping};

Check make(State state)
{
    return Check{"c", state, {}, {}};
}

void testNames()
{
    std::set<std::string> names;
    for (State state : kEvery)
    {
        names.insert(std::string(node_health::to_string(state)));
    }
    test::check(names.size() == kEvery.size(), "every state has its own name");
}

void testSeverityOrder()
{
    test::check(node_health::severity(State::ok) < node_health::severity(State::starting),
                "starting ranks above ok");
    test::check(node_health::severity(State::starting) == node_health::severity(State::stopping),
                "starting and stopping rank together");
    test::check(node_health::severity(State::degraded) == node_health::severity(State::unknown),
                "unknown ranks with degraded");
    test::check(node_health::severity(State::fault) > node_health::severity(State::degraded),
                "fault is the worst");
}

void testWorst()
{
    test::check(node_health::worst(std::vector<Check>{}) == State::ok, "no checks is ok");

    for (State state : kEvery)
    {
        const State expected = state == State::unknown ? State::degraded : state;
        test::check(node_health::worst(std::vector<Check>{make(state)}) == expected,
                    "a lone " + std::string(node_health::to_string(state)) + " check folds to " +
                        std::string(node_health::to_string(expected)));
    }

    test::check(node_health::worst(std::vector<Check>{make(State::ok), make(State::unknown)}) ==
                    State::degraded,
                "an unknown check makes an otherwise ok node degraded");
    test::check(node_health::worst(std::vector<Check>{make(State::degraded), make(State::fault),
                                                      make(State::ok)}) == State::fault,
                "fault wins wherever it is");
}

}  // namespace

int main()
{
    testNames();
    testSeverityOrder();
    testWorst();
    return test::finish();
}
