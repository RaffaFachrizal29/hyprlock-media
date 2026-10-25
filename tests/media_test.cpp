// Standalone checks for the media pipeline. Not installed and not built by
// default: configure with -DHYPRLOCK_BUILD_TESTS=ON.
//
// Usage: media_test <video.mp4> <animation.gif> <still.webp>

#include "core/Timer.hpp"
#include "core/hyprlock.hpp"
#include "renderer/Media.hpp"
#include "renderer/MediaPlayer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// The media manager only needs these three entry points from the app. Stub them
// so this harness can link without pulling in the whole lock screen.
ASP<CTimer> CHyprlock::addTimer(const std::chrono::system_clock::duration&, std::function<void(ASP<CTimer>, void*)>, void*, bool) {
    return nullptr;
}
void CHyprlock::renderAllOutputs() {}
void CHyprlock::renderOutput(const std::string&) {}
bool CHyprlock::isFadingOutOrTerminating() {
    return false;
}

static int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                                                            \
            g_failures++;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

static bool colorNear(const uint8_t* px, int r, int g, int b, int tolerance = 40) {
    return std::abs((int)px[0] - r) <= tolerance && std::abs((int)px[1] - g) <= tolerance && std::abs((int)px[2] - b) <= tolerance;
}

static std::vector<uint8_t> readTexture(GLuint tex, int w, int h) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("FAIL: framebuffer incomplete\n");
        g_failures++;
        glDeleteFramebuffers(1, &fbo);
        return {};
    }

    std::vector<uint8_t> pixels((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return pixels;
}

// glReadPixels' first row is the first row of texture data. Uploads put the
// image's top row there, so y counts from the top.
static const uint8_t* pixelAt(const std::vector<uint8_t>& pixels, int w, int h, int x, int yFromTop) {
    return &pixels[((size_t)yFromTop * w + x) * 4];
}

static void testDecoder(const std::string& path, bool expectAnimated, int expectFrames) {
    CMediaDecoder decoder;
    std::string   error;
    CHECK(decoder.open(path, error));
    if (!decoder.isOpen()) {
        std::printf("  open error: %s\n", error.c_str());
        return;
    }

    CHECK(decoder.width() == 320 || decoder.width() == 64);
    CHECK(decoder.height() == 240 || decoder.height() == 64);
    CHECK(decoder.animated() == expectAnimated);

    std::vector<uint8_t> rgba;
    double               pts = 0;
    int                  frames = 0;
    double               lastPts = -1;
    while (decoder.nextFrame(rgba, pts, error) == CMediaDecoder::eStatus::FRAME) {
        CHECK(rgba.size() == (size_t)decoder.width() * decoder.height() * 4);
        CHECK(pts >= lastPts - 1e-6);
        lastPts = pts;
        frames++;
    }

    CHECK(frames >= expectFrames - 1 && frames <= expectFrames + 1);
    std::printf("  decoder %s: %d frames, %.3fs, %dx%d\n", path.c_str(), frames, lastPts, decoder.width(), decoder.height());
}

static void testDecoderScale(const std::string& path) {
    CMediaDecoder decoder;
    std::string   error;
    CHECK(decoder.open(path, error));
    CHECK(decoder.width() == 320 && decoder.height() == 240);

    decoder.setOutputSize(160, 120);
    CHECK(decoder.width() == 160 && decoder.height() == 120);

    std::vector<uint8_t> rgba;
    double               pts = 0;
    CHECK(decoder.nextFrame(rgba, pts, error) == CMediaDecoder::eStatus::FRAME);
    CHECK(rgba.size() == 160 * 120 * 4);

    // Cap should never upscale beyond the source, even with media_scale > 1.
    decoder.setOutputSize(4000, 4000, 2.0);
    CHECK(decoder.width() == 320 && decoder.height() == 240);

    decoder.setOutputSize(4000, 4000);
    CHECK(decoder.width() == 320 && decoder.height() == 240);

    std::printf("  decoder scaling: 320x240 -> 160x120, no upscale\n");
}

static void testStillWebp(const std::string& path) {
    CMediaPlayer player;
    std::string  error;
    CHECK(player.load(path, {.loop = true, .speed = 1.f, .maxFPS = 0, .async = false}, error));
    CHECK(!player.animated());
    CHECK(player.tickAt(std::chrono::steady_clock::now()));
    CHECK(player.hasTexture());
    CHECK(player.texture().m_vSize.x == 64 && player.texture().m_vSize.y == 64);
    std::printf("  still webp: uploaded one frame as a texture\n");
}

static void testVideoPlayback(const std::string& path) {
    CMediaPlayer player;
    std::string  error;
    CHECK(player.load(path, {.loop = false, .speed = 1.f, .maxFPS = 0, .async = false}, error));
    CHECK(player.animated());
    CHECK(player.tickIntervalMs() == 100); // 10 fps source

    const auto BASE = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);

    CHECK(player.tickAt(BASE));
    CHECK(player.hasTexture());

    const int W = (int)player.texture().m_vSize.x;
    const int H = (int)player.texture().m_vSize.y;
    CHECK(W == 320 && H == 240);

    auto pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(!pixels.empty());
    if (!pixels.empty()) {
        const auto PRINT = [&](int x, int y) {
            const auto* P = pixelAt(pixels, W, H, x, y);
            std::printf("    pixel(%d,%d from top) = %d,%d,%d,%d\n", x, y, P[0], P[1], P[2], P[3]);
        };
        PRINT(5, 5);
        PRINT(W - 5, H - 5);
        CHECK(colorNear(pixelAt(pixels, W, H, 5, 5), 0, 128, 0));        // green marker, top-left
        CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 255, 0, 0)); // red, bottom-right
    }

    const uint64_t FIRSTSERIAL = player.frameSerial();

    // 0.55s is still the red half.
    player.tickAt(BASE + std::chrono::milliseconds(550));
    pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 255, 0, 0));

    // 1.15s is the blue half.
    player.tickAt(BASE + std::chrono::milliseconds(1150));
    pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(player.frameSerial() > FIRSTSERIAL);
    CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 0, 0, 255));

    // No loop: playback must stop after the 2s mark and keep the last frame.
    player.tickAt(BASE + std::chrono::milliseconds(2500));
    CHECK(player.finished());
    CHECK(colorNear(pixelAt(readTexture(player.texture().m_iTexID, W, H), W, H, W - 5, H - 5), 0, 0, 255));

    std::printf("  mp4 playback: timing, upload and orientation OK\n");
}

