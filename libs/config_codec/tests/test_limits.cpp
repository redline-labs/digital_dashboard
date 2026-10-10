// config_codec::applyLimits: every validate() hook in a config runs once, the
// nested ones before their parent, and each note says which struct it came
// from.
//
// "Exactly once" is the property worth a test. A parent that also calls its
// child's validate() clamps twice and reports twice, and nothing else notices:
// the clamp is idempotent, so the config comes out right and only the log is
// wrong. That is how map style notes were doubled before this existed.

#include "config_codec/config_apply_limits.h"
#include "config_codec/config_limits.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

REFLECT_STRUCT(Leaf,
    (int, level, 0)
)

inline std::vector<std::string> validate(Leaf& leaf)
{
    std::vector<std::string> notes;
    config_codec::limits::clampInto(leaf.level, 0, 10, "level", notes);
    return notes;
}

REFLECT_STRUCT(Parent,
    (Leaf, single, Leaf{}),
    (std::vector<Leaf>, many, {}),
    // Copied from `single` by the parent's own rule, which must see the
    // child's clamped value.
    (int, mirrored, 0)
)

inline std::vector<std::string> validate(Parent& parent)
{
    parent.mirrored = parent.single.level;
    return {};
}

REFLECT_STRUCT(NoHooks,
    (int, anything, 99)
)

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::size_t countContaining(const std::vector<std::string>& notes, const std::string& text)
{
    return static_cast<std::size_t>(std::count_if(notes.begin(), notes.end(), [&](const std::string& n)
    {
        return n.find(text) != std::string::npos;
    }));
}

void testNestedHooksRunWithTheirPath()
{
    Parent parent;
    parent.single.level = 50;
    parent.many.resize(3);
    parent.many[2].level = -4;

    const std::vector<std::string> notes = config_codec::applyLimits(parent);

    check(parent.single.level == 10, "a nested struct's own hook clamps it");
    check(parent.many[2].level == 0, "a vector element's hook clamps it");
    check(countContaining(notes, "single: level was 50") == 1,
          "the nested note names its struct, once");
    check(countContaining(notes, "many[2]: level was -4") == 1,
          "the element note names its index, once");
    check(notes.size() == 2, "nothing was reported that did not change");
}

void testChildrenBeforeParent()
{
    Parent parent;
    parent.single.level = 50;
    (void)config_codec::applyLimits(parent);
    check(parent.mirrored == 10, "the parent's rule saw its child already clamped");
}

void testAStructWithNoHooksIsUntouched()
{
    NoHooks plain;
    check(config_codec::applyLimits(plain).empty() && plain.anything == 99,
          "no hook, no change, no note");
}

}  // namespace

int main()
{
    testNestedHooksRunWithTheirPath();
    testChildrenBeforeParent();
    testAStructWithNoHooksIsUntouched();

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
