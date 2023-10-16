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

#include "config.h"
#include "GStreamerQuirkWesteros.h"

#if USE(GSTREAMER)

#include "GStreamerCommon.h"
#include <wtf/OptionSet.h>

namespace WebCore {

GST_DEBUG_CATEGORY_STATIC(webkit_westeros_quirks_debug);
#define GST_CAT_DEFAULT webkit_westeros_quirks_debug

GStreamerQuirkWesteros::GStreamerQuirkWesteros()
{
    GST_DEBUG_CATEGORY_INIT(webkit_westeros_quirks_debug, "webkitquirkswesteros", 0, "WebKit Westeros Quirks");
}

bool GStreamerQuirkWesteros::configureElement(GstElement* element, const OptionSet<ElementRuntimeCharacteristics>& characteristics)
{
    if (!characteristics.contains(ElementRuntimeCharacteristics::IsMediaStream))
        return false;

    GST_DEBUG("Enable 'immediate-output' in WesterosSink");
    g_object_set(element, "immediate-output", TRUE, nullptr);
    return true;
}

std::optional<bool> GStreamerQuirkWesteros::isHardwareAccelerated(GstElementFactory* factory)
{
    if (g_str_has_prefix(GST_OBJECT_NAME(factory), "westeros"))
        return true;

    return std::nullopt;
}

GstElement* GStreamerQuirkWesteros::createHolePunchVideoSink(bool isLegacyPlaybin, const MediaPlayer* player)
{
    AtomString val;
    bool isPIPRequested = player && player->doesHaveAttribute("pip"_s, &val) && equalLettersIgnoringASCIICase(val, "true"_s);
    if (isLegacyPlaybin && !isPIPRequested)
        return nullptr;
    // Westeros using holepunch.
    GstElement* videoSink = makeGStreamerElement("westerossink", "WesterosVideoSink");
    g_object_set(videoSink, "zorder", 0.0f, nullptr);
    if (isPIPRequested)
        g_object_set(videoSink, "res-usage", 0u, nullptr);
    return videoSink;
}

bool GStreamerQuirkWesteros::setHolePunchVideoRectangle(GstElement* videoSink, const IntRect& rect)
{
    GUniquePtr<gchar> rectString(g_strdup_printf("%d,%d,%d,%d", rect.x(), rect.y(), rect.width(), rect.height()));
    g_object_set(videoSink, "rectangle", rectString.get(), nullptr);
    return true;
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // USE(GSTREAMER)
