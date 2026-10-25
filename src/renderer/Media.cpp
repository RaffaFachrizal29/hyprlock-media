#include "Media.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/rational.h>
}

static constexpr const char* MEDIA_EXTENSIONS[] = {
    // animated images
    "gif", "gifv", "webp", "apng", "avif", "jxl",
    // video containers
    "mp4", "m4v", "mov", "mkv", "webm", "avi", "wmv", "flv", "mpg", "mpeg", "ts", "m2ts", "ogv", "3gp",
};

bool isMediaPath(const std::string& path) {
    if (path.empty())
        return false;

    std::string ext = std::filesystem::path(path).extension().string();
    if (ext.empty())
        return false;

    ext.erase(0, 1); // drop the dot
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });

    return std::ranges::any_of(MEDIA_EXTENSIONS, [&ext](const auto& e) { return ext == e; });
}

static std::string avErrorString(int code) {
    char        buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

CMediaDecoder::~CMediaDecoder() {
    close();
}

void CMediaDecoder::close() {
    if (m_sws)
        sws_freeContext(m_sws);
    if (m_packet)
        av_packet_free(&m_packet);
    if (m_frame)
        av_frame_free(&m_frame);
    if (m_codec)
        avcodec_free_context(&m_codec);
    if (m_fmt)
        avformat_close_input(&m_fmt);

    m_sws         = nullptr;
    m_packet      = nullptr;
    m_frame       = nullptr;
    m_codec       = nullptr;
    m_fmt         = nullptr;
    m_streamIndex = -1;
    m_srcWidth    = 0;
    m_srcHeight   = 0;
    m_outWidth    = 0;
    m_outHeight   = 0;
    m_lastPts     = -1;
    m_draining    = false;
}

void CMediaDecoder::recomputeOutputSize() {
    m_outWidth  = m_srcWidth;
    m_outHeight = m_srcHeight;

    if (m_outWidth <= 0 || m_outHeight <= 0)
        return;

    if (m_maxWidth > 0 && m_maxHeight > 0) {
        const double FIT = std::min({1.0, static_cast<double>(m_maxWidth) / m_srcWidth, static_cast<double>(m_maxHeight) / m_srcHeight});
        m_outWidth       = std::max(2, static_cast<int>(std::lround(m_srcWidth * FIT)));
        m_outHeight      = std::max(2, static_cast<int>(std::lround(m_srcHeight * FIT)));
    }

    if (m_outputScale > 0 && m_outputScale != 1.0) {
        m_outWidth  = std::max(2, static_cast<int>(std::lround(m_outWidth * m_outputScale)));
        m_outHeight = std::max(2, static_cast<int>(std::lround(m_outHeight * m_outputScale)));
    }
}

void CMediaDecoder::setOutputSize(int maxWidth, int maxHeight, double scale) {
    m_maxWidth    = std::max(0, maxWidth);
    m_maxHeight   = std::max(0, maxHeight);
    m_outputScale = scale > 0 ? scale : 1.0;

    if (m_srcWidth > 0 && m_srcHeight > 0)
        recomputeOutputSize();
}

bool CMediaDecoder::open(const std::string& path, std::string& error) {
    close();

    m_path = absolutePath(path, "");

    int ret = avformat_open_input(&m_fmt, m_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        error = avErrorString(ret);
        close();
        return false;
    }

    ret = avformat_find_stream_info(m_fmt, nullptr);
    if (ret < 0) {
        error = avErrorString(ret);
        close();
        return false;
    }

    const AVCodec* decoder = nullptr;
    m_streamIndex          = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (m_streamIndex < 0 || !decoder) {
        error = "no decodable video stream";
        close();
        return false;
    }

    AVStream* stream = m_fmt->streams[m_streamIndex];

    if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        error = "stream is cover art, not video";
        close();
        return false;
    }

    m_codec = avcodec_alloc_context3(decoder);
    if (!m_codec) {
        error = "out of memory";
        close();
        return false;
    }

    ret = avcodec_parameters_to_context(m_codec, stream->codecpar);
    if (ret < 0) {
        error = avErrorString(ret);
        close();
        return false;
    }

    m_codec->pkt_timebase = stream->time_base;

    // Enough threads to keep up with the pixel rate, but no more: every extra
    // decoder thread steals time from the compositor on small laptops.
    const int    SRCW      = m_codec->width > 0 ? m_codec->width : stream->codecpar->width;
    const int    SRCH      = m_codec->height > 0 ? m_codec->height : stream->codecpar->height;
    const AVRational GUESS = av_guess_frame_rate(m_fmt, stream, nullptr);
    const double FPS       = GUESS.num > 0 && GUESS.den > 0 ? av_q2d(GUESS) : 30.0;
    const double PIXELRATE = static_cast<double>(SRCW) * SRCH * (FPS > 0 ? FPS : 30);
    const double HDPIXELS  = 1920.0 * 1080.0;
    int          threads   = 1;
    if (PIXELRATE > HDPIXELS * 45.0)
        threads = 2;
    if (PIXELRATE > HDPIXELS * 80.0)
        threads = 3;
    if (PIXELRATE > HDPIXELS * 110.0)
        threads = 4;
    m_codec->thread_count = std::clamp(threads, 1, 4);

    ret = avcodec_open2(m_codec, decoder, nullptr);
    if (ret < 0) {
        error = avErrorString(ret);
        close();
        return false;
    }

    m_srcWidth  = m_codec->width;
    m_srcHeight = m_codec->height;
    if (m_srcWidth <= 0 || m_srcHeight <= 0) {
        error = "invalid frame size";
        close();
        return false;
    }

    recomputeOutputSize();

    m_timeBase = av_q2d(stream->time_base);
    if (m_timeBase <= 0)
        m_timeBase = 1.0 / 1000.0;

    m_duration = m_fmt->duration != AV_NOPTS_VALUE ? static_cast<double>(m_fmt->duration) / AV_TIME_BASE : 0;
    if (m_duration <= 0 && stream->duration != AV_NOPTS_VALUE)
        m_duration = stream->duration * m_timeBase;

    const AVRational GUESSED = av_guess_frame_rate(m_fmt, stream, nullptr);
    m_fps                    = GUESSED.num > 0 && GUESSED.den > 0 ? av_q2d(GUESSED) : 30.0;
    if (m_fps <= 0 || m_fps > 240)
        m_fps = 30.0;

    m_formatName = m_fmt->iformat->name ? m_fmt->iformat->name : "unknown";

    // A still image opened through this path (e.g. a .webp) reports no duration
    // and a single frame. Everything else is treated as motion.
    m_animated = (stream->nb_frames > 1) || m_duration > 0.05;

    m_packet = av_packet_alloc();
    m_frame  = av_frame_alloc();
    if (!m_packet || !m_frame) {
        error = "out of memory";
        close();
        return false;
    }

    Log::logger->log(Log::TRACE, "media: opened {} ({}x{} -> {}x{}, {:.2f}s, {:.2f} fps, {}, animated: {})", m_path, m_srcWidth, m_srcHeight, m_outWidth, m_outHeight, m_duration,
                     m_fps, m_formatName, m_animated);

    return true;
}

