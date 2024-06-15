#include "videodecoder.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QStandardPaths>

#include <vanilla.h>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

enum RecordingStream {
    VIDEO_STREAM_INDEX,
    AUDIO_STREAM_INDEX
};

static const int GAMEPAD_WIDTH = 854;
static const int GAMEPAD_HEIGHT = 480;
static const AVPixelFormat GAMEPAD_FORMAT = AV_PIX_FMT_YUV420P;

int openDecoder(AVCodecContext **ctx, const AVCodec *codec)
{
    (*ctx) = avcodec_alloc_context3(codec);
    (*ctx)->width = GAMEPAD_WIDTH;
    (*ctx)->height = GAMEPAD_HEIGHT;
    (*ctx)->pix_fmt = GAMEPAD_FORMAT;
    int r = avcodec_open2((*ctx), codec, NULL);
    if (r < 0) {
        avcodec_free_context(ctx);
    }
    return r;
}

VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent)
{
    m_recordingCtx = nullptr;
    
    // Try to use v4l2m2m hardware decoder (Raspberry Pi)
    if (openDecoder(&m_codecCtx, avcodec_find_decoder_by_name("h264_v4l2m2m")) < 0) {
        fprintf(stderr, "Failed to open v4l2m2m decoder\n");

        // Fallback to software decoder
        if (openDecoder(&m_codecCtx, avcodec_find_decoder(AV_CODEC_ID_H264)) < 0) {
            fprintf(stderr, "Failed to open software decoder\n");
            return;
        }
    }
    
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();

    m_filterGraph = avfilter_graph_alloc();

    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=30/1:pixel_aspect=1/1", GAMEPAD_WIDTH, GAMEPAD_HEIGHT, GAMEPAD_FORMAT);
    avfilter_graph_create_filter(&m_buffersrcCtx, avfilter_get_by_name("buffer"), "in", args, NULL, m_filterGraph);

    avfilter_graph_create_filter(&m_buffersinkCtx, avfilter_get_by_name("buffersink"), "out", NULL, NULL, m_filterGraph);

    enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGBA, AV_PIX_FMT_NONE};
    av_opt_set_int_list(m_buffersinkCtx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    
    avfilter_link(m_buffersrcCtx, 0, m_buffersinkCtx, 0);
    avfilter_graph_config(m_filterGraph, 0);
}

VideoDecoder::~VideoDecoder()
{
    avfilter_graph_free(&m_filterGraph);
    avcodec_free_context(&m_codecCtx);
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
}

void cleanupFrame(void *v)
{
    AVFrame *f = (AVFrame *) v;
    av_frame_free(&f);
}

int64_t VideoDecoder::getCurrentTimestamp(AVRational timebase)
{
    int64_t millis = QDateTime::currentMSecsSinceEpoch() - m_recordingStartTime;
    int64_t ts = av_rescale_q(millis, {1, 1000}, timebase);
    return ts;
}

