#include "sms_sender.h"

SmsSender::SmsSender(IModem &modem)
    : modem_(modem)
{
}

bool SmsSender::isAscii(const String &s)
{
    for (unsigned int i = 0; i < s.length(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == 0 || c > 0x7F)
        {
            return false;
        }
    }
    return true;
}

void SmsSender::restorePduMode()
{
    // Re-enter PDU mode after TinyGSM's sendSMS flipped us to text.
    // We don't actually care about the response — best-effort.
    modem_.sendAT("+CMGF=0");
    modem_.waitResponseOk(2000UL);
}

bool SmsSender::send(const String &number, const String &body)
{
    lastError_ = String();

    if (number.length() == 0)
    {
        lastError_ = "destination phone is empty";
        return false;
    }

    if (!isAscii(body))
    {
        lastError_ = "non-ASCII SMS replies not yet supported - needs RFC-0002 Unicode TX path";
        return false;
    }

    // The body must also fit in a single non-concat SMS for the first
    // cut. GSM-7 supports 160 chars per single PDU; TinyGSM's
    // sendSMS happily concatenates beyond that for some modems but
    // we don't currently rely on it. Hard-cap at 160.
    if (body.length() > 160)
    {
        lastError_ = "SMS reply too long (max 160 ASCII chars in first cut)";
        return false;
    }

    Serial.print("SmsSender: sending to ");
    Serial.print(number);
    Serial.print(" len=");
    Serial.println(body.length());

    bool ok = modem_.sendSMS(number, body);

    // Restore PDU mode regardless of success/failure — leaving the
    // modem in text mode would silently break the receive path.
    restorePduMode();

    if (!ok)
    {
        lastError_ = "modem rejected the outbound SMS";
        Serial.println("SmsSender: send FAILED");
        return false;
    }

    Serial.println("SmsSender: send OK");
    return true;
}
