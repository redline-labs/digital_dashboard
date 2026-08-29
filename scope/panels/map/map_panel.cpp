#include "map_panel/map_panel.h"

#include "scope/data_source.h"
#include "scope/time_base.h"

#include "map_render/tessellator.h"

// For the reflection-generated operator==, which is what lets applyConfig() tell
// a changed BINDING from a changed colour without a hand-written comparison
// that would rot against the config's fields.
#include "config_codec/config_yaml.h"
#include "qt_helpers/widget_colors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace scope
{
namespace
{

// A trail holds one position per fix, and a fix is 10-20 Hz at most on this bus.
// A 300 s window at 20 Hz is 6000 points; the cap is generous against a source
// that publishes faster.
constexpr std::size_t kMaxPointsPerSignal = 200000;

// The same headroom the plot and the table use, and for the same reason: one
// GUI tick at 30 Hz is 33 ms, and overflow means the GUI thread is wedged, at
// which point nothing is being drawn either way.
constexpr std::size_t kStagingCapacity = 4096;

// An extra ring of tiles around the viewport, fetched but not drawn. It is what
// makes a pan show map rather than background at the leading edge.
constexpr int kPrefetchRingTiles = 1;

// How many zoom levels below the drawn one to fetch as a coarse overview, so a
// jump to ground never visited at any zoom has an ancestor to stand in for it.
// One overview tile spans 16x16 of the drawn ones, so a viewport of forty is
// covered by one or two -- a rounding error on the request budget in exchange
// for never blanking.
//
// It must stay within substituteTiles()'s reach or the tiles would be fetched,
// cached and never looked at.
constexpr std::uint8_t kOverviewZoomDelta = 4;
static_assert(kOverviewZoomDelta <= map_render::kMaxSubstituteLevelsUp,
              "an overview deeper than substituteTiles() will look is fetched and never drawn");

// How much of a zoom level one wheel detent is worth, and the units Qt reports
// it in: eighths of a degree, with a detent at 15 degrees.
constexpr double kZoomPerWheelNotch = 0.5;
constexpr double kWheelUnitsPerNotch = 120.0;

// Points closer together than this on screen are redundant. Just under two
// pixels: below one the path corners visibly at a low zoom, above three a
// tight hairpin loses its shape.
constexpr double kThinPixels = 1.5;

const QColor kDiagnosticText("#C8CEDA");
const QColor kDiagnosticBackground(0, 0, 0, 170);
const QColor kLegendText("#C8CEDA");

// Viridis and Turbo, as a handful of anchors interpolated between. Not the full
// 256-entry tables: the eye cannot tell a piecewise-linear approximation of
// either from the real thing at the width of a track, and two 256-entry tables
// in a header is a lot of bytes to prove that.
struct RampStop
{
    double at;
    int r;
    int g;
    int b;
};

constexpr RampStop kViridis[] = {
    {0.00, 68, 1, 84},   {0.25, 59, 82, 139}, {0.50, 33, 145, 140},
    {0.75, 94, 201, 98}, {1.00, 253, 231, 37},
};

constexpr RampStop kTurbo[] = {
    {0.00, 48, 18, 59},   {0.25, 40, 187, 236}, {0.50, 160, 253, 58},
    {0.75, 253, 149, 39}, {1.00, 122, 4, 3},
};

QColor sampleRamp(const RampStop* stops, std::size_t count, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < count; ++i)
    {
        if (t <= stops[i].at)
        {
            const RampStop& lo = stops[i - 1];
            const RampStop& hi = stops[i];
            const double span = hi.at - lo.at;
            const double f = span > 0.0 ? (t - lo.at) / span : 0.0;
            return QColor(static_cast<int>(std::lround(lo.r + ((hi.r - lo.r) * f))),
                          static_cast<int>(std::lround(lo.g + ((hi.g - lo.g) * f))),
                          static_cast<int>(std::lround(lo.b + ((hi.b - lo.b) * f))));
        }
    }
    return QColor(stops[count - 1].r, stops[count - 1].g, stops[count - 1].b);
}

// Do these two bindings name THE SAME SIGNAL? The triple and nothing else --
// which is the identity the source issues a handle against, so two bindings
// equal here are two the source cannot tell apart.
bool sameSignal(const map_binding_t& a, const map_binding_t& b)
{
    return a.zenoh_key == b.zenoh_key && a.schema_type == b.schema_type &&
           a.value_expression == b.value_expression;
}

bool isBound(const map_binding_t& binding)
{
    return !binding.zenoh_key.empty() && !binding.value_expression.empty();
}

// Which fields carry latitude and longitude on the position schemas in this
// tree, so a TOPIC-level drop fills both roles in one go.
//
// A TABLE RATHER THAN A GUESS. Reading "the field called latitudeDeg" out of
// whatever was dropped would bind CarPlayLocation's `latitudeDeg` correctly and
// GsofInsFullNav's too, and then silently do nothing for a schema that spells
// it differently -- which reads as a drop that was ignored. Listing them means
// an unlisted schema is declined at the drop, and the user is told.
struct PositionSchema
{
    pub_sub::schema_type_t schema;
    const char* latitude;
    const char* longitude;
};

constexpr PositionSchema kPositionSchemas[] = {
    // The BD992's main position record, and what configs/dashboard/map_demo.yaml
    // points a map at.
    {pub_sub::schema_type_t::GsofLatLongHeight, "latitudeDeg", "longitudeDeg"},
    // The same fix with height above sea level rather than the ellipsoid.
    {pub_sub::schema_type_t::GsofLatLongMslHeight, "latitudeDeg", "longitudeDeg"},
    // Carries its own time and sigmas.
    {pub_sub::schema_type_t::GsofCodePosition, "latitudeDeg", "longitudeDeg"},
    // Inertial, which a BD992 never produces -- but the schema exists and a
    // receiver with an IMU would.
    {pub_sub::schema_type_t::GsofInsFullNav, "latitudeDeg", "longitudeDeg"},
    // The flat, self-contained one the phone provides.
    {pub_sub::schema_type_t::CarPlayLocation, "latitudeDeg", "longitudeDeg"},
};

const PositionSchema* positionSchemaFor(pub_sub::schema_type_t schema)
{
    for (const PositionSchema& entry : kPositionSchemas)
    {
        if (entry.schema == schema)
        {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

MapPanel::MapPanel(const config_t& cfg, DataSource& source, double history_seconds,
                   QWidget* parent)
    : Panel(parent), cfg_(cfg), source_(&source), history_seconds_(history_seconds)
{
    setMinimumSize(120, 90);
    setMouseTracking(false);

    // Null when QRhi found no backend for any of the platforms it tries. NOT
    // fatal: the trail and the marker are QPainter and still draw, which is the
    // difference between a map with no basemap and a panel that is not there.
    gpu_ = map_render::GpuRenderer::create();
    if (!gpu_)
    {
        SPDLOG_ERROR("Panel '{}': no GPU backend; the basemap will not draw.", cfg_.title);
    }

    rebindAll();
    openReaders();
}

MapPanel::~MapPanel()
{
    releaseAll();
}

// ------------------------------------------------------------------ bindings

const map_binding_t& MapPanel::bindingFor(Role role) const
{
    switch (role)
    {
        case Role::Latitude:
            return cfg_.latitude;
        case Role::Longitude:
            return cfg_.longitude;
        case Role::Color:
            break;
    }
    return cfg_.color_by;
}

map_binding_t& MapPanel::bindingFor(Role role)
{
    return const_cast<map_binding_t&>(std::as_const(*this).bindingFor(role));
}

const MapPanel::Signal& MapPanel::signalFor(Role role) const
{
    switch (role)
    {
        case Role::Latitude:
            return latitude_;
        case Role::Longitude:
            return longitude_;
        case Role::Color:
            break;
    }
    return color_;
}

MapPanel::Signal& MapPanel::signalFor(Role role)
{
    return const_cast<Signal&>(std::as_const(*this).signalFor(role));
}

void MapPanel::bindRole(Role role, const map_binding_t& binding)
{
    Signal& signal = signalFor(role);
    signal.binding = binding;
    signal.handle = kInvalidSignal;
    signal.bound = false;
    signal.buffer.reset();

    if (!isBound(binding))
    {
        return;
    }

    signal.buffer =
        std::make_shared<SignalBuffer>(history_seconds_, kMaxPointsPerSignal, kStagingCapacity);

    SignalKey key;
    key.zenoh_key = binding.zenoh_key;
    key.schema_type = binding.schema_type;
    key.value_expression = binding.value_expression;

    signal.handle = source_->bind(key, signal.buffer);
    signal.bound = signal.handle != kInvalidSignal;

    if (!signal.bound)
    {
        // Already logged in detail by the evaluator; this says which panel.
        SPDLOG_WARN("Panel '{}': signal '{}' on '{}' could not be bound.", cfg_.title,
                    binding.value_expression, binding.zenoh_key);
    }
}

void MapPanel::rebindAll()
{
    releaseAll();
    bindRole(Role::Latitude, cfg_.latitude);
    bindRole(Role::Longitude, cfg_.longitude);
    bindRole(Role::Color, cfg_.color_by);
}

void MapPanel::releaseAll()
{
    for (Signal* signal : {&latitude_, &longitude_, &color_})
    {
        if (signal->handle != kInvalidSignal)
        {
            source_->release(signal->handle);
            signal->handle = kInvalidSignal;
        }
        signal->bound = false;
    }
}

bool MapPanel::acceptsBinding(const BindingCandidate& candidate) const
{
    if (candidate.zenoh_key.empty() || candidate.schema_name.empty())
    {
        return false;
    }

    const auto schema =
        reflection::enum_traits<pub_sub::schema_type_t>::try_from_string(candidate.schema_name);
    if (!schema)
    {
        return false;
    }

    if (candidate.isTopicLevel())
    {
        // THE DROP THAT JUST WORKS. A recognised position topic fills latitude
        // and longitude together, which is the mirror of the video panel's
        // topic-level accept -- and the only sensible reading of dropping
        // `nodes/bd992/gsof/lat_long_height` onto a map.
        //
        // Declined once both are already bound: silently replacing a position
        // the user chose is worse than refusing the drop.
        return positionSchemaFor(*schema) != nullptr &&
               !(isBound(cfg_.latitude) && isBound(cfg_.longitude));
    }

    if (!candidate.isNumeric())
    {
        return false;
    }

    // Field-level drops fill the first empty role, latitude then longitude then
    // colour. Once all three are full there is nothing left for one to mean.
    if (!isBound(cfg_.latitude) || !isBound(cfg_.color_by))
    {
        // Longitude MUST share latitude's topic: every position schema carries
        // the pair on one message, and that shared message timestamp is the only
        // thing they pair on. Accepting a longitude from elsewhere would bind
        // cleanly and then draw nothing, with thousands of unpaired samples --
        // which reads as a broken panel rather than as a wrong drop.
        if (isBound(cfg_.latitude) && !isBound(cfg_.longitude))
        {
            return candidate.zenoh_key == cfg_.latitude.zenoh_key;
        }
        return true;
    }

    if (!isBound(cfg_.longitude))
    {
        return candidate.zenoh_key == cfg_.latitude.zenoh_key;
    }

    return false;
}

bool MapPanel::addBinding(const BindingCandidate& candidate)
{
    if (!acceptsBinding(candidate))
    {
        return false;
    }

    const auto schema =
        reflection::enum_traits<pub_sub::schema_type_t>::try_from_string(candidate.schema_name);
    if (!schema)
    {
        SPDLOG_WARN("Panel '{}': schema '{}' is not in the registry.", cfg_.title,
                    candidate.schema_name);
        return false;
    }

    if (candidate.isTopicLevel())
    {
        const PositionSchema* position = positionSchemaFor(*schema);
        if (position == nullptr)
        {
            return false;
        }

        cfg_.latitude.zenoh_key = candidate.zenoh_key;
        cfg_.latitude.schema_type = *schema;
        cfg_.latitude.value_expression = position->latitude;

        cfg_.longitude.zenoh_key = candidate.zenoh_key;
        cfg_.longitude.schema_type = *schema;
        cfg_.longitude.value_expression = position->longitude;

        bindRole(Role::Latitude, cfg_.latitude);
        bindRole(Role::Longitude, cfg_.longitude);
        emit configChanged();
        update();
        return true;
    }

    map_binding_t binding;
    binding.zenoh_key = candidate.zenoh_key;
    binding.schema_type = *schema;
    binding.value_expression = candidate.defaultExpression();

    const Role role = !isBound(cfg_.latitude)    ? Role::Latitude
                      : !isBound(cfg_.longitude) ? Role::Longitude
                                                 : Role::Color;

    bindingFor(role) = binding;
    bindRole(role, binding);
    emit configChanged();
    update();
    return true;
}

std::vector<QString> MapPanel::bindingLabels() const
{
    std::vector<QString> labels;
    // The ROLE is named, because unlike a plot's traces these three are not
    // interchangeable and "remove the second one" is otherwise a guess.
    const std::pair<const char*, const map_binding_t*> roles[] = {
        {"latitude", &cfg_.latitude},
        {"longitude", &cfg_.longitude},
        {"colour", &cfg_.color_by},
    };
    for (const auto& [name, binding] : roles)
    {
        if (isBound(*binding))
        {
            labels.push_back(QStringLiteral("%1 — %2")
                                 .arg(QString::fromUtf8(name),
                                      QString::fromStdString(binding->value_expression)));
        }
    }
    return labels;
}

std::size_t MapPanel::unboundBindingCount() const
{
    std::size_t unbound = 0;
    for (const Signal* signal : {&latitude_, &longitude_, &color_})
    {
        if (!signal->binding.zenoh_key.empty() && !signal->bound)
        {
            ++unbound;
        }
    }
    return unbound;
}

bool MapPanel::removeBinding(std::size_t index)
{
    // Indexed over what bindingLabels() returned, i.e. the BOUND roles only.
    std::vector<Role> present;
    if (isBound(cfg_.latitude))
    {
        present.push_back(Role::Latitude);
    }
    if (isBound(cfg_.longitude))
    {
        present.push_back(Role::Longitude);
    }
    if (isBound(cfg_.color_by))
    {
        present.push_back(Role::Color);
    }

    if (index >= present.size())
    {
        return false;
    }

    const Role role = present[index];
    Signal& signal = signalFor(role);
    if (signal.handle != kInvalidSignal)
    {
        source_->release(signal.handle);
    }
    signal = Signal{};
    bindingFor(role) = map_binding_t{};

    emit configChanged();
    update();
    return true;
}

// -------------------------------------------------------------------- wiring

void MapPanel::setTimeBase(TimeBase* time_base)
{
    if (time_base_ != nullptr)
    {
        disconnect(time_base_, nullptr, this, nullptr);
    }
    time_base_ = time_base;
    if (time_base_ != nullptr)
    {
        connect(time_base_, &TimeBase::frame, this, &MapPanel::onFrame);
        connect(time_base_, &TimeBase::cursorMoved, this, [this]() { update(); });
    }
}

void MapPanel::setHistorySeconds(double seconds)
{
    if (seconds == history_seconds_)
    {
        return;
    }
    history_seconds_ = seconds;
    // Rebinds, which discards the history already collected. That is the honest
    // outcome -- a buffer cannot grow a past it never recorded.
    rebindAll();
    update();
}

void MapPanel::rebindTo(DataSource& source)
{
    // THE ORDERING RULE from Panel::rebindTo(): release against the source that
    // issued the handles, BEFORE repointing. A handle means nothing to a source
    // that did not issue it, and repointing first leaks every subscription on
    // the old one.
    releaseAll();
    source_ = &source;
    rebindAll();
    // A new source is a new clock, so where the camera was told to look no
    // longer follows from where it was looking.
    drag_centre_.reset();
    drag_zoom_.reset();
    update();
}

QString MapPanel::title() const
{
    return QString::fromStdString(cfg_.title);
}

void MapPanel::applyConfig(const config_t& cfg)
{
    const config_t previous = cfg_;
    cfg_ = cfg;
    validate(cfg_);

    // Only rebind the roles whose SIGNAL changed. A colour, a width or a title
    // must not cost a binding its history.
    if (!sameSignal(previous.latitude, cfg_.latitude))
    {
        if (latitude_.handle != kInvalidSignal)
        {
            source_->release(latitude_.handle);
        }
        bindRole(Role::Latitude, cfg_.latitude);
    }
    if (!sameSignal(previous.longitude, cfg_.longitude))
    {
        if (longitude_.handle != kInvalidSignal)
        {
            source_->release(longitude_.handle);
        }
        bindRole(Role::Longitude, cfg_.longitude);
    }
    if (!sameSignal(previous.color_by, cfg_.color_by))
    {
        if (color_.handle != kInvalidSignal)
        {
            source_->release(color_.handle);
        }
        bindRole(Role::Color, cfg_.color_by);
    }

    // The style is baked into vertices at tessellation time and a TileReader
    // copies it at construction, so a style or tileset change means new
    // readers. Cheap: opening an archive is a metadata read.
    if (previous.tileset != cfg_.tileset || previous.overlay_tilesets != cfg_.overlay_tilesets ||
        !(previous.style == cfg_.style))
    {
        openReaders();
    }

    emit configChanged();
    update();
}

void MapPanel::setSettings(const scope_settings_t& settings)
{
    settings_ = settings;
    openReaders();
    update();
}

void MapPanel::openReaders()
{
    readers_.clear();
    reader_names_.clear();
    unknown_tilesets_.clear();

    std::vector<std::string> wanted;
    if (!cfg_.tileset.empty())
    {
        wanted.push_back(cfg_.tileset);
    }
    for (const std::string& overlay : cfg_.overlay_tilesets)
    {
        if (!overlay.empty())
        {
            wanted.push_back(overlay);
        }
    }

    for (const std::string& name : wanted)
    {
        const auto found = std::find_if(
            settings_.tilesets.begin(), settings_.tilesets.end(),
            [&name](const scope_tileset_t& tileset) { return tileset.name == name; });

        if (found == settings_.tilesets.end())
        {
            // Recorded rather than dropped, so the caption can name it. "Not
            // configured" and "configured but unreadable" are different fixes.
            unknown_tilesets_.push_back(name);
            continue;
        }

        readers_.push_back(std::make_unique<TileReader>(
            found->path, cfg_.style,
            // Arrives on the reader thread; a queued repaint is the only thing
            // that may happen from there.
            [this]() { QMetaObject::invokeMethod(this, [this]() { update(); },
                                                 Qt::QueuedConnection); }));
        reader_names_.push_back(name);
    }

    visible_.assign(readers_.size(), {});
    request_lists_.assign(readers_.size(), {});
    ready_.assign(readers_.size(), {});
    stand_ins_.assign(readers_.size(), {});
    stand_in_tiles_.assign(readers_.size(), {});
}

// ---------------------------------------------------------------- the clock

double MapPanel::readoutTime() const
{
    if (time_base_ == nullptr)
    {
        // A panel built outside a window -- a test -- still reads.
        return source_->now();
    }
    if (const std::optional<double> cursor = time_base_->cursor())
    {
        return *cursor;
    }
    return time_base_->viewEnd();
}

bool MapPanel::readingAtCursor() const
{
    return time_base_ != nullptr && time_base_->cursor().has_value();
}

void MapPanel::onFrame()
{
    // Draining is NOT optional, visible or not: the staging rings are bounded,
    // and a hidden tab that stopped draining would overflow them and count
    // drops against a perfectly healthy stream.
    for (Signal* signal : {&latitude_, &longitude_, &color_})
    {
        if (signal->buffer)
        {
            signal->buffer->drain(source_->now());
        }
    }

    std::size_t tiles_arrived = 0;
    for (auto& reader : readers_)
    {
        tiles_arrived += reader->drain();
    }

    // Rebuild the track only when what it is built FROM moved. The signature is
    // buffer identity (a rebind is a new buffer) plus received() and size()
    // (replaceHistory and drain move received; an empty refill still changes
    // size). Parked over a recording, this skips a full re-pair and Mercator
    // projection of every point, per frame, to reproduce the identical track.
    std::uint64_t signature = 0;
    const auto mix = [&signature](std::uint64_t v)
    { signature ^= v + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2); };
    for (const Signal* signal : {&latitude_, &longitude_, &color_})
    {
        mix(reinterpret_cast<std::uintptr_t>(signal->buffer.get()));
        if (signal->buffer)
        {
            mix(signal->buffer->received());
            mix(signal->buffer->history().size());
        }
    }

    const bool track_changed = signature != track_signature_;
    if (track_changed)
    {
        track_signature_ = signature;
        rebuildTrack();
    }

    // The marker rides the shared instant, so a view or cursor move needs a
    // repaint even with the track unchanged.
    const double readout = time_base_ != nullptr ? readoutTime() : 0.0;
    const bool time_moved = readout != last_frame_readout_;
    last_frame_readout_ = readout;

    // Everything else that changes the picture -- a config apply, a camera
    // drag, a resize, a click -- already calls update() at its own site; the
    // render tick only needs to repaint what arrived through it.
    if (track_changed || tiles_arrived > 0 || time_moved)
    {
        update();
    }
}

void MapPanel::rebuildTrack()
{
    if (!latitude_.buffer || !longitude_.buffer)
    {
        track_.clear();
        track_stats_ = {};
        track_color_valid_ = false;
        return;
    }

    track_stats_ =
        track::build(latitude_.buffer->history(), longitude_.buffer->history(),
                     color_.buffer ? &color_.buffer->history() : nullptr, track_);

    // The colour extremes, ONCE per rebuild. colorRange() used to scan track_
    // itself, and it is called per drawn segment -- recomputing this constant
    // cost more than everything else in paintTrack() put together.
    track_color_valid_ = false;
    for (const track::Point& point : track_)
    {
        if (!point.has_color)
        {
            continue;
        }
        track_color_low_ = track_color_valid_ ? std::min(track_color_low_, point.color)
                                              : point.color;
        track_color_high_ = track_color_valid_ ? std::max(track_color_high_, point.color)
                                               : point.color;
        track_color_valid_ = true;
    }
}

// ---------------------------------------------------------------- the camera

map_render::Camera MapPanel::camera() const
{
    map_render::Camera out;
    out.zoom = drag_zoom_.value_or(cfg_.zoom);
    out.bearing = 0.0;

    if (drag_centre_.has_value())
    {
        out.center = *drag_centre_;
        return out;
    }

    if (cfg_.follow_cursor && marker_valid_)
    {
        out.center = marker_coordinate_;
        return out;
    }

    out.center = map_render::Coordinate{cfg_.center_latitude, cfg_.center_longitude};
    return out;
}

void MapPanel::refreshTiles(const map_render::Projection& projection)
{
    // CLEARED, not left alone. A panel is constructed at Qt's default size and
    // may be resized to nothing by a layout that has not run yet; keeping the
    // tiles worked out for the default size would claim to need tiles it will
    // never draw.
    if (width() <= 0 || height() <= 0 || readers_.empty())
    {
        visible_.assign(readers_.size(), {});
        return;
    }

    visible_.assign(readers_.size(), {});
    request_lists_.resize(readers_.size());

    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        if (!readers_[s]->ok())
        {
            continue;
        }

        // The ARCHIVE's range, not the configured one -- which is the camera's
        // business and may legitimately reach past what the archive holds.
        // Known at open here, so there is nothing to learn and no repaint to
        // trigger on learning it.
        const TileReader::ZoomRange range = readers_[s]->zoomRange();
        const std::uint8_t z = projection.tileZoom(range.min, range.max);

        auto tiles = projection.visibleTilesWithMargin(z, kPrefetchRingTiles);
        visible_[s] = std::move(tiles.drawn);

        // Sorted centre-outward HERE and not in visible_: the request order
        // decides which tiles are served first, and the DRAW order must stay
        // stable or the renderer re-uploads every tile whenever the camera
        // reshuffles it.
        projection.sortCentreOutward(tiles.withMargin);

        // The coarse overview goes on the END, so it can only ever spend slots
        // the viewport did not want.
        if (z > range.min)
        {
            const auto overview = static_cast<std::uint8_t>(
                std::max(int(z) - int(kOverviewZoomDelta), int(range.min)));
            for (const map_render::TileId& id : projection.visibleTiles(overview, 0))
            {
                tiles.withMargin.push_back(id);
            }
        }

        request_lists_[s] = std::move(tiles.withMargin);
        readers_[s]->request(request_lists_[s]);
    }
}

void MapPanel::assembleBatches()
{
    batches_.clear();
    label_tiles_.clear();
    ready_.resize(readers_.size());
    stand_ins_.resize(readers_.size());
    stand_in_tiles_.resize(readers_.size());

    // The stand-in budget is SHARED across readers and counted against what
    // every reader together already wants to draw. Working it out per reader
    // would let two of them each claim the whole frame's headroom and overrun
    // kMaxTilesPerFrame, which silently drops whatever came last.
    std::size_t visible_total = 0;
    for (const auto& ids : visible_)
    {
        visible_total += ids.size();
    }
    std::size_t budget = map_render::GpuRenderer::kMaxTilesPerFrame > visible_total
                             ? map_render::GpuRenderer::kMaxTilesPerFrame - visible_total
                             : 0;

    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        readers_[s]->ready(visible_[s], ready_[s]);

        have_.assign(visible_[s].size(), false);
        for (std::size_t i = 0; i < visible_[s].size(); ++i)
        {
            have_[i] = static_cast<bool>(ready_[s][i]);
        }

        TileReader& reader = *readers_[s];
        stand_ins_[s] = map_render::substituteTiles(
            visible_[s], have_,
            [&reader](const map_render::TileId& id) { return reader.drawable(id); }, budget);
        budget -= std::min(budget, stand_ins_[s].size());
        reader.ready(stand_ins_[s], stand_in_tiles_[s]);
    }

    // Stand-ins FIRST, so they are drawn UNDER within each layer pass and a real
    // tile that has arrived covers its own ground. Every reader's stand-ins
    // before any reader's real tiles, and not per reader, because the renderer
    // draws LAYER-major across the whole batch list -- so within one layer the
    // order here is the only thing deciding what covers what.
    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        for (std::size_t i = 0; i < stand_ins_[s].size(); ++i)
        {
            if (stand_in_tiles_[s][i])
            {
                batches_.push_back(
                    map_render::GpuBatch{stand_ins_[s][i], stand_in_tiles_[s][i].geometry, 1.0F});
            }
        }
    }
    last_tiles_stand_in_ = static_cast<int>(batches_.size());

    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        for (std::size_t i = 0; i < visible_[s].size(); ++i)
        {
            if (!ready_[s][i])
            {
                continue;
            }
            batches_.push_back(map_render::GpuBatch{visible_[s][i], ready_[s][i].geometry, 1.0F});
            label_tiles_.push_back(map_render::LabelTile{visible_[s][i], ready_[s][i].labels});
        }
    }
    last_tiles_drawn_ = static_cast<int>(batches_.size()) - last_tiles_stand_in_;

    // Stand-ins label too, but only AFTER every real tile, and that order is the
    // whole trick: without them the text blinks out for the frames a zoom is in
    // flight while the geometry underneath stays, which is a worse artefact than
    // the blank this was meant to fix. Duplicates take care of themselves --
    // layOutText() rejects a candidate colliding with one already placed, and
    // real tiles going in first decides which wins.
    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        for (std::size_t i = 0; i < stand_ins_[s].size(); ++i)
        {
            if (stand_in_tiles_[s][i])
            {
                label_tiles_.push_back(
                    map_render::LabelTile{stand_ins_[s][i], stand_in_tiles_[s][i].labels});
            }
        }
    }
}

