#include "Image.hpp"
#include "../Renderer.hpp"
#include "../AsyncResourceManager.hpp"
#include "../../core/hyprlock.hpp"
#include "../../helpers/Log.hpp"
#include "../../helpers/MiscFunctions.hpp"
#include "../../config/ConfigDataValues.hpp"
#include <cmath>
#include <hyprlang.hpp>
#include <hyprutils/math/Vector2D.hpp>

CImage::~CImage() {
    reset();
}

void CImage::registerSelf(const ASP<CImage>& self) {
    m_self = self;
}

static void onTimer(AWP<CImage> ref) {
    if (auto PIMAGE = ref.lock(); PIMAGE) {
        PIMAGE->onTimerUpdate();
        PIMAGE->plantTimer();
    }
}

void CImage::onTimerUpdate() {
    if (m_pendingResource) {
        Log::logger->log(Log::WARN, "Trying to update image, but a resource is still pending! Skipping update.");
        return;
    }

    const std::string OLDPATH = path;

    if (!reloadCommand.empty()) {
        path = spawnSync(reloadCommand);

        if (path.ends_with('\n'))
            path.pop_back();

        if (path.starts_with("file://"))
            path = path.substr(7);

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
        if (loadMedia())
            g_pHyprlock->renderAllOutputs();
        else
            path = OLDPATH;
        return;
    }

    if (m_media) {
        unloadMedia();
        resourceID = 0;
        imageFB.destroyBuffer();
    }

    m_pendingResource = true;

    AWP<IWidget> widget(m_self);
    g_asyncResourceManager->requestImage(path, m_imageRevision, widget);
}

void CImage::plantTimer() {

    if (reloadTime == 0) {
        imageTimer = g_pHyprlock->addTimer(std::chrono::hours(1), [REF = m_self](auto, auto) { onTimer(REF); }, nullptr, true);
    } else if (reloadTime > 0)
        imageTimer = g_pHyprlock->addTimer(std::chrono::seconds(reloadTime), [REF = m_self](auto, auto) { onTimer(REF); }, nullptr, false);
}

void CImage::configure(const std::unordered_map<std::string, std::any>& props, const SP<COutput>& pOutput) {
    reset();

    viewport   = pOutput->getViewport();
    stringPort = pOutput->stringPort;

    shadow.configure(m_self, props, viewport);

    try {
        size      = std::any_cast<Hyprlang::INT>(props.at("size"));
        rounding  = std::any_cast<Hyprlang::INT>(props.at("rounding"));
        border    = std::any_cast<Hyprlang::INT>(props.at("border_size"));
        color     = *CGradientValueData::fromAnyPv(props.at("border_color"));
        configPos = CLayoutValueData::fromAnyPv(props.at("position"))->getAbsolute(viewport);
        halign    = std::any_cast<Hyprlang::STRING>(props.at("halign"));
        valign    = std::any_cast<Hyprlang::STRING>(props.at("valign"));
        angle     = std::any_cast<Hyprlang::FLOAT>(props.at("rotate"));

        path           = std::any_cast<Hyprlang::STRING>(props.at("path"));
        reloadTime     = std::any_cast<Hyprlang::INT>(props.at("reload_time"));
        reloadCommand  = std::any_cast<Hyprlang::STRING>(props.at("reload_cmd"));
        onclickCommand = std::any_cast<Hyprlang::STRING>(props.at("onclick"));
        m_mediaLoop    = std::any_cast<Hyprlang::INT>(props.at("media_loop")) != 0;
        m_mediaFPS     = std::any_cast<Hyprlang::INT>(props.at("media_fps"));
        m_mediaSpeed   = std::any_cast<Hyprlang::FLOAT>(props.at("media_speed"));
        m_mediaScale   = std::any_cast<Hyprlang::FLOAT>(props.at("media_scale"));
    } catch (const std::bad_any_cast& e) {
        RASSERT(false, "Failed to construct CImage: {}", e.what()); //
    } catch (const std::out_of_range& e) {
        RASSERT(false, "Missing propperty for CImage: {}", e.what()); //
    }

    if (isMediaPath(path)) {
        if (!loadMedia())
            resourceID = g_asyncResourceManager->requestImage(path, m_imageRevision, nullptr);
        else
            resourceID = 0;
    } else
        resourceID = g_asyncResourceManager->requestImage(path, m_imageRevision, nullptr);

    angle = angle * M_PI / 180.0;

    if (reloadTime > -1) {
        try {
            modificationTime = std::filesystem::last_write_time(absolutePath(path, ""));
        } catch (std::exception& e) { Log::logger->log(Log::ERR, "{}", e.what()); }

        plantTimer();
    }
}

void CImage::reset() {
    if (imageTimer) {
        imageTimer->cancel();
        imageTimer.reset();
    }

    unloadMedia();

    if (g_pHyprlock->isTerminating())
        return;

    imageFB.destroyBuffer();

    if (asset && reloadTime > -1) // Don't unload asset if it's a static image
        g_asyncResourceManager->unload(asset);

    asset             = nullptr;
    m_pendingResource = false;
    resourceID        = 0;
}

