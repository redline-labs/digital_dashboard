#include "decode_thread.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}


DecodeThread::DecodeThread() :
    QThread(),
    _should_terminate{false}
{
    // Silence!
    av_log_set_level(AV_LOG_ERROR);


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
};

DecodeThread::~DecodeThread()
{
    _should_terminate = true;
    wait();

    av_parser_close(_parser);
    avcodec_free_context(&_codec_context);
    av_frame_free(&_frame);
    av_packet_free(&_pkt);
}

void DecodeThread::run()
{
    SPDLOG_DEBUG("Starting decode thread.");

    auto endTime = std::chrono::steady_clock::now();

    while (_should_terminate == false)
    {
        endTime += std::chrono::milliseconds(250);

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
}

//Save RGB image as PPM file format
static void ppm_save(unsigned char* buf, int wrap, int xsize, int ysize)
{
    FILE* f;
    int i;

    f = fopen("output.ppm", "wb");
    fprintf(f, "P6\n%d %d\n%d\n", xsize, ysize, 255);

    for (i = 0; i < ysize; i++)
    {
        fwrite(buf + i * wrap, 1, xsize*3, f);
    }

    fclose(f);
}


static void ppm_save_to_buffer(unsigned char* buf, int wrap, int xsize, int ysize, uint8_t* output_buffer, uint32_t output_len)
{
    FILE* f = fmemopen(output_buffer, output_len, "w");
    int i;

    fprintf(f, "P6\n%d %d\n%d\n", xsize, ysize, 255);

    for (i = 0; i < ysize; i++)
    {
        fwrite(buf + i * wrap, 1, xsize*3, f);
    }

    fclose(f);
}

void DecodeThread::decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt)
{
    struct SwsContext* sws_ctx = NULL;
    static uint8_t ppm_image[5u * 1024u * 1024u] = {};

    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        SPDLOG_ERROR("Error sending a packet for decoding.");
        return;
    }

    //Create SWS Context for converting from decode pixel format (like YUV420) to RGB
    ////////////////////////////////////////////////////////////////////////////
    sws_ctx = sws_getContext(dec_ctx->width,
                             dec_ctx->height,
                             dec_ctx->pix_fmt,
                             dec_ctx->width,
                             dec_ctx->height,
                             AV_PIX_FMT_RGB24,
                             SWS_BICUBIC,
                             NULL,
                             NULL,
                             NULL);

    if (sws_ctx == nullptr)
    {
        SPDLOG_ERROR("Failed to get SWS context.");
        return;
    }

    AVFrame* pRGBFrame = av_frame_alloc();

    pRGBFrame->format = AV_PIX_FMT_RGB24;
    pRGBFrame->width = dec_ctx->width;
    pRGBFrame->height = dec_ctx->height;

    int sts = av_frame_get_buffer(pRGBFrame, 0);

    if (sts < 0)
    {
        SPDLOG_ERROR("Failed to AV frame buffer.");
        return;  //Error!
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


        //Convert from input format (e.g YUV420) to RGB and save to PPM:
        ////////////////////////////////////////////////////////////////////////////
        sts = sws_scale(sws_ctx,                //struct SwsContext* c,
                        frame->data,            //const uint8_t* const srcSlice[],
                        frame->linesize,        //const int srcStride[],
                        0,                      //int srcSliceY,
                        frame->height,          //int srcSliceH,
                        pRGBFrame->data,        //uint8_t* const dst[],
                        pRGBFrame->linesize);   //const int dstStride[]);

        if (sts != frame->height)
        {
            SPDLOG_ERROR("STS does not match frame height.");
            break;  //Error!
        }

        /* the picture is allocated by the decoder. no need to free it */
        ppm_save_to_buffer(pRGBFrame->data[0], pRGBFrame->linesize[0], pRGBFrame->width, pRGBFrame->height, &ppm_image[0], sizeof(ppm_image));


        QPixmap pix_map;
        if (pix_map.loadFromData(ppm_image, sizeof(ppm_image), "PPM") == false)
        {
            SPDLOG_ERROR("Failed to convert to pixmap.");
        }

        emit (
            imageReady(pix_map)
        );
    }

    //Free
    sws_freeContext(sws_ctx);
    av_frame_free(&pRGBFrame);
}

#include "moc_decode_thread.cpp"
