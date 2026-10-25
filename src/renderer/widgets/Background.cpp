#include "Background.hpp"
#include "../Renderer.hpp"
#include "../AsyncResourceManager.hpp"
#include "../Framebuffer.hpp"
#include "../../core/hyprlock.hpp"
#include "../../helpers/Log.hpp"
#include "../../helpers/MiscFunctions.hpp"
#include "../../core/AnimationManager.hpp"
#include "../../config/ConfigManager.hpp"
#include <chrono>
#include <hyprlang.hpp>
#include <filesystem>
#include <GLES3/gl32.h>

CBackground::CBackground() {
    blurredFB        = makeUnique<CFramebuffer>();
    pendingBlurredFB = makeUnique<CFramebuffer>();
    transformedScFB  = makeUnique<CFramebuffer>();
}

CBackground::~CBackground() {
    reset();
}

void CBackground::registerSelf(const ASP<CBackground>& self) {
    m_self = self;
}

static std::string runAndGetPath(const std::string& reloadCommand) {
    std::string path = spawnSync(reloadCommand);

    if (path.ends_with('\0'))
        path.pop_back();

    if (path.ends_with('\n'))
        path.pop_back();

    if (path.starts_with("file://"))
        path = path.substr(7);
    return path;
}

void CBackground::configure(const std::unordered_map<std::string, std::any>& props, const SP<COutput>& pOutput) {
    reset();

    try {
        color             = std::any_cast<Hyprlang::INT>(props.at("color"));
        blurPasses        = std::any_cast<Hyprlang::INT>(props.at("blur_passes"));
        blurSize          = std::any_cast<Hyprlang::INT>(props.at("blur_size"));
        vibrancy          = std::any_cast<Hyprlang::FLOAT>(props.at("vibrancy"));
        vibrancy_darkness = std::any_cast<Hyprlang::FLOAT>(props.at("vibrancy_darkness"));
        noise             = std::any_cast<Hyprlang::FLOAT>(props.at("noise"));
        brightness        = std::any_cast<Hyprlang::FLOAT>(props.at("brightness"));
        contrast          = std::any_cast<Hyprlang::FLOAT>(props.at("contrast"));
        path              = std::any_cast<Hyprlang::STRING>(props.at("path"));
        reloadCommand     = std::any_cast<Hyprlang::STRING>(props.at("reload_cmd"));
        reloadTime        = std::any_cast<Hyprlang::INT>(props.at("reload_time"));
        m_mediaLoop       = std::any_cast<Hyprlang::INT>(props.at("media_loop")) != 0;
        m_mediaFPS        = std::any_cast<Hyprlang::INT>(props.at("media_fps"));
        m_mediaSpeed      = std::any_cast<Hyprlang::FLOAT>(props.at("media_speed"));
        m_mediaScale      = std::any_cast<Hyprlang::FLOAT>(props.at("media_scale"));

    } catch (const std::bad_any_cast& e) {
        RASSERT(false, "Failed to construct CBackground: {}", e.what()); //
    } catch (const std::out_of_range& e) {
        RASSERT(false, "Missing propperty for CBackground: {}", e.what()); //
    }

    isScreenshot = path == "screenshot";

    viewport     = pOutput->getViewport();
    outputPort   = pOutput->stringPort;
    transform    = wlTransformToHyprutils(invertTransform(pOutput->transform));
    scResourceID = CAsyncResourceManager::resourceIDForScreencopy(pOutput->stringPort);

    g_pAnimationManager->createAnimation(0.f, crossFadeProgress, g_pConfigManager->m_AnimationTree.getConfig("fadeIn"));

    if (!g_asyncResourceManager->checkIdPresent(scResourceID)) {
        Log::logger->log(Log::INFO, "Missing screenshot for output {}", outputPort);
        scResourceID = 0;
    }

    if (!reloadCommand.empty() && path.empty())
        path = runAndGetPath(reloadCommand);

    if (isScreenshot) {
        resourceID = scResourceID; // Fallback to solid background:color when scResourceID==0

        if (!g_pHyprlock->getScreencopy()) {
            Log::logger->log(Log::ERR, "No screencopy support! path=screenshot won't work. Falling back to background color.");
            resourceID = 0;
        }
    } else if (!path.empty()) {
        if (isMediaPath(path)) {
            if (!loadMedia())
                resourceID = g_asyncResourceManager->requestImage(path, m_imageRevision, nullptr);
        } else {
            resourceID = g_asyncResourceManager->requestImage(path, m_imageRevision, nullptr);
        }
    }

    if (!reloadCommand.empty() && reloadTime > -1) {
        try {
            if (!isScreenshot)
                modificationTime = std::filesystem::last_write_time(absolutePath(path, ""));
        } catch (std::exception& e) { Log::logger->log(Log::ERR, "{}", e.what()); }

        plantReloadTimer(); // No reloads if reloadCommand is empty
    }
}

