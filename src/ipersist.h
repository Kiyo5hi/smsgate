#pragma once

#include <Arduino.h>
#include <stdint.h>

// Narrow persistence interface for the TG -> SMS pipeline (RFC-0003).
// Wraps just the operations TelegramPoller and the reply-target ring
// buffer need: an int32 watermark for the last-seen Telegram update_id,
// and an opaque blob for the ring buffer contents.
//
// Real implementation: src/real_persist.h, backed by ESP32 NVS via
// the arduino-esp32 Preferences library, with namespace "tgsms".
//
// Fake implementation: test/support/fake_persist.h, in-memory.
//
// Lives in its own header (rather than reusing imodem.h or
// ibot_client.h) so it's clear that this is durable state, not
// per-loop hardware state.
class IPersist
{
public:
    virtual ~IPersist() = default;

    // Last seen Telegram update_id (the watermark we pass as
    // `offset = lastUpdateId + 1` on the next getUpdates call).
    // Returns 0 if no value has ever been written.
    virtual int32_t loadLastUpdateId() = 0;
    virtual void saveLastUpdateId(int32_t id) = 0;

    // Reply-target ring buffer storage. The buffer is a fixed-size
    // blob — one slot per entry, slots indexed by `message_id % N`
    // where N = ReplyTargetMap::kSlotCount. The TelegramPoller and
    // SmsHandler both go through ReplyTargetMap, which serializes
    // and deserializes the slot table; this interface only sees
    // raw bytes.
    //
    // loadReplyTargets fills `buf` with up to `bufSize` bytes from
    // NVS and returns the number of bytes actually read (0 if no
    // value has ever been stored).
    virtual size_t loadReplyTargets(void *buf, size_t bufSize) = 0;
    virtual void saveReplyTargets(const void *buf, size_t bufSize) = 0;

    // Generic NVS blob load/save by key name (max 15 chars).
    // loadBlob returns bytes actually read (0 if key absent).
    virtual size_t loadBlob(const char *key, void *buf, size_t bufSize) = 0;
    virtual void   saveBlob(const char *key, const void *buf, size_t bufSize) = 0;
};
