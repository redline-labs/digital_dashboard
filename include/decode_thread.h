#ifndef DECODE_THREAD_H_
#define DECODE_THREAD_H_

#include <QObject>

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

class DecodeThread : public QObject
{
    Q_OBJECT

  public:
    DecodeThread();
    ~DecodeThread();

    void accept_new_data(const uint8_t* buffer, uint32_t buffer_len);

    void stop();

  signals:
    void imageReady(AVFrame *frame);

  private:
    void run();
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

    std::atomic<bool> _should_terminate;
    std::thread _decode_thread;
};



#endif  // DECODE_THREAD_H_