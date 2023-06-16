/* GStreamer ClearKey common encryption decryptor
 *
 * Copyright (C) 2013 YouView TV Ltd. <alex.ashley@youview.com>
 * Copyright (C) 2016 Metrological
 * Copyright (C) 2016 Igalia S.L
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#include "config.h"
#include "WebKitCommonEncryptionDecryptorGStreamer.h"

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)

#include "GStreamerEMEUtilities.h"
#include "SharedBuffer.h"
#include <CDMInstance.h>
#include <wtf/Condition.h>
#include <wtf/Optional.h>
#include <wtf/PrintStream.h>
#include <wtf/RunLoop.h>
#include <wtf/text/Base64.h>
#include <wtf/text/StringHash.h>

using WebCore::CDMInstance;
using WebCore::GstMappedBuffer;

#define WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), WEBKIT_TYPE_MEDIA_CENC_DECRYPT, WebKitMediaCommonEncryptionDecryptPrivate))
struct _WebKitMediaCommonEncryptionDecryptPrivate {
    std::optional<Ref<WebCore::SharedBuffer>> m_currentKeyID;
    Lock m_mutex;
    Condition m_condition;
    RefPtr<CDMInstance> m_cdmInstance;
    WTF::HashMap<String, WebCore::InitData> m_initDatas;
    Vector<GRefPtr<GstEvent>> m_protectionEvents;
    uint32_t m_currentEvent { 0 };
    bool m_isFlushing { false };
};

static GstStateChangeReturn webKitMediaCommonEncryptionDecryptorChangeState(GstElement*, GstStateChange transition);
static void webKitMediaCommonEncryptionDecryptorSetContext(GstElement*, GstContext*);
static void webKitMediaCommonEncryptionDecryptorFinalize(GObject*);
static GstCaps* webkitMediaCommonEncryptionDecryptTransformCaps(GstBaseTransform*, GstPadDirection, GstCaps*, GstCaps*);
static gboolean webkitMediaCommonEncryptionDecryptAcceptCaps(GstBaseTransform*, GstPadDirection, GstCaps*);
static GstFlowReturn webkitMediaCommonEncryptionDecryptTransformInPlace(GstBaseTransform*, GstBuffer*);
static gboolean webkitMediaCommonEncryptionDecryptSinkEventHandler(GstBaseTransform*, GstEvent*);
static void webkitMediaCommonEncryptionDecryptProcessProtectionEvents(WebKitMediaCommonEncryptionDecrypt*, Ref<WebCore::SharedBuffer>&&);
static bool webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(WebKitMediaCommonEncryptionDecrypt*);

GST_DEBUG_CATEGORY_STATIC(webkit_media_common_encryption_decrypt_debug_category);
#define GST_CAT_DEFAULT webkit_media_common_encryption_decrypt_debug_category

#define webkit_media_common_encryption_decrypt_parent_class parent_class
G_DEFINE_TYPE(WebKitMediaCommonEncryptionDecrypt, webkit_media_common_encryption_decrypt, GST_TYPE_BASE_TRANSFORM);

static void webkit_media_common_encryption_decrypt_class_init(WebKitMediaCommonEncryptionDecryptClass* klass)
{
    GObjectClass* gobjectClass = G_OBJECT_CLASS(klass);
    gobjectClass->finalize = webKitMediaCommonEncryptionDecryptorFinalize;

    GST_DEBUG_CATEGORY_INIT(webkit_media_common_encryption_decrypt_debug_category,
        "webkitcenc", 0, "Common Encryption base class");

    GstElementClass* elementClass = GST_ELEMENT_CLASS(klass);
    elementClass->change_state = GST_DEBUG_FUNCPTR(webKitMediaCommonEncryptionDecryptorChangeState);
    elementClass->set_context = GST_DEBUG_FUNCPTR(webKitMediaCommonEncryptionDecryptorSetContext);

    GstBaseTransformClass* baseTransformClass = GST_BASE_TRANSFORM_CLASS(klass);
    baseTransformClass->transform_ip = GST_DEBUG_FUNCPTR(webkitMediaCommonEncryptionDecryptTransformInPlace);
    baseTransformClass->transform_caps = GST_DEBUG_FUNCPTR(webkitMediaCommonEncryptionDecryptTransformCaps);
    baseTransformClass->accept_caps = GST_DEBUG_FUNCPTR(webkitMediaCommonEncryptionDecryptAcceptCaps);
    baseTransformClass->transform_ip_on_passthrough = FALSE;
    baseTransformClass->passthrough_on_same_caps = TRUE;
    baseTransformClass->sink_event = GST_DEBUG_FUNCPTR(webkitMediaCommonEncryptionDecryptSinkEventHandler);

    klass->setupCipher = [](WebKitMediaCommonEncryptionDecrypt*, GstBuffer*) { return true; };
    klass->releaseCipher = [](WebKitMediaCommonEncryptionDecrypt*) { };
    klass->handleKeyId = [](WebKitMediaCommonEncryptionDecrypt*, const WebCore::SharedBuffer&) { return true; };
    klass->attemptToDecryptWithLocalInstance = [](WebKitMediaCommonEncryptionDecrypt*, const WebCore::SharedBuffer&) { return false; };

    g_type_class_add_private(klass, sizeof(WebKitMediaCommonEncryptionDecryptPrivate));
}

static void webkit_media_common_encryption_decrypt_init(WebKitMediaCommonEncryptionDecrypt* self)
{
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);

    self->priv = priv;
    new (priv) WebKitMediaCommonEncryptionDecryptPrivate();

    GstBaseTransform* base = GST_BASE_TRANSFORM(self);
    gst_base_transform_set_in_place(base, TRUE);
    gst_base_transform_set_passthrough(base, FALSE);
    gst_base_transform_set_gap_aware(base, FALSE);
}

static void webKitMediaCommonEncryptionDecryptorFinalize(GObject* object)
{
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(object);
    WebKitMediaCommonEncryptionDecryptPrivate* priv = self->priv;

    priv->~WebKitMediaCommonEncryptionDecryptPrivate();
    GST_CALL_PARENT(G_OBJECT_CLASS, finalize, (object));
}

static GstCaps* webkitMediaCommonEncryptionDecryptTransformCaps(GstBaseTransform* base, GstPadDirection direction, GstCaps* caps, GstCaps* filter)
{
    if (direction == GST_PAD_UNKNOWN)
        return nullptr;

    GST_LOG_OBJECT(base, "direction: %s, caps: %" GST_PTR_FORMAT " filter: %" GST_PTR_FORMAT, (direction == GST_PAD_SRC) ? "src" : "sink", caps, filter);

    GstCaps* transformedCaps = gst_caps_new_empty();
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(base);

    unsigned size = gst_caps_get_size(caps);
    for (unsigned i = 0; i < size; ++i) {
        GstStructure* incomingStructure = gst_caps_get_structure(caps, i);
        GUniquePtr<GstStructure> outgoingStructure = nullptr;

        if (direction == GST_PAD_SINK) {
            bool canDoPassthru = false;
            if (!gst_structure_has_field(incomingStructure, "original-media-type")) {
                // Let's check compatibility with src pad caps to see if we can do passthru.
                GRefPtr<GstCaps> srcPadTemplateCaps = adoptGRef(gst_pad_get_pad_template_caps(GST_BASE_TRANSFORM_SRC_PAD(base)));
                unsigned srcPadTemplateCapsSize = gst_caps_get_size(srcPadTemplateCaps.get());
                for (unsigned j = 0; !canDoPassthru && j < srcPadTemplateCapsSize; ++j)
                    canDoPassthru = gst_structure_can_intersect(incomingStructure, gst_caps_get_structure(srcPadTemplateCaps.get(), j));

                if (!canDoPassthru)
                    continue;
            }

            outgoingStructure = GUniquePtr<GstStructure>(gst_structure_copy(incomingStructure));

            if (!canDoPassthru)
                gst_structure_set_name(outgoingStructure.get(), gst_structure_get_string(outgoingStructure.get(), "original-media-type"));

            // Filter out the DRM related fields from the down-stream caps.
            for (int j = 0; j < gst_structure_n_fields(incomingStructure); ++j) {
                const gchar* fieldName = gst_structure_nth_field_name(incomingStructure, j);

                if (g_str_has_prefix(fieldName, "protection-system")
                    || g_str_has_prefix(fieldName, "original-media-type")
                    || g_str_has_prefix(fieldName, "encryption-algorithm")
                    || g_str_has_prefix(fieldName, "encoding-scope")
                    || g_str_has_prefix(fieldName, "cipher-mode"))
                    gst_structure_remove_field(outgoingStructure.get(), fieldName);
            }
        } else {
            outgoingStructure = GUniquePtr<GstStructure>(gst_structure_copy(incomingStructure));

            if (!gst_base_transform_is_passthrough(base)) {
                // Filter out the video related fields from the up-stream caps,
                // because they are not relevant to the input caps of this element and
                // can cause caps negotiation failures with adaptive bitrate streams.
                for (int index = gst_structure_n_fields(outgoingStructure.get()) - 1; index >= 0; --index) {
                    const gchar* fieldName = gst_structure_nth_field_name(outgoingStructure.get(), index);
                    GST_TRACE("Check field \"%s\" for removal", fieldName);

                    if (!g_strcmp0(fieldName, "base-profile")
                        || !g_strcmp0(fieldName, "codec_data")
                        || !g_strcmp0(fieldName, "height")
                        || !g_strcmp0(fieldName, "framerate")
                        || !g_strcmp0(fieldName, "level")
                        || !g_strcmp0(fieldName, "pixel-aspect-ratio")
                        || !g_strcmp0(fieldName, "profile")
                        || !g_strcmp0(fieldName, "rate")
                        || !g_strcmp0(fieldName, "width")) {
                        gst_structure_remove_field(outgoingStructure.get(), fieldName);
                        GST_TRACE("Removing field %s", fieldName);
                    }
                }

                gst_structure_set(outgoingStructure.get(), "original-media-type", G_TYPE_STRING, gst_structure_get_name(incomingStructure), nullptr);

                WebKitMediaCommonEncryptionDecryptPrivate* priv = self->priv;
                LockHolder locker(priv->m_mutex);

                if (webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(self))
                    gst_structure_set_name(outgoingStructure.get(),
                        WebCore::GStreamerEMEUtilities::isUnspecifiedKeySystem(priv->m_cdmInstance->keySystem()) ? "application/x-webm-enc" : "application/x-cenc");
            }
        }

        bool duplicate = false;
        unsigned capsSize = gst_caps_get_size(transformedCaps);

        for (unsigned index = 0; !duplicate && index < capsSize; ++index) {
            GstStructure* structure = gst_caps_get_structure(transformedCaps, index);
            if (gst_structure_is_equal(structure, outgoingStructure.get()))
                duplicate = true;
        }

        if (!duplicate)
            gst_caps_append_structure(transformedCaps, outgoingStructure.release());
    }

    if (filter) {
        GstCaps* intersection;

        GST_LOG_OBJECT(base, "Using filter caps %" GST_PTR_FORMAT, filter);
        intersection = gst_caps_intersect_full(transformedCaps, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(transformedCaps);
        transformedCaps = intersection;
    }

    GST_LOG_OBJECT(base, "returning %" GST_PTR_FORMAT, transformedCaps);
    return transformedCaps;
}

static gboolean webkitMediaCommonEncryptionDecryptAcceptCaps(GstBaseTransform* trans, GstPadDirection direction, GstCaps* caps)
{
    gboolean result = GST_BASE_TRANSFORM_CLASS(parent_class)->accept_caps(trans, direction, caps);

    if (result || direction == GST_PAD_SRC)
        return result;

    GRefPtr<GstCaps> srcPadTemplateCaps = adoptGRef(gst_pad_get_pad_template_caps(GST_BASE_TRANSFORM_SRC_PAD(trans)));
    result = gst_caps_can_intersect(caps, srcPadTemplateCaps.get());
    GST_TRACE_OBJECT(trans, "attempted to match %" GST_PTR_FORMAT " with the src pad template caps %" GST_PTR_FORMAT " to see if we can go passthru mode, result %s", caps, srcPadTemplateCaps.get(), boolForPrinting(result));

    return result;
}

static void attemptToDecryptWithLocalInstance(WebKitMediaCommonEncryptionDecrypt* self, const Ref<WebCore::SharedBuffer>& keyID)
{
  RELEASE_ASSERT(WEBKIT_IS_MEDIA_CENC_DECRYPT(self));
  WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);
  WebKitMediaCommonEncryptionDecryptClass* klass = WEBKIT_MEDIA_CENC_DECRYPT_GET_CLASS(self);
  ASSERT(priv->m_mutex.isLocked());
  priv->m_currentKeyID.reset();
  bool keyReceived = klass->attemptToDecryptWithLocalInstance(self, keyID.get());
  if (keyReceived)
      priv->m_currentKeyID = keyID.copyRef();
}

static GstFlowReturn webkitMediaCommonEncryptionDecryptTransformInPlace(GstBaseTransform* base, GstBuffer* buffer)
{
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(base);
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);
    WebKitMediaCommonEncryptionDecryptClass* klass = WEBKIT_MEDIA_CENC_DECRYPT_GET_CLASS(self);

    GstProtectionMeta* protectionMeta = reinterpret_cast<GstProtectionMeta*>(gst_buffer_get_protection_meta(buffer));
    if (!protectionMeta) {
        GST_TRACE_OBJECT(self, "buffer %p has no protection meta, assuming it's not encrypted", buffer);
        return GST_FLOW_OK;
    }

    LockHolder locker(priv->m_mutex);
    const GValue* streamEncryptionEventsList = gst_structure_get_value(protectionMeta->info, "stream-encryption-events");
    if (streamEncryptionEventsList && GST_VALUE_HOLDS_LIST(streamEncryptionEventsList)) {
        unsigned streamEncryptionEventsListSize = gst_value_list_get_size(streamEncryptionEventsList);
        for (unsigned i = 0; i < streamEncryptionEventsListSize; ++i)
            priv->m_protectionEvents.append(GRefPtr<GstEvent>(static_cast<GstEvent*>(g_value_get_boxed(gst_value_list_get_value(streamEncryptionEventsList, i)))));
        gst_structure_remove_field(protectionMeta->info, "stream-encryption-events");
        if (!gst_structure_n_fields(protectionMeta->info)) {
            GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Not enough protection meta-data"), ("Buffer %p did not have enough protection meta-data", buffer));
            return GST_FLOW_NOT_SUPPORTED;
        }
    }

    const GValue* value;
    value = gst_structure_get_value(protectionMeta->info, "kid");
    GstBuffer* keyIDBuffer = nullptr;
    if (!value) {
        GST_ELEMENT_ERROR (self, STREAM, DECRYPT_NOKEY, ("No key ID available for encrypted sample"), (NULL));
        return GST_FLOW_NOT_SUPPORTED;
    }

    keyIDBuffer = gst_value_get_buffer(value);

    GstMappedBuffer mappedKeyID(keyIDBuffer, GST_MAP_READ);
    if (!mappedKeyID) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to map key ID buffer."), (NULL));
        return GST_FLOW_NOT_SUPPORTED;
    }
    auto keyId = WebCore::SharedBuffer::create(mappedKeyID.data(), mappedKeyID.size());

    if (!priv->m_protectionEvents.isEmpty())
        webkitMediaCommonEncryptionDecryptProcessProtectionEvents(self, keyId.copyRef());

    GST_MEMDUMP_OBJECT(self, "requested key ID", reinterpret_cast<const guint8*>(keyId->data()), keyId->size());
    if (priv->m_currentKeyID.has_value())
        GST_MEMDUMP_OBJECT(self, "current key ID", reinterpret_cast<const guint8*>(priv->m_currentKeyID.value()->data()), priv->m_currentKeyID.value()->size());
    else
        GST_TRACE_OBJECT(self, "current key ID not set");

    if (!priv->m_currentKeyID.has_value() || priv->m_currentKeyID.value().ptr() != keyId.ptr()) {
        priv->m_currentKeyID.reset();
        if (GST_STATE(GST_ELEMENT(self)) < GST_STATE_PAUSED || (GST_STATE_TARGET(GST_ELEMENT(self)) != GST_STATE_VOID_PENDING && GST_STATE_TARGET(GST_ELEMENT(self)) < GST_STATE_PAUSED)) {
            GST_ELEMENT_ERROR (self, STREAM, FAILED, ("can't process key requests in less than PAUSED state"), (NULL));
            return GST_FLOW_NOT_SUPPORTED;
        }
        if (!priv->m_condition.waitFor(priv->m_mutex, WEBCORE_GSTREAMER_EME_LICENSE_KEY_RESPONSE_TIMEOUT, [self, priv, keyID = WTFMove(keyId)] {
            if (priv->m_isFlushing)
                return true;
            if (!priv->m_currentKeyID.has_value())
                attemptToDecryptWithLocalInstance(self, keyID);
            if (priv->m_currentKeyID.has_value())
                return true;
            else {
                GST_DEBUG_OBJECT(self, "key not received yet, may wait for it");
                return false;
            }
        })) {
            GST_ELEMENT_ERROR (self, STREAM, DECRYPT_NOKEY, ("Key not available"), (NULL));
            return GST_FLOW_NOT_SUPPORTED;
        }

        if (priv->m_isFlushing) {
            GST_DEBUG_OBJECT(self, "flushing");
            return GST_FLOW_FLUSHING;
        }
    }

    GST_TRACE_OBJECT(self, "key available, preparing for decryption");

    unsigned ivSize;
    if (!gst_structure_get_uint(protectionMeta->info, "iv_size", &ivSize)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to get iv_size"), (NULL));
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    gboolean encrypted;
    if (!gst_structure_get_boolean(protectionMeta->info, "encrypted", &encrypted)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to get encrypted flag"), (NULL));
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    if (!ivSize || !encrypted) {
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_OK;
    }

    GST_TRACE_OBJECT(base, "protection meta: %" GST_PTR_FORMAT, protectionMeta->info);

    unsigned subSampleCount;
    if (!gst_structure_get_uint(protectionMeta->info, "subsample_count", &subSampleCount)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to get subsample_count"), (NULL));
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    GstBuffer* subSamplesBuffer = nullptr;
    if (subSampleCount) {
        value = gst_structure_get_value(protectionMeta->info, "subsamples");
        if (!value) {
            GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to get subsamples"), (NULL));
            gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
            return GST_FLOW_NOT_SUPPORTED;
        }
        subSamplesBuffer = gst_value_get_buffer(value);
    }

    GST_MEMDUMP_OBJECT(self, "key ID for sample", mappedKeyID.data(), mappedKeyID.size());

    if (!klass->setupCipher(self, keyIDBuffer)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to configure cipher"), (NULL));
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    value = gst_structure_get_value(protectionMeta->info, "iv");
    if (!value) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Failed to get IV for sample"), (NULL));
        klass->releaseCipher(self);
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    GstBuffer* ivBuffer = gst_value_get_buffer(value);
    GST_TRACE_OBJECT(self, "decrypting");
    if (!klass->decrypt(self, keyIDBuffer, ivBuffer, buffer, subSampleCount, subSamplesBuffer)) {
        GST_ELEMENT_ERROR (self, STREAM, DECRYPT, ("Decryption failed"), (NULL));
        klass->releaseCipher(self);
        gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
        return GST_FLOW_NOT_SUPPORTED;
    }

    klass->releaseCipher(self);
    gst_buffer_remove_meta(buffer, reinterpret_cast<GstMeta*>(protectionMeta));
    return GST_FLOW_OK;
}

static bool webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(WebKitMediaCommonEncryptionDecrypt* self)
{
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);

    ASSERT(priv->m_mutex.isLocked());

    if (!priv->m_cdmInstance) {
        GRefPtr<GstContext> context = adoptGRef(gst_element_get_context(GST_ELEMENT(self), "drm-cdm-instance"));
        // According to the GStreamer documentation, if we can't find the context, we should run a downstream query, then an upstream one and then send a bus
        // message. In this case that does not make a lot of sense since only the app (player) answers it, meaning that no query is going to solve it. A message
        // could be helpful but the player sets the context as soon as it gets the CDMInstance and if it does not have it, we have no way of asking for one as it is
        // something provided by crossplatform code. This means that we won't be able to answer the bus request in any way either. Summing up, neither queries nor bus
        // requests are useful here.
        if (context) {
            const GValue* value = gst_structure_get_value(gst_context_get_structure(context.get()), "cdm-instance");
            priv->m_cdmInstance = value ? reinterpret_cast<CDMInstance*>(g_value_get_pointer(value)) : nullptr;
            if (priv->m_cdmInstance)
                GST_DEBUG_OBJECT(self, "received new CDMInstance %p", priv->m_cdmInstance.get());
            else
                GST_TRACE_OBJECT(self, "former instance was detached");
        }
    }

    GST_TRACE_OBJECT(self, "CDMInstance available %s", boolForPrinting(priv->m_cdmInstance.get()));
    return priv->m_cdmInstance;
}

static void handleKeyID(WebKitMediaCommonEncryptionDecrypt* self, Ref<WebCore::SharedBuffer>&& keyID)
{
  RELEASE_ASSERT(WEBKIT_IS_MEDIA_CENC_DECRYPT(self));
  WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);
  WebKitMediaCommonEncryptionDecryptClass* klass = WEBKIT_MEDIA_CENC_DECRYPT_GET_CLASS(self);
  priv->m_currentKeyID.reset();
  bool keyReceived = webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(self) && !klass->handleKeyId(self, keyID.copyRef());
  if (keyReceived)
      priv->m_currentKeyID = WTFMove(keyID);
}

static void webkitMediaCommonEncryptionDecryptProcessProtectionEvents(WebKitMediaCommonEncryptionDecrypt* self, Ref<WebCore::SharedBuffer>&& kid)
{
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);

    ASSERT(priv->m_mutex.isLocked());

    bool isCDMInstanceAvailable = webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(self);

    WebCore::InitData concatenatedInitDatas;
    for (auto& event : priv->m_protectionEvents) {
        GstBuffer* buffer = nullptr;
        const char* eventKeySystemUUID = nullptr;
        const char* origin = nullptr;
        gst_event_parse_protection(event.get(), &eventKeySystemUUID, &buffer, &origin);
        const char* eventKeySystem = WebCore::GStreamerEMEUtilities::uuidToKeySystem(eventKeySystemUUID);

        GST_TRACE_OBJECT(self, "handling protection event %u for %s", GST_EVENT_SEQNUM(event.get()), eventKeySystem);

        if (isCDMInstanceAvailable && g_strcmp0(WebCore::GStreamerEMEUtilities::keySystemToUuid(eventKeySystem), WebCore::GStreamerEMEUtilities::keySystemToUuid(priv->m_cdmInstance->keySystem()))) {
            GST_TRACE_OBJECT(self, "protection event for a different key system");
            continue;
        }

        if (priv->m_currentEvent == GST_EVENT_SEQNUM(event.get())) {
            GST_TRACE_OBJECT(self, "event %u already handled", priv->m_currentEvent);
            continue;
        }

        WebCore::InitData initData;
        if (isCDMInstanceAvailable)
            initData = priv->m_initDatas.get(priv->m_cdmInstance->keySystem());

        priv->m_currentEvent = GST_EVENT_SEQNUM(event.get());

        if (initData.isEmpty() || gst_buffer_memcmp(buffer, 0, initData.characters8(), initData.sizeInBytes())) {
            GstMappedBuffer mappedBuffer(buffer, GST_MAP_READ);
            if (!mappedBuffer) {
                GST_WARNING_OBJECT(self, "cannot map protection data");
                continue;
            }

            const uint8_t* data = mappedBuffer.data();
            size_t dataSize = mappedBuffer.size();
            GRefPtr<GstBuffer> initDataBuffer = buffer;
            Vector<uint8_t> decodedData;
            if (!g_strcmp0(origin, "smooth-streaming") && WTF::base64Decode(reinterpret_cast<const char*>(data), dataSize, { decodedData })) {
                data = decodedData.data();
                dataSize = decodedData.size();
                initDataBuffer = adoptGRef(gst_buffer_new_allocate(nullptr, dataSize, nullptr));
                GstMappedBuffer mappedInitDataBuffer(initDataBuffer.get(), GST_MAP_WRITE);
                ASSERT(mappedInitDataBuffer);
                memcpy(mappedInitDataBuffer.data(), decodedData.data(), mappedInitDataBuffer.size());
            }

            initData = WebCore::InitData(data, dataSize);
            GST_DEBUG_OBJECT(self, "init data of size %u", dataSize);
            GST_TRACE_OBJECT(self, "init data MD5 %s", WebCore::GStreamerEMEUtilities::initDataMD5(initData).utf8().data());
            GST_MEMDUMP_OBJECT(self, "init data", mappedBuffer.data(), mappedBuffer.size());
            priv->m_initDatas.set(eventKeySystem, initData);
            GST_MEMDUMP_OBJECT(self, "key ID", reinterpret_cast<const uint8_t*>(kid->data()), kid->size());
            handleKeyID(self, WTFMove(kid));
            if (!priv->m_currentKeyID.has_value()) {
                if (isCDMInstanceAvailable) {
                    GST_DEBUG_OBJECT(self, "posting event and considering key not received");
                    gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self),
                        gst_structure_new("drm-initialization-data-encountered", "init-data", GST_TYPE_BUFFER, initDataBuffer.get(), "key-system-uuid", G_TYPE_STRING, eventKeySystemUUID, nullptr)));
                    break;
                } else {
                    GST_TRACE_OBJECT(self, "concatenating init data and considering key not received");
                    priv->m_currentEvent = 0;
                    concatenatedInitDatas.append(initData);
                }
            } else {
                GST_DEBUG_OBJECT(self, "key is already usable");
                priv->m_condition.notifyOne();
                break;
            }
        } else {
            GST_DEBUG_OBJECT(self, "init data already present");
            break;
        }
    }

    priv->m_protectionEvents.clear();

    if (!isCDMInstanceAvailable && !concatenatedInitDatas.isEmpty()) {
        GRefPtr<GstBuffer> buffer = adoptGRef(gst_buffer_new_allocate(nullptr, concatenatedInitDatas.sizeInBytes(), nullptr));
        GstMappedBuffer mappedBuffer(buffer.get(), GST_MAP_WRITE);
        if (!mappedBuffer) {
            GST_WARNING_OBJECT(self, "cannot map writable init data");
            return;
        }
        memcpy(mappedBuffer.data(), concatenatedInitDatas.characters8(), concatenatedInitDatas.sizeInBytes());
        GST_DEBUG_OBJECT(self, "reporting concatenated init datas of size %u", concatenatedInitDatas.sizeInBytes());
        GST_TRACE_OBJECT(self, "init data MD5 %s", WebCore::GStreamerEMEUtilities::initDataMD5(concatenatedInitDatas).utf8().data());
        GST_MEMDUMP_OBJECT(self, "init data", reinterpret_cast<const uint8_t*>(concatenatedInitDatas.characters8()), concatenatedInitDatas.sizeInBytes());
        gst_element_post_message(GST_ELEMENT(self), gst_message_new_element(GST_OBJECT(self), gst_structure_new("drm-initialization-data-encountered", "init-data", GST_TYPE_BUFFER, buffer.get(), nullptr)));
    }
}

static gboolean webkitMediaCommonEncryptionDecryptSinkEventHandler(GstBaseTransform* trans, GstEvent* event)
{
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(trans);
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);
    gboolean result = FALSE;

    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_PROTECTION: {
        const char* systemId = nullptr;

        gst_event_parse_protection(event, &systemId, nullptr, nullptr);
        GST_TRACE_OBJECT(self, "received protection event %u for %s", GST_EVENT_SEQNUM(event), systemId);

        LockHolder locker(priv->m_mutex);
        priv->m_protectionEvents.append(event);
        result = TRUE;
        gst_event_unref(event);
        break;
    }
    case GST_EVENT_CUSTOM_DOWNSTREAM_OOB: {
        const GstStructure* structure = gst_event_get_structure(event);
        if (gst_structure_has_name(structure, "drm-attempt-to-decrypt-with-local-instance")) {
            gst_event_unref(event);
            result = TRUE;
            LockHolder locker(priv->m_mutex);
            RELEASE_ASSERT(webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(self));
            GST_DEBUG_OBJECT(self, "attempting to decrypt with local instance %p", priv->m_cdmInstance.get());
            priv->m_condition.notifyOne();
        }
        break;
    }
    case GST_EVENT_FLUSH_START: {
        {
            LockHolder locker(priv->m_mutex);
            ASSERT(!priv->m_isFlushing);
            priv->m_isFlushing = true;
            GST_DEBUG_OBJECT(self, "flushing");
            priv->m_condition.notifyOne();
        }
        result = GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event(trans, event);
        break;
    }
    case GST_EVENT_FLUSH_STOP: {
        {
            LockHolder locker(priv->m_mutex);
            ASSERT(priv->m_isFlushing);
            priv->m_isFlushing = false;
            GST_DEBUG_OBJECT(self, "flushing done");
        }
        result = GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event(trans, event);
        break;
    }
    default:
        result = GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event(trans, event);
        break;
    }

    return result;
}

static GstStateChangeReturn webKitMediaCommonEncryptionDecryptorChangeState(GstElement* element, GstStateChange transition)
{
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(element);
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);

    switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
        GST_DEBUG_OBJECT(self, "PAUSED->READY");
        priv->m_isFlushing = false;
        priv->m_condition.notifyOne();
        break;
    default:
        break;
    }

    GstStateChangeReturn result = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

    // Add post-transition code here.

    return result;
}

static void webKitMediaCommonEncryptionDecryptorSetContext(GstElement* element, GstContext* context)
{
    WebKitMediaCommonEncryptionDecrypt* self = WEBKIT_MEDIA_CENC_DECRYPT(element);
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);

    if (gst_context_has_context_type(context, "drm-cdm-instance")) {
        const GValue* value = gst_structure_get_value(gst_context_get_structure(context), "cdm-instance");
        LockHolder locker(priv->m_mutex);
        priv->m_cdmInstance = value ? reinterpret_cast<CDMInstance*>(g_value_get_pointer(value)) : nullptr;
        GST_DEBUG_OBJECT(self, "received new CDMInstance %p", priv->m_cdmInstance.get());
        return;
    }

    GST_ELEMENT_CLASS(parent_class)->set_context(element, context);
}

RefPtr<CDMInstance> webKitMediaCommonEncryptionDecryptCDMInstance(WebKitMediaCommonEncryptionDecrypt* self)
{
    WebKitMediaCommonEncryptionDecryptPrivate* priv = WEBKIT_MEDIA_CENC_DECRYPT_GET_PRIVATE(self);
    ASSERT(priv->m_mutex.isLocked());
    return webkitMediaCommonEncryptionDecryptIsCDMInstanceAvailable(self) ? priv->m_cdmInstance : nullptr;
}
#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER)
