// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real model -- NGS's SDEFLEC2022 North America grids, verbatim, from
// models/deflec2022 -- at every one of NGS's published test points inside
// them. The files are float32 exactly as NGS computed them, so the only
// difference allowed is NGS's rounding of the published values: a wrong byte
// order, grid orientation, component, or interpolation kernel is off by
// arcseconds (bilinear by up to 3", other bicubics by 1.5").
//
// The directory resolves as the node's does, through core::paths::resource.
// A checkout that never ran `git lfs pull` has pointer files there: that is a
// skip, said loudly, not a failure and not a pass.

#include "deflec/model.h"

#include "core/core.h"

#include <cmath>
#include <cstdio>
#include <numbers>

namespace {

struct NgsPoint {
    const char* name;
    double lat, lon, eta, xi;  // degrees (longitude east, 0-360); arcseconds
};

// NAPGD2022TestCases.beta_v0a.csv (NGS, 2026-04-23): every point inside the
// North America grid, coordinates to NGS's eight decimals -- on Mount
// Whitney's slope four decimals is already 0.01" off.
constexpr NgsPoint kPoints[] = {
    {"Center", 45.0, 260.0, 0.285, -3.044},
    {"New York", 40.71296814, -74.01361774, -5.878, -2.438},
    {"Los Angeles", 34.05875104, -118.2785129, -4.855, -16.458},
    {"Toronto", 43.64258362, -79.3871521, 0.74, -1.507},
    {"Mexico City", 19.433333, -99.133333, -1.033, 1.464},
    {"Chicago", 41.881944, -87.627778, 1.073, 2.117},
    {"Dallas", 32.7695729, -96.80769497, -6.953, -0.672},
    {"Houston", 29.75706116, -95.37304523, -0.766, 1.329},
    {"Washington DC", 38.8894787, -77.03534209, 3.521, -3.532},
    {"Philadelphia", 39.96518095, -75.18037257, -1.996, 3.228},
    {"Atlanta", 33.74497362, -84.39056463, -0.948, 1.594},
    {"Miami", 25.76255582, -80.20132012, 7.901, -2.326},
    {"San Diego", 32.71835537, -117.1612133, -9.806, -8.129},
    {"Monterrey", 25.7111101, -100.3130365, 8.169, 8.912},
    {"Phoenix", 33.44534417, -112.0653013, -4.032, 0.011},
    {"Boston", 42.35972149, -71.05693588, -3.506, -5.478},
    {"Detroit", 42.33687333, -83.04910534, 2.198, -2.408},
    {"San Francisco", 37.78808605, -122.4064385, -1.684, -2.305},
    {"Montr al", 45.46980254, -73.6849855, -2.422, 5.061},
    {"Santo Domingo", 18.46160711, -69.93727393, 11.027, -17.872},
    {"Seattle", 47.62053517, -122.3492345, 2.288, 2.66},
    {"Minneapolis", 44.96021753, -93.27376282, 3.68, 0.506},
    {"Tampa", 27.95163703, -82.456438, 2.315, 2.301},
    {"Puebla", 19.0393685, -98.20969377, -4.184, -4.424},
    {"San Jose", 37.3351424, -121.8888423, -2.767, 0.357},
    {"Guatemala City", 14.63385611, -90.51333154, 0.153, -1.353},
    {"Denver", 39.75320284, -105.0001604, 10.758, 2.592},
    {"Vancouver", 49.27247776, -123.1242905, -1.758, -11.844},
    {"Baltimore", 39.28561704, -76.61091192, 6.566, -3.161},
    {"St. Louis", 38.62474069, -90.18482335, -1.138, 4.617},
    {"Orlando", 28.53536725, -81.38297247, 2.982, -1.615},
    {"Charlotte", 35.22721204, -80.84244192, -2.787, 2.343},
    {"San Antonio", 29.43214086, -98.50037596, 5.926, -0.773},
    {"Port-au-Prince", 18.57946264, -72.292591, 3.62, 8.565},
    {"Portland", 45.58955224, -122.5977557, -2.002, -1.647},
    {"Pittsburgh", 40.44170823, -80.01160035, -2.043, 2.774},
    {"Austin", 30.26657185, -97.74313734, 3.674, -1.881},
    {"Sacramento", 38.57624161, -121.493796, -2.736, -2.81},
    {"San Salvador", 13.70473372, -89.20875315, 1.855, -5.618},
    {"El Paso Ju rez", 31.69139438, -106.4253631, 3.218, -1.205},
    {"Las Vegas", 36.17042244, -115.155567, 4.788, -1.46},
    {"Cincinnati", 39.09581497, -84.51600381, 0.76, -0.844},
    {"Kansas City", 39.114289, -94.61007331, 1.214, 0.479},
    {"Columbus", 39.96053992, -82.99884803, 5.696, -0.283},
    {"Cleveland", 41.49797824, -81.69428831, -1.517, 3.835},
    {"Indianapolis", 39.77173413, -86.15700531, 3.843, 3.483},
    {"Anchorage", 61.20249452, -150.0174511, -19.877, 3.256},
    {"New Orleans", 29.96572295, -90.10997437, 0.139, 4.291},
    {"Nashville", 36.16240659, -86.7808981, -0.163, 3.58},
    {"Mount McKinley", 63.069167, -151.006389, -4.375, -30.466},
    {"Mount Logan", 60.567222, -140.405278, 9.782, -46.217},
    {"Pico de Orizaba", 19.0327113, -97.2773811, 5.394, -2.356},
    {"Mount Whitney", 36.5785071, -118.2974098, 12.508, -0.846},
    {"Mount Saint Elias", 60.2933381, -140.9345943, -1.183, -52.075},
    {"Mount Elbert", 39.1178231, -106.4549724, 2.538, -0.991},
    {"San Juan", 18.4042535, -66.1016083, -0.237, 35.747},
    {"Badwater Basin", 36.250278, -116.825833, -8.193, -5.795},
    {"Laramie WY", 41.3066979, -105.6332559, 2.585, 0.643},
    {"Mauna Kea", 19.8206202, -155.4732435, 10.631, 18.056},
    {"Nuuk", 64.1791383, -51.748009, -1.948, 3.198},
    {"Gunnbjorn Fjeld", 68.9166701, -29.788483, -11.231, 9.669},
    {"Puerto Rico Trench", 19.583333, -66.5, 2.716, -11.493},
    {"Jakobshavn Glacier", 69.1666663, -49.9166666, -11.689, 0.588},
    {"Hudson Bay", 58.0, -88.0, -1.427, -0.151},
    {"Mount Whitney", 36.57858641, -118.2920147, 15.605, 0.019},
    {"Table Mountain", 40.13074713, -105.2326524, 22.985, -3.024},
    {"Death Valley", 36.20688598, -116.8699532, 5.184, -3.866},
    {"Alaska", 61.20357127, -145.5282415, -1.078, -2.104},
    {"Puerto Rico", 18.45982339, -66.1165986, -1.292, 40.908},
    {"Midway", 28.20918153, -177.3716912, 2.183, -6.283},
    {"Meades Ranch", 39.22409289, -98.54216317, 3.773, -1.003},
    {"Rimouski", 48.47501082, -68.51222553, -7.548, 5.96},
};

constexpr double kTolerance = 0.0006;  // NGS publishes to 0.001"

}  // namespace

