#include "decode_thread.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
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

        // Handle both hardware and software decoded frames
        AVFrame* sw_frame = frame;
        AVFrame* temp_frame = nullptr;
        
        // If we got a hardware frame, transfer it to system memory
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
        {
            //SPDLOG_INFO("Hardware frame detected, transferring to software...");
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
            
            //SPDLOG_INFO("Successfully transferred to software format: {} ({})", 
            //           av_get_pix_fmt_name((AVPixelFormat)temp_frame->format), temp_frame->format);
            sw_frame = temp_frame;
        }
        else
        {
            //SPDLOG_INFO("Software frame format, using directly");
        }

        // Validate frame data before copying
        if (sw_frame->data[0] == nullptr || sw_frame->data[1] == nullptr)
        {
            SPDLOG_ERROR("Invalid frame data pointers: Y={}, U={}", 
                        (void*)sw_frame->data[0], (void*)sw_frame->data[1]);
            if (temp_frame != nullptr)
            {
                av_frame_free(&temp_frame);
            }
            continue;
        }
        
        // For YUV420P format, also check V pointer
        if (sw_frame->format != AV_PIX_FMT_NV12 && sw_frame->data[2] == nullptr)
        {
            SPDLOG_ERROR("Invalid V plane pointer for YUV420P format: V={}", (void*)sw_frame->data[2]);
            if (temp_frame != nullptr)
            {
                av_frame_free(&temp_frame);
            }
            continue;
        }
        
        // Copy YUV data to ensure it remains valid after this frame is reused
        // Handle format-specific processing: NV12 (hardware) vs YUV420P (software)
        
        const int yStride = sw_frame->linesize[0];
        const int uStride = sw_frame->linesize[1];
        const int vStride = sw_frame->linesize[2];
        const int ySize = yStride * sw_frame->height;
        const int uSize = uStride * (sw_frame->height / 2);
        const int vSize = vStride * (sw_frame->height / 2);
        
        //SPDLOG_INFO("Frame data: {}x{}, strides Y={} U={} V={}, sizes Y={} U={} V={}", 
        //           sw_frame->width, sw_frame->height, yStride, uStride, vStride, ySize, uSize, vSize);

        QByteArray yData(reinterpret_cast<const char*>(sw_frame->data[0]), ySize);
        QByteArray uData, vData;
        
        // Handle different pixel formats
        if (sw_frame->format == AV_PIX_FMT_NV12)
        {
            //SPDLOG_INFO("Converting NV12 to separate U/V planes");
            // NV12 has interleaved UV data in data[1]
            const uint8_t* uvData = sw_frame->data[1];
            const int uvStride = sw_frame->linesize[1];
            const int uvHeight = sw_frame->height / 2;
            
            // Allocate separate U and V arrays
            uData.resize(uvHeight * (sw_frame->width / 2));
            vData.resize(uvHeight * (sw_frame->width / 2));
            
            // De-interleave UV data
            for (int y = 0; y < uvHeight; y++)
            {
                for (int x = 0; x < sw_frame->width / 2; x++)
                {
                    const int uvIndex = y * uvStride + x * 2;
                    const int uvOutIndex = y * (sw_frame->width / 2) + x;
                    
                    uData[uvOutIndex] = uvData[uvIndex];     // U component
                    vData[uvOutIndex] = uvData[uvIndex + 1]; // V component
                }
            }
            
            //SPDLOG_INFO("NV12 conversion complete: U size={}, V size={}", uData.size(), vData.size());
        }
        else
        {
            // Standard YUV420P format
            uData = QByteArray(reinterpret_cast<const char*>(sw_frame->data[1]), uSize);
            vData = QByteArray(reinterpret_cast<const char*>(sw_frame->data[2]), vSize);
        }

        emit (
            imageReady(yData, uData, vData, sw_frame->width, sw_frame->height, 
                      yStride, 
                      sw_frame->format == AV_PIX_FMT_NV12 ? sw_frame->width / 2 : uStride,
                      sw_frame->format == AV_PIX_FMT_NV12 ? sw_frame->width / 2 : vStride)
        );
        
        // Clean up temporary frame if we created one
        if (temp_frame != nullptr)
        {
            av_frame_free(&temp_frame);
        }
    }
}

#include "moc_decode_thread.cpp"
