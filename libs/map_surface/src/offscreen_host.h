// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host that works everywhere: render into a texture, read it back, blit
// it with a QPainter.
//
// It is what the map has always done, and it is what agent control and every
// `gui` test see, because a QRhiWidget does not come up under
// QT_QPA_PLATFORM=offscreen at all. It is also the slow one -- MEASURED, the
// readback and the blit are ten to fifteen times the cost of recording the pass
// -- which is why it is no longer the only one.
#ifndef MAP_SURFACE_OFFSCREEN_HOST_H
#define MAP_SURFACE_OFFSCREEN_HOST_H

#include <memory>

#include <QWidget>

#include "map_host.h"
#include "map_render/offscreen_renderer.h"

namespace map_surface
{

class OffscreenHost : public QWidget, public MapHost
{
    Q_OBJECT

  public:
    explicit OffscreenHost(QWidget* parent = nullptr);
    ~OffscreenHost() override;

    QWidget* widget() override { return this; }
    void submit(MapContent content) override;
    void setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                 bool atlasDirty) override;
    bool repaintsOverlay() const override { return true; }
    double lastMapMs() const override { return mLastPaintMs; }
    // Counts paints that actually RENDERED. A repaint served from the frame
    // memo drew no map, and in this arrangement there are a great many of them
    // -- see paintEvent().
    std::uint64_t framesDrawn() const override { return mFramesPainted; }
    bool isUsable() const override { return mRenderer != nullptr; }
    bool usesRhi() const override { return false; }
    QString backendName() const override;
    map_render::MapPass::Stats stats() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    std::unique_ptr<map_render::OffscreenRenderer> mRenderer;
    MapContent mContent;
    double mLastPaintMs { 0.0 };
    std::uint64_t mFramesPainted { 0 };
};

} // namespace map_surface

#endif // MAP_SURFACE_OFFSCREEN_HOST_H
