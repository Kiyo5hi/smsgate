#pragma once

// RFC-0088: Phone number alias store.
//
// Maps short alias names (e.g. "alice") to E.164 phone numbers so
// TelegramPoller can expand "@alice" in /send and /test commands.
//
// Header-only; no hardware dependencies beyond String and IPersist.
// Persisted via IPersist as a compact binary blob (key: "aliases").

#include <Arduino.h>
#include <functional>
#include <stdint.h>

#include "ipersist.h"

class SmsAliasStore
{
public:
    static constexpr int kMaxAliases      = 10;
    static constexpr int kMaxNameLen      = 16; // chars, not including NUL
    static constexpr int kMaxPhoneLen     = 21; // chars, not including NUL

    explicit SmsAliasStore(IPersist &persist) : persist_(persist) {}

    // Load from NVS. Call once at startup. Idempotent.
    void load()
    {
        Blob blob{};
        persist_.loadBlob("aliases", &blob, sizeof(blob));
        if (blob.magic != kMagic || blob.count < 0 || blob.count > kMaxAliases)
        {
            count_ = 0;
            return;
        }
        count_ = blob.count;
        for (int i = 0; i < count_; i++)
            entries_[i] = blob.entries[i];
    }

    // Add or replace an alias. Returns false if name or phone is too long
    // or the store is full and name is not already present.
    bool set(const String &name, const String &phone)
    {
        if (name.length() == 0 || (int)name.length() > kMaxNameLen) return false;
        if (phone.length() == 0 || (int)phone.length() > kMaxPhoneLen) return false;
        // Check for existing entry to update.
        for (int i = 0; i < count_; i++)
        {
            if (nameMatch(name, String(entries_[i].name)))
            {
                strncpy(entries_[i].phone, phone.c_str(), kMaxPhoneLen);
                entries_[i].phone[kMaxPhoneLen] = '\0';
                save();
                return true;
            }
        }
        if (count_ >= kMaxAliases) return false;
        strncpy(entries_[count_].name,  name.c_str(),  kMaxNameLen);
        strncpy(entries_[count_].phone, phone.c_str(), kMaxPhoneLen);
        entries_[count_].name[kMaxNameLen]   = '\0';
        entries_[count_].phone[kMaxPhoneLen] = '\0';
        count_++;
        save();
        return true;
    }

    // Remove an alias by name. Returns false if not found.
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

    // Look up a phone number by alias name. Returns empty string on miss.
    String lookup(const String &name) const
    {
        for (int i = 0; i < count_; i++)
            if (nameMatch(name, String(entries_[i].name)))
                return String(entries_[i].phone);
        return String();
    }

    // Returns a human-readable list of all aliases.
    String list() const
    {
        if (count_ == 0) return String("(no aliases defined)");
        String out;
        for (int i = 0; i < count_; i++)
        {
            out += String("@"); out += entries_[i].name;
            out += String(" \xe2\x86\x92 "); // →
            out += entries_[i].phone; out += "\n";
        }
        return out;
    }

    int count() const { return count_; }

    // Enumerate all entries. Callback receives (name, phone) pairs.
    void forEach(std::function<void(const String &name, const String &phone)> fn) const
    {
        for (int i = 0; i < count_; i++)
            fn(String(entries_[i].name), String(entries_[i].phone));
    }

private:
    // Case-insensitive ASCII comparison (avoids equalsIgnoreCase which is
    // unavailable in the host test stub).
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
        char name[kMaxNameLen + 1]  = {};
        char phone[kMaxPhoneLen + 1] = {};
    };

    static constexpr uint32_t kMagic = 0x414C4953; // "ALIS"

    struct Blob {
        uint32_t magic = 0;
        int32_t  count = 0;
        Entry    entries[kMaxAliases] = {};
    };

    void save()
    {
        Blob blob;
        blob.magic = kMagic;
        blob.count = count_;
        for (int i = 0; i < count_; i++)
            blob.entries[i] = entries_[i];
        persist_.saveBlob("aliases", &blob, sizeof(blob));
    }

    IPersist &persist_;
    Entry     entries_[kMaxAliases] = {};
    int       count_ = 0;
};