// ----------------------------------------------------------------- painting

void MapPanel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QColor background = qt_helpers::toQColor(cfg_.style.background);
    painter.fillRect(rect(), background);

    if (width() <= 0 || height() <= 0)
    {
        return;
    }

    // The marker instant is settled BEFORE the camera is built, because Follow
    // Cursor makes the camera depend on it.
    marker_t_ = readoutTime();
    marker_valid_ = false;
    if (const std::optional<std::size_t> index = track::at(track_, marker_t_))
    {
        marker_valid_ = true;
        marker_coordinate_ = map_render::coordinateFor(track_[*index].world);
    }

    const map_render::Projection projection(camera(), width(), height(),
                                            devicePixelRatioF());

    refreshTiles(projection);
    assembleBatches();

    if (gpu_ && !batches_.empty())
    {
        const map_render::LabelStats label_stats = map_render::layOutText(
            projection, label_tiles_, cfg_.style, label_cache_, devicePixelRatioF(), text_quads_);
        (void)label_stats;
        gpu_->setText(text_quads_, label_cache_.atlas().page(), label_cache_.atlas().dirty());

        const QImage& frame = gpu_->render(projection, batches_, cfg_.style, background);
        if (!frame.isNull())
        {
            painter.drawImage(rect(), frame);
        }
    }

    // Thinned once, and BOTH the path and the hit test use the result -- so a
    // click can never land on a point that is not drawn, which would read as a
    // mis-aimed mouse.
    track::thin(track_, projection, kThinPixels, thinned_);

    paintTrack(painter, projection);
    paintMarker(painter, projection);

    if (cfg_.show_color_legend && color_.bound)
    {
        paintLegend(painter);
    }
    if (cfg_.show_status)
    {
        paintDiagnostic(painter);
    }
}

