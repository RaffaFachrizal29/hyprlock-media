#include "MediaPlayer.hpp"
#include "../core/hyprlock.hpp"
#include "../helpers/Log.hpp"

#include <algorithm>
#include <cmath>

using namespace std::chrono;

static constexpr size_t MEDIA_READY_FRAMES = 3;
static constexpr size_t MEDIA_MAX_POOL     = 2;

//
// CMediaManager
//

CMediaManager& CMediaManager::instance() {
    static CMediaManager manager;
    return manager;
}

void CMediaManager::registerPlayer(CMediaPlayer* player) {
    if (!player || std::ranges::find(m_players, player) != m_players.end())
        return;

    m_players.emplace_back(player);
    schedule();
}

void CMediaManager::unregisterPlayer(CMediaPlayer* player) {
    std::erase(m_players, player);

    if (m_players.empty() && m_timer) {
        m_timer->cancel();
        m_timer.reset();
    }

    if (m_players.empty())
        m_hasEpoch = false;

    schedule();
}

std::chrono::steady_clock::time_point CMediaManager::playbackEpoch() {
    if (!m_hasEpoch) {
        m_epoch    = std::chrono::steady_clock::now();
        m_hasEpoch = true;
    }
    return m_epoch;
}

int CMediaManager::intervalMs() const {
    int  interval = 0;
    bool any      = false;

    for (const auto* player : m_players) {
        if (!player || !player->active())
            continue;

        const int DELAY = player->nextFrameDelayMs();
        interval        = any ? std::min(interval, DELAY) : DELAY;
        any             = true;
    }

    if (!any)
        return 0;

    return std::clamp(interval, 1, 1000);
}

void CMediaManager::schedule() {
    if (m_timer || !g_pHyprlock || g_pHyprlock->isFadingOutOrTerminating())
        return;

    const int INTERVAL = intervalMs();
    if (INTERVAL <= 0)
        return;

    m_timer = g_pHyprlock->addTimer(milliseconds(INTERVAL), [](auto, auto) { CMediaManager::instance().tick(); }, nullptr, false);
}

void CMediaManager::tick() {
    m_timer.reset();

    if (!g_pHyprlock || g_pHyprlock->isFadingOutOrTerminating())
        return;

    std::vector<std::string> duePorts;
    bool                     allOutputs = false;

    for (const auto* player : m_players) {
        if (!player || !player->active())
            continue;

        if (player->nextFrameDelayMs() > 5)
            continue;

        const auto& PORT = player->outputPort();
        if (PORT.empty()) {
            allOutputs = true;
            break;
        }

        if (std::ranges::find(duePorts, PORT) == duePorts.end())
            duePorts.emplace_back(PORT);
    }

    if (allOutputs || duePorts.empty())
        g_pHyprlock->renderAllOutputs();
    else {
        for (const auto& PORT : duePorts)
            g_pHyprlock->renderOutput(PORT);
    }

    schedule();
}

//
// CMediaPlayer
//

CMediaPlayer::~CMediaPlayer() {
    unload();
}

bool CMediaPlayer::load(const std::string& path, const SOptions& options, std::string& error) {
    unload();

    m_path    = path;
    m_options = options;

    if (m_options.speed <= 0.f)
        m_options.speed = 1.f;
    if (m_options.scale <= 0)
        m_options.scale = 1.0;

    if (!m_decoder.open(path, error))
        return false;

    m_decoder.setOutputSize(m_options.maxWidth, m_options.maxHeight, m_options.scale);

    m_duration = m_decoder.duration();
    m_fps      = m_decoder.fps();
    m_loaded   = true;
    m_animated = m_decoder.animated();

    Log::logger->log(Log::INFO, "media: playing {} via {} ({}x{} -> {}x{}, {:.2f}s)", path, m_decoder.formatName(), m_decoder.sourceWidth(), m_decoder.sourceHeight(),
                     m_decoder.width(), m_decoder.height(), m_duration);

    if (m_animated && m_options.async) {
        // Decode the first frame now so the widget never flashes the fallback
        // colour, then let the worker own the decoder.
        SFrame      first;
        double      pts = 0;
        std::string err;
        if (m_decoder.nextFrame(first.rgba, pts, err) == CMediaDecoder::eStatus::FRAME) {
            m_ptsOffset       = pts;
            first.pts         = 0;
            m_lastProducedPts = 0;
            m_ready.emplace_back(std::move(first));
            m_decodeEnded = false;
            startWorker();
        } else {
            m_decodeEnded = true;
            m_finished    = true;
        }
    }

    if (m_animated)
        CMediaManager::instance().registerPlayer(this);

    return true;
}