int main() {
    const std::string dir = core::paths::resource("models/deflec2022");
    auto model = deflec::Model::open(dir);
    if (!model) {
        std::fprintf(stderr, "SKIP: %s\n", model.error().message.c_str());
        const bool absent = model.error().error == deflec::LoadError::lfs_pointer ||
                            model.error().error == deflec::LoadError::not_found;
        return absent ? PROJECT_TEST_SKIP_CODE : 1;  // absent is a skip; damaged is a failure
    }
    constexpr double kRad = std::numbers::pi / 180.0;
    int failures = 0;
    double worst = 0.0;
    for (const auto& p : kPoints) {
        const auto d = model->at(p.lat * kRad, p.lon * kRad);
        if (!d) {
            std::fprintf(stderr, "FAIL: %s is outside the model\n", p.name);
            ++failures;
            continue;
        }
        const double e = std::max(std::fabs(d->eta_arcsec - p.eta), std::fabs(d->xi_arcsec - p.xi));
        worst = std::max(worst, e);
        if (e > kTolerance) {
            std::fprintf(stderr, "FAIL: %s: eta %.4f (NGS %.3f), xi %.4f (NGS %.3f)\n", p.name, d->eta_arcsec, p.eta,
                         d->xi_arcsec, p.xi);
            ++failures;
        }
    }
    std::printf("%zu NGS test points: worst difference %.5f\"\n", std::size(kPoints), worst);
    // Outside: south of the grid, and west of it.
    if (model->at(-1.0 * kRad, 250.0 * kRad) || model->at(40.0 * kRad, 160.0 * kRad)) {
        std::fprintf(stderr, "FAIL: a point outside the grid answered\n");
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("ngs: all passed");
    return 0;
}