bool MapPanel::colorRange(double& low, double& high) const
{
    if (!cfg_.color_autoscale)
    {
        low = cfg_.color_min;
        high = cfg_.color_max;
        return high > low;
    }

    if (!track_color_valid_)
    {
        return false;
    }
    low = track_color_low_;
    high = track_color_high_;

    // A constant signal has no range to scale onto. Widening it puts every
    // point at the ramp's midpoint, which is honest -- the alternative divides
    // by zero and paints NaN, which paints nothing.
    if (!(high > low))
    {
        low -= 0.5;
        high += 0.5;
    }
    return true;
}

QColor MapPanel::colorForValue(double value) const
{
    double low = 0.0;
    double high = 1.0;
    if (!colorRange(low, high))
    {
        return qt_helpers::toQColor(cfg_.track_color);
    }

    const double t = (value - low) / (high - low);
    switch (cfg_.color_ramp)
    {
        case map_color_ramp_t::viridis:
            return sampleRamp(kViridis, std::size(kViridis), t);
        case map_color_ramp_t::turbo:
            return sampleRamp(kTurbo, std::size(kTurbo), t);
        case map_color_ramp_t::gray:
            break;
    }
    const int level = static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * 255.0));
    return QColor(level, level, level);
}

void MapPanel::paintTrack(QPainter& painter, const map_render::Projection& projection)
{
    if (thinned_.size() < 2)
    {
        return;
    }

    // The stretch the plots beside this panel are showing. Everything else is
    // drawn faint -- that band is the only thing on screen relating the map to
    // the time base, and without it this is a picture beside the plots rather
    // than a view of the same window.
    const double view_begin = time_base_ != nullptr ? time_base_->viewBegin()
                                                    : -std::numeric_limits<double>::infinity();
    const double view_end = time_base_ != nullptr ? time_base_->viewEnd()
                                                  : std::numeric_limits<double>::infinity();

    const bool ramp = color_.bound;
    const QColor flat = qt_helpers::toQColor(cfg_.track_color);

    painter.save();

    // ONE pen, mutated only when the colour or width actually changes. A fresh
    // QPen per segment is a heap allocation each, several hundred per frame,
    // and on an un-ramped track every one of them is identical.
    QPen pen(flat);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    QColor pen_colour;
    double pen_width = -1.0;
    bool pen_set = false;

    // Segment by segment rather than one path, because both the opacity and the
    // colour change along it. A single QPainterPath can carry neither.
    for (std::size_t i = 1; i < thinned_.size(); ++i)
    {
        const track::Point& from = thinned_[i - 1];
        const track::Point& to = thinned_[i];

        const map_render::ScreenPoint a = projection.screenFor(from.world);
        const map_render::ScreenPoint b = projection.screenFor(to.world);

        const bool in_view = to.t >= view_begin && to.t <= view_end;

        QColor colour = flat;
        if (ramp && to.has_color)
        {
            colour = colorForValue(to.color);
        }
        colour.setAlphaF(in_view ? 1.0F : static_cast<float>(cfg_.track_opacity));

        const double width = in_view ? cfg_.view_track_width : cfg_.track_width;
        if (!pen_set || colour != pen_colour || width != pen_width)
        {
            pen.setColor(colour);
            pen.setWidthF(width);
            painter.setPen(pen);
            pen_colour = colour;
            pen_width = width;
            pen_set = true;
        }
        painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    }

    painter.restore();
}

