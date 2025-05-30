#include "decode_thread.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}


DecodeThread::DecodeThread() :
    QObject(),
    _should_terminate{false}
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
        SPDLOG_ERROR("Failed to find libavcodec codec.");
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

    if (avcodec_open2(_codec_context, _codec, nullptr) < 0)
    {
        SPDLOG_ERROR("Could not open codec.");
    }

    _frame = av_frame_alloc();
    if (_frame == nullptr)
    {
        SPDLOG_ERROR("Could not allocate frame.");
    }

    // Start the thread.
     _decode_thread = std::thread(std::bind(&DecodeThread::run, this));
};

DecodeThread::~DecodeThread()
{
    stop();

    av_parser_close(_parser);
    avcodec_free_context(&_codec_context);
    av_frame_free(&_frame);
    av_packet_free(&_pkt);
}

void DecodeThread::run()
{
    SPDLOG_DEBUG("Starting decode thread.");

    while (_should_terminate == false)
    {
        auto endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);

        std::unique_lock lk(_m);
        auto res = _cv.wait_until(lk, endTime);
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

    SPDLOG_DEBUG("Exiting decode thread.");
};

void DecodeThread::accept_new_data(const uint8_t* buffer, uint32_t buffer_len)
{
    {
        std::lock_guard lk(_m);
        std::copy_n(&buffer[0], std::min(buffer_len, static_cast<uint32_t>(sizeof(_receive_buffer))), &_receive_buffer[0]);
        _receive_length = buffer_len;
    }

    _cv.notify_one();
}

void DecodeThread::stop()
{
    _should_terminate = true;
    if (_decode_thread.joinable() == true)
    {
        _decode_thread.join();
    }
}

void DecodeThread::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
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

        // AV_PIX_FMT_YUVJ420P

        emit (
            imageReady(frame)
        );
    }
}

#include "moc_decode_thread.cpp"
