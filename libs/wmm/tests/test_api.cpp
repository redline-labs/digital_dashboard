// The compiled interface: this file sees only "wmm/wmmhr2025.h" (no #embed,
// no parser) and is built as C++20 to show that consumers need neither C++26
// nor the embedded data on their include path.
#include <cmath>
#include <cstdio>

#include "wmm/wmmhr2025.h"

#ifdef WMM_COF_H_INCLUDED
#error "the parser should not be reachable from the public header"
#endif

int main() {
    namespace hr = wmm::wmmhr2025;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };

    const hr::Model& model = hr::model();
    check(model.name() == "WMMHR-2025" && model.nmax == 133 && model.epoch == 2025.0, "model metadata");
    check(model.release_date == wmm::Date{2024, 11, 13} && model.valid_until() == 2030.0, "validity window");
    check(&model == &hr::model(), "model() returns the one instance");

    // First and last rows of WMMHR2025_TEST_VALUES.txt.
    const auto a = hr::magnetic_field({80.0, 0.0, 0.0}, 2025.0);
    check(std::fabs(a.X - 6517.4) < 0.05 && std::fabs(a.Z - 54701.3) < 0.05 && std::fabs(a.D - 1.27) < 0.005,
          "test point 1");
    const auto b = hr::magnetic_field({-80.0, 240.0, 100.0}, 2027.5);
    check(std::fabs(b.F - 51861.8) < 0.05 && std::fabs(b.GV - -52.12) < 0.005 && std::fabs(b.Z_dot - 88.2) < 0.05,
          "test point 12");

    check(hr::magnetic_field({48.0, 16.0, 0.0}, wmm::Date{2026, 1, 1}).F ==
              hr::magnetic_field({48.0, 16.0, 0.0}, 2026.0).F,
          "Date overload");
    check(hr::uncertainty(a).F == 134, "uncertainty");
    check(model(1, 0).g == -29351.7976 && model(133, 133).h == -0.0005, "coefficients");

    std::printf("%s\n", failures ? "compiled API: FAILED" : "compiled API: ok");
    return failures ? 1 : 0;
}
