// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/map_widget.h"

#include "map/labels.h"

#include <QFont>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace
{

// An extra ring of tiles around the viewport, fetched but not visible. It is
// what makes a pan show map rather than background at the leading edge.
constexpr int kPrefetchRingTiles = 1;

QColor toQColor(const helpers::Color& color)
{
    return QColor(QString::fromStdString(color.value()));
}

} // namespace

MapWidget::MapWidget(const config_t& config, QWidget* parent) :
    QWidget(parent), mConfig(config)
{
    setAutoFillBackground(false);

    // A failure here is not fatal and must not throw: there is no CPU fallback,
    // but a widget that reports "no GPU" in its own frame is far easier to
    // diagnose than one that refuses to construct and takes the layout with it.
    mGpu = map_widget::GpuRenderer::create();
    if (!mGpu)
    {
        SPDLOG_ERROR("[map] no QRhi backend; the map will draw labels and marker only");
    }
    else
    {
        SPDLOG_INFO("[map] GPU backend: {}", mGpu->backendName().toStdString());
    }

    // This callback fires ON A ZENOH THREAD. The only safe thing it may do is
    // post to the GUI thread; touching the widget here would race the paint.
    //
    // The exchange is the coalescing: the first tile of a burst posts one
    // invoke, the rest see the flag set and post nothing, and the single
    // handler drains however many arrived. See mDrainPending.
    mTiles = std::make_unique<map_widget::TileSource>(
        mConfig.tileset, mConfig.tile_zenoh_key, mConfig.request_timeout_ms, mConfig.style,
        [this]() {
            if (mDrainPending.exchange(true))
            {
                return;
            }

            QMetaObject::invokeMethod(
                this,
                [this]() {
                    mDrainPending.store(false);
                    if (mTiles && mTiles->drain() > 0)
                    {
                        update();
                    }
                },
                Qt::QueuedConnection);
        });

    if (!mConfig.position_zenoh_key.empty())
    {
        if (!mConfig.latitude_expression.empty())
        {
            mLatitudeSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.latitude_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setLatitude, "map latitude");
        }
        if (!mConfig.longitude_expression.empty())
        {
            mLongitudeSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.longitude_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setLongitude, "map longitude");
        }
        if (!mConfig.heading_expression.empty())
        {
            mHeadingSubscription = dashboard::makeExpressionSubscription<double>(
                mConfig.position_schema_type, mConfig.heading_expression,
                mConfig.position_zenoh_key, this, &MapWidget::setHeading, "map heading");
        }

        if (mConfig.rotate_with_heading && mConfig.heading_expression.empty())
        {
            SPDLOG_WARN("[map] rotate_with_heading is set but heading_expression is empty; the "
                        "map will keep the configured bearing");
        }
    }

    // No refreshTiles() here on purpose. A widget is constructed at Qt's
    // default 640x480 and sized by its layout afterwards, so fetching now would
    // request a dozen tiles for a viewport this widget never has. The paint
    // pass asks for what it is about to draw.
}

MapWidget::~MapWidget() = default;

map_widget::Camera MapWidget::camera() const
{
    map_widget::Camera out;

    // The vehicle wins once there is one and follow is on; otherwise the
    // configured centre, which is also what the editor previews.
    if (mConfig.follow_vehicle && hasPosition())
    {
        out.center = map_widget::Coordinate { *mLatitude, *mLongitude };
    }
    else
    {
        out.center =
            map_widget::Coordinate { mConfig.center_latitude, mConfig.center_longitude };
    }

    out.zoom = mConfig.zoom;
    out.bearing = (mConfig.rotate_with_heading && mHeading.has_value()) ? *mHeading
                                                                       : mConfig.bearing;
    return out;
}

