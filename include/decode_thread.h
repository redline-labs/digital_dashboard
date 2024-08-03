#ifndef DECODE_THREAD_H_
#define DECODE_THREAD_H_

#include <QThread>
#include <QPixmap>

#include <condition_variable>
#include <mutex>

// Forward declarations for libavcodec stuff.
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class DecodeThread : public QThread
{
    Q_OBJECT

  public:
    DecodeThread();
    ~DecodeThread();

    void run() override;

    void accept_new_data(const uint8_t* buffer, uint32_t buffer_len);

  signals:
    void imageReady(const QPixmap &);

  private:
    void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt);

    const AVCodec* _codec;
    AVCodecParserContext* _parser;
    AVCodecContext* _codec_context;
    AVFrame* _frame;
    AVPacket* _pkt;

    uint8_t _receive_buffer[512u * 1024u];
    uint32_t _receive_length;

    std::mutex _m;
    std::condition_variable _cv;
};



#endif  // DECODE_THREAD_H_