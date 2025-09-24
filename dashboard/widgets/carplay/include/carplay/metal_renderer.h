#ifndef CARPLAY_METAL_RENDERER_H_
#define CARPLAY_METAL_RENDERER_H_

#include <QtGui/QWindow>

struct AVFrame;

class MetalWindow : public QWindow
{

  public:
    MetalWindow();
    ~MetalWindow() override;

    void presentYUV420P(const AVFrame* frame);

  protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void initializeIfNeeded();
    void updateDrawableSize();

    // Opaque pointers to Objective-C objects
    void* _device;           // id<MTLDevice>
    void* _commandQueue;     // id<MTLCommandQueue>
    void* _pipelineState;    // id<MTLRenderPipelineState>
    void* _samplerState;     // id<MTLSamplerState>
    void* _metalLayer;       // CAMetalLayer*

    // Re-usable textures for Y, U, V planes
    void* _texY;             // id<MTLTexture>
    void* _texU;             // id<MTLTexture>
    void* _texV;             // id<MTLTexture>
};

#endif  // CARPLAY_METAL_RENDERER_H_


