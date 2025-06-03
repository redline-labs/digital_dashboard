#include "carplay_widget.h"
#include "touch_action.h"

#include <spdlog/spdlog.h>
#include <QOpenGLShaderProgram>
#include <QTimer>
#include <vector>
#include <chrono>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// Helper function to check and log OpenGL errors
static void checkGLError(const char* operation) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        const char* errorString;
        switch (error) {
            case GL_INVALID_ENUM: errorString = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: errorString = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: errorString = "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION: errorString = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
            default: errorString = "Unknown Error"; break;
        }
        SPDLOG_ERROR("OpenGL error during {}: {} (0x{:x})", operation, errorString, error);
    }
}

static const char* vertexShaderSource = R"(
#version 120
attribute vec2 aPos;
attribute vec2 aTexCoord;

varying vec2 TexCoord;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char* fragmentShaderSource = R"(
#version 120
varying vec2 TexCoord;

uniform sampler2D textureY;
uniform sampler2D textureU;
uniform sampler2D textureV;

void main()
{
    // Use optimized YUV->RGB conversion matrix
    float y = texture2D(textureY, TexCoord).r;
    float u = texture2D(textureU, TexCoord).r - 0.5;
    float v = texture2D(textureV, TexCoord).r - 0.5;
    
    // BT.601 optimized conversion (avoid expensive operations)
    vec3 rgb;
    rgb.r = y + 1.402 * v;
    rgb.g = y - 0.344136 * u - 0.714136 * v;  
    rgb.b = y + 1.772 * u;
    
    // Clamp in one operation
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

CarPlayWidget::CarPlayWidget() :
    QOpenGLWidget(),
    m_shaderProgram(nullptr),
    m_textureY(0),
    m_textureU(0),
    m_textureV(0),
    m_vbo(0),
    m_pboIndex(0),
    m_nextPboIndex(1),
    m_frameWidth(0),
    m_frameHeight(0),
    m_hasFrame(false),
    m_phoneConnected(true),
    _codec(nullptr),
    _parser(nullptr),
    _codec_context(nullptr),
    _frame(nullptr),
    _pkt(nullptr),
    _receive_length(0),
    _should_terminate(false),
    _frame_ready(false),
    _frame_check_timer(nullptr)
{
    setStyleSheet("QOpenGLWidget { background-color : black; }");
    
    // Initialize PBO array
    for (int i = 0; i < 6; i++) {
        m_pboIds[i] = 0;
    }
    
    // Initialize decoder
    initializeDecoder();
    
    // Set up timer to check for new frames (30 FPS max to reduce threading issues)
    _frame_check_timer = new QTimer(this);
    connect(_frame_check_timer, &QTimer::timeout, this, &CarPlayWidget::checkForNewFrame);
    _frame_check_timer->start(33); // ~30 FPS
}

CarPlayWidget::~CarPlayWidget()
{
    // Stop the timer first to prevent any more frame checks
    if (_frame_check_timer) {
        _frame_check_timer->stop();
    }
    
    // Stop the decoder thread before cleaning up OpenGL resources
    stop_decoder();
    cleanupDecoder();
    
    // Clear frame data
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        _current_frame.clear();
        _pending_frame.clear();
    }
    
    // Now clean up OpenGL resources
    makeCurrent();
    
    if (m_textureY) glDeleteTextures(1, &m_textureY);
    if (m_textureU) glDeleteTextures(1, &m_textureU);
    if (m_textureV) glDeleteTextures(1, &m_textureV);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    
    // Cleanup PBOs
    for (int i = 0; i < 6; i++) {
        if (m_pboIds[i]) glDeleteBuffers(1, &m_pboIds[i]);
    }
    
    delete m_shaderProgram;
    
    doneCurrent();
}

void CarPlayWidget::setSize(uint32_t width_px, uint32_t height_px)
{
    setFixedSize({static_cast<int>(width_px), static_cast<int>(height_px)});
}

