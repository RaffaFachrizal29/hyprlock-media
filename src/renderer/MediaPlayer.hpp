#pragma once

#include "../core/Timer.hpp"
#include "Media.hpp"
#include "Texture.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Owns the single redraw timer shared by all animated media. A timer per widget
// would render the whole lock surface once per widget per frame.
class CMediaManager {
  public:
    static CMediaManager& instance();

    void                  registerPlayer(class CMediaPlayer* player);
    void                  unregisterPlayer(class CMediaPlayer* player);

    int                   intervalMs() const;

    // Shared wall-clock origin so every output shows the same frame at the
    // same time (and wakes the single redraw timer once, not once per output).
    std::chrono::steady_clock::time_point playbackEpoch();

  private:
    void schedule();
    void tick();

    std::vector<class CMediaPlayer*>      m_players;
    ASP<CTimer>                           m_timer;
    std::chrono::steady_clock::time_point m_epoch;
    bool                                  m_hasEpoch = false;
};

class CMediaPlayer {
  public:
    struct SOptions {
        bool   loop      = true;
        float  speed     = 1.0f;
        int    maxFPS    = 0; // 0 = follow the source
        bool   async     = true;
        int    maxWidth  = 0; // cap for decoded frames, 0 = native
        int    maxHeight = 0;
        double scale     = 1.0; // extra multiplier on the capped size
        std::string port;       // output this player belongs to, for targeted redraws
    };

    CMediaPlayer() = default;
    ~CMediaPlayer();

    CMediaPlayer(const CMediaPlayer&)            = delete;
    CMediaPlayer& operator=(const CMediaPlayer&) = delete;

    // Opens `path`, preloads the first frame and (for animated media) starts
    // the decode thread. No GL calls are made here.
    bool load(const std::string& path, const SOptions& options, std::string& error);
    void unload();

    // Advances playback and uploads the newest due frame. Requires a current GL
    // context. Returns true when a new frame was uploaded.
    bool tick();
    bool tickAt(std::chrono::steady_clock::time_point now); // deterministic testing hook

    bool            hasTexture() const {
        return m_hasTexture;
    }
    const CTexture& texture() const {
        return m_texture;
    }
    uint64_t frameSerial() const {
        return m_serial;
    }
    bool animated() const {
        return m_animated;
    }
    bool active() const {
        return m_loaded && m_animated && !m_finished;
    }
    bool finished() const {
        return m_finished;
    }

    int                tickIntervalMs() const;
    // Milliseconds until the next frame should be shown (0 = now). Used to
    // schedule the shared redraw timer precisely instead of a fixed interval,
    // which drifts against 1000/30 ms and drops frames periodically.
    int                nextFrameDelayMs() const;
    double             duration() const {
        return m_duration;
    }
    const std::string& path() const {
        return m_path;
    }
    const std::string& outputPort() const {
        return m_options.port;
    }

  private:
    struct SFrame {
        std::vector<uint8_t> rgba;
        double               pts = 0;
    };

    bool tickSync(std::chrono::steady_clock::time_point now);
    bool tickAsync(std::chrono::steady_clock::time_point now);
    bool decodeNext(std::string& error);
    void upload(const std::vector<uint8_t>& pixels, double pts, std::chrono::steady_clock::time_point now);
    void handleEnd(std::chrono::steady_clock::time_point now);
    void returnBuffer(std::vector<uint8_t>&& buffer);

    void startWorker();
    void stopWorker();
    void workerLoop();

    CMediaDecoder m_decoder;
    CTexture      m_texture;
    SOptions      m_options;
    std::string   m_path;

    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_lastUpload;
    double                                m_duration        = 0;
    double                                m_fps             = 30;
    double                                m_ptsOffset       = 0;
    bool                                  m_loaded          = false;
    bool                                  m_animated        = false;
    bool                                  m_hasTexture      = false;
    bool                                  m_finished        = false;
    bool                                  m_hasUploaded     = false;
    bool                                  m_loggedDecodeError = false;
    uint64_t                              m_serial          = 0;
    double                                m_lastUploadedPts = 0;

    // Synchronous playback (tests and still frames)
    std::vector<uint8_t> m_pendingPixels;
    std::vector<uint8_t> m_duePixels;
    double               m_duePts     = 0;
    double               m_pendingPts = 0;
    bool                 m_hasPending = false;

    // Asynchronous playback
    std::thread                       m_worker;
    mutable std::mutex                m_mutex;
    std::condition_variable           m_frameCV;
    std::condition_variable           m_spaceCV;
    std::deque<SFrame>                m_ready;
    std::vector<std::vector<uint8_t>> m_freeBuffers;
    std::atomic_bool                  m_stop{false};
    std::atomic_bool                  m_decodeEnded{false};
    std::atomic_bool                  m_decodeFailed{false};
    std::atomic<double>               m_playhead{0};
    double                            m_lastProducedPts = 0;
};