void CMediaPlayer::unload() {
    if (!m_loaded)
        return;

    CMediaManager::instance().unregisterPlayer(this);

    stopWorker();

    m_decoder.close();
    m_texture.destroyTexture();

    m_pendingPixels.clear();
    m_duePixels.clear();
    m_duePts           = 0;
    m_pendingPts       = 0;
    m_ptsOffset        = 0;
    m_hasPending       = false;
    m_loaded           = false;
    m_animated         = false;
    m_hasTexture       = false;
    m_finished         = false;
    m_loggedDecodeError = false;
    m_serial           = 0;
    m_hasUploaded      = false;
    m_lastUploadedPts  = 0;
    m_duration         = 0;
    m_fps              = 30;
    m_playhead         = 0;
    m_lastProducedPts  = 0;
}

bool CMediaPlayer::decodeNext(std::string& error) {
    m_pendingPixels.clear();

    double                        pts = 0;
    const CMediaDecoder::eStatus STATUS = m_decoder.nextFrame(m_pendingPixels, pts, error);

    if (STATUS == CMediaDecoder::eStatus::FRAME) {
        m_pendingPts = pts;
        m_hasPending = true;
        return true;
    }

    if (STATUS == CMediaDecoder::eStatus::ERROR && !m_loggedDecodeError) {
        Log::logger->log(Log::ERR, "media: failed to decode {}: {}", m_path, error);
        m_loggedDecodeError = true;
    }

    return false;
}

void CMediaPlayer::upload(const std::vector<uint8_t>& pixels, double pts, steady_clock::time_point now) {
    const int W = m_decoder.width();
    const int H = m_decoder.height();

    if (W <= 0 || H <= 0 || pixels.size() < static_cast<size_t>(W) * H * 4)
        return;

    if (!m_texture.m_bAllocated) {
        m_texture.allocate();
        m_texture.m_iType   = TEXTURE_RGBA;
        m_texture.m_iTarget = GL_TEXTURE_2D;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture.m_iTexID);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture.m_iTexID);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    const bool SIZEUNCHANGED = m_texture.m_vSize.x == W && m_texture.m_vSize.y == H;

    if (SIZEUNCHANGED)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_texture.m_vSize = Vector2D(W, H);
    m_hasTexture      = true;
    m_hasUploaded     = true;
    m_lastUpload      = now;
    m_lastUploadedPts = pts;
    m_serial++;

    Log::logger->log(Log::TRACE, "media: {} frame {} at {:.3f}s", m_path, m_serial, m_lastUploadedPts);
}

bool CMediaPlayer::tick() {
    return tickAt(steady_clock::now());
}

bool CMediaPlayer::tickAt(steady_clock::time_point now) {
    if (!m_loaded)
        return false;

    return (m_animated && m_options.async) ? tickAsync(now) : tickSync(now);
}

bool CMediaPlayer::tickSync(steady_clock::time_point now) {
    if (!m_hasTexture) {
        std::string error;
        if (!decodeNext(error)) {
            m_finished = true;
            return false;
        }

        m_ptsOffset = m_pendingPts;
        upload(m_pendingPixels, 0, now);
        m_hasPending = false;
        m_start      = now;

        if (!m_animated)
            m_finished = true; // a still frame opened through the media path

        return true;
    }

    if (m_finished)
        return false;

    // Throttle display updates, not playback position: if the lock screen is
    // drawn faster than media_fps, skip this tick and let the next one pick the
    // newest due frame.
    if (m_options.maxFPS > 0 && m_hasUploaded && std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpload).count() < 1000 / m_options.maxFPS)
        return false;

    const double TARGET = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_start).count() * m_options.speed;

    if (!m_hasPending) {
        std::string error;
        if (!decodeNext(error)) {
            handleEnd(now);
            return false;
        }
    }

    // Catch up by dropping frames: only the newest frame whose pts is due gets
    // uploaded. Without this a slow renderer would fall further and further
    // behind the wall clock.
    bool haveDue  = false;
    bool uploaded = false;

    while (m_hasPending && (m_pendingPts - m_ptsOffset) <= TARGET + 1e-4) {
        m_duePixels.swap(m_pendingPixels);
        m_duePts     = m_pendingPts - m_ptsOffset;
        haveDue      = true;
        m_hasPending = false;

        std::string error;
        if (!decodeNext(error)) {
            upload(m_duePixels, m_duePts, now);
            uploaded = true;
            haveDue  = false;
            handleEnd(now);
            uploaded = true; // handleEnd may have shown the first looped frame
            break;
        }
    }

    if (haveDue) {
        upload(m_duePixels, m_duePts, now);
        uploaded = true;
    }

    return uploaded;
}

