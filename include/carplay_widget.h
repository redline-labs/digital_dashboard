#ifndef CARPLAY_WIDGET_H_
#define CARPLAY_WIDGET_H_

#include "touch_action.h"

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QMouseEvent>
#include <QByteArray>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// Forward declarations for libavcodec stuff.
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Structure to hold decoded frame data
struct DecodedFrame {
    uint8_t* yData = nullptr;
    uint8_t* uData = nullptr;
    uint8_t* vData = nullptr;
    int width = 0;
    int height = 0;
    int yStride = 0;
    int uStride = 0;
    int vStride = 0;
    bool isNV12 = false;
    bool isValid = false;
    
    void clear() {
        delete[] yData;
        delete[] uData;
        delete[] vData;
        yData = uData = vData = nullptr;
        width = height = yStride = uStride = vStride = 0;
        isNV12 = isValid = false;
    }
    
    // Move constructor
    DecodedFrame(DecodedFrame&& other) noexcept {
        *this = std::move(other);
    }
    
    // Move assignment
    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            clear();
            yData = other.yData;
            uData = other.uData;
            vData = other.vData;
            width = other.width;
            height = other.height;
            yStride = other.yStride;
            uStride = other.uStride;
            vStride = other.vStride;
            isNV12 = other.isNV12;
            isValid = other.isValid;
            
            // Clear the source
            other.yData = other.uData = other.vData = nullptr;
            other.width = other.height = other.yStride = other.uStride = other.vStride = 0;
            other.isNV12 = other.isValid = false;
        }
        return *this;
    }
    
    // Disable copy constructor and copy assignment
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
    
    // Default constructor
    DecodedFrame() = default;
    
    ~DecodedFrame() {
        clear();
    }
};

class CarPlayWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    CarPlayWidget();
    ~CarPlayWidget();

    void setSize(uint32_t width_px, uint32_t height_px);
    
    // New methods for integrated decoding
    void accept_new_data(const uint8_t* buffer, uint32_t buffer_len);
    void stop_decoder();

  public slots:
    void phone_connected(bool is_connected);
    // Remove the old updateYUVFrame slot - we'll handle this internally now

  signals:
    void touchEvent(TouchAction action, uint32_t x, uint32_t y);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

  private slots:
    void checkForNewFrame();

  private:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

    void setupShaders();
    void setupTextures();
    void uploadYUVTextures(const DecodedFrame& frame);
    void createTestPattern();
    
    // Integrated decoding methods
    void initializeDecoder();
    void cleanupDecoder();
    void run_decode_thread();
    void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt);

    // OpenGL rendering members
    QOpenGLShaderProgram* m_shaderProgram;
    GLuint m_textureY;
    GLuint m_textureU;
    GLuint m_textureV;
    GLuint m_vbo;
    
    // Pixel Buffer Objects for async texture uploads (double buffering)
    GLuint m_pboIds[6]; // Y, U, V for front and back buffers
    int m_pboIndex;     // Current PBO index (0 or 1)
    int m_nextPboIndex; // Next PBO index
    
    int m_frameWidth;
    int m_frameHeight;
    bool m_hasFrame;
    bool m_phoneConnected;
    
    // Integrated decode thread members
    const AVCodec* _codec;
    AVCodecParserContext* _parser;
    AVCodecContext* _codec_context;
    AVFrame* _frame;
    AVPacket* _pkt;

    uint8_t _receive_buffer[512u * 1024u];
    uint32_t _receive_length;

    std::mutex _decode_mutex;
    std::condition_variable _decode_cv;
    std::atomic<bool> _should_terminate;
    std::thread _decode_thread;
    
    // Frame buffer for decoded frames
    std::mutex _frame_mutex;
    DecodedFrame _current_frame;
    DecodedFrame _pending_frame;
    std::atomic<bool> _frame_ready;
    
    // Timer to check for new frames
    QTimer* _frame_check_timer;
};

#endif  // CARPLAY_WIDGET_H_