// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fast host: MapPass recorded straight into the widget's own swapchain.
//
// THIS FILE IS THE PART CI CANNOT RUN. Under QT_QPA_PLATFORM=offscreen a
// QRhiWidget never initialises, so nothing below this line is exercised by any
// test in the tree -- which is exactly why it is this short and why every
// decision about what a pixel looks like lives in MapPass instead. Read it in
// one sitting; there is no test that will tell you it drifted.
#ifndef MAP_SURFACE_RHI_WIDGET_HOST_H
#define MAP_SURFACE_RHI_WIDGET_HOST_H

#include <QRhiWidget>

#include "map_host.h"

namespace map_surface
{

class RhiWidgetHost : public QRhiWidget, public MapHost
{
    Q_OBJECT

  public:
    explicit RhiWidgetHost(QWidget* parent = nullptr);
    ~RhiWidgetHost() override;

    QWidget* widget() override { return this; }
    void submit(MapContent content) override;
    void setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                 bool atlasDirty) override;
    bool repaintsOverlay() const override { return false; }
    double lastMapMs() const override { return mLastRecordMs; }
    std::uint64_t framesDrawn() const override { return mFramesRecorded; }
    bool isUsable() const override { return !mInitialiseFailed; }
    bool usesRhi() const override { return true; }
    QString backendName() const override;
    map_render::MapPass::Stats stats() const override { return mPass.stats(); }

  protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

  private:
    map_render::MapPass mPass;
    MapContent mContent;

    // What the pass's resources were built against. QRhiWidget hands back a NEW
    // render pass descriptor after a resize that reallocates the target, and a
    // pipeline built against the old one draws nothing while reporting success
    // -- so this guard is load-bearing. It also stops a window drag throwing
    // away every tile already on the GPU.
    QRhi* mBuiltFor { nullptr };
    QRhiRenderPassDescriptor* mBuiltAgainst { nullptr };
    bool mInitialiseFailed { false };
    std::uint64_t mFramesRecorded { 0 };
    double mLastRecordMs { 0.0 };
};

} // namespace map_surface

#endif // MAP_SURFACE_RHI_WIDGET_HOST_H