void CarPlayWidget::initializeGL()
{
    initializeOpenGLFunctions();
    
    // Log OpenGL info for debugging
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    SPDLOG_INFO("OpenGL Version: {}", version ? version : "Unknown");
    SPDLOG_INFO("OpenGL Vendor: {}", vendor ? vendor : "Unknown");
    SPDLOG_INFO("OpenGL Renderer: {}", renderer ? renderer : "Unknown");
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_TEXTURE_2D);
    
    setupShaders();
    setupTextures();
    
    // Setup vertex data for a fullscreen quad
    float vertices[] = {
        // positions   // texture coords
        -1.0f,  1.0f,  0.0f, 0.0f,  // top left
        -1.0f, -1.0f,  0.0f, 1.0f,  // bottom left
         1.0f, -1.0f,  1.0f, 1.0f,  // bottom right
         1.0f,  1.0f,  1.0f, 0.0f   // top right
    };
    
    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0
    };
    
    glGenBuffers(1, &m_vbo);
    GLuint ebo;
    glGenBuffers(1, &ebo);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}

void CarPlayWidget::setupShaders()
{
    m_shaderProgram = new QOpenGLShaderProgram();
    
    if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource)) {
        SPDLOG_ERROR("Failed to compile vertex shader: {}", m_shaderProgram->log().toStdString());
        return;
    }
    
    if (!m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource)) {
        SPDLOG_ERROR("Failed to compile fragment shader: {}", m_shaderProgram->log().toStdString());
        return;
    }
    
    if (!m_shaderProgram->link()) {
        SPDLOG_ERROR("Failed to link shader program: {}", m_shaderProgram->log().toStdString());
        return;
    }
}

void CarPlayWidget::setupTextures()
{
    glGenTextures(1, &m_textureY);
    glGenTextures(1, &m_textureU);
    glGenTextures(1, &m_textureV);
    
    // Check if texture generation succeeded
    if (m_textureY == 0 || m_textureU == 0 || m_textureV == 0) {
        SPDLOG_ERROR("Failed to generate OpenGL textures");
        return;
    }
    
    // Generate PBOs for async texture uploads (double buffering)
    glGenBuffers(6, m_pboIds);
    
    // Configure textures
    for (GLuint texture : {m_textureY, m_textureU, m_textureV}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Check for OpenGL errors after each texture configuration
        checkGLError("texture setup");
    }
    
    // Unbind texture
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Create a test pattern to verify rendering works
    createTestPattern();
}

void CarPlayWidget::createTestPattern()
{
    const int width = 64;
    const int height = 64;
    
    // Create test Y plane (white to black gradient)
    std::vector<uint8_t> yData(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            yData[y * width + x] = static_cast<uint8_t>(255 * x / width);
        }
    }
    
    // Create test U and V planes (neutral gray)
    std::vector<uint8_t> uvData(width/2 * height/2, 128);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, yData.data());
    checkGLError("Y texture creation");
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, uvData.data());
    checkGLError("U texture creation");
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, uvData.data());
    checkGLError("V texture creation");
    
    // Reset to texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    m_frameWidth = width;
    m_frameHeight = height;
    m_hasFrame = true;
    
    SPDLOG_INFO("Created test pattern: {}x{}", width, height);
}

void CarPlayWidget::initializeDecoder()
{
    // Get set up for libavcodec.
    _pkt = av_packet_alloc();
    if (_pkt == nullptr)
    {
        SPDLOG_ERROR("Failed to find allocate packet.");
    }

    _codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (_codec == nullptr)
    {
        SPDLOG_ERROR("Failed to find H.264 codec.");
    }

    _parser = av_parser_init(_codec->id);
    if (_parser == nullptr)
    {
        SPDLOG_ERROR("Failed to init parser.");
    }

     _codec_context = avcodec_alloc_context3(_codec);
    if (_codec_context == nullptr)
    {
        SPDLOG_ERROR("Failed to init codec context.");
    }

    // Try hardware decoding first on macOS
    bool hw_decode_success = false;
#ifdef __APPLE__
    SPDLOG_INFO("Attempting to initialize VideoToolbox hardware decoding...");
    
    // Set up VideoToolbox hardware acceleration context
    AVBufferRef* hw_device_ctx = nullptr;
    int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (ret >= 0)
    {
        // Set the hardware device context in the codec context
        _codec_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        
        // Try to open the codec with hardware acceleration
        if (avcodec_open2(_codec_context, _codec, nullptr) >= 0)
        {
            hw_decode_success = true;
            SPDLOG_INFO("VideoToolbox hardware decoding ENABLED");
        }
        else
        {
            SPDLOG_WARN("Failed to open H.264 codec with VideoToolbox, falling back to software");
            // Clean up and prepare for software fallback
            av_buffer_unref(&_codec_context->hw_device_ctx);
            avcodec_free_context(&_codec_context);
            _codec_context = avcodec_alloc_context3(_codec);
        }
        
        av_buffer_unref(&hw_device_ctx);
    }
    else
    {
        SPDLOG_WARN("Failed to create VideoToolbox hardware device context, using software decoding");
    }
#endif

    // Fallback to software decoding if hardware failed or not on macOS
    if (!hw_decode_success)
    {
        if (avcodec_open2(_codec_context, _codec, nullptr) < 0)
        {
            SPDLOG_ERROR("Could not open codec.");
        }
        else
        {
            SPDLOG_INFO("🔧 Software decoding ENABLED");
        }
    }

    _frame = av_frame_alloc();
    if (_frame == nullptr)
    {
        SPDLOG_ERROR("Could not allocate frame.");
    }

    // Start the decode thread.
    _decode_thread = std::thread(std::bind(&CarPlayWidget::run_decode_thread, this));
}