void MapWidget::refreshTiles()
{
    // CLEARED, not left alone. A widget is constructed at Qt's default size and
    // may then be resized to nothing by a layout that has not run yet; keeping
    // the tiles worked out for the default size would have it claim to need
    // tiles it will never draw, and status() would report a healthy map.
    if (width() <= 0 || height() <= 0 || !mTiles)
    {
        mVisible.clear();
        return;
    }

    const map_widget::Projection projection(camera(), width(), height());
    const std::uint8_t z = projection.tileZoom(static_cast<std::uint8_t>(mConfig.min_zoom),
                                               static_cast<std::uint8_t>(mConfig.max_zoom));

    // Both sets from one walk of the grid. The prefetch ring is requested but
    // never drawn, which is what keeps mVisible honest about what the paint
    // pass will look at -- and what status() reports.
    auto tiles = projection.visibleTilesWithMargin(z, kPrefetchRingTiles);
    mVisible = std::move(tiles.drawn);

    // Sorted centre-outward HERE and not in mVisible: the request order decides
    // which tiles win the in-flight slots, and the draw order must stay stable
    // or the renderer re-uploads every tile whenever the camera reshuffles it.
    projection.sortCentreOutward(tiles.withMargin);
    mTiles->request(tiles.withMargin);
}

void MapWidget::resizeEvent(QResizeEvent* event)
{
    // No refreshTiles() here. A resize is always followed by a paint, and the
    // paint pass recomputes at the size actually being drawn -- doing it twice
    // only walked the tile grid twice for the same answer.
    QWidget::resizeEvent(event);
}

void MapWidget::setLatitude(double degrees)
{
    mLatitude = degrees;
    onPositionChanged();
}

void MapWidget::setLongitude(double degrees)
{
    mLongitude = degrees;
    onPositionChanged();
}

void MapWidget::setHeading(double degrees)
{
    mHeading = degrees;
    if (mConfig.rotate_with_heading)
    {
        // update() only: the paint pass refreshes the tile set itself, at the
        // camera it is actually about to draw.
        update();
    }
}

void MapWidget::onPositionChanged()
{
    if (!hasPosition())
    {
        return;
    }

    const map_widget::Coordinate here { *mLatitude, *mLongitude };

    if (mConfig.show_track && mConfig.track_points > 0)
    {
        mTrack.push_back(map_widget::worldFor(here));
        while (mTrack.size() > mConfig.track_points)
        {
            mTrack.pop_front();
        }
    }

    update();
}

void MapWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    const QColor background = toQColor(mConfig.style.background);

    if (!mTiles || width() <= 0 || height() <= 0)
    {
        mVisible.clear();
        mLastTilesDrawn = 0;
        mLastLabelsPlaced = 0;
        painter.fillRect(rect(), background);
        return;
    }

    // Recomputed here rather than only on resize: unlike resizeEvent this is
    // guaranteed to run before anything is drawn, at the size actually painted.
    refreshTiles();

    const map_widget::Projection projection(camera(), width(), height());
    const auto tiles = mTiles->ready(mVisible);

    // --- 1. geometry, on the GPU -------------------------------------------

    std::vector<map_widget::GpuBatch> batches;
    std::vector<map_widget::LabelTile> labelTiles;
    batches.reserve(mVisible.size());
    labelTiles.reserve(mVisible.size());

    for (std::size_t i = 0; i < mVisible.size(); ++i)
    {
        if (!tiles[i])
        {
            continue;
        }
        batches.push_back(map_widget::GpuBatch { mVisible[i], tiles[i].geometry });
        labelTiles.push_back(map_widget::LabelTile { mVisible[i], tiles[i].tile });
    }
    mLastTilesDrawn = static_cast<int>(batches.size());

    if (mGpu)
    {
        const QImage& frame = mGpu->render(projection, batches, mConfig.style, background);
        if (frame.isNull())
        {
            painter.fillRect(rect(), background);
        }
        else
        {
            // Straight blit, no scaling: the renderer was asked for exactly this
            // viewport. SmoothPixmapTransform would be pure cost.
            painter.drawImage(0, 0, frame);
        }
    }
    else
    {
        painter.fillRect(rect(), background);
    }

    // --- 2. labels, on the CPU ---------------------------------------------

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const map_widget::LabelStats labels =
        map_widget::paintLabels(painter, projection, labelTiles, mConfig.style, mLabelCache);
    mLastLabelsPlaced = labels.placed;

    // --- 3. the vehicle ----------------------------------------------------

    paintMarker(painter, projection);

    if (mConfig.show_status)
    {
        paintDiagnostic(painter);
    }
}

