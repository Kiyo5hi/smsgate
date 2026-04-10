#pragma once

#include <Arduino.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ipersist.h"

// In-memory IPersist for native tests. Survives across multiple poller
// instantiations within a single test (since the test owns the
// FakePersist object), so we can simulate a "restart" by destroying
// and recreating the TelegramPoller while keeping the FakePersist
// untouched.
class FakePersist : public IPersist
{
public:
    int32_t loadLastUpdateId() override
    {
        return lastUpdateId_;
    }

    void saveLastUpdateId(int32_t id) override
    {
        lastUpdateId_ = id;
        saveLastUpdateIdCalls_++;
    }

    size_t loadReplyTargets(void *buf, size_t bufSize) override
    {
        if (replyTargets_.empty())
        {
            return 0;
        }
        size_t n = replyTargets_.size();
        if (n > bufSize)
        {
            return 0;
        }
        std::memcpy(buf, replyTargets_.data(), n);
        return n;
    }

    void saveReplyTargets(const void *buf, size_t bufSize) override
    {
        replyTargets_.assign(static_cast<const uint8_t *>(buf),
                             static_cast<const uint8_t *>(buf) + bufSize);
        saveReplyTargetsCalls_++;
    }

    size_t loadBlob(const char *key, void *buf, size_t bufSize) override
    {
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return 0;
        size_t n = std::min(bufSize, it->second.size());
        std::memcpy(buf, it->second.data(), n);
        return n;
    }

    void saveBlob(const char *key, const void *buf, size_t bufSize) override
    {
        blobs_[key] = std::vector<uint8_t>(
            static_cast<const uint8_t *>(buf),
            static_cast<const uint8_t *>(buf) + bufSize);
    }

    // RFC-0184: Wipe all persisted data.
    void clearAll() override
    {
        lastUpdateId_ = 0;
        replyTargets_.clear();
        blobs_.clear();
        clearAllCalls_++;
    }

    // Test introspection
    int saveLastUpdateIdCalls() const { return saveLastUpdateIdCalls_; }
    int saveReplyTargetsCalls() const { return saveReplyTargetsCalls_; }
    int clearAllCalls()         const { return clearAllCalls_; }

private:
    int32_t lastUpdateId_ = 0;
    std::vector<uint8_t> replyTargets_;
    std::map<std::string, std::vector<uint8_t>> blobs_;
    int saveLastUpdateIdCalls_ = 0;
    int saveReplyTargetsCalls_ = 0;
    int clearAllCalls_         = 0;
};