bool CImage::loadMedia() {
    auto        player = makeUnique<CMediaPlayer>();
    std::string error;

    // A 4K video shown in a 300px badge does not need to be decoded at 4K.
    const int MAXDIM = std::max(64, size * 2);

    const CMediaPlayer::SOptions OPTIONS{
        .loop      = m_mediaLoop,
        .speed     = m_mediaSpeed,
        .maxFPS    = m_mediaFPS,
        .async     = true,
        .maxWidth  = MAXDIM,
        .maxHeight = MAXDIM,
        .scale     = m_mediaScale > 0 ? m_mediaScale : 1.0,
        .port      = stringPort,
    };

    if (!player->load(path, OPTIONS, error)) {
        Log::logger->log(Log::ERR, "Image: failed to load animated media {}: {}", path, error);
        return false;
    }

    unloadMedia();

    if (asset) {
        g_asyncResourceManager->unload(asset);
        asset = nullptr;
    }

    resourceID = 0;
    imageFB.destroyBuffer();
    m_media = std::move(player);
    return true;
}

void CImage::unloadMedia() {
    m_media.reset();
    m_mediaSerial = 0;
}

bool CImage::draw(const SRenderData& data) {
    bool newMediaFrame = false;

    if (m_media) {
        newMediaFrame = m_media->tick();

        if (!m_media->hasTexture())
            return false;
    } else {
        if (resourceID == 0)
            return false;

        if (!asset)
            asset = g_asyncResourceManager->getAssetByID(resourceID);

        if (!asset)
            return true;

        if (asset->m_iType == TEXTURE_INVALID) {
            g_asyncResourceManager->unload(asset);
            resourceID = 0;
            return false;
        }
    }

    const CTexture& TEX = m_media ? m_media->texture() : *asset;

    const Vector2D IMAGEPOS  = {border, border};
    const Vector2D BORDERPOS = {0.0, 0.0};
    const Vector2D TEXSIZE   = TEX.m_vSize;
    const float    SCALEX    = size / TEXSIZE.x;
    const float    SCALEY    = size / TEXSIZE.y;

    // image with borders offset, with extra pixel for anti-aliasing when rotated
    CBox texbox = {angle == 0 ? IMAGEPOS : IMAGEPOS + Vector2D{1.0, 1.0}, TEXSIZE};

    texbox.w *= std::max(SCALEX, SCALEY);
    texbox.h *= std::max(SCALEX, SCALEY);

    // plus borders if any
    CBox borderBox = {angle == 0 ? BORDERPOS : BORDERPOS + Vector2D{1.0, 1.0}, texbox.size() + IMAGEPOS * 2.0};

    borderBox.round();

    Vector2D       FBSIZE      = angle == 0 ? borderBox.size() : borderBox.size() + Vector2D{2.0, 2.0};
    FBSIZE.x                   = std::max(1.0, std::round(FBSIZE.x));
    FBSIZE.y                   = std::max(1.0, std::round(FBSIZE.y));
    const int      ROUND       = roundingForBox(texbox, rounding);
    const int      BORDERROUND = roundingForBorderBox(borderBox, rounding, border);

    const bool     RESIZED     = imageFB.isAllocated() && (imageFB.m_vSize.x != FBSIZE.x || imageFB.m_vSize.y != FBSIZE.y);
    const bool     NEEDRENDER  = !imageFB.isAllocated() || RESIZED || newMediaFrame;

    if (NEEDRENDER) {
        if (RESIZED)
            imageFB.destroyBuffer();

        if (!imageFB.isAllocated())
            imageFB.alloc(FBSIZE.x, FBSIZE.y, true);

        g_pRenderer->pushFb(imageFB.m_iFb);
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);

        if (border > 0)
            g_pRenderer->renderBorder(borderBox, color, border, BORDERROUND, 1.0);

        texbox.round();
        g_pRenderer->renderTexture(texbox, TEX, 1.0, ROUND, HYPRUTILS_TRANSFORM_NORMAL);
        g_pRenderer->popFb();
    }

    CTexture* tex    = &imageFB.m_cTex;
    CBox      dstBox = {{}, tex->m_vSize};

    if (firstRender) {
        firstRender = false;
        shadow.markShadowDirty();
    }

    shadow.draw(data);

    pos = posFromHVAlign(viewport, tex->m_vSize, configPos, halign, valign, angle);

    dstBox.x = pos.x;
    dstBox.y = pos.y;

    dstBox.round();
    dstBox.rot = angle;
    g_pRenderer->renderTexture(dstBox, *tex, data.opacity, 0, HYPRUTILS_TRANSFORM_FLIPPED_180);

    return data.opacity < 1.0;
}

void CImage::onAssetUpdate(ResourceID id, ASP<CTexture> newAsset) {
    m_pendingResource = false;

    if (m_media) {
        if (newAsset)
            g_asyncResourceManager->unload(newAsset);
        return;
    }

    if (!newAsset)
        Log::logger->log(Log::ERR, "asset update failed, resourceID: {} not available on update!", id);
    else if (newAsset->m_iType == TEXTURE_INVALID) {
        g_asyncResourceManager->unload(newAsset);
        Log::logger->log(Log::ERR, "New image asset has an invalid texture!");
    } else {
        g_asyncResourceManager->unload(asset);
        imageFB.destroyBuffer();

        asset       = newAsset;
        resourceID  = id;
        firstRender = true;
    }
}

CBox CImage::getBoundingBoxWl() const {
    if (!imageFB.isAllocated())
        return CBox{};

    return {
        Vector2D{pos.x, viewport.y - pos.y - imageFB.m_cTex.m_vSize.y},
        imageFB.m_cTex.m_vSize,
    };
}

void CImage::onClick(uint32_t button, bool down, const Vector2D& pos) {
    if (down && !onclickCommand.empty())
        spawnAsync(onclickCommand);
}

void CImage::onHover(const Vector2D& pos) {
    if (!onclickCommand.empty())
        g_pSeatManager->m_pCursorShape->setShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
}