static void testAsyncPlayback(const std::string& path) {
    CMediaPlayer player;
    std::string  error;
    CHECK(player.load(path, {.loop = true, .speed = 1.f, .maxFPS = 0, .async = true, .maxWidth = 160, .maxHeight = 120, .scale = 1.0}, error));
    CHECK(player.animated());

    for (int i = 0; i < 100 && !player.hasTexture(); ++i) {
        player.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(player.hasTexture());
    if (!player.hasTexture())
        return;

    CHECK((int)player.texture().m_vSize.x == 160 && (int)player.texture().m_vSize.y == 120);

    const uint64_t SERIAL = player.frameSerial();

    // Drive it like the lock screen would (a tick per render frame).
    const auto START = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - START < std::chrono::milliseconds(1250)) {
        player.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    CHECK(player.frameSerial() > SERIAL);

    const int W = (int)player.texture().m_vSize.x;
    const int H = (int)player.texture().m_vSize.y;
    auto      pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(!pixels.empty());
    if (!pixels.empty())
        CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 0, 0, 255));

    std::printf("  async playback: worker thread, scaled upload, wall-clock sync OK\n");
}

static void testLoop(const std::string& path) {
    CMediaPlayer player;
    std::string  error;
    CHECK(player.load(path, {.loop = true, .speed = 1.f, .maxFPS = 0, .async = false}, error));

    const auto BASE = std::chrono::steady_clock::time_point{} + std::chrono::seconds(200);
    player.tickAt(BASE);

    player.tickAt(BASE + std::chrono::milliseconds(2200)); // past the 2s end
    const int W = (int)player.texture().m_vSize.x;
    const int H = (int)player.texture().m_vSize.y;
    auto      pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 255, 0, 0)); // looped back to red
    CHECK(!player.finished());

    std::printf("  mp4 loop: restarted at the first frame\n");
}

