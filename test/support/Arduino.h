#pragma once

// Hand-rolled minimal Arduino subset for the native (host) test env.
//
// This file is a stand-in for the real `Arduino.h` so `sms_codec` and
// `sms_handler` can be compiled and linked on the host without pulling
// in the arduino-esp32 framework. It deliberately exposes ONLY what
// those two modules (and their tests) need.
//
// Subset currently implemented:
//
//   String
//     - default ctor, ctor from const char*, ctor from int, copy ctor
//     - length()
//     - operator[] (read only)
//     - operator+=(char)
//     - operator+=(const String&)
//     - operator+=(const char*)
//     - operator==(const String&)
//     - operator==(const char*)
//     - trim()
//     - substring(unsigned)
//     - substring(unsigned, unsigned)
//     - startsWith(const String&)
//     - startsWith(const char*)
//     - indexOf(char)
//     - indexOf(char, unsigned)
//     - indexOf(const char*)
//     - indexOf(const char*, unsigned)
//     - indexOf(const String&)
//     - indexOf(const String&, unsigned)
//     - toInt()
//     - toLowerCase()
//     - c_str() (convenience for asserts)
//
//   Free operator+: String+String, String+const char*, const char*+String
//
//   SerialStub + global `Serial` — no-op print/println overloads so
//   sms_handler.cpp's Serial.print* calls compile on host.
//
//   millis(), delay() — no-ops returning 0 / doing nothing. The test
//   code never relies on them.
//
//   PROGMEM, F() — defined away to nothing.
//
// If you extend sms_codec or sms_handler and hit a missing method,
// ADD it here rather than pulling in a real Arduino-core shim. The
// whole point of this stub is hermetic host tests.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef F
#define F(x) (x)
#endif

class String
{
public:
    String() = default;
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(int v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(const String &) = default;
    String(String &&) = default;
    String &operator=(const String &) = default;
    String &operator=(String &&) = default;

    unsigned int length() const { return (unsigned int)s_.size(); }

    char operator[](unsigned int i) const
    {
        if (i >= s_.size())
            return '\0';
        return s_[i];
    }

    String &operator+=(char c)
    {
        s_.push_back(c);
        return *this;
    }

    String &operator+=(const String &o)
    {
        s_ += o.s_;
        return *this;
    }

    String &operator+=(const char *o)
    {
        if (o)
            s_ += o;
        return *this;
    }

    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const { return o && s_ == o; }
    bool operator!=(const String &o) const { return !(*this == o); }
    bool operator!=(const char *o) const { return !(*this == o); }

    void trim()
    {
        size_t start = 0;
        while (start < s_.size() && isspaceChar(s_[start]))
            ++start;
        size_t end = s_.size();
        while (end > start && isspaceChar(s_[end - 1]))
            --end;
        s_ = s_.substr(start, end - start);
    }

    String substring(unsigned int from) const
    {
        if (from >= s_.size())
            return String();
        return String(s_.substr(from));
    }

    String substring(unsigned int from, unsigned int to) const
    {
        if (from >= s_.size() || to <= from)
            return String();
        if (to > s_.size())
            to = (unsigned int)s_.size();
        return String(s_.substr(from, to - from));
    }

    bool startsWith(const String &prefix) const
    {
        if (prefix.s_.size() > s_.size())
            return false;
        return s_.compare(0, prefix.s_.size(), prefix.s_) == 0;
    }

    bool startsWith(const char *prefix) const
    {
        return startsWith(String(prefix));
    }

    int indexOf(char c) const
    {
        size_t p = s_.find(c);
        return p == std::string::npos ? -1 : (int)p;
    }

    int indexOf(char c, unsigned int from) const
    {
        if (from >= s_.size())
            return -1;
        size_t p = s_.find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }

    int indexOf(const char *needle) const
    {
        size_t p = s_.find(needle ? needle : "");
        return p == std::string::npos ? -1 : (int)p;
    }

    int indexOf(const char *needle, unsigned int from) const
    {
        if (from > s_.size())
            return -1;
        size_t p = s_.find(needle ? needle : "", from);
        return p == std::string::npos ? -1 : (int)p;
    }

    int indexOf(const String &needle) const
    {
        size_t p = s_.find(needle.s_);
        return p == std::string::npos ? -1 : (int)p;
    }

    int indexOf(const String &needle, unsigned int from) const
    {
        if (from > s_.size())
            return -1;
        size_t p = s_.find(needle.s_, from);
        return p == std::string::npos ? -1 : (int)p;
    }

    int toInt() const
    {
        if (s_.empty())
            return 0;
        return (int)std::strtol(s_.c_str(), nullptr, 10);
    }

    void toLowerCase()
    {
        for (auto &c : s_)
        {
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
    }

    const char *c_str() const { return s_.c_str(); }
    const std::string &std_str() const { return s_; }

private:
    static bool isspaceChar(char c)
    {
        return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\v' || c == '\f';
    }

    std::string s_;
    friend String operator+(const String &a, const String &b);
    friend String operator+(const String &a, const char *b);
    friend String operator+(const char *a, const String &b);
    friend String operator+(const String &a, char b);
};

inline String operator+(const String &a, const String &b)
{
    String out(a);
    out += b;
    return out;
}

inline String operator+(const String &a, const char *b)
{
    String out(a);
    out += b;
    return out;
}

inline String operator+(const char *a, const String &b)
{
    String out(a);
    out += b;
    return out;
}

inline String operator+(const String &a, char b)
{
    String out(a);
    out += b;
    return out;
}

// ---------- Serial stub (no-op) ----------

class SerialStub
{
public:
    void begin(unsigned long) {}
    void print(const char *) {}
    void print(const String &) {}
    void print(char) {}
    void print(int) {}
    void print(long) {}
    void print(unsigned) {}
    void print(unsigned long) {}
    void println() {}
    void println(const char *) {}
    void println(const String &) {}
    void println(char) {}
    void println(int) {}
    void println(long) {}
    void println(unsigned) {}
    void println(unsigned long) {}
    template <typename... Args>
    void printf(const char *, Args...) {}
};

extern SerialStub Serial;

// ---------- Misc helpers ----------

inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
