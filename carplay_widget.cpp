#include "carplay_widget.h"
#include "touch_action.h"

#include <spdlog/spdlog.h>
#include <QOpenGLShaderProgram>
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
    float y = texture2D(textureY, TexCoord).r;
    float u = texture2D(textureU, TexCoord).r - 0.5;
    float v = texture2D(textureV, TexCoord).r - 0.5;
    
    vec3 rgb;
    rgb.r = y + 1.402 * v;
    rgb.g = y - 0.344136 * u - 0.714136 * v;  
    rgb.b = y + 1.772 * u;
    
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
    m_hasFrame(false),
    m_phoneConnected(true),
    _codec(nullptr),
    _parser(nullptr),
    _codec_context(nullptr),
    _frame(nullptr),
    _pkt(nullptr),
    _receive_length(0),
    _should_terminate(false),
    _new_frame_available(false)
{
    setStyleSheet("QOpenGLWidget { background-color : black; }");
    initializeDecoder();
}

CarPlayWidget::~CarPlayWidget()
{
    stop_decoder();
    cleanupDecoder();
    
    // Clear frame data
    {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        _current_frame.clear();
    }
    
    // Clean up OpenGL resources
    makeCurrent();
    
    if (m_textureY) glDeleteTextures(1, &m_textureY);
    if (m_textureU) glDeleteTextures(1, &m_textureU);
    if (m_textureV) glDeleteTextures(1, &m_textureV);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    
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
    
    // Log OpenGL info
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    SPDLOG_INFO("OpenGL Version: {}", version ? version : "Unknown");
    
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
    
    unsigned int indices[] = {0, 1, 2, 2, 3, 0};
    
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
    
    // Configure textures with simple settings
    for (GLuint texture : {m_textureY, m_textureU, m_textureV}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void CarPlayWidget::initializeDecoder()
{
    // Initialize codec components
    _pkt = av_packet_alloc();
    _codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    _parser = av_parser_init(_codec->id);
    _codec_context = avcodec_alloc_context3(_codec);

    if (!_pkt || !_codec || !_parser || !_codec_context) {
        SPDLOG_ERROR("Failed to initialize codec components");
        return;
    }

    // Try hardware decoding on macOS
    bool hw_success = false;
#ifdef __APPLE__
    AVBufferRef* hw_device_ctx = nullptr;
    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) >= 0) {
        _codec_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        if (avcodec_open2(_codec_context, _codec, nullptr) >= 0) {
            hw_success = true;
            SPDLOG_INFO("Hardware decoding enabled");
        } else {
            av_buffer_unref(&_codec_context->hw_device_ctx);
            avcodec_free_context(&_codec_context);
            _codec_context = avcodec_alloc_context3(_codec);
        }
        av_buffer_unref(&hw_device_ctx);
    }
#endif

    // Fallback to software decoding
    if (!hw_success) {
        if (avcodec_open2(_codec_context, _codec, nullptr) >= 0) {
            SPDLOG_INFO("Software decoding enabled");
        } else {
            SPDLOG_ERROR("Failed to open codec");
        }
    }

    _frame = av_frame_alloc();
    
    // Start decode thread
    _decode_thread = std::thread(&CarPlayWidget::run_decode_thread, this);
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
        std::copy_n(buffer, std::min(buffer_len, static_cast<uint32_t>(sizeof(_receive_buffer))), _receive_buffer);
        _receive_length = buffer_len;
    }
    _decode_cv.notify_one();
}

void CarPlayWidget::stop_decoder()
{
    _should_terminate = true;
    _decode_cv.notify_all();
    if (_decode_thread.joinable()) {
        _decode_thread.join();
    }
}

