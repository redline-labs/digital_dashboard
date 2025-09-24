#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>

#include <QtGui/QWindow>
#include <QtGui/QExposeEvent>
#include <QtGui/QResizeEvent>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "carplay/metal_renderer.h"

// Simple passthrough vertex and fragment shaders embedded as Metal source.
static NSString* const kMetalShaderSource = @"\
using namespace metal;\n\
struct VSOut { float4 position [[position]]; float2 uv; };\n\
vertex VSOut vmain(uint vid [[vertex_id]]) {\n\
  constexpr float2 pos[6] = { {-1,1}, {-1,-1}, {1,-1}, { -1,1 }, {1,-1}, {1,1} };\n\
  constexpr float2 uv[6]  = { {0,0}, {0,1}, {1,1}, { 0,0 }, {1,1}, {1,0} };\n\
  VSOut o; o.position = float4(pos[vid], 0, 1); o.uv = uv[vid]; return o;\n\
}\n\
fragment float4 fmain(VSOut in [[stage_in]],\n\
                     texture2d<float> texY [[texture(0)]],\n\
                     texture2d<float> texU [[texture(1)]],\n\
                     texture2d<float> texV [[texture(2)]],\n\
                     sampler s [[sampler(0)]]) {\n\
  float y = texY.sample(s, in.uv).r;\n\
  float u = texU.sample(s, in.uv).r - 0.5;\n\
  float v = texV.sample(s, in.uv).r - 0.5;\n\
  float3 rgb;\n\
  rgb.r = y + 1.402 * v;\n\
  rgb.g = y - 0.344136 * u - 0.714136 * v;\n\
  rgb.b = y + 1.772 * u;\n\
  return float4(rgb, 1.0);\n\
}\n";

class MetalWindowPrivate {
  public:
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSamplerState> sampler = nil;
    CAMetalLayer* layer = nil;
    id<MTLTexture> texY = nil;
    id<MTLTexture> texU = nil;
    id<MTLTexture> texV = nil;
    int frameWidth = 0;
    int frameHeight = 0;
};

MetalWindow::MetalWindow()
    : QWindow()
    , _device(nullptr)
    , _commandQueue(nullptr)
    , _pipelineState(nullptr)
    , _samplerState(nullptr)
    , _metalLayer(nullptr)
    , _texY(nullptr)
    , _texU(nullptr)
    , _texV(nullptr)
{
    setSurfaceType(QWindow::MetalSurface);
}

MetalWindow::~MetalWindow()
{
    // ARC handles Objective-C objects
}

static id<MTLTexture> createOrResizePlaneTexture(id<MTLDevice> device,
                                                 id<MTLTexture> existing,
                                                 NSUInteger width,
                                                 NSUInteger height)
{
    if (existing && existing.width == width && existing.height == height) return existing;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:desc];
}

void MetalWindow::initializeIfNeeded()
{
    if (_device) return;

    auto d = new MetalWindowPrivate();
    d->device = MTLCreateSystemDefaultDevice();
    if (!d->device) {
        SPDLOG_ERROR("Metal device unavailable");
        return;
    }
    d->queue = [d->device newCommandQueue];

    // Build shaders
    NSError* err = nil;
    id<MTLLibrary> lib = [d->device newLibraryWithSource:kMetalShaderSource options:nil error:&err];
    if (!lib) {
        SPDLOG_ERROR("Metal shader compile failed: {}", err.localizedDescription.UTF8String);
        return;
    }
    id<MTLFunction> vfunc = [lib newFunctionWithName:@"vmain"];
    id<MTLFunction> ffunc = [lib newFunctionWithName:@"fmain"];
    MTLRenderPipelineDescriptor* pdesc = [[MTLRenderPipelineDescriptor alloc] init];
    pdesc.vertexFunction = vfunc;
    pdesc.fragmentFunction = ffunc;
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d->pipeline = [d->device newRenderPipelineStateWithDescriptor:pdesc error:&err];
    if (!d->pipeline) {
        SPDLOG_ERROR("Metal pipeline creation failed: {}", err.localizedDescription.UTF8String);
        return;
    }

    MTLSamplerDescriptor* sdesc = [[MTLSamplerDescriptor alloc] init];
    sdesc.minFilter = MTLSamplerMinMagFilterLinear;
    sdesc.magFilter = MTLSamplerMinMagFilterLinear;
    d->sampler = [d->device newSamplerStateWithDescriptor:sdesc];

    // Do NOT alter the NSView's layer; Qt's Cocoa plugin owns it for MetalSurface
    d->layer = nil;

    _device = (__bridge_retained void*)d->device;
    _commandQueue = (__bridge_retained void*)d->queue;
    _pipelineState = (__bridge_retained void*)d->pipeline;
    _samplerState = (__bridge_retained void*)d->sampler;
    _metalLayer = (__bridge_retained void*)d->layer;
}

