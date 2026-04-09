#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "ipersist.h"

// Production IPersist adapter, backed by ESP32 NVS via the
// arduino-esp32 Preferences library. Header-only — only included
// from main.cpp, never from anything that the native test env
// compiles. NOT compiled in [env:native].
//
// We use a single namespace "tgsms" with two keys:
//   "uid"  -> int32_t last update_id (RFC-0003 §1)
//   "rtm"  -> blob of serialized ReplyTargetMap slot table
//
// NVS write amplification note: the reply-target blob is ~5 KB
// (200 slots * ~24 bytes). Preferences writes go through wear
// levelling and the ESP32 NVS sector is 4 KB, so a full blob
// rewrite costs 1-2 sector erases per SMS forward. That's fine
// for the expected traffic (a few SMS per day in typical use).
class RealPersist : public IPersist
{
public:
    bool begin()
    {
        return prefs_.begin("tgsms", false);
    }

    int32_t loadLastUpdateId() override
    {
        return prefs_.getInt("uid", 0);
    }

    void saveLastUpdateId(int32_t id) override
    {
        prefs_.putInt("uid", id);
    }

    size_t loadReplyTargets(void *buf, size_t bufSize) override
    {
        size_t storedLen = prefs_.getBytesLength("rtm");
        if (storedLen == 0)
        {
            return 0;
        }
        if (storedLen > bufSize)
        {
            // Stored blob is bigger than the in-RAM buffer — likely a
            // ReplyTargetMap version mismatch. Drop it.
            return 0;
        }
        return prefs_.getBytes("rtm", buf, bufSize);
    }

    void saveReplyTargets(const void *buf, size_t bufSize) override
    {
        prefs_.putBytes("rtm", buf, bufSize);
    }

private:
    Preferences prefs_;
};