void CBackground::reset() {
    if (reloadTimer) {
        reloadTimer->cancel();
        reloadTimer.reset();
    }

    unloadMedia();

    blurredFB->destroyBuffer();
    pendingBlurredFB->destroyBuffer();
}

bool CBackground::loadMedia() {
    auto        player = makeUnique<CMediaPlayer>();
    std::string error;

    // With blur enabled the detail is lost anyway, so decode at half the
    // viewport size. That quarters both the sws conversion and the texture
    // upload, and the blur itself runs at half resolution.
    int decodeW = static_cast<int>(viewport.x);
    int decodeH = static_cast<int>(viewport.y);
    if (blurPasses > 0) {
        decodeW = std::max(2, decodeW / 2);
        decodeH = std::max(2, decodeH / 2);
    }

    const CMediaPlayer::SOptions OPTIONS{
        .loop      = m_mediaLoop,
        .speed     = m_mediaSpeed,
        .maxFPS    = m_mediaFPS,
        .async     = true,
        .maxWidth  = decodeW,
        .maxHeight = decodeH,
        .scale     = m_mediaScale > 0 ? m_mediaScale : 1.0,
        .port      = outputPort,
    };

    if (!player->load(path, OPTIONS, error)) {
        Log::logger->log(Log::ERR, "Background: failed to load animated media {}: {}", path, error);
        return false;
    }

    if (!player->animated())
        Log::logger->log(Log::WARN, "Background: {} is not animated, treating it as a still frame", path);

    unloadMedia();

    if (asset) {
        g_asyncResourceManager->unload(asset);
        asset = nullptr;
    }
    resourceID = 0;

    m_media = std::move(player);
    return true;
}

void CBackground::unloadMedia() {
    m_media.reset();
}

void CBackground::updatePrimaryAsset() {
    if (m_media || asset || resourceID == 0)
        return;

    asset = g_asyncResourceManager->getAssetByID(resourceID);
    if (!asset)
        return;

    const bool NEEDFB = (isScreenshot || blurPasses > 0 || asset->m_vSize != viewport || transform != HYPRUTILS_TRANSFORM_NORMAL) && (!blurredFB->isAllocated() || firstRender);
    if (NEEDFB)
        renderToFB(*asset, *blurredFB, blurPasses, isScreenshot);
}

void CBackground::updatePendingAsset() {
    // For crossfading a new asset
    if (!pendingAsset || blurPasses == 0 || pendingBlurredFB->isAllocated())
        return;

    renderToFB(*pendingAsset, *pendingBlurredFB, blurPasses);
}

void CBackground::updateScAsset() {
    if (scAsset || scResourceID == 0)
        return;

    // path=screenshot -> scAsset = asset
    scAsset = (asset && isScreenshot) ? asset : g_asyncResourceManager->getAssetByID(scResourceID);
    if (!scAsset)
        return;

    const bool NEEDSCTRANSFORM = transform != HYPRUTILS_TRANSFORM_NORMAL;
    if (NEEDSCTRANSFORM)
        renderToFB(*scAsset, *transformedScFB, 0, true);
}

const CTexture& CBackground::getPrimaryAssetTex() const {
    // This case is only for background:path=screenshot with blurPasses=0
    if (isScreenshot && blurPasses == 0 && transformedScFB->isAllocated())
        return transformedScFB->m_cTex;

    return (blurredFB->isAllocated()) ? blurredFB->m_cTex : *asset;
}

const CTexture& CBackground::getPendingAssetTex() const {
    return (pendingBlurredFB->isAllocated()) ? pendingBlurredFB->m_cTex : *pendingAsset;
}

const CTexture& CBackground::getScAssetTex() const {
    return (transformedScFB->isAllocated()) ? transformedScFB->m_cTex : *scAsset;
}

void CBackground::renderRect(CHyprColor color) {
    CBox monbox = {0, 0, viewport.x, viewport.y};
    g_pRenderer->renderRect(monbox, color, 0);
}

static void onReloadTimer(AWP<CBackground> ref) {
    if (auto PBG = ref.lock(); PBG) {
        PBG->onReloadTimerUpdate();
        PBG->plantReloadTimer();
    }
}