void MetalWindow::updateDrawableSize()
{
    CAMetalLayer* layer = (__bridge CAMetalLayer*)_metalLayer;
    if (!layer) {
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(winId());
        if (!view) return;
        CALayer* current = view.layer;
        if (![current isKindOfClass:[CAMetalLayer class]]) return;
        layer = (CAMetalLayer*)current;
        _metalLayer = (__bridge_retained void*)layer;
    }
    const QSize sz = size();
    layer.drawableSize = CGSizeMake(sz.width() * devicePixelRatio(), sz.height() * devicePixelRatio());
}

void MetalWindow::exposeEvent(QExposeEvent* /*event*/)
{
    initializeIfNeeded();
    updateDrawableSize();
}

void MetalWindow::resizeEvent(QResizeEvent* /*event*/)
{
    updateDrawableSize();
}

void MetalWindow::presentYUV420P(const AVFrame* frame)
{
    if (!frame) return;
    initializeIfNeeded();

    id<MTLDevice> device = (__bridge id<MTLDevice>)_device;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)_commandQueue;
    id<MTLRenderPipelineState> pipeline = (__bridge id<MTLRenderPipelineState>)_pipelineState;
    id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)_samplerState;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)_metalLayer;
    if (!layer) {
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(winId());
        if (!view) return;
        CALayer* current = view.layer;
        if (![current isKindOfClass:[CAMetalLayer class]]) return;
        layer = (CAMetalLayer*)current;
        _metalLayer = (__bridge_retained void*)layer;
    }
    if (!device || !queue || !pipeline || !layer) return;

    const int w = frame->width;
    const int h = frame->height;

    // (Re)create plane textures if needed
    id<MTLTexture> texY = createOrResizePlaneTexture(device, (__bridge id<MTLTexture>)_texY, w, h);
    id<MTLTexture> texU = createOrResizePlaneTexture(device, (__bridge id<MTLTexture>)_texU, w/2, h/2);
    id<MTLTexture> texV = createOrResizePlaneTexture(device, (__bridge id<MTLTexture>)_texV, w/2, h/2);
    _texY = (__bridge_retained void*)texY;
    _texU = (__bridge_retained void*)texU;
    _texV = (__bridge_retained void*)texV;

    // Upload planes; handle linesize stride
    MTLRegion fullY = MTLRegionMake2D(0, 0, w, h);
    [texY replaceRegion:fullY mipmapLevel:0 withBytes:frame->data[0] bytesPerRow:frame->linesize[0]];
    MTLRegion fullU = MTLRegionMake2D(0, 0, w/2, h/2);
    [texU replaceRegion:fullU mipmapLevel:0 withBytes:frame->data[1] bytesPerRow:frame->linesize[1]];
    MTLRegion fullV = MTLRegionMake2D(0, 0, w/2, h/2);
    [texV replaceRegion:fullV mipmapLevel:0 withBytes:frame->data[2] bytesPerRow:frame->linesize[2]];

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipeline];

    [enc setFragmentTexture:texY atIndex:0];
    [enc setFragmentTexture:texU atIndex:1];
    [enc setFragmentTexture:texV atIndex:2];
    [enc setFragmentSamplerState:sampler atIndex:0];

    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}