static void testSpeed(const std::string& path) {
    CMediaPlayer player;
    std::string  error;
    CHECK(player.load(path, {.loop = false, .speed = 4.f, .maxFPS = 0, .async = false}, error));

    const auto BASE = std::chrono::steady_clock::time_point{} + std::chrono::seconds(300);
    player.tickAt(BASE);
    player.tickAt(BASE + std::chrono::milliseconds(400)); // 1.6s of media at 4x

    const int W = (int)player.texture().m_vSize.x;
    const int H = (int)player.texture().m_vSize.y;
    auto      pixels = readTexture(player.texture().m_iTexID, W, H);
    CHECK(colorNear(pixelAt(pixels, W, H, W - 5, H - 5), 0, 0, 255));

    std::printf("  playback speed: 4x lands on the blue half\n");
}

static void testBadFile() {
    CMediaPlayer player;
    std::string  error;
    CHECK(!player.load("/definitely/not/a/file.mp4", {.loop = true, .speed = 1.f, .maxFPS = 0, .async = false}, error));
    CHECK(!error.empty());
    std::printf("  bad file: rejected with \"%s\"\n", error.c_str());
}

static bool initEGL() {
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getPlatformDisplay) {
        std::printf("FAIL: no eglGetPlatformDisplayEXT\n");
        return false;
    }

    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY) {
        std::printf("FAIL: no surfaceless EGL display\n");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        std::printf("FAIL: eglInitialize\n");
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        std::printf("FAIL: eglBindAPI\n");
        return false;
    }

    const EGLint CONFIGATTRS[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };

    EGLConfig config;
    EGLint    configs = 0;
    if (!eglChooseConfig(display, CONFIGATTRS, &config, 1, &configs) || configs == 0) {
        std::printf("FAIL: eglChooseConfig\n");
        return false;
    }

    const EGLint CONTEXTATTRS[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext   context        = eglCreateContext(display, config, EGL_NO_CONTEXT, CONTEXTATTRS);
    if (context == EGL_NO_CONTEXT) {
        std::printf("FAIL: eglCreateContext\n");
        return false;
    }

    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        std::printf("FAIL: eglMakeCurrent\n");
        return false;
    }

    std::printf("EGL: %d.%d, %s\n", major, minor, (const char*)glGetString(GL_VERSION));
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: %s <video.mp4> <animation.gif> <still.webp>\n", argv[0]);
        return 2;
    }

    CHECK(isMediaPath("a.gif"));
    CHECK(isMediaPath("a.MP4"));
    CHECK(!isMediaPath("a.png"));
    CHECK(!isMediaPath(""));

    std::printf("decoder checks\n");
    testDecoder(argv[1], true, 20);
    testDecoder(argv[2], true, 20);
    testDecoder(argv[3], false, 1);
    testDecoderScale(argv[1]);

    if (!initEGL())
        return 1;

    std::printf("texture checks\n");
    testStillWebp(argv[3]);
    testVideoPlayback(argv[1]);
    testAsyncPlayback(argv[1]);
    testLoop(argv[1]);
    testSpeed(argv[1]);
    testBadFile();

    if (g_failures == 0)
        std::printf("ALL TESTS PASSED\n");

    return g_failures == 0 ? 0 : 1;
}
