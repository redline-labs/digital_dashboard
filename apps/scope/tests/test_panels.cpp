// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window and panel layer against a real widget tree: adding and removing
// panels, what a panel will and will not accept, and the dock-state fallback.
//
// A `gui` test, so it runs offscreen. It uses a stub DataSource rather than the
// live zenoh one -- everything here is about widget behaviour, and needing a
// bus would make it a `net` test that skips itself on a machine without one,
// which is exactly the coverage you lose first and miss most.
//
// ONE executable, split into suites by area (test_panels_<area>.cpp) purely
// for navigability; the shared stubs live in test_panels_common.h. The ctest
// entry, the counters and the exit code are unchanged.

#include "test_panels_common.h"

int main(int argc, char** argv)
{
    // Forced here as well as by the test harness, so a manual run behaves the
    // same as a ctest one.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    spdlog::set_level(spdlog::level::off);

    panel_tests::runRegistryTests();
    panel_tests::runPlotTests();
    panel_tests::runTableTests();
    panel_tests::runWindowTests();
    panel_tests::runChromeTests();
    panel_tests::runGestureTests();
    panel_tests::runMapPanelTests();

    std::fprintf(stderr, "%d checks, %d failures\n", panel_tests::checks,
                 panel_tests::failures);
    return panel_tests::failures == 0 ? 0 : 1;
}
