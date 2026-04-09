#include "utils.h"
#include <Arduino.h>

String humanReadablePhoneNumber(String number)
{
    // xxxxxxxxxxx -> +86 xxx-xxxx-xxxx
    // +86xxxxxxxxxxx -> +86 xxx-xxxx-xxxx
    // other length -> unchanged

    if (number.length() == 11)
    {
        return "+86 " + number.substring(0, 3) + "-" + number.substring(3, 7) + "-" + number.substring(7);
    }
    else if (number.length() == 13 && number.startsWith("+86"))
    {
        return "+86 " + number.substring(3, 6) + "-" + number.substring(6, 10) + "-" + number.substring(10);
    }
    else
    {
        return number;
    }
}

String timestampToRFC3339(String timestamp)
{
    // Input format: "yy/MM/dd,HH:mm:ss+zz"
    // Output format: "YYYY-MM-DDTHH:mm:ssZ"
    if (timestamp.length() < 17)
        return "";

    String year = "20" + timestamp.substring(0, 2);
    String month = timestamp.substring(3, 5);
    String day = timestamp.substring(6, 8);
    String hour = timestamp.substring(9, 11);
    String minute = timestamp.substring(12, 14);
    String second = timestamp.substring(15, 17);

    return year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second + "+08:00";
}

bool isHexString(const String &s)
{
    // True iff s is non-empty, even-length, and all chars are [0-9A-Fa-f].
    // Used to guard against feeding plain ASCII (GSM 7bit text) into the UCS2 decoder.
    if (s.length() == 0 || (s.length() % 2) != 0)
        return false;
    for (size_t i = 0; i < s.length(); ++i)
    {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok)
            return false;
    }
    return true;
}
