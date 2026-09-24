#pragma once

// A deflection-of-the-vertical model: NGS's two component grids, loaded at
// run time from a directory, and the gravity it makes.
//
// The deflection is how far the plumb line (real gravity) leans off the
// ellipsoid normal (normal gravity): xi north-south, eta east-west, in
// arcseconds, NGS's signs -- xi = -dN/dnorth / R, eta = -dN/deast / R for a
// geoid height N, which the geoid confirms. Gravity's horizontal components are
// then -g xi north and -g eta east.
//
// The library knows nothing about where the files are; the node resolves the
// directory (core::paths::resource("models/deflec2022"), or its config) and
// passes it in.

#include "deflec/ngs_grid.h"

#include "geodesy/gravity.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace deflec {

struct Deflection {
    double xi_arcsec = 0.0;   // north-south (meridional)
    double eta_arcsec = 0.0;  // east-west (prime vertical)
};

class Model {
  public:
    // SDEFLEC2022 over North America: <dir>/SDEFLEC2022.NA.{eta,xi}.beta_v0a.bin.
    static constexpr const char* kEtaFile = "SDEFLEC2022.NA.eta.beta_v0a.bin";
    static constexpr const char* kXiFile = "SDEFLEC2022.NA.xi.beta_v0a.bin";
    static std::expected<Model, LoadFailure> open(const std::filesystem::path& dir);

    // The deflection at geodetic latitude and longitude, radians, or nothing
    // outside the model.
    std::optional<Deflection> at(double lat_rad, double lon_rad) const;

    const NgsGrid& eta() const { return eta_; }
    const NgsGrid& xi() const { return xi_; }

  private:
    Model(NgsGrid eta, NgsGrid xi) : eta_(std::move(eta)), xi_(std::move(xi)) {}
    NgsGrid eta_, xi_;
};

// Normal gravity tilted by the deflection where the model covers, normal
// gravity alone where it does not.
class DeflectedGravity final : public geodesy::GravityModel {
  public:
    explicit DeflectedGravity(std::shared_ptr<const Model> model) : model_(std::move(model)) {}
    csym::Vector3<double> gravityEcef(const csym::Vector3<double>& p_e) const override;
    bool refinesNormalAt(const csym::Vector3<double>& p_e) const override;

  private:
    std::shared_ptr<const Model> model_;
};

}  // namespace deflec