void CarPlayWidget::run_decode_thread()
{
    SPDLOG_DEBUG("Starting decode thread");

    while (!_should_terminate) {
        auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        
        std::unique_lock lk(_decode_mutex);
        auto res = _decode_cv.wait_until(lk, timeout);
        if (res == std::cv_status::timeout) continue;

        const uint8_t* data = _receive_buffer;
        uint32_t data_len = _receive_length;

        while (data_len > 0) {
            int ret = av_parser_parse2(_parser, _codec_context, &_pkt->data, &_pkt->size,
                                      data, data_len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) break;
            
            data += ret;
            data_len -= ret;

            if (_pkt->size) {
                decode(_codec_context, _frame, _pkt);
            }
        }
    }

    SPDLOG_DEBUG("Decode thread finished");
}

void CarPlayWidget::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    SPDLOG_INFO("Decoding frame");
    if (avcodec_send_packet(dec_ctx, pkt) < 0) return;

    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        // Validate frame dimensions
        if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096) {
            SPDLOG_ERROR("Invalid frame dimensions: {}x{}", frame->width, frame->height);
            continue;
        }

        // Handle hardware frames - transfer to software format
        AVFrame* sw_frame = frame;
        AVFrame* temp_frame = nullptr;
        
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
            SPDLOG_DEBUG("Hardware frame detected (format: {}), transferring to software", frame->format);
            temp_frame = av_frame_alloc();
            if (temp_frame == nullptr) {
                SPDLOG_ERROR("Failed to allocate temporary frame");
                continue;
            }
            
            if (av_hwframe_transfer_data(temp_frame, frame, 0) < 0) {
                SPDLOG_ERROR("Failed to transfer frame data");
                av_frame_free(&temp_frame);
                continue;
            }
            
            sw_frame = temp_frame;
        }

        // Validate frame data pointers
        if (sw_frame->data[0] == nullptr || sw_frame->data[1] == nullptr) {
            SPDLOG_ERROR("Invalid frame data pointers: Y={}, U={}", 
                        static_cast<void*>(sw_frame->data[0]), static_cast<void*>(sw_frame->data[1]));
            if (temp_frame) av_frame_free(&temp_frame);
            continue;
        }
        
        // For standard YUV420P, also check V pointer
        if (sw_frame->format != AV_PIX_FMT_NV12 && sw_frame->data[2] == nullptr) {
            SPDLOG_ERROR("Invalid V plane pointer for YUV420P format");
            if (temp_frame) av_frame_free(&temp_frame);
            continue;
        }

        SPDLOG_DEBUG("Valid frame: {}x{}, format: {}, strides: Y={}, U={}, V={}", 
                    sw_frame->width, sw_frame->height, sw_frame->format,
                    sw_frame->linesize[0], sw_frame->linesize[1], sw_frame->linesize[2]);

        // Update current frame
        {
            std::lock_guard<std::mutex> lock(_frame_mutex);
            _current_frame.clear();
            
            _current_frame.width = sw_frame->width;
            _current_frame.height = sw_frame->height;
            _current_frame.yStride = sw_frame->linesize[0];
            _current_frame.uStride = sw_frame->linesize[1];
            _current_frame.vStride = sw_frame->linesize[2];
            
            const int ySize = _current_frame.yStride * _current_frame.height;
            const int uSize = _current_frame.uStride * (_current_frame.height / 2);
            const int vSize = _current_frame.vStride * (_current_frame.height / 2);
            
            try {
                _current_frame.yData = new uint8_t[ySize];
                std::memcpy(_current_frame.yData, sw_frame->data[0], ySize);
                
                if (sw_frame->format == AV_PIX_FMT_NV12) {
                    // Handle NV12 format (interleaved UV)
                    SPDLOG_DEBUG("Processing NV12 format frame");
                    const uint8_t* uvData = sw_frame->data[1];
                    const int uvStride = sw_frame->linesize[1];
                    const int uvHeight = sw_frame->height / 2;
                    
                    const int uvPlaneSize = uvHeight * (sw_frame->width / 2);
                    _current_frame.uData = new uint8_t[uvPlaneSize];
                    _current_frame.vData = new uint8_t[uvPlaneSize];
                    
                    // De-interleave UV data
                    for (int y = 0; y < uvHeight; y++) {
                        for (int x = 0; x < sw_frame->width / 2; x++) {
                            const int uvIndex = y * uvStride + x * 2;
                            const int uvOutIndex = y * (sw_frame->width / 2) + x;
                            
                            _current_frame.uData[uvOutIndex] = uvData[uvIndex];     // U component
                            _current_frame.vData[uvOutIndex] = uvData[uvIndex + 1]; // V component
                        }
                    }
                    
                    // Update strides for separate planes
                    _current_frame.uStride = sw_frame->width / 2;
                    _current_frame.vStride = sw_frame->width / 2;
                } else {
                    // Handle standard YUV420P format
                    SPDLOG_DEBUG("Processing YUV420P format frame");
                    _current_frame.uData = new uint8_t[uSize];
                    _current_frame.vData = new uint8_t[vSize];
                    std::memcpy(_current_frame.uData, sw_frame->data[1], uSize);
                    std::memcpy(_current_frame.vData, sw_frame->data[2], vSize);
                }
                
                _current_frame.isValid = true;
                _new_frame_available = true;
                m_hasFrame = true;
                
                SPDLOG_INFO("Frame ready: {}x{}", _current_frame.width, _current_frame.height);

                // Direct notification to Qt thread
                notifyFrameReady();
                
            } catch (const std::bad_alloc&) {
                SPDLOG_ERROR("Failed to allocate frame data");
                _current_frame.clear();
            }
        }
        
        if (temp_frame) av_frame_free(&temp_frame);
    }
}