bool CMediaPlayer::tickAsync(steady_clock::time_point now) {
    if (!m_hasTexture) {
        SFrame frame;

        {
            std::unique_lock lk(m_mutex);
            if (m_ready.empty()) {
                if (m_decodeEnded)
                    m_finished = true;
                return false;
            }

            frame = std::move(m_ready.front());
            m_ready.pop_front();
        }

        m_spaceCV.notify_one();
        upload(frame.rgba, frame.pts, now);
        returnBuffer(std::move(frame.rgba));
        m_start = CMediaManager::instance().playbackEpoch();
        return true;
    }

    if (m_finished)
        return false;

    if (m_options.maxFPS > 0 && m_hasUploaded && std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpload).count() < 1000 / m_options.maxFPS)
        return false;

    const double TARGET = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_start).count() * m_options.speed;
    m_playhead.store(TARGET, std::memory_order_relaxed);

    SFrame chosen;
    bool   haveFrame = false;

    {
        std::unique_lock lk(m_mutex);

        size_t take = SIZE_MAX;
        for (size_t i = 0; i < m_ready.size(); ++i) {
            if (m_ready[i].pts <= TARGET + 1e-4)
                take = i;
        }

        if (take == SIZE_MAX) {
            if (m_decodeEnded && m_ready.empty())
                m_finished = true;
            return false;
        }

        for (size_t i = 0; i < take; ++i) {
            if (m_freeBuffers.size() < MEDIA_MAX_POOL)
                m_freeBuffers.emplace_back(std::move(m_ready[i].rgba));
        }

        chosen    = std::move(m_ready[take]);
        haveFrame = true;
        m_ready.erase(m_ready.begin(), m_ready.begin() + take + 1);
    }

    if (!haveFrame)
        return false;

    m_spaceCV.notify_all();
    upload(chosen.rgba, chosen.pts, now);
    returnBuffer(std::move(chosen.rgba));
    return true;
}

void CMediaPlayer::handleEnd(steady_clock::time_point now) {
    if (!m_options.loop) {
        m_finished = true;
        return;
    }

    std::string error;
    if (!m_decoder.rewind(error)) {
        Log::logger->log(Log::ERR, "media: failed to loop {}: {}", m_path, error);
        m_finished = true;
        return;
    }

    m_hasPending = false;

    if (!decodeNext(error)) {
        m_finished = true;
        return;
    }

    upload(m_pendingPixels, 0, now);
    m_hasPending = false;
    m_start      = now;
}

void CMediaPlayer::returnBuffer(std::vector<uint8_t>&& buffer) {
    std::unique_lock lk(m_mutex);
    if (m_freeBuffers.size() < MEDIA_MAX_POOL)
        m_freeBuffers.emplace_back(std::move(buffer));
    m_spaceCV.notify_one();
}

void CMediaPlayer::startWorker() {
    m_stop         = false;
    m_decodeEnded  = false;
    m_decodeFailed = false;

    m_worker = std::thread([this] { workerLoop(); });
}

void CMediaPlayer::stopWorker() {
    m_stop = true;
    m_frameCV.notify_all();
    m_spaceCV.notify_all();

    if (m_worker.joinable())
        m_worker.join();

    std::unique_lock lk(m_mutex);
    m_ready.clear();
    m_freeBuffers.clear();
    m_stop         = false;
    m_decodeEnded  = false;
    m_decodeFailed = false;
}

