#pragma once

// RFC-0227: SMS template store.
//
// Maps short template names (e.g. "omw") to SMS body texts so
// TelegramPoller can expand them in /tsend and /tschedule commands.
//
// Header-only; no hardware dependencies beyond String and IPersist.
// Persisted via IPersist as a compact binary blob (key: "sms_templates").

#include <Arduino.h>
#include <stdint.h>

#include "ipersist.h"

class SmsTemplateStore
{
public:
    static constexpr int kMaxTemplates   = 10;
    static constexpr int kMaxNameLen     = 20; // chars, not including NUL
    static constexpr int kMaxBodyLen     = 160; // chars, not including NUL

    explicit SmsTemplateStore(IPersist &persist) : persist_(persist) {}

    // Load from NVS. Call once at startup. Idempotent.
    void load()
    {
        Blob blob{};
        persist_.loadBlob("sms_templates", &blob, sizeof(blob));
        if (blob.magic != kMagic || blob.count < 0 || blob.count > kMaxTemplates)
        {
            count_ = 0;
            return;
        }
        count_ = blob.count;
        for (int i = 0; i < count_; i++)
            entries_[i] = blob.entries[i];
    }

    // Returns true if every character in `name` is [a-zA-Z0-9_-].
    static bool isValidName(const String &name)
    {
        if (name.length() == 0) return false;
        for (unsigned int i = 0; i < name.length(); i++)
        {
            char c = name[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    // Add or replace a template. Returns false if name is invalid, name or
    // body is too long, or the store is full and name is not already present.
    bool set(const String &name, const String &body)
    {
        if (!isValidName(name)) return false;
        if ((int)name.length() > kMaxNameLen) return false;
        if (body.length() == 0 || (int)body.length() > kMaxBodyLen) return false;
        // Check for existing entry to update.
        for (int i = 0; i < count_; i++)
        {
            if (nameMatch(name, String(entries_[i].name)))
            {
                strncpy(entries_[i].body, body.c_str(), kMaxBodyLen);
                entries_[i].body[kMaxBodyLen] = '\0';
                save();
                return true;
            }
        }
        if (count_ >= kMaxTemplates) return false;
        strncpy(entries_[count_].name, name.c_str(), kMaxNameLen);
        strncpy(entries_[count_].body, body.c_str(), kMaxBodyLen);
        entries_[count_].name[kMaxNameLen] = '\0';
        entries_[count_].body[kMaxBodyLen] = '\0';
        count_++;
        save();
        return true;
    }

    // Look up a template body by name. Returns empty string on miss.
    String get(const String &name) const
    {
        for (int i = 0; i < count_; i++)
            if (nameMatch(name, String(entries_[i].name)))
                return String(entries_[i].body);
        return String();
    }

    // Remove a template by name. Returns false if not found.
    bool remove(const String &name)
    {
        for (int i = 0; i < count_; i++)
        {
            if (nameMatch(name, String(entries_[i].name)))
            {
                for (int j = i; j < count_ - 1; j++)
                    entries_[j] = entries_[j + 1];
                memset(&entries_[count_ - 1], 0, sizeof(Entry));
                count_--;
                save();
                return true;
            }
        }
        return false;
    }

    // Remove all templates.
    void clear()
    {
        count_ = 0;
        for (auto &e : entries_) memset(&e, 0, sizeof(e));
        save();
    }

    // Returns a human-readable list of all templates (name → preview).
    String list() const
    {
        if (count_ == 0) return String("(no templates defined)");
        String out;
        for (int i = 0; i < count_; i++)
        {
            out += String(entries_[i].name);
            out += String(": ");
            String preview = String(entries_[i].body);
            if (preview.length() > 40) preview = preview.substring(0, 40) + String("...");
            out += preview;
            out += String("\n");
        }
        return out;
    }

    int count() const { return count_; }

private:
    // Case-insensitive ASCII comparison.
    static bool nameMatch(const String &a, const String &b)
    {
        if (a.length() != b.length()) return false;
        for (unsigned int i = 0; i < a.length(); i++) {
            char ca = a[i]; if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            char cb = b[i]; if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    }

    struct Entry {
        char name[kMaxNameLen + 1] = {};
        char body[kMaxBodyLen + 1] = {};
    };

    static constexpr uint32_t kMagic = 0x54504C54; // "TPLT"

    struct Blob {
        uint32_t magic = 0;
        int32_t  count = 0;
        Entry    entries[kMaxTemplates] = {};
    };

    void save()
    {
        Blob blob;
        blob.magic = kMagic;
        blob.count = count_;
        for (int i = 0; i < count_; i++)
            blob.entries[i] = entries_[i];
        persist_.saveBlob("sms_templates", &blob, sizeof(blob));
    }

    IPersist &persist_;
    Entry     entries_[kMaxTemplates] = {};
    int       count_ = 0;
};