void CarPlayWidget::cleanupDecoder()
{
    if (_parser) av_parser_close(_parser);
    if (_codec_context) avcodec_free_context(&_codec_context);
    if (_frame) av_frame_free(&_frame);
    if (_pkt) av_packet_free(&_pkt);
}

void CarPlayWidget::accept_new_data(const uint8_t* buffer, uint32_t buffer_len)
{
    {
        std::lock_guard lk(_decode_mutex);
        std::copy_n(&buffer[0], std::min(buffer_len, static_cast<uint32_t>(sizeof(_receive_buffer))), &_receive_buffer[0]);
        _receive_length = buffer_len;
    }

    _decode_cv.notify_one();
}

void CarPlayWidget::stop_decoder()
{
    _should_terminate = true;
    _decode_cv.notify_all();
    if (_decode_thread.joinable())
    {
        _decode_thread.join();
    }
}

void CarPlayWidget::run_decode_thread()
{
    SPDLOG_DEBUG("Starting integrated decode thread.");

    while (_should_terminate == false)
    {
        auto endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);

        std::unique_lock lk(_decode_mutex);
        auto res = _decode_cv.wait_until(lk, endTime);
        if (res == std::cv_status::timeout)
        {
            continue;
        }

        const uint8_t* data = _receive_buffer;
        uint32_t data_len = _receive_length;

        while (data_len > 0)
        {
            int ret = av_parser_parse2(
                _parser,
                _codec_context,
                &_pkt->data,
                &_pkt->size,
                data,
                data_len,
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);

            if (ret < 0)
            {
                SPDLOG_ERROR("Failed to parse data.");
                break;
            }
            else
            {
                data += ret;
                data_len -= ret;
            }

            if (_pkt->size)
            {
                decode(_codec_context, _frame, _pkt);
            }
        }
    }

    SPDLOG_DEBUG("Exiting integrated decode thread.");
}