void MapPanel::paintMarker(QPainter& painter, const map_render::Projection& projection)
{
    if (!marker_valid_)
    {
        return;
    }

    const map_render::ScreenPoint at = projection.screenFor(marker_coordinate_);
    const double radius = cfg_.marker_size;

    painter.save();
    // The ring is what keeps the marker legible over a trail of a similar
    // colour, which with a ramp is every colour.
    painter.setPen(QPen(qt_helpers::toQColor(cfg_.marker_outline_color), 2.0));
    painter.setBrush(qt_helpers::toQColor(cfg_.marker_color));
    painter.drawEllipse(QPointF(at.x, at.y), radius, radius);
    painter.restore();
}

void MapPanel::paintLegend(QPainter& painter)
{
    double low = 0.0;
    double high = 1.0;
    if (!colorRange(low, high))
    {
        return;
    }

    constexpr double kBarWidth = 12.0;
    constexpr double kBarHeight = 90.0;
    constexpr double kMargin = 10.0;

    const double left = width() - kMargin - kBarWidth;
    const double top = height() - kMargin - kBarHeight;

    painter.save();

    // Bottom of the bar is the LOW end, which is the way every colour bar in
    // the world reads.
    for (int y = 0; y < static_cast<int>(kBarHeight); ++y)
    {
        const double t = 1.0 - (double(y) / kBarHeight);
        painter.setPen(colorForValue(low + (t * (high - low))));
        painter.drawLine(QPointF(left, top + y), QPointF(left + kBarWidth, top + y));
    }

    painter.setPen(kLegendText);
    const QFontMetricsF metrics(painter.font());
    const QString high_text = QString::number(high, 'g', 4);
    const QString low_text = QString::number(low, 'g', 4);
    painter.drawText(QPointF(left - kMargin - metrics.horizontalAdvance(high_text),
                             top + metrics.ascent()),
                     high_text);
    painter.drawText(
        QPointF(left - kMargin - metrics.horizontalAdvance(low_text), top + kBarHeight),
        low_text);

    painter.restore();
}

