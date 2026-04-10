#include "reply_target_map.h"

#include <cstring>
#include <memory>

ReplyTargetMap::ReplyTargetMap(IPersist &persist)
    : persist_(persist)
{
    // Constructor leaves the in-RAM table empty. Call load() once
    // after construction (typically from main.cpp's setup() right
    // after the persist begin() succeeds).
}

void ReplyTargetMap::load()
{
    // Wipe the in-RAM table first.
    for (size_t i = 0; i < kSlotCount; ++i)
    {
        slots_[i].messageId = 0;
        slots_[i].phone[0] = '\0';
    }

    // RFC-0268: heap-allocate; 5608 bytes would stress the 8192-byte
    // loop task stack when save() is called from the SMS forward path.
    auto bufOwn = std::make_unique<uint8_t[]>(kBlobSize);
    uint8_t *buf = bufOwn.get();
    std::memset(buf, 0, kBlobSize);
    size_t got = persist_.loadReplyTargets(buf, kBlobSize);
    if (got == 0)
    {
        // Nothing stored yet — fresh table.
        return;
    }
    if (got < sizeof(Header))
    {
        Serial.println("ReplyTargetMap: stored blob too small, dropping");
        return;
    }

    Header h;
    std::memcpy(&h, buf, sizeof(h));
    if (h.version != kFormatVersion ||
        h.slotCount != (uint16_t)kSlotCount ||
        h.phoneMax != (uint16_t)kPhoneMax)
    {
        Serial.print("ReplyTargetMap: stored blob format mismatch (v=");
        Serial.print((int)h.version);
        Serial.print(" slots=");
        Serial.print((int)h.slotCount);
        Serial.print(" phoneMax=");
        Serial.print((int)h.phoneMax);
        Serial.println("), dropping");
        return;
    }
    if (got < kBlobSize)
    {
        Serial.println("ReplyTargetMap: stored blob truncated, dropping");
        return;
    }

    std::memcpy(slots_, buf + sizeof(Header), sizeof(slots_));
    // Defensive: any slot with non-NUL-terminated phone gets cleared.
    for (size_t i = 0; i < kSlotCount; ++i)
    {
        if (slots_[i].messageId == 0)
        {
            slots_[i].phone[0] = '\0';
            continue;
        }
        bool terminated = false;
        for (size_t j = 0; j < kPhoneMax; ++j)
        {
            if (slots_[i].phone[j] == '\0')
            {
                terminated = true;
                break;
            }
        }
        if (!terminated)
        {
            slots_[i].messageId = 0;
            slots_[i].phone[0] = '\0';
        }
    }
}

void ReplyTargetMap::save()
{
    // RFC-0268: heap-allocate; 5608 bytes would stress the 8192-byte
    // loop task stack when save() is called from the SMS forward path.
    auto bufOwn = std::make_unique<uint8_t[]>(kBlobSize);
    uint8_t *buf = bufOwn.get();
    std::memset(buf, 0, kBlobSize);

    Header h;
    h.version = kFormatVersion;
    h.slotCount = (uint16_t)kSlotCount;
    h.phoneMax = (uint16_t)kPhoneMax;
    h.reserved = 0;
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(Header), slots_, sizeof(slots_));

    persist_.saveReplyTargets(buf, kBlobSize);
}

void ReplyTargetMap::put(int32_t messageId, const String &phone)
{
    if (messageId <= 0)
    {
        return; // ignore sentinel ids
    }
    size_t slot = (size_t)((uint32_t)messageId % kSlotCount);
    slots_[slot].messageId = messageId;
    // Copy phone, truncating if necessary.
    size_t plen = (size_t)phone.length();
    if (plen >= kPhoneMax)
    {
        Serial.print("ReplyTargetMap: phone truncated for msg=");
        Serial.println(messageId);
        plen = kPhoneMax - 1;
    }
    for (size_t i = 0; i < plen; ++i)
    {
        slots_[slot].phone[i] = phone[i];
    }
    slots_[slot].phone[plen] = '\0';
    save();
}

bool ReplyTargetMap::lookup(int32_t messageId, String &outPhone) const
{
    if (messageId <= 0)
    {
        return false;
    }
    size_t slot = (size_t)((uint32_t)messageId % kSlotCount);
    if (slots_[slot].messageId != messageId)
    {
        return false;
    }
    if (slots_[slot].phone[0] == '\0')
    {
        return false;
    }
    outPhone = String(slots_[slot].phone);
    return true;
}

int32_t ReplyTargetMap::slotMessageIdFor(int32_t messageId) const
{
    if (messageId <= 0)
    {
        return 0;
    }
    size_t slot = (size_t)((uint32_t)messageId % kSlotCount);
    return slots_[slot].messageId;
}

size_t ReplyTargetMap::occupiedSlots() const
{
    size_t n = 0;
    for (size_t i = 0; i < kSlotCount; ++i)
    {
        if (slots_[i].messageId != 0)
        {
            ++n;
        }
    }
    return n;
}