void CarPlayWidget::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        SPDLOG_ERROR("Error sending a packet for decoding.");
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            SPDLOG_ERROR("Error during decoding");
            break;
        }

        // Validate frame dimensions before processing
        if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096) {
            SPDLOG_ERROR("Invalid frame dimensions: {}x{}", frame->width, frame->height);
            continue;
        }

        // Handle both hardware and software decoded frames
        AVFrame* sw_frame = frame;
        AVFrame* temp_frame = nullptr;
        
        // If we got a hardware frame, transfer it to system memory
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        {
            temp_frame = av_frame_alloc();
            if (temp_frame == nullptr)
            {
                SPDLOG_ERROR("Failed to allocate temporary frame for hardware decode transfer");
                continue;
            }
            
            // Transfer data from hardware frame to software frame
            if (av_hwframe_transfer_data(temp_frame, frame, 0) < 0)
            {
                SPDLOG_ERROR("Failed to transfer data from hardware frame");
                av_frame_free(&temp_frame);
                continue;
            }
            
            sw_frame = temp_frame;
        }

        // Validate frame data before copying
        if (sw_frame->data[0] == nullptr || sw_frame->data[1] == nullptr)
        {
            SPDLOG_ERROR("Invalid frame data pointers: Y={}, U={}", 
                        static_cast<void*>(sw_frame->data[0]), static_cast<void*>(sw_frame->data[1]));
            if (temp_frame != nullptr)
            {
                av_frame_free(&temp_frame);
            }
            continue;
        }
        
        // For YUV420P format, also check V pointer
        if (sw_frame->format != AV_PIX_FMT_NV12 && sw_frame->data[2] == nullptr)
        {
            SPDLOG_ERROR("Invalid V plane pointer for YUV420P format: V={}", static_cast<void*>(sw_frame->data[2]));
            if (temp_frame != nullptr)
            {
                av_frame_free(&temp_frame);
            }
            continue;
        }
        
        // Validate strides
        if (sw_frame->linesize[0] <= 0 || sw_frame->linesize[1] <= 0) {
            SPDLOG_ERROR("Invalid frame linesize: Y={}, U={}", sw_frame->linesize[0], sw_frame->linesize[1]);
            if (temp_frame != nullptr)
            {
                av_frame_free(&temp_frame);
            }
            continue;
        }
        
        // Store the decoded frame in our frame buffer
        {
            std::lock_guard<std::mutex> lock(_frame_mutex);
            
            // Clear the pending frame
            _pending_frame.clear();
            
            // Set up frame parameters
            _pending_frame.width = sw_frame->width;
            _pending_frame.height = sw_frame->height;
            _pending_frame.yStride = sw_frame->linesize[0];
            _pending_frame.uStride = sw_frame->linesize[1];
            _pending_frame.vStride = sw_frame->linesize[2];
            _pending_frame.isNV12 = (sw_frame->format == AV_PIX_FMT_NV12);
            
            // Copy frame data with size validation
            const int ySize = _pending_frame.yStride * _pending_frame.height;
            const int uSize = _pending_frame.uStride * (_pending_frame.height / 2);
            const int vSize = _pending_frame.vStride * (_pending_frame.height / 2);
            
            // Sanity check sizes
            if (ySize <= 0 || ySize > 10 * 1024 * 1024 || uSize <= 0 || uSize > 5 * 1024 * 1024) {
                SPDLOG_ERROR("Invalid calculated frame sizes: Y={}, U={}, V={}", ySize, uSize, vSize);
                if (temp_frame != nullptr)
                {
                    av_frame_free(&temp_frame);
                }
                continue;
            }
            
            try {
                _pending_frame.yData = new uint8_t[ySize];
                std::memcpy(_pending_frame.yData, sw_frame->data[0], ySize);
                
                if (_pending_frame.isNV12)
                {
                    // NV12 has interleaved UV data in data[1]
                    const uint8_t* uvData = sw_frame->data[1];
                    const int uvStride = sw_frame->linesize[1];
                    const int uvHeight = sw_frame->height / 2;
                    
                    // Allocate separate U and V arrays
                    const int uvPlaneSize = uvHeight * (sw_frame->width / 2);
                    _pending_frame.uData = new uint8_t[uvPlaneSize];
                    _pending_frame.vData = new uint8_t[uvPlaneSize];
                    
                    // De-interleave UV data
                    for (int y = 0; y < uvHeight; y++)
                    {
                        for (int x = 0; x < sw_frame->width / 2; x++)
                        {
                            const int uvIndex = y * uvStride + x * 2;
                            const int uvOutIndex = y * (sw_frame->width / 2) + x;
                            
                            _pending_frame.uData[uvOutIndex] = uvData[uvIndex];     // U component
                            _pending_frame.vData[uvOutIndex] = uvData[uvIndex + 1]; // V component
                        }
                    }
                    
                    // Update strides for separate planes
                    _pending_frame.uStride = sw_frame->width / 2;
                    _pending_frame.vStride = sw_frame->width / 2;
                }
                else
                {
                    // Standard YUV420P format
                    _pending_frame.uData = new uint8_t[uSize];
                    _pending_frame.vData = new uint8_t[vSize];
                    std::memcpy(_pending_frame.uData, sw_frame->data[1], uSize);
                    std::memcpy(_pending_frame.vData, sw_frame->data[2], vSize);
                }
                
                _pending_frame.isValid = true;
                _frame_ready = true;
            } catch (const std::bad_alloc& e) {
                SPDLOG_ERROR("Memory allocation failed for frame data: {}", e.what());
                _pending_frame.clear();
                if (temp_frame != nullptr)
                {
                    av_frame_free(&temp_frame);
                }
                continue;
            }
        }
        
        // Clean up temporary frame if we created one
        if (temp_frame != nullptr)
        {
            av_frame_free(&temp_frame);
        }
    }
}