static CBox getScaledBoxForTextureSize(const Vector2D& size, const Vector2D& viewport) {
    CBox  texbox = {{}, size};

    float scaleX = viewport.x / size.x;
    float scaleY = viewport.y / size.y;

    texbox.w *= std::max(scaleX, scaleY);
    texbox.h *= std::max(scaleX, scaleY);

    if (scaleX > scaleY)
        texbox.y = -(texbox.h - viewport.y) / 2.f;
    else
        texbox.x = -(texbox.w - viewport.x) / 2.f;
    texbox.round();

    return texbox;
}

void CBackground::renderToFB(const CTexture& tex, CFramebuffer& fb, int passes, bool applyTransform, const Vector2D& targetSize) {
    if (firstRender)
        firstRender = false;

    Vector2D TARGET = targetSize.x > 0 && targetSize.y > 0 ? targetSize : viewport;
    TARGET.x        = std::max(2.0, std::round(TARGET.x));
    TARGET.y        = std::max(2.0, std::round(TARGET.y));

    // make it brah
    Vector2D size = tex.m_vSize;
    if (applyTransform && transform % 2 == 1) {
        size.x = tex.m_vSize.y;
        size.y = tex.m_vSize.x;
    }

    const auto TEXBOX = getScaledBoxForTextureSize(size, TARGET);

    const bool RESIZE = !fb.isAllocated() || fb.m_vSize.x != TARGET.x || fb.m_vSize.y != TARGET.y;
    if (RESIZE) {
        fb.destroyBuffer();
        fb.alloc(TARGET.x, TARGET.y); // TODO 10 bit
    }

    if (TARGET != viewport)
        g_pRenderer->pushProjection(TARGET);

    fb.bind();

    g_pRenderer->renderTexture(TEXBOX, tex, 1.0, 0, applyTransform ? transform : HYPRUTILS_TRANSFORM_NORMAL);

    if (blurPasses > 0)
        g_pRenderer->blurFB(fb,
                            CRenderer::SBlurParams{
                                .size              = std::max(1, static_cast<int>(std::lround(blurSize * (TARGET.x / viewport.x)))),
                                .passes            = passes,
                                .noise             = noise,
                                .contrast          = contrast,
                                .brightness        = brightness,
                                .vibrancy          = vibrancy,
                                .vibrancy_darkness = vibrancy_darkness,
                            });
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    if (TARGET != viewport)
        g_pRenderer->popProjection();
}

bool CBackground::draw(const SRenderData& data) {
    if (m_media) {
        const bool NEWFRAME = m_media->tick();

        if (!m_media->hasTexture()) {
            renderRect(color);
            return false;
        }

        const auto& TEX     = m_media->texture();
        const bool  NEEDFB  = blurPasses > 0 || transform != HYPRUTILS_TRANSFORM_NORMAL;
        const auto  TEXBOX  = getScaledBoxForTextureSize(TEX.m_vSize, viewport);

        if (!NEEDFB) {
            // No blur and no output transform: let the GPU scale the texture
            // straight to the screen instead of rendering a full viewport FBO.
            g_pRenderer->renderTexture(TEXBOX, TEX, data.opacity, 0);
            return false;
        }

        const Vector2D FBSIZE = blurPasses > 0 ? Vector2D(std::max(2.0, viewport.x / 2.0), std::max(2.0, viewport.y / 2.0)) : viewport;

        // The blurred framebuffer is a cache for static images. An animated
        // background has to be re-rendered (and re-blurred) on every new frame.
        if (!blurredFB->isAllocated() || NEWFRAME)
            renderToFB(TEX, *blurredFB, blurPasses, false, FBSIZE);

        const auto& FBTEX    = blurredFB->m_cTex;
        const auto  FBTEXBOX = getScaledBoxForTextureSize(FBTEX.m_vSize, viewport);
        g_pRenderer->renderTexture(FBTEXBOX, FBTEX, data.opacity, 0);
        return false;
    }

    updatePrimaryAsset();
    updatePendingAsset();
    updateScAsset();

    if (asset && asset->m_iType == TEXTURE_INVALID) {
        g_asyncResourceManager->unload(asset);
        resourceID = 0;
        renderRect(color);
        return false;
    }

    if (!asset || resourceID == 0) {
        // fade in/out with a solid color
        if (data.opacity < 1.0 && scAsset) {
            const auto& SCTEX    = getScAssetTex();
            const auto  SCTEXBOX = getScaledBoxForTextureSize(SCTEX.m_vSize, viewport);
            g_pRenderer->renderTexture(SCTEXBOX, SCTEX, 1, 0, HYPRUTILS_TRANSFORM_FLIPPED_180);
            CHyprColor col = color;
            col.a *= data.opacity;
            renderRect(col);
            return true;
        }

        renderRect(color);
        return !asset && resourceID > 0; // resource not ready
    }

    const auto& TEX    = getPrimaryAssetTex();
    const auto  TEXBOX = getScaledBoxForTextureSize(TEX.m_vSize, viewport);
    if (data.opacity < 1.0 && scAsset) {
        const auto& SCTEX = getScAssetTex();
        g_pRenderer->renderTextureMix(TEXBOX, SCTEX, TEX, 1.0, data.opacity, 0);
    } else if (crossFadeProgress->isBeingAnimated()) {
        const auto& PENDINGTEX = getPendingAssetTex();
        g_pRenderer->renderTextureMix(TEXBOX, TEX, PENDINGTEX, 1.0, crossFadeProgress->value(), 0);
    } else
        g_pRenderer->renderTexture(TEXBOX, TEX, 1, 0);

    return crossFadeProgress->isBeingAnimated() || data.opacity < 1.0;
}

