// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two hosts of the pass, side by side, through the same MapSurface.
//
// map_bench cannot answer this one. It runs under QT_QPA_PLATFORM=offscreen,
// where a QRhiWidget never initialises -- so the only way to find out what the
// readback and the blit are actually costing is to put a window on a screen and
// drive both hosts through the same façade, the same camera path and the
// same overlay in one process on one machine.
//
//   map_surface_bench --tiles ~/Documents/map_data/socal.mbtiles
//   map_surface_bench --tiles ... --width 1920 --height 1080 --pitch 55
//
// NOT a test: it asserts nothing, always exits 0, and needs a display. It opens
// a window, drives it, and closes it.
//
// What it reports is GUI-THREAD time per frame -- the number that decides
// whether the dashboard's paint budget is met. For the offscreen host that
// includes waiting for the GPU and copying the frame back, because a QPainter
// cannot draw the map until both have happened.

#include "map_render/labels.h"
#include "map_render/projection.h"
#include "map_render/tessellator.h"
#include "map_surface/map_surface.h"

#include "mbtiles/archive.h"
#include "mvt/decode.h"
#include "mvt/gzip.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QThread>
#include <QWidget>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

using map_render::Camera;
using map_render::Coordinate;
using map_render::GpuBatch;
using map_render::LabelTile;
using map_render::Projection;
using map_render::TileGeometry;
using map_render::TileId;
using map_render::TileIdHash;

constexpr double kStartLat = 33.6865966;
constexpr double kStartLon = -117.8557874;
constexpr double kMetresPerFix = 25.0;

struct Cached
{
    std::shared_ptr<const map_render::LabelSet> labels;
    std::shared_ptr<const TileGeometry> geometry;
};

// Everything a frame needs, so the camera path and the tile set are provably
// the same for both hosts.
struct Scene
{
    std::unordered_map<TileId, Cached, TileIdHash> cache;
    MapStyle_t style;
    QColor background { QColor("#0d0f13") };
    std::uint8_t z { 14 };
    std::uint8_t archiveMin { 0 };
    double zoom { 14.0 };
    double bearing { 0.0 };
    double pitch { 0.0 };
    int width { 660 };
    int height { 640 };

    Coordinate at(int frame) const
    {
        const double degPerMetreLon =
            1.0 / (111320.0 * std::cos(kStartLat * std::numbers::pi / 180.0));
        return Coordinate { kStartLat,
                            kStartLon + (double(frame) * kMetresPerFix * degPerMetreLon) };
    }

    std::vector<GpuBatch> batchesFor(const Projection& projection,
                                     std::vector<LabelTile>& labelTiles) const
    {
        std::vector<GpuBatch> batches;
        labelTiles.clear();
        for (const TileId& id : projection.visibleTilesWithMargin(z, 1, archiveMin).drawn)
        {
            const auto found = cache.find(id);
            if (found == cache.end())
            {
                continue;
            }
            batches.push_back(GpuBatch { id, found->second.geometry });
            labelTiles.push_back(LabelTile { id, found->second.labels });
        }
        return batches;
    }
};

class Stage
{
  public:
    void add(double ms) { mSamples.push_back(ms); }
    bool empty() const { return mSamples.empty(); }
    void report(const std::string& name) const
    {
        if (mSamples.empty())
        {
            SPDLOG_INFO("  {:<30} no frames", name);
            return;
        }
        std::vector<double> sorted = mSamples;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](double f) {
            return sorted[std::min(sorted.size() - 1, std::size_t(f * double(sorted.size())))];
        };
        SPDLOG_INFO("  {:<30} median {:7.3f}   p90 {:7.3f}   worst {:7.3f} ms   over {} frames",
                    name, at(0.5), at(0.9), sorted.back(), sorted.size());
    }

  private:
    std::vector<double> mSamples;
};

// Both loops paced to the same period, and that is not a nicety.
//
// A QRhiWidget presents on vsync, so its loop is idle for most of every
// interval -- and an idle core downclocks. MEASURED: left unpaced, the
// offscreen host ran flat out at 580 fps while the RHI one sat at the
// display's 120, and layOutText, which is IDENTICAL work either way, reported
// three to five times the cost on the RHI side. That is a report on P states,
// not on hosts. Pacing both to the same period puts the cores in the same
// condition; the absolute figures are then both a little pessimistic and the
// comparison between them is honest.
void paceTo(const QElapsedTimer& since, double periodMs)
{
    const double elapsed = double(since.nsecsElapsed()) / 1.0e6;
    if (elapsed < periodMs)
    {
        QThread::usleep(static_cast<unsigned long>((periodMs - elapsed) * 1000.0));
    }
}

