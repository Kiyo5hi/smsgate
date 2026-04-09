#include "sms_handler.h"
#include "sms_codec.h"

SmsHandler::SmsHandler(IModem &modem, IBotClient &bot, RebootFn reboot)
    : modem_(modem), bot_(bot), reboot_(std::move(reboot))
{
}

void SmsHandler::handleSmsIndex(int idx)
{
    Serial.print("-------- SMS @ index ");
    Serial.print(idx);
    Serial.println(" --------");

    String raw;
    modem_.sendAT("+CMGR=" + String(idx));
    int8_t res = modem_.waitResponse(5000UL, raw);
    if (res != 1)
    {
        Serial.println("CMGR failed");
        return;
    }

    String sender, timestamp, content;
    if (!sms_codec::parseCmgrBody(raw, sender, timestamp, content))
    {
        Serial.println("Unable to parse CMGR body. Raw:");
        Serial.println(raw);
        // Nothing useful here; delete so we don't loop on a malformed slot.
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
        return;
    }

    Serial.print("Sender:    ");
    Serial.println(sender);
    Serial.print("Timestamp: ");
    Serial.println(timestamp);
    Serial.print("Content:   ");
    Serial.println(content);

    String formatted = sms_codec::humanReadablePhoneNumber(sender) + " | " +
                       sms_codec::timestampToRFC3339(timestamp) +
                       "\n-----\n" +
                       content;

    if (bot_.sendMessage(formatted))
    {
        consecutiveFailures_ = 0;
        Serial.println("Posted to Telegram OK, deleting SMS.");
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
    }
    else
    {
        consecutiveFailures_++;
        Serial.print("Post to Telegram FAILED (");
        Serial.print(consecutiveFailures_);
        Serial.println(" consecutive). Keeping SMS on SIM.");
        if (consecutiveFailures_ >= MAX_CONSECUTIVE_FAILURES)
        {
            Serial.println("Too many consecutive failures, rebooting to recover...");
            if (reboot_)
            {
                reboot_();
            }
        }
    }
}

void SmsHandler::sweepExistingSms()
{
    String data;
    modem_.sendAT("+CMGL=\"ALL\"");
    int8_t res = modem_.waitResponse(10000UL, data);
    if (res != 1)
    {
        Serial.println("Initial CMGL sweep failed");
        return;
    }

    int search = 0;
    while (true)
    {
        int start = data.indexOf("+CMGL:", search);
        if (start == -1)
            break;
        int colon = start + 6;
        int comma = data.indexOf(',', colon);
        if (comma == -1)
            break;
        int idx = data.substring(colon, comma).toInt();
        search = comma;
        if (idx > 0)
        {
            handleSmsIndex(idx);
        }
    }
}