QString MapPanel::diagnostic() const
{
    // ORDER MATTERS: the most specific cause first, so a panel with two
    // problems names the one to fix.
    if (!unknown_tilesets_.empty())
    {
        return tr("Tileset '%1' is not configured — File ▸ Settings…")
            .arg(QString::fromStdString(unknown_tilesets_.front()));
    }

    for (std::size_t s = 0; s < readers_.size(); ++s)
    {
        if (!readers_[s]->ok())
        {
            return tr("'%1' could not be opened: %2")
                .arg(QString::fromStdString(reader_names_[s]),
                     QString::fromStdString(readers_[s]->error()));
        }
    }

    if (!latitude_.bound || !longitude_.bound)
    {
        return tr("No position bound — drag a position topic onto this panel");
    }

    if (track_stats_.paired == 0 && track_stats_.unpaired_latitude > 0)
    {
        // The one failure that looks exactly like "no data" and is not.
        return tr("Latitude and longitude never share a timestamp — are they on the same topic?");
    }

    if (gpu_ == nullptr)
    {
        return tr("No GPU backend — map geometry cannot be drawn");
    }

    if (readers_.empty())
    {
        return tr("No tileset configured");
    }

    if (last_tiles_drawn_ == 0 && last_tiles_stand_in_ == 0)
    {
        // Tests BOTH, because a perfectly good stand-in frame captioned "no
        // coverage" is a line of text flashing over visible roads.
        bool anything_requested = false;
        for (const auto& reader : readers_)
        {
            anything_requested = anything_requested || reader->stats().requested > 0;
        }
        if (!anything_requested)
        {
            return tr("No tiles requested");
        }

        bool waiting = false;
        for (const auto& reader : readers_)
        {
            waiting = waiting || reader->stats().inFlight > 0;
        }
        if (waiting)
        {
            return tr("Reading tiles…");
        }
        return tr("No coverage here in '%1'").arg(QString::fromStdString(cfg_.tileset));
    }

    return {};
}

