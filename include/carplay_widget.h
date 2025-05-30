#ifndef CARPLAY_WIDGET_H_
#define CARPLAY_WIDGET_H_

#include "touch_action.h"

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QMouseEvent>

// Forward declaration for FFmpeg
struct AVFrame;

class CarPlayWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    CarPlayWidget();
    ~CarPlayWidget();

    void setSize(uint32_t width_px, uint32_t height_px);

  public slots:
    void phone_connected(bool is_connected);
    void updateYUVFrame(AVFrame* frame);

  signals:
    void touchEvent(TouchAction action, uint32_t x, uint32_t y);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

  private:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

    void setupShaders();
    void setupTextures();
    void uploadYUVTextures(AVFrame* frame);
    void createTestPattern();

    QOpenGLShaderProgram* m_shaderProgram;
    GLuint m_textureY;
    GLuint m_textureU;
    GLuint m_textureV;
    GLuint m_vbo;
    
    int m_frameWidth;
    int m_frameHeight;
    bool m_hasFrame;
    bool m_phoneConnected;
};

#endif  // CARPLAY_WIDGET_H_