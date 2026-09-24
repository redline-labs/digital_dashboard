#include "deflec/model.h"

#include "geodesy/geodetic.h"

#include <numbers>

namespace deflec {
namespace {
constexpr double kDeg = 180.0 / std::numbers::pi;
constexpr double kArcsec = std::numbers::pi / (180.0 * 3600.0);
}  // namespace

std::expected<Model, LoadFailure> Model::open(const std::filesystem::path& dir) {
    auto eta = NgsGrid::open(dir / kEtaFile);
    if (!eta) return std::unexpected(eta.error());
    auto xi = NgsGrid::open(dir / kXiFile);
    if (!xi) return std::unexpected(xi.error());
    if (!eta->sameGridAs(*xi))
        return std::unexpected(LoadFailure{LoadError::extent_differs,
                                           (dir / kEtaFile).string() + " and " + kXiFile + " are on different grids"});
    return Model(std::move(*eta), std::move(*xi));
}

std::optional<Deflection> Model::at(double lat_rad, double lon_rad) const {
    const auto e = eta_.at(lat_rad * kDeg, lon_rad * kDeg);
    const auto x = xi_.at(lat_rad * kDeg, lon_rad * kDeg);
    if (!e || !x) return std::nullopt;
    return Deflection{*x, *e};
}

csym::Vector3<double> DeflectedGravity::gravityEcef(const csym::Vector3<double>& p_e) const {
    const auto llh = geodesy::ecefToLlh(p_e);
    csym::Vector3<double> g = geodesy::normalGravityNed(llh.lat, llh.h);
    if (const auto d = model_ ? model_->at(llh.lat, llh.lon) : std::nullopt) {
        // The plumb line leans xi north and eta east of the normal: gravity,
        // which points down it, gains -g xi north and -g eta east.
        g[0] -= g[2] * d->xi_arcsec * kArcsec;
        g[1] -= g[2] * d->eta_arcsec * kArcsec;
    }
    return geodesy::rotEcefFromNed(llh.lat, llh.lon) * g;
}

bool DeflectedGravity::refinesNormalAt(const csym::Vector3<double>& p_e) const {
    if (!model_) return false;
    const auto llh = geodesy::ecefToLlh(p_e);
    return model_->at(llh.lat, llh.lon).has_value();
}

}  // namespace deflec
