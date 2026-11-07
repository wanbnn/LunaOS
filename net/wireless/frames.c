#include "../../include/net/luna80211.h"

static void copy6(uint8_t to[6], const uint8_t *from) {
    for (unsigned i = 0; i < 6; ++i)
        to[i] = from[i];
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int luna80211_header(const uint8_t *frame, size_t length, Luna80211Header *out) {
    if (!frame || !out || length < 24)
        return L80211_EINVAL;
    uint16_t fc = le16(frame);
    if ((fc & 3) != 0)
        return L80211_ENOTSUP;
    uint8_t type = (fc >> 2) & 3;
    if (type != 0 && type != 2)
        return L80211_ENOTSUP;
    uint8_t to_ds = (fc >> 8) & 1, from_ds = (fc >> 9) & 1;
    uint8_t qos = (type == 2 && (fc & 0x0080)) ? 1 : 0;
    size_t header_len = 24;
    if (type == 0 && (to_ds || from_ds))
        return L80211_ENOTSUP;
    if (type == 2 && to_ds && from_ds)
        header_len += 6;
    if (qos)
        header_len += 2;
    if (fc & 0x8000) {
        if (!qos)
            return L80211_ENOTSUP;
        header_len += 4; /* HT control on QoS frames with Order set. */
    }
    if (length < header_len)
        return L80211_EINVAL;
    out->type = type;
    out->subtype = (fc >> 4) & 15;
    out->to_ds = to_ds;
    out->from_ds = from_ds;
    out->protected_frame = (fc >> 14) & 1;
    out->qos = qos;
    out->header_len = header_len;
    for (unsigned i = 0; i < 6; ++i)
        out->bssid[i] = 0;
    if (type == 0 || (!to_ds && !from_ds)) {
        copy6(out->destination, frame + 4);
        copy6(out->source, frame + 10);
        copy6(out->bssid, frame + 16);
    } else if (to_ds && !from_ds) {
        copy6(out->destination, frame + 16);
        copy6(out->source, frame + 10);
        copy6(out->bssid, frame + 4);
    } else if (!to_ds && from_ds) {
        copy6(out->destination, frame + 4);
        copy6(out->source, frame + 16);
        copy6(out->bssid, frame + 10);
    } else {
        copy6(out->destination, frame + 16);
        copy6(out->source, frame + 24);
    }
    return L80211_OK;
}

int luna80211_bss(const uint8_t *frame, size_t length, Luna80211Bss *out) {
    if (!out)
        return L80211_EINVAL;
    Luna80211Header h;
    int rc = luna80211_header(frame, length, &h);
    if (rc != L80211_OK)
        return rc;
    if (h.type != 0 || (h.subtype != 8 && h.subtype != 5))
        return L80211_ENOTSUP;
    if (length - h.header_len < 12)
        return L80211_EINVAL;
    Luna80211Bss b = {0};
    copy6(b.bssid, h.bssid);
    b.capability = le16(frame + h.header_len + 10);
    size_t pos = h.header_len + 12;
    int found = 0;
    while (pos < length) {
        if (length - pos < 2)
            return L80211_EINVAL;
        uint8_t id = frame[pos++], n = frame[pos++];
        if ((size_t)n > length - pos)
            return L80211_EINVAL;
        if (id == 0) {
            if (found || n > 32)
                return L80211_EINVAL;
            found = 1;
            b.ssid_len = n;
            for (unsigned i = 0; i < n; ++i)
                b.ssid[i] = frame[pos + i];
        } else if (id == 3 && n == 1) {
            b.channel = frame[pos];
        }
        pos += n;
    }
    if (!found)
        return L80211_ENOENT;
    *out = b;
    return L80211_OK;
}

int luna80211_to_ethernet(const uint8_t *frame, size_t length,
                         uint8_t *ethernet, size_t capacity, size_t *written) {
    if (!frame || !ethernet || !written)
        return L80211_EINVAL;
    *written = 0;
    Luna80211Header h;
    int rc = luna80211_header(frame, length, &h);
    if (rc != L80211_OK)
        return rc;
    if (h.type != 2 || (h.subtype & 4) || h.protected_frame)
        return L80211_ENOTSUP;
    if (length - h.header_len < 8)
        return L80211_EINVAL;
    const uint8_t *llc = frame + h.header_len;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03 ||
        llc[3] != 0 || llc[4] != 0 || llc[5] != 0)
        return L80211_ENOTSUP;
    size_t payload = length - h.header_len - 8;
    if (payload > capacity || capacity - payload < 14)
        return L80211_ENOSPC;
    copy6(ethernet, h.destination);
    copy6(ethernet + 6, h.source);
    ethernet[12] = llc[6];
    ethernet[13] = llc[7];
    for (size_t i = 0; i < payload; ++i)
        ethernet[14 + i] = llc[8 + i];
    *written = payload + 14;
    return L80211_OK;
}

int luna80211_from_ethernet(const uint8_t *ethernet, size_t length,
                           const uint8_t bssid[6], uint16_t sequence,
                           uint8_t *frame, size_t capacity, size_t *written) {
    if (!ethernet || !bssid || !frame || !written || length < 14 || sequence > 4095)
        return L80211_EINVAL;
    *written = 0;
    size_t payload = length - 14;
    if (payload > capacity || capacity - payload < 32)
        return L80211_ENOSPC;
    for (unsigned i = 0; i < 32; ++i)
        frame[i] = 0;
    frame[0] = 0x08; /* Data. */
    frame[1] = 0x01; /* ToDS, station -> AP. */
    copy6(frame + 4, bssid);          /* receiver */
    copy6(frame + 10, ethernet + 6); /* transmitter */
    copy6(frame + 16, ethernet);     /* final destination */
    uint16_t sc = (uint16_t)(sequence << 4);
    frame[22] = (uint8_t)sc;
    frame[23] = (uint8_t)(sc >> 8);
    frame[24] = 0xaa;
    frame[25] = 0xaa;
    frame[26] = 0x03;
    frame[30] = ethernet[12];
    frame[31] = ethernet[13];
    for (size_t i = 0; i < payload; ++i)
        frame[32 + i] = ethernet[14 + i];
    *written = payload + 32;
    return L80211_OK;
}