void CarPlayWidget::notifyFrameReady()
{
    // Use Qt's thread-safe way to trigger a repaint
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void CarPlayWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (!m_hasFrame || !m_phoneConnected || !m_shaderProgram) {
        return;
    }
    
    // Upload new frame data if available
    if (_new_frame_available.load()) {
        std::lock_guard<std::mutex> lock(_frame_mutex);
        if (_current_frame.isValid) {
            uploadYUVTextures(_current_frame);
            _new_frame_available = false;
        }
    }
    
    m_shaderProgram->bind();
    
    // Set texture uniforms
    m_shaderProgram->setUniformValue("textureY", 0);
    m_shaderProgram->setUniformValue("textureU", 1);
    m_shaderProgram->setUniformValue("textureV", 2);
    
    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    
    // Setup vertex attributes
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    
    GLint posAttrib = m_shaderProgram->attributeLocation("aPos");
    GLint texAttrib = m_shaderProgram->attributeLocation("aTexCoord");
    
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    
    glEnableVertexAttribArray(texAttrib);
    glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 
                         reinterpret_cast<void*>(2 * sizeof(float)));
    
    // Draw
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    
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
    
    // Simple direct texture upload - no PBOs, no complex buffering
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.yStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height, 
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.yData);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.uStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width/2, frame.height/2, 
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.uData);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.vStride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width/2, frame.height/2, 
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.vData);
    
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    checkGLError("texture upload");
}

void CarPlayWidget::mousePressEvent(QMouseEvent* e)
{
    emit touchEvent(TouchAction::Down, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()));
}

void CarPlayWidget::mouseReleaseEvent(QMouseEvent* e)
{
    emit touchEvent(TouchAction::Up, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()));
}

void CarPlayWidget::mouseMoveEvent(QMouseEvent* e)
{
    emit touchEvent(TouchAction::Move, static_cast<uint32_t>(e->pos().x()), static_cast<uint32_t>(e->pos().y()));
}

void CarPlayWidget::phone_connected(bool is_connected)
{
    m_phoneConnected = is_connected;
    if (!is_connected) {
        m_hasFrame = false;
    }
    update();
}

#include "moc_carplay_widget.cpp"