bool CMediaDecoder::rewind(std::string& error) {
    return seekTo(0, error);
}

bool CMediaDecoder::seekTo(double seconds, std::string& error) {
    if (!m_fmt || !m_codec)
        return false;

    if (seconds < 0)
        seconds = 0;

    const int64_t TARGET = (int64_t)std::llround(seconds / m_timeBase);

    int           ret = av_seek_frame(m_fmt, m_streamIndex, TARGET, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
        ret = avformat_seek_file(m_fmt, m_streamIndex, INT64_MIN, TARGET, INT64_MAX, 0);

    if (ret < 0) {
        error = avErrorString(ret);
        return false;
    }

    avcodec_flush_buffers(m_codec);
    m_lastPts  = -1;
    m_draining = false;
    return true;
}

bool CMediaDecoder::feedPacket(std::string& error) {
    while (true) {
        const int RET = av_read_frame(m_fmt, m_packet);

        if (RET == AVERROR(EAGAIN))
            continue;

        if (RET == AVERROR_EOF) {
            if (!m_draining) {
                m_draining = true;
                avcodec_send_packet(m_codec, nullptr);
                return true;
            }
            return false;
        }

        if (RET < 0) {
            error = avErrorString(RET);
            return false;
        }

        if (m_packet->stream_index != m_streamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        const int SENT = avcodec_send_packet(m_codec, m_packet);
        av_packet_unref(m_packet);

        if (SENT < 0 && SENT != AVERROR(EAGAIN)) {
            error = avErrorString(SENT);
            return false;
        }

        return true;
    }
}

CMediaDecoder::eStatus CMediaDecoder::nextFrame(std::vector<uint8_t>& rgba, double& ptsSeconds, std::string& error) {
    if (!m_fmt || !m_codec || !m_frame)
        return eStatus::ERROR;

    while (true) {
        const int RET = avcodec_receive_frame(m_codec, m_frame);

        if (RET == 0) {
            const int W = m_frame->width > 0 ? m_frame->width : m_srcWidth;
            const int H = m_frame->height > 0 ? m_frame->height : m_srcHeight;

            if (W <= 0 || H <= 0 || m_outWidth <= 0 || m_outHeight <= 0) {
                error = "decoder returned an empty frame";
                return eStatus::ERROR;
            }

            m_sws = sws_getCachedContext(m_sws, W, H, static_cast<AVPixelFormat>(m_frame->format), m_outWidth, m_outHeight, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
                                         nullptr, nullptr, nullptr);
            if (!m_sws) {
                error = "failed to create the color converter";
                return eStatus::ERROR;
            }

            rgba.resize(static_cast<size_t>(m_outWidth) * m_outHeight * 4);

            uint8_t* dstData[4]     = {rgba.data(), nullptr, nullptr, nullptr};
            int      dstLinesize[4] = {m_outWidth * 4, 0, 0, 0};

            const int SCALED = sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, H, dstData, dstLinesize);
            if (SCALED != m_outHeight) {
                error = "color conversion failed";
                return eStatus::ERROR;
            }

            int64_t pts = m_frame->pts;
            if (pts == AV_NOPTS_VALUE)
                pts = m_frame->best_effort_timestamp;

            double seconds = pts != AV_NOPTS_VALUE ? pts * m_timeBase : (m_lastPts < 0 ? 0 : m_lastPts + 1.0 / m_fps);
            if (seconds < 0)
                seconds = 0;

            m_lastPts  = seconds;
            ptsSeconds = seconds;

            av_frame_unref(m_frame);
            return eStatus::FRAME;
        }

        if (RET == AVERROR_EOF)
            return eStatus::END;

        if (RET != AVERROR(EAGAIN)) {
            error = avErrorString(RET);
            return eStatus::ERROR;
        }

        if (!feedPacket(error)) {
            if (!error.empty())
                return eStatus::ERROR;

            // Drained and no more output is coming.
            return eStatus::END;
        }
    }
}
