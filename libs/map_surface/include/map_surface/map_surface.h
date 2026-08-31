// SPDX-License-Identifier: GPL-3.0-or-later
//
// A map you can put in a layout. ONE type, whichever way the pixels get there.
//
// There are two ways to draw this map and they have nothing in common
// structurally:
//
//   * a QRhiWidget, which records the pass straight into the widget's own
//     swapchain -- no readback, no blit, and the GUI thread never waits for the
//     GPU. MEASURED on a real display, GUI-thread milliseconds per frame:
//     3.09 -> 0.27 at 660x640, 5.04 -> 0.36 at 1920x1080 pitched. Ten to
//     fifteen times cheaper for the same drawing.
//   * an offscreen QRhi texture read back into a QImage and blitted by a
//     QPainter. Slower, and the only one that exists under
//     QT_QPA_PLATFORM=offscreen -- which is every agent-control session and
//     every `gui` test, because QOffscreenIntegration::hasCapability returns
//     false for RhiBasedRendering unconditionally and QRhiWidget bails before
//     it ever picks a backend.
//
// Neither can be dropped. So the choice is made HERE, once, in the constructor,
// and nothing outside this library ever sees it: an embedder -- the map
// widget, the scope panel -- adds a MapSurface as a child, hands it a frame,
// and hands it a callback for its own marker and trail.
//
// WHAT THE EMBEDDER HAS TO KNOW, and it is only this:
//
//   * MapSurface is WA_TransparentForMouseEvents. Gestures land on the
//     embedder, as they did when it painted the map itself. That is also why floating
//     chrome -- the map_controls buttons -- must stay a child of the EMBEDDER
//     and not of this widget: Qt's hit test does not descend into a
//     transparent-for-mouse subtree, so a button parented in here would draw
//     and never be clickable. MEASURED: a sibling button over the nested
//     render-to-texture widget composites correctly and is found by childAt().
//   * Overlay drawing goes through setOverlayPainter(), not through the
//     embedder's own paintEvent. A QRhiWidget HAS NO PAINT ENGINE -- QPainter on one logs
//     "QWidget::paintEngine: Should no longer be called" and draws nothing --
//     so the embedder's marker cannot be painted after a blit any more. It is
//     painted on a transparent child layer above the map instead, which Qt
//     composites for us.
//   * refreshOverlay() repaints the marker and trail WITHOUT redrawing the map.
//     A moving vehicle over a still map is the common case and it now costs no
//     GPU work at all.
#ifndef MAP_SURFACE_H
#define MAP_SURFACE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QColor>
#include <QImage>
#include <QString>
#include <QWidget>

#include "map_render/map_pass.h"
#include "map_render/projection.h"
#include "map_render/style.h"

class QPainter;

namespace map_surface
{

// True when a QRhiWidget can come up at all in this process. False under
// QT_QPA_PLATFORM=offscreen, where the platform integration refuses
// RhiBasedRendering. Constant for the life of the process.
//
// Public because a caller may want to SAY which path it got -- the map widget's
// diagnostic overlay does -- not because anyone should branch on it.
bool rhiWidgetsAreAvailable();

class MapHost;
class MapOverlay;

class MapSurface : public QWidget
{
    Q_OBJECT

  public:
    using GpuBatch = map_render::GpuBatch;
    using Highlight = map_render::MapPass::Highlight;
    using Stats = map_render::MapPass::Stats;

    // The embedder's own drawing, over the map: its marker, its trail, its legend,
    // its diagnostics. Given a painter in LOGICAL widget coordinates with the
    // map already beneath it, so the bodies that used to run at the end of a
    // paintEvent move across unchanged.
    using OverlayPainter = std::function<void(QPainter&)>;

    explicit MapSurface(QWidget* parent = nullptr);
    ~MapSurface() override;

    // Which host is picked, so a caller can force the slow one. Mostly for
    // the bench and for tests that want to exercise both on a machine that has
    // a display.
    enum class Host
    {
        automatic,
        offscreen
    };
    explicit MapSurface(Host host, QWidget* parent = nullptr);

    void setOverlayPainter(OverlayPainter painter);

    // What to draw next. COPIED rather than referenced: with the QRhiWidget
    // host the drawing happens later, when Qt gets round to the frame, and
    // by then the caller's projection and batch list are long gone. The copies
    // are shared_ptrs and a few hundred ids, not geometry.
    void setFrame(const map_render::Projection& projection, std::vector<GpuBatch> batches,
                  const MapStyle_t& style, const QColor& background,
                  const Highlight& highlight = Highlight {});
    void setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                 bool atlasDirty);

    // Repaint the overlay alone. The marker moved; the map did not. setFrame()
    // already repaints it, so this is only for the case where nothing was
    // handed to the map -- and with the QRhiWidget host it costs no GPU work
    // whatsoever, which is the common case on a vehicle standing still.
    void refreshOverlay();

    // GUI-thread milliseconds the last map frame cost, and how many have been
    // drawn -- the same quantity whichever host is underneath, excluding the
    // overlay. What the map widget's diagnostic overlay reports, and what the
    // bench compares.
    double lastMapMs() const;
    std::uint64_t framesDrawn() const;
    // The same, for the transparent layer: the whole paintEvent, including the
    // host's own callback. Reported apart from the map because the two are
    // charged to different people -- one is this library's, one is the embedder's.
    double lastOverlayMs() const;
    std::uint64_t overlayFramesPainted() const;

    // False when no host at all could be created -- no GPU. That is a hard
    // failure with no fallback left, and the embedder has to say so rather
    // than show an empty widget.
    bool isUsable() const;
    bool usesRhi() const;
    QString backendName() const;
    Stats stats() const;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void build(Host host);

    std::unique_ptr<MapHost> mHost;
    // Owned by the MapHost's widget, so it is a CHILD of the render-to-texture
    // widget rather than a sibling -- the arrangement Qt is documented and
    // measured to composite.
    MapOverlay* mOverlay { nullptr };
};

} // namespace map_surface

#endif // MAP_SURFACE_H