void CBackground::onAssetUpdate(ResourceID id, ASP<CTexture> newAsset) {
    if (m_media) {
        if (newAsset)
            g_asyncResourceManager->unload(newAsset);
        return;
    }

    if (!newAsset)
        Log::logger->log(Log::ERR, "Background asset update failed, resourceID: {} not available on update!", id);
    else if (newAsset->m_iType == TEXTURE_INVALID) {
        g_asyncResourceManager->unload(newAsset);
        Log::logger->log(Log::ERR, "New background asset has an invalid texture!");
    } else {
        pendingAsset = newAsset;
        crossFadeProgress->setValueAndWarp(0);
        *crossFadeProgress = 1.0;

        crossFadeProgress->setCallbackOnEnd(
            [REF = m_self, id](auto) {
                if (const auto PSELF = REF.lock()) {
                    if (PSELF->asset)
                        g_asyncResourceManager->unload(PSELF->asset);

                    PSELF->asset        = PSELF->pendingAsset;
                    PSELF->pendingAsset = nullptr;
                    PSELF->resourceID   = id;

                    PSELF->blurredFB->destroyBuffer();
                    PSELF->blurredFB = std::move(PSELF->pendingBlurredFB);

                    PSELF->pendingResource = false;
                }
            },
            true);
    }
}

void CBackground::plantReloadTimer() {

    if (reloadTime == 0)
        reloadTimer = g_pHyprlock->addTimer(std::chrono::hours(1), [REF = m_self](auto, auto) { onReloadTimer(REF); }, nullptr, true);
    else if (reloadTime > 0)
        reloadTimer = g_pHyprlock->addTimer(std::chrono::seconds(reloadTime), [REF = m_self](auto, auto) { onReloadTimer(REF); }, nullptr, true);
}

void CBackground::onReloadTimerUpdate() {
    const std::string OLDPATH = path;

    // Path parsing and early returns

    if (!reloadCommand.empty()) {
        path = runAndGetPath(reloadCommand);

        if (path.empty())
            return;
    }

    try {
        const auto MTIME = std::filesystem::last_write_time(absolutePath(path, ""));
        if (OLDPATH == path && MTIME == modificationTime)
            return;

        modificationTime = MTIME;
        if (OLDPATH == path)
            m_imageRevision++;
        else
            m_imageRevision = 0;
    } catch (std::exception& e) {
        path = OLDPATH;
        Log::logger->log(Log::ERR, "{}", e.what());
        return;
    }

    if (isMediaPath(path)) {
        pendingResource = false;
        if (loadMedia())
            g_pHyprlock->renderAllOutputs();
        else
            path = OLDPATH;
        return;
    }

    if (m_media) {
        // switched from an animated background back to a static image
        unloadMedia();
        if (asset) {
            g_asyncResourceManager->unload(asset);
            asset = nullptr;
        }
        resourceID = 0;
        blurredFB->destroyBuffer();
    }

    if (pendingResource) {
        Log::logger->log(Log::WARN, "Background image update still pending! - Refusing to load {}", path);
        return;
    }

    pendingResource = true;

    // Issue the next request
    AWP<IWidget> widget(m_self);
    g_asyncResourceManager->requestImage(path, m_imageRevision, widget);
}