void CMediaPlayer::workerLoop() {
    double loopBase = 0;
    double lastPts  = -1;

    const double FRAMEDURATION = m_fps > 0 ? 1.0 / m_fps : 1.0 / 30.0;

    while (!m_stop) {
        const double PLAYHEAD = m_playhead.load(std::memory_order_relaxed);

        // After a long stall the consumer jumps forward. Seek instead of
        // decoding every missed frame.
        if (m_duration > 0.2 && m_lastProducedPts + 1.5 < PLAYHEAD) {
            const double BASE    = std::floor(PLAYHEAD / m_duration) * m_duration;
            const double DESIRED = std::clamp(PLAYHEAD - BASE, 0.0, std::max(0.0, m_duration - FRAMEDURATION));

            std::string error;
            if (m_decoder.seekTo(DESIRED + m_ptsOffset, error)) {
                loopBase          = BASE;
                lastPts           = -1;
                m_lastProducedPts = PLAYHEAD;

                // Old queued frames belong to the skipped interval.
                {
                    std::unique_lock lk(m_mutex);
                    for (auto& old : m_ready) {
                        if (m_freeBuffers.size() < MEDIA_MAX_POOL)
                            m_freeBuffers.emplace_back(std::move(old.rgba));
                    }
                    m_ready.clear();
                }
                m_spaceCV.notify_all();
            }
        }

        std::vector<uint8_t> buffer;
        {
            std::unique_lock lk(m_mutex);
            m_spaceCV.wait(lk, [this] { return m_stop || m_ready.size() < MEDIA_READY_FRAMES; });
            if (m_stop)
                break;

            if (!m_freeBuffers.empty()) {
                buffer = std::move(m_freeBuffers.back());
                m_freeBuffers.pop_back();
            }
        }

        double      pts = 0;
        std::string error;

        const CMediaDecoder::eStatus STATUS = m_decoder.nextFrame(buffer, pts, error);

        if (STATUS == CMediaDecoder::eStatus::FRAME) {
            SFrame frame;
            frame.rgba = std::move(buffer);
            frame.pts  = (pts - m_ptsOffset) + loopBase;

            lastPts           = pts;
            m_lastProducedPts = frame.pts;

            std::unique_lock lk(m_mutex);
            m_ready.emplace_back(std::move(frame));
            m_frameCV.notify_one();
            continue;
        }

        if (STATUS == CMediaDecoder::eStatus::ERROR) {
            if (!m_loggedDecodeError) {
                Log::logger->log(Log::ERR, "media: failed to decode {}: {}", m_path, error);
                m_loggedDecodeError = true;
            }
            m_decodeFailed = true;
            m_decodeEnded  = true;
            break;
        }

        // END
        if (!m_options.loop) {
            m_decodeEnded = true;
            break;
        }

        if (!m_decoder.rewind(error)) {
            Log::logger->log(Log::ERR, "media: failed to loop {}: {}", m_path, error);
            m_decodeEnded = true;
            break;
        }

        const double LOOPLENGTH = m_duration > 0.2 ? m_duration : (lastPts - m_ptsOffset + FRAMEDURATION);
        loopBase += std::max(FRAMEDURATION, LOOPLENGTH);
        lastPts = -1;

        if (!buffer.empty())
            returnBuffer(std::move(buffer));
    }

    m_frameCV.notify_all();
}

int CMediaPlayer::tickIntervalMs() const {
    if (!m_loaded || !m_animated)
        return 0;

    double interval = 1000.0 / std::clamp(m_fps, 1.0, 120.0);

    if (m_options.maxFPS > 0)
        interval = std::max(interval, 1000.0 / m_options.maxFPS);

    return std::clamp(static_cast<int>(std::llround(interval)), 5, 1000);
}

int CMediaPlayer::nextFrameDelayMs() const {
    if (!m_loaded || !m_animated || m_finished)
        return 1000;

    if (!m_hasTexture)
        return 0;

    const auto   NOW    = steady_clock::now();
    const double TARGET = std::chrono::duration_cast<std::chrono::duration<double>>(NOW - m_start).count() * m_options.speed;

    double nextPts = -1;

    if (m_options.async) {
        std::lock_guard lk(m_mutex);
        for (const auto& FRAME : m_ready) {
            if (FRAME.pts > TARGET + 1e-4) {
                nextPts = FRAME.pts;
                break;
            }
        }
    } else if (m_hasPending)
        nextPts = m_pendingPts - m_ptsOffset;

    if (nextPts < 0) {
        double delay = 15.0;
        if (m_options.maxFPS > 0 && m_hasUploaded) {
            const double SINCE = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(NOW - m_lastUpload).count();
            delay              = std::max(delay, 1000.0 / m_options.maxFPS - SINCE);
        }
        return std::clamp(static_cast<int>(std::ceil(delay)), 1, 1000);
    }

    double delay = (nextPts - TARGET) / std::max(0.01, static_cast<double>(m_options.speed)) * 1000.0 + 2.0; // timer wake-up jitter

    if (m_options.maxFPS > 0 && m_hasUploaded) {
        const double SINCE = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(NOW - m_lastUpload).count();
        delay              = std::max(delay, 1000.0 / m_options.maxFPS - SINCE);
    }

    return std::clamp(static_cast<int>(std::ceil(delay)), 0, 1000);
}