void MapPanel::paintDiagnostic(QPainter& painter)
{
    const QString message = diagnostic();
    if (message.isEmpty())
    {
        return;
    }

    painter.save();
    const QFontMetricsF metrics(painter.font());
    const QRectF box(6.0, 6.0, metrics.horizontalAdvance(message) + 16.0,
                     metrics.height() + 10.0);
    painter.fillRect(box, kDiagnosticBackground);
    painter.setPen(kDiagnosticText);
    painter.drawText(box.adjusted(8.0, 0.0, 0.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft, message);
    painter.restore();
}

// -------------------------------------------------------------- interaction

void MapPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        Panel::mousePressEvent(event);
        return;
    }

    const map_render::ScreenPoint screen{event->position().x(), event->position().y()};
    const map_render::Projection projection(camera(), width(), height(), devicePixelRatioF());

    press_screen_ = screen;
    press_world_ = projection.worldForScreen(screen);

    // THE ARBITRATION, decided once here and not revisited: a press within
    // click_radius_px of the drawn track scrubs; anything else pans. Deciding
    // per move would let a drag change its mind halfway, which is the sort of
    // thing that feels like a bug rather than reads as one.
    if (cfg_.click_seeks && time_base_ != nullptr)
    {
        if (const std::optional<std::size_t> hit =
                track::nearest(thinned_, projection, screen, cfg_.click_radius_px))
        {
            gesture_ = Gesture::Scrubbing;
            const double t = thinned_[*hit].t;
            time_base_->setCursor(t);
            // Outside the window the cursor would mark an instant no plot is
            // drawing -- and on a recorded source the view's right edge IS the
            // playhead, so the view has to move with it.
            if (t < time_base_->viewBegin() || t > time_base_->viewEnd())
            {
                time_base_->seek(t);
            }
            update();
            return;
        }
    }

    if (cfg_.interactive)
    {
        gesture_ = Gesture::Panning;
    }
}