void MapWidget::paintDiagnostic(QPainter& painter)
{
    // A map with nothing on it has several causes that look identical. Saying
    // which one it is here costs a line of text and saves the alternative,
    // which is reading app_logs to find out why a screenshot is empty.
    QString message;

    if (!mGpu)
    {
        message = QStringLiteral("No GPU backend — map geometry cannot be drawn");
    }
    else if (mLastTilesDrawn == 0)
    {
        const map_widget::TileSourceStats sourceStats = mTiles->stats();

        if (sourceStats.requested == 0)
        {
            message = QStringLiteral("No tiles requested");
        }
        else if (sourceStats.decoded == 0 && sourceStats.failed > 0)
        {
            message = QStringLiteral("No reply from map_server on '%1'")
                          .arg(QString::fromStdString(mConfig.tile_zenoh_key));
        }
        else if (sourceStats.decoded == 0)
        {
            message = QStringLiteral("Waiting for tiles…");
        }
        else
        {
            message = QStringLiteral("No coverage here in tileset '%1'")
                          .arg(QString::fromStdString(mConfig.tileset));
        }
    }

    if (message.isEmpty())
    {
        return;
    }

    QFont font = painter.font();
    font.setPointSizeF(12.0);
    painter.setFont(font);
    painter.setPen(toQColor(mConfig.style.label_text));
    painter.drawText(rect(), Qt::AlignCenter, message);
}

void MapWidget::paintMarker(QPainter& painter, const map_widget::Projection& projection)
{
    if (!hasPosition())
    {
        return;
    }

    const QColor markerColor = toQColor(mConfig.marker_color);

    if (mConfig.show_track && mTrack.size() >= 2)
    {
        QPainterPath path;
        bool started = false;
        for (const map_widget::WorldPoint& point : mTrack)
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

        QPen pen(markerColor);
        pen.setWidthF(mConfig.track_width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.setOpacity(mConfig.track_opacity);
        painter.drawPath(path);
        painter.setOpacity(1.0);
    }

    const auto at = projection.screenFor(map_widget::Coordinate { *mLatitude, *mLongitude });
    const QPointF centre(at.x, at.y);
    const double radius = static_cast<double>(mConfig.marker_size);

    if (mHeading.has_value())
    {
        // A triangle pointing where the vehicle is going. When the map itself
        // rotates with heading the triangle points up the screen, which is
        // exactly right -- the marker's rotation is relative to the map, and
        // the map's is relative to north.
        const double screenHeading = *mHeading - projection.camera().bearing;
        const double radians = screenHeading * 3.14159265358979323846 / 180.0;
        const double sin = std::sin(radians);
        const double cos = std::cos(radians);

        // Nose forward, two tails behind. In an unrotated frame "forward" is
        // -y, because screen y grows downward.
        const auto rotate = [&](double x, double y) {
            return QPointF(centre.x() + (x * cos) - (y * sin),
                           centre.y() + (x * sin) + (y * cos));
        };

        QPolygonF arrow;
        arrow << rotate(0.0, -radius * 1.4) << rotate(radius * 0.9, radius)
              << rotate(0.0, radius * 0.45) << rotate(-radius * 0.9, radius);

        painter.setPen(QPen(toQColor(mConfig.marker_outline_color), 2.0));
        painter.setBrush(markerColor);
        painter.drawPolygon(arrow);
    }
    else
    {
        painter.setPen(QPen(toQColor(mConfig.marker_outline_color), 2.0));
        painter.setBrush(markerColor);
        painter.drawEllipse(centre, radius, radius);
    }
}

MapWidget::Status MapWidget::status() const
{
    Status out;
    if (mTiles)
    {
        out.tiles = mTiles->stats();
    }
    out.tilesVisible = static_cast<int>(mVisible.size());
    out.tilesDrawn = mLastTilesDrawn;
    out.labelsPlaced = mLastLabelsPlaced;
    out.hasPosition = hasPosition();
    out.gpuReady = (mGpu != nullptr);
    if (mGpu)
    {
        out.gpu = mGpu->stats();
    }
    return out;
}
