#include "carplay_widget.h"
#include "touch_action.h"

#include <spdlog/spdlog.h>
#include <QOpenGLShaderProgram>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
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
    
    // BT.601 color conversion (YUV to RGB)
    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;
    
    // Clamp values to [0,1] range
    r = clamp(r, 0.0, 1.0);
    g = clamp(g, 0.0, 1.0);
    b = clamp(b, 0.0, 1.0);
    
    gl_FragColor = vec4(r, g, b, 1.0);
}
)";

CarPlayWidget::CarPlayWidget() :
    QOpenGLWidget(),
    m_shaderProgram(nullptr),
    m_textureY(0),
    m_textureU(0),
    m_textureV(0),
    m_vbo(0),
    m_frameWidth(0),
    m_frameHeight(0),
    m_hasFrame(false),
    m_phoneConnected(true)
{
    setStyleSheet("QOpenGLWidget { background-color : black; }");
}

CarPlayWidget::~CarPlayWidget()
{
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
    
    // Configure textures
    for (GLuint texture : {m_textureY, m_textureU, m_textureV}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    
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
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, uvData.data());
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width/2, height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, uvData.data());
    
    m_frameWidth = width;
    m_frameHeight = height;
    m_hasFrame = true;
    
    SPDLOG_INFO("Created test pattern: {}x{}", width, height);
}

void CarPlayWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (!m_hasFrame || !m_phoneConnected) {
        // Just clear to black when no frame or phone not connected
        SPDLOG_DEBUG("Not rendering: hasFrame={}, phoneConnected={}", m_hasFrame, m_phoneConnected);
        return;
    }
    
    if (!m_shaderProgram) {
        SPDLOG_ERROR("No shader program available");
        return;
    }
    
    m_shaderProgram->bind();
    
    // Bind textures with error checking
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    m_shaderProgram->setUniformValue("textureY", 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    m_shaderProgram->setUniformValue("textureU", 1);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    m_shaderProgram->setUniformValue("textureV", 2);
    
    // Check for OpenGL errors before rendering
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error before rendering: 0x{:x}", error);
    }
    
    // Setup vertex attributes manually for OpenGL 2.1
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    
    GLint posAttrib = m_shaderProgram->attributeLocation("aPos");
    GLint texAttrib = m_shaderProgram->attributeLocation("aTexCoord");
    
    if (posAttrib == -1 || texAttrib == -1) {
        SPDLOG_ERROR("Failed to get attribute locations: aPos={}, aTexCoord={}", posAttrib, texAttrib);
    }
    
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    
    glEnableVertexAttribArray(texAttrib);
    glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    // Draw quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    // Check for OpenGL errors after rendering
    error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after rendering: 0x{:x}", error);
    }
    
    glDisableVertexAttribArray(posAttrib);
    glDisableVertexAttribArray(texAttrib);
    
    m_shaderProgram->release();
}

void CarPlayWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void CarPlayWidget::updateYUVFrame(AVFrame* frame)
{
    if (!frame) {
        return;
    }
    
    makeCurrent();
    uploadYUVTextures(frame);
    m_hasFrame = true;
    update(); // Trigger a repaint
    doneCurrent();
}

void CarPlayWidget::uploadYUVTextures(AVFrame* frame)
{
    m_frameWidth = frame->width;
    m_frameHeight = frame->height;
    
    SPDLOG_DEBUG("Uploading YUV frame: {}x{}, format: {}", frame->width, frame->height, frame->format);
    
    // Upload Y plane (full resolution)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame->width, frame->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    // Upload U plane (chroma, half resolution)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame->width/2, frame->height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[1]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    // Upload V plane (chroma, half resolution)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame->width/2, frame->height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[2]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        SPDLOG_ERROR("OpenGL error after texture upload: 0x{:x}", error);
    }
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