void CarPlayWidget::checkForNewFrame()
{
    // Safety check - don't process frames if we're shutting down
    if (_should_terminate.load()) {
        return;
    }
    
    if (_frame_ready.load())
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        if (_pending_frame.isValid)
        {
            // Swap frames
            _current_frame.clear();
            _current_frame = std::move(_pending_frame);
            _pending_frame = DecodedFrame{};
            _frame_ready = false;
            
            // Update and trigger repaint
            m_hasFrame = true;
            update();
        }
    }
}

void CarPlayWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (!m_hasFrame || !m_phoneConnected) {
        // Just clear to black when no frame or phone not connected
        return;
    }
    
    if (!m_shaderProgram) {
        return;
    }
    
    // Upload new frame data if available (we're already in OpenGL context)
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        if (_current_frame.isValid) {
            uploadYUVTextures(_current_frame);
        }
    }
    
    m_shaderProgram->bind();
    
    // Set texture uniforms once (they don't change)
    static bool uniformsSet = false;
    if (!uniformsSet) {
        m_shaderProgram->setUniformValue("textureY", 0);
        m_shaderProgram->setUniformValue("textureU", 1);
        m_shaderProgram->setUniformValue("textureV", 2);
        uniformsSet = true;
    }
    
    // Explicitly bind textures to their respective texture units
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    
    // Setup vertex attributes manually for OpenGL 2.1
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    
    GLint posAttrib = m_shaderProgram->attributeLocation("aPos");
    GLint texAttrib = m_shaderProgram->attributeLocation("aTexCoord");
    
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(texAttrib);
    glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    glDisableVertexAttribArray(posAttrib);
    glDisableVertexAttribArray(texAttrib);
    
    m_shaderProgram->release();
}

void CarPlayWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void CarPlayWidget::uploadYUVTextures(const DecodedFrame& frame)
{
    if (!frame.isValid) return;
    
    m_frameWidth = frame.width;
    m_frameHeight = frame.height;
    
    // Calculate actual data sizes
    const int yDataSize = frame.yStride * frame.height;
    const int uDataSize = frame.uStride * (frame.height / 2);
    const int vDataSize = frame.vStride * (frame.height / 2);
    
    // Use double-buffered PBOs for async uploads
    const int yPbo = m_pboIndex * 3 + 0;
    const int uPbo = m_pboIndex * 3 + 1; 
    const int vPbo = m_pboIndex * 3 + 2;
    
    const int nextYPbo = m_nextPboIndex * 3 + 0;
    const int nextUPbo = m_nextPboIndex * 3 + 1;
    const int nextVPbo = m_nextPboIndex * 3 + 2;
    
    // Validate PBO indices
    if (yPbo >= 6 || uPbo >= 6 || vPbo >= 6 || nextYPbo >= 6 || nextUPbo >= 6 || nextVPbo >= 6) {
        SPDLOG_ERROR("Invalid PBO index calculation: current=({},{},{}), next=({},{},{})", 
                     yPbo, uPbo, vPbo, nextYPbo, nextUPbo, nextVPbo);
        return;
    }
    
    // Bind and upload to current PBOs
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[yPbo]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, yDataSize, frame.yData, GL_STREAM_DRAW);
    
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[uPbo]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, uDataSize, frame.uData, GL_STREAM_DRAW);
    
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[vPbo]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, vDataSize, frame.vData, GL_STREAM_DRAW);
    
    // Upload textures from previous frame's PBOs (async)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[nextYPbo]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.yStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[nextUPbo]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.uStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width/2, frame.height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pboIds[nextVPbo]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.vStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width/2, frame.height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    // Unbind PBO
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    
    // Swap buffer indices for next frame
    m_pboIndex = m_nextPboIndex;
    m_nextPboIndex = (m_nextPboIndex + 1) % 2;
    
    // Check for OpenGL errors
    checkGLError("texture upload");
}

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    emit (
        touchEvent(TouchAction::Down, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()))
    );
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    emit (
        touchEvent(TouchAction::Up, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()))
    );
}

void CarPlayWidget::mouseMoveEvent(QMouseEvent* e)
{
    emit (
        touchEvent(TouchAction::Move, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()))
    );
}

void CarPlayWidget::phone_connected(bool is_connected)
{
    m_phoneConnected = is_connected;
    if (!is_connected) {
        m_hasFrame = false;
    }
    update(); // Trigger repaint
}

#include "moc_carplay_widget.cpp"