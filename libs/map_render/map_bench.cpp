// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a map frame costs, stage by stage.
//
// NOT a test -- it asserts nothing and always exits 0. It exists because every
// performance claim about this widget is otherwise a guess: the paint pass has
// five stages with wildly different costs, and which one dominates is not
// something reading the code tells you.
//
// It reads real tiles straight from an .mbtiles archive rather than going
// through zenoh, so it needs no bus and no map_server -- the transport is not
// what it measures.
//
//   map_bench --tiles ~/Documents/map_data/socal.mbtiles
//   map_bench --tiles ... --width 2560 --height 1440 --dpr 2
//   map_bench --tiles ... --bearing 30      # the label stage's expensive case
//
// The number to watch is `uploads`: it should climb only when the visible tile
// SET changes. If it tracks the frame count, something is invalidating the
// vertex cache every frame and the whole tessellate-once design is off.

#include "map_render/offscreen_renderer.h"
#include "map_render/labels.h"
#include "map_render/projection.h"
#include "map_render/tessellator.h"

#include "mbtiles/archive.h"
#include "mvt/decode.h"
#include "mvt/gzip.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

using map_render::Camera;
using map_render::Coordinate;
using map_render::GpuBatch;
using map_render::OffscreenRenderer;
using map_render::LabelTile;
using map_render::Projection;
using map_render::TileGeometry;
using map_render::TileId;
using map_render::TileIdHash;
using map_render::WorldPoint;

// Irvine. The bench archive covers Southern California, so anywhere else
// measures the cost of drawing nothing.
constexpr double kStartLat = 33.6865966;
constexpr double kStartLon = -117.8557874;

// A stage's timings across the run. Median rather than mean: one page fault or
// one scheduler hiccup moves a mean and does not move a median.
class Stage
{
  public:
    explicit Stage(std::string name) : mName(std::move(name)) {}

    void add(double ms) { mSamples.push_back(ms); }

    void report() const
    {
        if (mSamples.empty())
        {
            return;
        }
        std::vector<double> sorted = mSamples;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](double fraction) {
            const std::size_t index = std::min(sorted.size() - 1,
                                               std::size_t(fraction * double(sorted.size())));
            return sorted[index];
        };
        const double median = at(0.5);
        const double worst = sorted.back();
        double total = 0.0;
        for (const double sample : sorted)
        {
            total += sample;
        }
        // p90/p99 as well as the median, because the interesting failure here
        // is not a slow average but a stall: a frame that brings in a new tile
        // costs many times one that does not, and a median hides that entirely.
        SPDLOG_INFO("  {:<22} median {:7.3f}   p90 {:7.3f}   p99 {:7.3f}   worst {:7.3f} ms"
                    "   total {:8.1f} ms",
                    mName, median, at(0.90), at(0.99), worst, total);
    }

    double median() const
    {
        if (mSamples.empty())
        {
            return 0.0;
        }
        std::vector<double> sorted = mSamples;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

  private:
    std::string mName;
    std::vector<double> mSamples;
};

class Timer
{
  public:
    Timer() : mStart(std::chrono::steady_clock::now()) {}