void MapPanel::mouseMoveEvent(QMouseEvent* event)
{
    const map_render::ScreenPoint screen{event->position().x(), event->position().y()};

    if (gesture_ == Gesture::Scrubbing && time_base_ != nullptr)
    {
        const map_render::Projection projection(camera(), width(), height(),
                                                devicePixelRatioF());
        if (const std::optional<std::size_t> hit =
                track::nearest(thinned_, projection, screen, cfg_.click_radius_px))
        {
            time_base_->setCursor(thinned_[*hit].t);
            update();
        }
        return;
    }

    if (gesture_ != Gesture::Panning)
    {
        return;
    }

    // Anchored rather than approximate: the world point grabbed at press time
    // is put back under the pointer, which is what keeps a drag from sliding.
    // In world units, so it stays correct under rotation.
    const map_render::Projection projection(camera(), width(), height(), devicePixelRatioF());
    const map_render::WorldPoint now = projection.worldForScreen(screen);
    const map_render::Camera current = projection.camera();
    const map_render::WorldPoint centre = map_render::worldFor(current.center);

    const map_render::WorldPoint moved{centre.x - (now.x - press_world_.x),
                                       centre.y - (now.y - press_world_.y)};
    drag_centre_ = map_render::coordinateFor(moved);
    drag_zoom_ = current.zoom;
    update();
}

