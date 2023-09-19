/*
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2015 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BitmapTexturePool.h"
#include <numeric>
#include <wtf/text/StringToIntegerConversion.h>

#if USE(TEXTURE_MAPPER_GL)
#include "BitmapTextureGL.h"
#endif

namespace WebCore {

static const Seconds releaseUnusedSecondsTolerance { 3_s };
static const Seconds releaseUnusedSecondsToleranceWhenPixelLimitExceeded { 50_ms };
static const Seconds releaseUnusedTexturesTimerInterval { 250_ms };
constexpr uint64_t defaultStoredPixelsLimit { 10 * 1920 * 1080 };

#if USE(TEXTURE_MAPPER_GL)
BitmapTexturePool::BitmapTexturePool(const TextureMapperContextAttributes& contextAttributes)
    : m_contextAttributes(contextAttributes)
    , m_releaseUnusedTexturesTimer(RunLoop::current(), this, &BitmapTexturePool::releaseUnusedTexturesTimerFired)
{
}
#endif

RefPtr<BitmapTexture> BitmapTexturePool::acquireTexture(const IntSize& size, const BitmapTexture::Flags flags)
{
    Entry* selectedEntry = std::find_if(m_textures.begin(), m_textures.end(),
        [&](Entry& entry) {
            return entry.m_texture->refCount() == 1
                && entry.m_texture->size() == size
                && (entry.m_texture->flags() & BitmapTexture::DepthBuffer) == (flags & BitmapTexture::DepthBuffer);
        });

    if (selectedEntry == m_textures.end()) {
        m_textures.append(Entry(createTexture(flags)));
        selectedEntry = &m_textures.last();
    }

    scheduleReleaseUnusedTextures();
    selectedEntry->markIsInUse();
    return selectedEntry->m_texture.copyRef();
}

void BitmapTexturePool::scheduleReleaseUnusedTextures()
{
    if (m_releaseUnusedTexturesTimer.isActive())
        return;

    m_releaseUnusedTexturesTimer.startOneShot(releaseUnusedTexturesTimerInterval);
}

void BitmapTexturePool::releaseUnusedTexturesTimerFired()
{
    if (m_textures.isEmpty())
        return;

    // Delete entries, which have been unused in releaseUnusedSecondsTolerance.
    MonotonicTime minUsedTime = MonotonicTime::now() - releaseUnusedSecondsTolerance;

    static uint64_t storedPixelsLimit = defaultStoredPixelsLimit;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, []() {
        String envString = String::fromLatin1(getenv("WPE_BITMAP_TEXTURE_POOL_PIXEL_LIMIT"));
        if (!envString.isEmpty()) {
            uint64_t limit = parseInteger<uint64_t>(envString).value_or(0);
            storedPixelsLimit = limit > 0 ? limit : defaultStoredPixelsLimit;
        }
    });
    const uint64_t storedPixelsNumber = std::accumulate(
        m_textures.begin(),
        m_textures.end(),
        0,
        [](const uint64_t accumulator, const auto& textureEntry) {
            const auto& textureSize = textureEntry.m_texture->size();
            return accumulator + textureSize.width() * textureSize.height();
        }
    );
    if (storedPixelsNumber >= storedPixelsLimit)
        minUsedTime = MonotonicTime::now() - releaseUnusedSecondsToleranceWhenPixelLimitExceeded;

    m_textures.removeAllMatching([&minUsedTime](const Entry& entry) {
        return entry.canBeReleased(minUsedTime);
    });

    if (!m_textures.isEmpty())
        scheduleReleaseUnusedTextures();
}

RefPtr<BitmapTexture> BitmapTexturePool::createTexture(const BitmapTexture::Flags flags)
{
#if USE(TEXTURE_MAPPER_GL)
    return BitmapTextureGL::create(m_contextAttributes, flags);
#else
    UNUSED_PARAM(flags);
    return nullptr;
#endif
}

} // namespace WebCore