std::string argumentAfter(int argc, char** argv, const std::string& flag,
                          const std::string& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (flag == argv[i])
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

// One run of one host, through the same façade and the same overlay the
// dashboard would use.
struct Run
{
    Stage prepare;  // labels and batches -- shared work, identical either way
    Stage map;      // the map host's own frame: the number the exercise is about
    Stage overlay;  // the embedder's own marker and trail
};

Run drive(QApplication& app, const Scene& scene, map_surface::MapSurface::Host host,
          int frames, double paceMs, const std::string& dumpPath,
          const std::string& overlayMode)
{
    Run run;

    // An embedder, exactly as the dashboard is one: it owns the surface and
    // its chrome, and it never learns which host the surface picked.
    QWidget embedder;
    embedder.resize(scene.width, scene.height);

    map_surface::MapSurface surface(host, &embedder);
    surface.setGeometry(0, 0, scene.width, scene.height);

    // The embedder's own drawing. Stands in for paintMarker/paintTrack: enough
    // QPainter work to be worth timing, on the transparent layer over the map.
    int frame = 0;
    surface.setOverlayPainter([&](QPainter& painter) {
        if (overlayMode != "none")
        {
            QPainterPath trail;
            const int points = 60;
            for (int i = 0; i <= points; ++i)
            {
                // "trail" is what a vehicle actually leaves: contiguous points a
                // few pixels apart. "scatter" is the same POINT COUNT flung
                // across the whole widget, which is a completely different
                // amount of ink and the reason to measure rather than assume.
                const QPointF at =
                    overlayMode == "scatter"
                        ? QPointF(double(((frame + i) * 7) % scene.width),
                                  double(((frame + i) * 13) % scene.height))
                        : QPointF((scene.width / 2.0) + ((i - points) * 2.0),
                                  (scene.height / 2.0) + (12.0 * std::sin(double(i) * 0.3)));
                if (i == 0) { trail.moveTo(at); } else { trail.lineTo(at); }
            }
            if (overlayMode != "marker")
            {
                painter.setPen(QPen(QColor("#FF3B30"), 3.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(trail);
            }
            painter.setPen(QPen(QColor("#101216"), 2.0));
            painter.setBrush(QColor("#FFFFFF"));
            painter.drawEllipse(QPointF(scene.width / 2.0, scene.height / 2.0), 7.0, 7.0);
        }
    });

    embedder.show();

    map_render::LabelCache labels;
    std::vector<map_render::TextQuad> textQuads;

    const auto push = [&](int i) {
        frame = i;
        const Camera camera { scene.at(i), scene.zoom, scene.bearing, scene.pitch };
        const Projection projection(camera, surface.width(), surface.height(),
                                    surface.devicePixelRatioF());
        std::vector<LabelTile> labelTiles;
        std::vector<GpuBatch> batches = scene.batchesFor(projection, labelTiles);
        map_render::layOutText(projection, labelTiles, scene.style, labels,
                               surface.devicePixelRatioF(), textQuads);
        surface.setText(textQuads, labels.atlas().page(), labels.atlas().dirty());
        labels.atlas().markClean();
        surface.setFrame(projection, std::move(batches), scene.style, scene.background);
    };

    // Warm-up, uncounted: the widget comes up, the first upload happens, the
    // glyph atlas fills.
    for (int i = 0; i < 8; ++i)
    {
        push(0);
        surface.refreshOverlay();
        app.processEvents();
    }
    run = Run {};
    std::uint64_t drawn = surface.framesDrawn();
    std::uint64_t overlayPainted = surface.overlayFramesPainted();

    for (int i = 1; i <= frames; ++i)
    {
        QElapsedTimer iteration;
        iteration.start();

        QElapsedTimer prepareTimer;
        prepareTimer.start();
        frame = i;
        const Camera camera { scene.at(i), scene.zoom, scene.bearing, scene.pitch };
        const Projection projection(camera, surface.width(), surface.height(),
                                    surface.devicePixelRatioF());
        std::vector<LabelTile> labelTiles;
        std::vector<GpuBatch> batches = scene.batchesFor(projection, labelTiles);
        map_render::layOutText(projection, labelTiles, scene.style, labels,
                               surface.devicePixelRatioF(), textQuads);
        surface.setText(textQuads, labels.atlas().page(), labels.atlas().dirty());
        labels.atlas().markClean();
        run.prepare.add(double(prepareTimer.nsecsElapsed()) / 1.0e6);

        surface.setFrame(projection, std::move(batches), scene.style, scene.background);
        // NOT also refreshOverlay(): setFrame() repaints the overlay itself,
        // and calling both cost the offscreen host two overlay paints per
        // frame -- the layer is a child, so Qt repaints it whenever the map
        // beneath it is repainted.
        app.processEvents();

        // Sampled only when a frame actually got drawn. The QRhiWidget host
        // presents on vsync and Qt coalesces what it cannot keep up with, so
        // counting every iteration would fold dropped frames into the median as
        // if they had been free.
        if (surface.framesDrawn() != drawn)
        {
            drawn = surface.framesDrawn();
            run.map.add(surface.lastMapMs());
        }
        if (surface.overlayFramesPainted() != overlayPainted)
        {
            overlayPainted = surface.overlayFramesPainted();
            run.overlay.add(surface.lastOverlayMs());
        }

        paceTo(iteration, paceMs);
    }

    if (!dumpPath.empty())
    {
        const QImage grabbed = embedder.grab().toImage();
        if (!grabbed.isNull() && grabbed.save(QString::fromStdString(dumpPath)))
        {
            SPDLOG_INFO("  frame written to {}", dumpPath);
        }
    }
    return run;
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    QApplication app(argc, argv);

    Scene scene;
    const std::string tilesPath = argumentAfter(argc, argv, "--tiles", "");
    const int frames = std::atoi(argumentAfter(argc, argv, "--frames", "250").c_str());
    scene.width = std::atoi(argumentAfter(argc, argv, "--width", "660").c_str());
    scene.height = std::atoi(argumentAfter(argc, argv, "--height", "640").c_str());
    scene.zoom = std::atof(argumentAfter(argc, argv, "--zoom", "14").c_str());
    scene.bearing = std::atof(argumentAfter(argc, argv, "--bearing", "0").c_str());
    scene.pitch = std::atof(argumentAfter(argc, argv, "--pitch", "0").c_str());
    scene.z = static_cast<std::uint8_t>(scene.zoom);
    const std::string dumpPath = argumentAfter(argc, argv, "--dump", "");
    // none | marker | trail | scatter -- see the overlay painter below.
    const std::string overlayMode = argumentAfter(argc, argv, "--overlay", "trail");

    const double refresh = QApplication::primaryScreen() != nullptr
                               ? QApplication::primaryScreen()->refreshRate()
                               : 60.0;
    const double paceMs = std::atof(
        argumentAfter(argc, argv, "--pace", std::to_string(1000.0 / std::max(refresh, 1.0)))
            .c_str());

    if (tilesPath.empty())
    {
        SPDLOG_ERROR("--tiles <archive.mbtiles> is required");
        return 0;
    }

    auto archive = mbtiles::Archive::open(tilesPath);
    if (!archive)
    {
        SPDLOG_ERROR("could not open {}", tilesPath);
        return 0;
    }
    scene.archiveMin = archive->metadata().minzoom;

    {
        std::unordered_map<TileId, bool, TileIdHash> wanted;
        for (int i = 0; i < frames; i += 5)
        {
            const Projection sample(
                Camera { scene.at(i), scene.zoom, scene.bearing, scene.pitch }, scene.width,
                scene.height, 2.0);
            for (const TileId& id :
                 sample.visibleTilesWithMargin(scene.z, 1, scene.archiveMin).withMargin)
            {
                wanted.emplace(id, true);
            }
        }
        for (const auto& entry : wanted)
        {
            auto blob = archive->tile(entry.first.z, entry.first.x, entry.first.y);
            if (!blob || !blob->has_value())
            {
                continue;
            }
            auto raw = mvt::inflateIfCompressed((*blob)->data);
            if (!raw)
            {
                continue;
            }
            auto tile = mvt::decode(*raw);
            if (!tile)
            {
                continue;
            }
            scene.cache.emplace(
                entry.first,
                Cached { std::make_shared<const map_render::LabelSet>(
                             map_render::extractLabels(*tile)),
                         std::make_shared<const TileGeometry>(
                             map_render::tessellate(*tile, scene.style)) });
        }
    }

    SPDLOG_INFO("");
    SPDLOG_INFO("viewport   {}x{} logical at z{:.1f}, bearing {:.0f}, pitch {:.0f}", scene.width,
                scene.height, scene.zoom, scene.bearing, scene.pitch);
    SPDLOG_INFO("tiles      {} decoded", scene.cache.size());
    SPDLOG_INFO("screen     device pixel ratio {}, {:.2f} ms paced for BOTH hosts",
                QApplication::primaryScreen() != nullptr
                    ? QApplication::primaryScreen()->devicePixelRatio()
                    : 1.0,
                paceMs);
    SPDLOG_INFO("");

    const Run offscreen =
        drive(app, scene, map_surface::MapSurface::Host::offscreen, frames, paceMs,
              dumpPath.empty() ? std::string() : dumpPath + ".offscreen.png", overlayMode);
    const Run automatic =
        drive(app, scene, map_surface::MapSurface::Host::automatic, frames, paceMs,
              dumpPath.empty() ? std::string() : dumpPath + ".rhi.png", overlayMode);

    SPDLOG_INFO("GUI-thread cost of one map frame, through the same MapSurface:");
    SPDLOG_INFO("");
    SPDLOG_INFO(" offscreen host (readback + QPainter blit)");
    offscreen.prepare.report("  labels + batches");
    offscreen.map.report("  the map frame");
    offscreen.overlay.report("  overlay (marker + trail)");
    SPDLOG_INFO("");
    SPDLOG_INFO(" automatic host ({})",
                map_surface::rhiWidgetsAreAvailable() ? "QRhiWidget" : "offscreen -- no RHI here");
    automatic.prepare.report("  labels + batches");
    automatic.map.report("  the map frame");
    automatic.overlay.report("  overlay (marker + trail)");
    SPDLOG_INFO("");
    SPDLOG_INFO("`labels + batches` is identical work in both and is the control: if the two");
    SPDLOG_INFO("disagree, the pacing is not holding the cores in the same state and the rest of");
    SPDLOG_INFO("the table cannot be trusted.");
    SPDLOG_INFO("");
    return 0;
}