void MapPanel::mouseReleaseEvent(QMouseEvent* event)
{
    gesture_ = Gesture::None;
    Panel::mouseReleaseEvent(event);
}

void MapPanel::wheelEvent(QWheelEvent* event)
{
    if (!cfg_.interactive)
    {
        Panel::wheelEvent(event);
        return;
    }

    const double notches = event->angleDelta().y() / kWheelUnitsPerNotch;
    if (notches == 0.0)
    {
        return;
    }

    const map_render::Projection projection(camera(), width(), height(), devicePixelRatioF());
    const map_render::ScreenPoint screen{event->position().x(), event->position().y()};
    // The point under the pointer, kept there while the scale changes.
    const map_render::WorldPoint anchor = projection.worldForScreen(screen);

    const map_render::Camera current = projection.camera();
    const double zoom = std::clamp(current.zoom + (notches * kZoomPerWheelNotch),
                                   double(cfg_.min_zoom), double(cfg_.max_zoom));

    map_render::Camera zoomed = current;
    zoomed.zoom = zoom;
    const map_render::Projection after(zoomed, width(), height(), devicePixelRatioF());
    const map_render::WorldPoint moved_anchor = after.worldForScreen(screen);
    const map_render::WorldPoint centre = map_render::worldFor(current.center);

    drag_centre_ = map_render::coordinateFor(
        map_render::WorldPoint{centre.x - (moved_anchor.x - anchor.x),
                               centre.y - (moved_anchor.y - anchor.y)});
    drag_zoom_ = zoom;
    update();
}

// -------------------------------------------------------------------- stats

MapPanelStats_t MapPanel::stats() const
{
    MapPanelStats_t out;

    out.latitude_bound = latitude_.bound;
    out.longitude_bound = longitude_.bound;
    out.color_bound = color_.bound;

    if (latitude_.buffer)
    {
        out.latitude_samples = latitude_.buffer->history().size();
        out.dropped += latitude_.buffer->dropped();
    }
    if (longitude_.buffer)
    {
        out.longitude_samples = longitude_.buffer->history().size();
        out.dropped += longitude_.buffer->dropped();
    }
    if (color_.buffer)
    {
        out.color_samples = color_.buffer->history().size();
        out.dropped += color_.buffer->dropped();
    }

    out.paired_points = track_stats_.paired;
    out.unpaired_latitude = track_stats_.unpaired_latitude;
    out.unpaired_longitude = track_stats_.unpaired_longitude;
    out.track_points_drawn = thinned_.size();

    if (!track_.empty())
    {
        out.t_first = track_.front().t;
        out.t_last = track_.back().t;
    }

    out.marker_valid = marker_valid_;
    out.marker_t = marker_t_;
    out.marker_latitude = marker_coordinate_.latitude;
    out.marker_longitude = marker_coordinate_.longitude;
    out.at_cursor = readingAtCursor();

    const map_render::Camera cam = camera();
    out.camera_latitude = cam.center.latitude;
    out.camera_longitude = cam.center.longitude;
    out.camera_zoom = cam.zoom;
    out.camera_moved = drag_centre_.has_value();

    for (const auto& reader : readers_)
    {
        const TileReaderStats stats = reader->stats();
        out.tiles_requested += stats.requested;
        out.tiles_decoded += stats.decoded;
        out.tiles_absent += stats.absent;
        out.tiles_failed += stats.failed;
        out.tiles_cached_bytes += stats.cachedBytes;
    }
    out.tiles_drawn = static_cast<std::uint64_t>(std::max(last_tiles_drawn_, 0));
    out.tiles_stand_in = static_cast<std::uint64_t>(std::max(last_tiles_stand_in_, 0));

    out.gpu_ready = gpu_ != nullptr;
    out.diagnostic = diagnostic().toStdString();

    return out;
}

}  // namespace scope

#include "map_panel/moc_map_panel.cpp"
