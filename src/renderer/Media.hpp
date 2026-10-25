#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Extensions that may contain motion. This is deliberately cheap (no probing)
// so it can be used before a decoder exists, e.g. when preloading resources.
bool isMediaPath(const std::string& path);

// Decodes a video or animated image into tightly packed RGBA8888 frames.
// One instance is not thread safe: decode on the thread that owns the widget.
class CMediaDecoder {
  public:
    enum class eStatus {
        FRAME,
        END,
        ERROR,
    };

    CMediaDecoder() = default;
    ~CMediaDecoder();

    CMediaDecoder(const CMediaDecoder&)            = delete;
    CMediaDecoder& operator=(const CMediaDecoder&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    // Caps the decoded size. Aspect ratio is preserved and the source is never
    // upscaled. `scale` is an extra multiplier (0 or 1 = no change).
    void setOutputSize(int maxWidth, int maxHeight, double scale = 1.0);

    // Decodes the next frame. On eStatus::FRAME, `rgba` holds width * height * 4
    // bytes and `ptsSeconds` is the presentation time relative to the start.
    eStatus nextFrame(std::vector<uint8_t>& rgba, double& ptsSeconds, std::string& error);
    bool    rewind(std::string& error);
    bool    seekTo(double seconds, std::string& error);

    bool        isOpen() const {
        return m_fmt != nullptr;
    }
    bool        animated() const {
        return m_animated;
    }
    int width() const {
        return m_outWidth;
    }
    int height() const {
        return m_outHeight;
    }
    int sourceWidth() const {
        return m_srcWidth;
    }
    int sourceHeight() const {
        return m_srcHeight;
    }
    double duration() const {
        return m_duration;
    }
    double fps() const {
        return m_fps;
    }
    const std::string& formatName() const {
        return m_formatName;
    }

  private:
    bool feedPacket(std::string& error);
    void recomputeOutputSize();

    AVFormatContext* m_fmt         = nullptr;
    AVCodecContext*  m_codec       = nullptr;
    SwsContext*      m_sws         = nullptr;
    AVPacket*        m_packet      = nullptr;
    AVFrame*         m_frame       = nullptr;
    int              m_streamIndex = -1;
    double           m_timeBase    = 0;
    double           m_duration    = 0;
    double           m_fps         = 30;
    double           m_lastPts     = -1;
    int              m_srcWidth    = 0;
    int              m_srcHeight   = 0;
    int              m_outWidth    = 0;
    int              m_outHeight   = 0;
    int              m_maxWidth    = 0;
    int              m_maxHeight   = 0;
    double           m_outputScale = 1.0;
    bool             m_animated    = false;
    bool             m_draining    = false;
    std::string      m_formatName;
    std::string      m_path;
};
