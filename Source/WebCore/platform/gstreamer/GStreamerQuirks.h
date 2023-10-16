/*
 * Copyright (C) 2023 Igalia S.L
 * Copyright (C) 2023 Metrological Group B.V.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#if USE(GSTREAMER)

#include "MediaPlayer.h"
#include "GRefPtrGStreamer.h"
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace WebCore {

enum class ElementRuntimeCharacteristics : uint8_t {
    IsMediaStream = 1 << 0,
    HasVideo = 1 << 1,
    HasAudio = 1 << 2,
    IsLiveStream = 1 << 3,
};

class GStreamerQuirk {
    WTF_MAKE_FAST_ALLOCATED;
public:
    GStreamerQuirk() = default;
    virtual ~GStreamerQuirk() = default;

    virtual const char* identifier() = 0;

    virtual bool isPlatformSupported() const { return true; }
    virtual GstElement* createWebAudioSink() { return nullptr; }
    virtual GstElement* createHolePunchVideoSink(bool, const MediaPlayer*) { return nullptr; }
    virtual bool setHolePunchVideoRectangle(GstElement*, const IntRect&) { return false; }
    virtual bool configureElement(GstElement*, const OptionSet<ElementRuntimeCharacteristics>&) { return false; }
    virtual std::optional<bool> isHardwareAccelerated(GstElementFactory*) { return std::nullopt; }
    virtual bool supportsVideoHolePunchRendering() const { return false; }
    virtual std::optional<GstElementFactoryListType> audioVideoDecoderFactoryListType() const { return std::nullopt; }
    virtual Vector<String> disallowedWebAudioDecoders() const { return { }; }

};

class GStreamerQuirksManager {
    friend NeverDestroyed<GStreamerQuirksManager>;
    WTF_MAKE_FAST_ALLOCATED;

public:
    static GStreamerQuirksManager& singleton();

    bool isEnabled() const;

    GstElement* createWebAudioSink();
    GstElement* createHolePunchVideoSink(bool isLegacyPlaybin, const MediaPlayer*);
    void setHolePunchVideoRectangle(GstElement*, const IntRect&);
    void configureElement(GstElement*, OptionSet<ElementRuntimeCharacteristics>&&);
    std::optional<bool> isHardwareAccelerated(GstElementFactory*) const;
    bool supportsVideoHolePunchRendering() const;
    GstElementFactoryListType audioVideoDecoderFactoryListType() const;
    Vector<String> disallowedWebAudioDecoders() const;

private:
    GStreamerQuirksManager();

    Vector<std::unique_ptr<GStreamerQuirk>> m_quirks;
};

} // namespace WebCore

#endif // USE(GSTREAMER)