void VideoDecoder::sendPacket(const QByteArray &data)
{
    int ret;

    // Copy data into buffer that FFmpeg will take ownership of
    uint8_t *buffer = (uint8_t *) av_malloc(data.size());
    memcpy(buffer, data.data(), data.size());

    // Create AVPacket from this data
    ret = av_packet_from_data(m_packet, buffer, data.size());
    if (ret < 0) {
        fprintf(stderr, "Failed to initialize packet from data: %i\n", ret);
        av_free(buffer);
        return;
    }

    // If recording, send packet to file
    if (m_recordingCtx) {
        AVPacket *encPkt = av_packet_clone(m_packet);
        encPkt->stream_index = VIDEO_STREAM_INDEX;

        int64_t ts = getCurrentTimestamp(m_videoStream->time_base);

        encPkt->dts = ts;
        encPkt->pts = ts;

        av_interleaved_write_frame(m_recordingCtx, encPkt);
        av_packet_free(&encPkt);
    }

    // Send packet to decoder
    ret = avcodec_send_packet(m_codecCtx, m_packet);
    av_packet_unref(m_packet);
    if (ret < 0) {
        fprintf(stderr, "Failed to send packet to decoder: %i\n", ret);
        return;
    }

    // Retrieve frame from decoder
    ret = avcodec_receive_frame(m_codecCtx, m_frame);
    if (ret == AVERROR(EAGAIN)) {
        // Decoder wants another packet before it can output a frame. Silently exit.
    } else if (ret < 0) {
        fprintf(stderr, "Failed to receive frame from decoder: %i\n", ret);
    } else if (0) {
        ret = av_buffersrc_add_frame_flags(m_buffersrcCtx, m_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(m_frame);
        if (ret < 0) {
            fprintf(stderr, "Failed to add frame to buffersrc: %i\n", ret);
            return;
        }

        AVFrame *filtered = av_frame_alloc();
        ret = av_buffersink_get_frame(m_buffersinkCtx, filtered);
        if (ret < 0) {
            fprintf(stderr, "Failed to get frame from buffersink: %i\n", ret);
            av_frame_free(&filtered);
            return;
        }

        QImage image(filtered->data[0], filtered->width, filtered->height, filtered->linesize[0], QImage::Format_RGBA8888, cleanupFrame, filtered);
        emit frameReady(image);
    } else {
        QImage image(GAMEPAD_WIDTH, GAMEPAD_HEIGHT, QImage::Format_Grayscale8);
        for (int y = 0; y < GAMEPAD_HEIGHT; y++) {
            memcpy(image.scanLine(y), m_frame->data[0] + m_frame->linesize[0] * y, qMin(image.bytesPerLine(), m_frame->linesize[0]));
        }
        emit frameReady(image);
        av_frame_unref(m_frame);
    }
}

void VideoDecoder::sendAudio(const QByteArray &data)
{
    if (m_recordingCtx) {
        int ret;

        // Copy data into buffer that FFmpeg will take ownership of
        uint8_t *buffer = (uint8_t *) av_malloc(data.size());
        memcpy(buffer, data.data(), data.size());

        // Create AVPacket from this data
        AVPacket *audPkt = av_packet_alloc();
        int64_t ts;
        ret = av_packet_from_data(audPkt, buffer, data.size());
        if (ret < 0) {
            fprintf(stderr, "Failed to initialize packet from data: %i\n", ret);
            av_free(buffer);
            goto free;
        }

        ts = getCurrentTimestamp(m_audioStream->time_base);

        audPkt->stream_index = AUDIO_STREAM_INDEX;
        audPkt->dts = ts;
        audPkt->pts = ts;

        av_interleaved_write_frame(m_recordingCtx, audPkt);

free:
        av_packet_free(&audPkt);
    }
}

void VideoDecoder::enableRecording(bool e)
{
    if (e) startRecording(); else stopRecording();
}

void VideoDecoder::startRecording()
{
    m_recordingFilename = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("vanilla-recording-%0.mp4").arg(QDateTime::currentSecsSinceEpoch());
    
    QByteArray filenameUtf8 = m_recordingFilename.toUtf8();

    int r = avformat_alloc_output_context2(&m_recordingCtx, nullptr, nullptr, filenameUtf8.constData());
    if (r < 0) {
        emit recordingError(r);
        return;
    }

    m_videoStream = avformat_new_stream(m_recordingCtx, nullptr);
    if (!m_videoStream) {
        emit recordingError(AVERROR(ENOMEM));
        goto freeContext;
    }

    m_videoStream->id = VIDEO_STREAM_INDEX;
    m_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    m_videoStream->codecpar->width = GAMEPAD_WIDTH;
    m_videoStream->codecpar->height = GAMEPAD_HEIGHT;
    m_videoStream->codecpar->format = GAMEPAD_FORMAT;
    m_videoStream->codecpar->codec_id = AV_CODEC_ID_H264;

    size_t sps_pps_size;
    vanilla_retrieve_sps_pps_data(nullptr, &sps_pps_size);
    m_videoStream->codecpar->extradata_size = sps_pps_size;
    m_videoStream->codecpar->extradata = (uint8_t *) av_malloc(sps_pps_size);
    vanilla_retrieve_sps_pps_data(m_videoStream->codecpar->extradata, &sps_pps_size);

    m_audioStream = avformat_new_stream(m_recordingCtx, nullptr);
    if (!m_audioStream) {
        emit recordingError(AVERROR(ENOMEM));
        goto freeContext;
    }

    m_audioStream->id = AUDIO_STREAM_INDEX;
    m_audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    m_audioStream->codecpar->sample_rate = 48000;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57,24,100)
    m_audioStream->codecpar->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
#else
    m_audioStream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
    m_audioStream->codecpar->format = AV_SAMPLE_FMT_S16;
    m_audioStream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;

    r = avio_open2(&m_recordingCtx->pb, filenameUtf8.constData(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (r < 0) {
        emit recordingError(r);
        goto freeContext;
    }

    r = avformat_write_header(m_recordingCtx, nullptr);
    if (r < 0) {
        emit recordingError(r);
        goto freeContext;
    }

    emit requestIDR();

    m_recordingStartTime = QDateTime::currentMSecsSinceEpoch();
    return;

freeContext:
    avformat_free_context(m_recordingCtx);
    m_recordingCtx = nullptr;
}

void VideoDecoder::stopRecording()
{
    if (m_recordingCtx) {
        av_write_trailer(m_recordingCtx);
        avio_closep(&m_recordingCtx->pb);
        avformat_free_context(m_recordingCtx);
        m_recordingCtx = nullptr;
        emit recordingFinished(m_recordingFilename);
    }
}