    double ms() const
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - mStart).count();
    }

  private:
    std::chrono::steady_clock::time_point mStart;
};

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

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("%v");

    // The renderer draws into a texture, never a surface -- so offscreen is not
    // a limitation here, it is the supported configuration. See offscreen_renderer.h.
    ::qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const std::string tilesPath = argumentAfter(argc, argv, "--tiles", "");
    const int frames = std::atoi(argumentAfter(argc, argv, "--frames", "300").c_str());
    const int width = std::atoi(argumentAfter(argc, argv, "--width", "660").c_str());
    const int height = std::atoi(argumentAfter(argc, argv, "--height", "640").c_str());
    const double zoom = std::atof(argumentAfter(argc, argv, "--zoom", "14").c_str());
    // The screen's device pixel ratio, which the GPU stage renders at and the
    // label stage draws at. Worth a knob rather than a constant 1: --dpr 2 is
    // four times the fragments and four times the readback, and whether that
    // shows up in `gpu render` is the whole question behind drawing the map at
    // the resolution the screen has.
    const double dpr = std::atof(argumentAfter(argc, argv, "--dpr", "1").c_str());
    // The camera's bearing, which the label stage is far more sensitive to
    // than anything else here.
    //
    // At bearing 0 most roads in a grid city run straight across the screen,
    // and a straight road's name is one blit of a cached string. Turn the map
    // and the same names are laid out character by character along their
    // roads. Measuring only at 0 reports the cheap path and misses the cost of
    // the feature entirely.
    const double bearing = std::atof(argumentAfter(argc, argv, "--bearing", "0").c_str());
    // The camera's pitch. Above 0 the tile walk becomes the quadtree descent
    // and the far field draws from shallower zooms, so the preload below
    // switches from a fixed corridor to the union of what the pitched walk
    // actually wants.
    const double pitch = std::atof(argumentAfter(argc, argv, "--pitch", "0").c_str());
    // Where to write the run's LAST frame as a PNG. The medians say what a
    // frame costs; only a picture says whether it was the right frame.
    const std::string dumpPath = argumentAfter(argc, argv, "--dump", "");

    if (tilesPath.empty())
    {
        SPDLOG_ERROR("--tiles <archive.mbtiles> is required");
        return 2;
    }

    auto archive = mbtiles::Archive::open(tilesPath);
    if (!archive)
    {
        SPDLOG_ERROR("{}: {}", tilesPath, mbtiles::to_string(archive.error()));
        return 1;
    }

    MapStyle_t style;
    const QColor background("#0d0f13");

    auto gpu = OffscreenRenderer::create();
    if (!gpu)
    {
        SPDLOG_ERROR("no GPU backend");
        return 1;
    }

    // ---- load every tile the run will pass over, once -----------------------
    //
    // Decode and tessellation are deliberately OUTSIDE the frame loop: they
    // happen on a zenoh thread once per tile in the real widget, and folding
    // them into the per-frame number would measure a cost the paint pass does
    // not pay.
    struct Cached
    {
        std::shared_ptr<const map_render::LabelSet> labels;
        std::shared_ptr<const TileGeometry> geometry;
    };
    std::unordered_map<TileId, Cached, TileIdHash> cache;

    const auto z = static_cast<std::uint8_t>(zoom);
    const Camera startCamera { Coordinate { kStartLat, kStartLon }, zoom, bearing, pitch };
    const Projection probe(startCamera, width, height, dpr);
    const std::uint8_t archiveMin = archive->metadata().minzoom;

    // The whole corridor the camera will drive along, plus a generous margin.
    const WorldPoint centre = map_render::worldFor(Coordinate { kStartLat, kStartLon });
    const double side = std::exp2(double(z));
    const auto centreX = static_cast<std::int64_t>(centre.x * side);
    const auto centreY = static_cast<std::int64_t>(centre.y * side);

    // Which tiles to decode up front. Flat: the fixed corridor, unchanged so
    // the baseline numbers stay comparable across runs. Pitched: the union of
    // the walk's own answers along the corridor, because the far field pulls
    // shallower tiles no fixed-z loop would name.
    std::vector<TileId> toLoad;
    if (pitch <= 0.0)
    {
        for (std::int64_t dy = -3; dy <= 3; ++dy)
        {
            for (std::int64_t dx = -3; dx <= 12; ++dx)
            {
                toLoad.push_back(TileId { z, static_cast<std::uint32_t>(centreX + dx),
                                          static_cast<std::uint32_t>(centreY + dy) });
            }
        }
    }
    else
    {
        std::unordered_map<TileId, bool, TileIdHash> wanted;
        const double degPerMetre =
            1.0 / (111320.0 * std::cos(kStartLat * std::numbers::pi / 180.0));
        for (int i = 0; i < frames; i += 10)
        {
            const Coordinate here { kStartLat, kStartLon + (double(i) * 25.0 * degPerMetre) };
            const Projection sample(Camera { here, zoom, bearing, pitch }, width, height, dpr);
            for (const TileId& id : sample.visibleTilesWithMargin(z, 1, archiveMin).withMargin)
            {
                wanted.emplace(id, true);
            }
        }
        for (const auto& entry : wanted)
        {
            toLoad.push_back(entry.first);
        }
    }

    std::size_t decoded = 0;
    std::size_t absent = 0;
    std::uint64_t vertices = 0;
    const Timer loadTimer;
    {
        for (const TileId& id : toLoad)
        {
            auto blob = archive->tile(id.z, id.x, id.y);
            if (!blob || !blob->has_value())
            {
                ++absent;
                continue;
            }
            auto raw = mvt::inflateIfCompressed((*blob)->data);
            if (!raw)
            {
                ++absent;
                continue;
            }
            auto tile = mvt::decode(*raw);
            if (!tile)
            {
                ++absent;
                continue;
            }
            auto geometry =
                std::make_shared<const TileGeometry>(map_render::tessellate(*tile, style));
            auto labels =
                std::make_shared<const map_render::LabelSet>(map_render::extractLabels(*tile));
            vertices += geometry->vertices.size();
            cache.emplace(id, Cached { std::move(labels), std::move(geometry) });
            ++decoded;
        }
    }
    const double loadMs = loadTimer.ms();

    SPDLOG_INFO("");
    SPDLOG_INFO("archive    {}", tilesPath);
    SPDLOG_INFO("viewport   {}x{} logical at z{:.1f}, {}x device pixel ratio, bearing {:.0f}, "
                "pitch {:.0f}",
                width, height, zoom, dpr, bearing, pitch);
    SPDLOG_INFO("backend    {} ({}x MSAA)", gpu->backendName().toStdString(),
                gpu->stats().sampleCount);
    SPDLOG_INFO("tiles      {} decoded, {} absent, {} vertices, {:.0f} ms to load+tessellate",
                decoded, absent, vertices, loadMs);
    SPDLOG_INFO("");

    if (decoded == 0)
    {
        SPDLOG_ERROR("no tiles at z{} near Irvine in this archive; nothing to measure", z);
        return 1;
    }

    // ---- the frame loop -----------------------------------------------------

    Stage visible("visible tiles");
    Stage ready("gather batches");
    Stage render("gpu render");
    // The same frames, split by whether the vertex buffer had to be rewritten.
    // A pan that brings in a tile is a different cost class from one that does
    // not, and the combined median reports neither of them.
    Stage renderUpload("  ^ upload frames");
    Stage renderSteady("  ^ steady frames");
    Stage labels("labels (place only)");
    Stage trackCoord("track (Coordinate)");
    Stage trackWorld("track (WorldPoint)");
    Stage frame("WHOLE FRAME");

    // A track behind the vehicle, as the widget keeps one.
    constexpr int kTrackPoints = 600;
    std::deque<Coordinate> track;
    std::deque<WorldPoint> trackWorldPoints;

    // At the ratio, like the widget's backing store: the label pass renders
    // its glyphs at the canvas's ratio, so a 1x canvas would measure cheaper
    // text than the screen actually draws.
    QImage canvas(int(std::lround(width * dpr)), int(std::lround(height * dpr)),
                  QImage::Format_RGBA8888);
    canvas.setDevicePixelRatio(dpr);
    map_render::LabelCache labelCache;
    std::vector<map_render::TextQuad> textQuads;

    // ~25 m per fix at 10 Hz is about 90 km/h, and moving every frame is the
    // point: a stationary camera would measure the memoised case, not the
    // driving one.
    constexpr double kMetresPerFix = 25.0;
    const double degPerMetreLon = 1.0 / (111320.0 * std::cos(kStartLat * std::numbers::pi / 180.0));

    for (int i = 0; i < frames; ++i)
    {
        const Coordinate here { kStartLat, kStartLon + (double(i) * kMetresPerFix * degPerMetreLon) };

        track.push_back(here);
        trackWorldPoints.push_back(map_render::worldFor(here));
        while (track.size() > kTrackPoints)
        {
            track.pop_front();
            trackWorldPoints.pop_front();
        }

        const Timer frameTimer;

        const Camera camera { here, zoom, bearing, pitch };
        const Projection projection(camera, width, height, dpr);

        // Exactly what the widget does: the drawn set in its stable order, and
        // the prefetch ring sorted centre-outward for the request path only.
        const Timer visibleTimer;
        auto sets = projection.visibleTilesWithMargin(z, 1, archiveMin);
        const std::vector<TileId> wanted = sets.drawn;
        projection.sortCentreOutward(sets.withMargin);
        visible.add(visibleTimer.ms());

        const Timer readyTimer;
        std::vector<GpuBatch> batches;
        std::vector<LabelTile> labelTiles;
        batches.reserve(wanted.size());
        labelTiles.reserve(wanted.size());
        for (const TileId& id : wanted)
        {
            const auto found = cache.find(id);
            if (found == cache.end())
            {
                continue;
            }
            batches.push_back(GpuBatch { id, found->second.geometry });
            labelTiles.push_back(LabelTile { id, found->second.labels });
        }
        ready.add(readyTimer.ms());

        // Text is placed on the CPU and drawn by the GPU with the tiles, so
        // the label stage now times placement and collision only -- the draw
        // shows up in `gpu render`.
        const Timer labelTimer;
        const map_render::LabelStats placed =
            map_render::layOutText(projection, labelTiles, style, labelCache, dpr, textQuads);
        labels.add(labelTimer.ms());
        gpu->setText(textQuads, labelCache.atlas().page(), labelCache.atlas().dirty());
        labelCache.atlas().markClean();

        const std::uint64_t uploadsBeforeFrame = gpu->stats().uploads;
        const Timer renderTimer;
        const QImage& gpuFrame = gpu->render(projection, batches, style, background);
        const double renderMs = renderTimer.ms();
        render.add(renderMs);
        if (gpu->stats().uploads != uploadsBeforeFrame)
        {
            renderUpload.add(renderMs);
        }
        else
        {
            renderSteady.add(renderMs);
        }

        if (!dumpPath.empty() && i == frames - 1)
        {
            gpuFrame.save(QString::fromStdString(dumpPath));
            SPDLOG_INFO("frame {} written to {}", i, dumpPath);
        }

        QPainter painter(&canvas);
        if (!gpuFrame.isNull())
        {
            painter.drawImage(QPointF(0.0, 0.0), gpuFrame);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);


        (void)placed;

        // Both track projections, side by side, so the cost of the change is
        // visible in one run rather than across two.
        {
            const Timer t;
            QPainterPath path;
            bool started = false;
            for (const Coordinate& point : track)
            {
                const auto at = projection.screenFor(point);
                const QPointF pixel(at.x, at.y);
                if (!started)
                {
                    path.moveTo(pixel);
                    started = true;
                }
                else
                {
                    path.lineTo(pixel);
                }
            }
            QPen pen(QColor("#FF3B30"));
            pen.setWidthF(3.0);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            trackCoord.add(t.ms());
        }
        {
            const Timer t;
            QPainterPath path;
            bool started = false;
            for (const WorldPoint& point : trackWorldPoints)
            {
                const auto at = projection.screenFor(point);
                const QPointF pixel(at.x, at.y);
                if (!started)
                {
                    path.moveTo(pixel);
                    started = true;
                }
                else
                {
                    path.lineTo(pixel);
                }
            }
            QPen pen(QColor("#FF3B30"));
            pen.setWidthF(3.0);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            trackWorld.add(t.ms());
        }

        painter.end();
        frame.add(frameTimer.ms());
    }

    const OffscreenRenderer::Stats stats = gpu->stats();

    SPDLOG_INFO("{} frames:", frames);
    visible.report();
    ready.report();
    render.report();
    renderUpload.report();
    renderSteady.report();
    labels.report();
    trackCoord.report();
    trackWorld.report();
    frame.report();
    SPDLOG_INFO("");
    SPDLOG_INFO("  uploads {} over {} frames   ({} draw calls, {} vertices resident)",
                stats.uploads, frames, stats.drawCalls, stats.vertices);
    // The number that says whether uploads are incremental. `uploadedVertices`
    // divided by `uploads` is the average frame's traffic; if it approaches
    // `vertices resident` then every upload is rewriting the whole buffer and
    // the arena is not doing its job. `compactions` is how often it had to.
    SPDLOG_INFO("  {} vertices uploaded in total ({:.0f} per upload frame), {} compactions",
                stats.uploadedVertices,
                stats.uploads > 0 ? double(stats.uploadedVertices) / double(stats.uploads) : 0.0,
                stats.compactions);
    // The camera moves every frame here, so this must stay 0. Anything else
    // means the frame memo's key is missing an input.
    SPDLOG_INFO("  frames reused {} (expected 0: the camera moves every frame)", stats.reused);
    SPDLOG_INFO("");

    // --- the crossfade scenario -------------------------------------------
    //
    // A parked camera with one tile's alpha ramping 0 -> 1: every frame is a
    // memo miss by design, so this is the animated-frame cost -- the number
    // that decides whether the synchronous GPU readback needs its own project.
    // Uploads must NOT climb: a fade is a uniform write, never a re-upload.
    {
        Stage fadeStage("fade frame");
        const Camera camera { Coordinate { kStartLat, kStartLon }, zoom, bearing };
        const Projection projection(camera, width, height, dpr);
        auto sets = projection.visibleTilesWithMargin(z, 0);

        std::vector<GpuBatch> batches;
        for (const TileId& id : sets.drawn)
        {
            const auto found = cache.find(id);
            if (found != cache.end())
            {
                batches.push_back(GpuBatch { id, found->second.geometry });
            }
        }
        // One unmeasured frame first: this batch set differs from the main
        // loop's last frame, so its vertices upload once, legitimately. The
        // invariant under test is that the FADE causes no further uploads.
        if (!batches.empty())
        {
            gpu->render(projection, batches, style, background);
        }
        const std::uint64_t uploadsBefore = gpu->stats().uploads;
        const std::uint64_t reusedBefore = gpu->stats().reused;
        constexpr int kFadeFrames = 60;
        for (int i = 0; i < kFadeFrames && !batches.empty(); ++i)
        {
            // The LAST tile fades; the rest sit solid, like a viewport where
            // one tile just arrived.
            batches.back().alpha = float(i) / float(kFadeFrames - 1);
            const Timer t;
            gpu->render(projection, batches, style, background);
            fadeStage.add(t.ms());
        }
        fadeStage.report();
        SPDLOG_INFO("  fade uploads {} (expected 0: alpha is uniform-only), reused {} "
                    "(expected 0: every fade frame is a fresh picture)",
                    gpu->stats().uploads - uploadsBefore, gpu->stats().reused - reusedBefore);
        SPDLOG_INFO("");
    }

    return 0;
}
