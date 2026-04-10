#include "delivery_report_handler.h"
#include "sms_codec.h"

#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>
#endif

void DeliveryReportHandler::onStatusReport(const String &pduHex)
{
    sms_codec::StatusReport report;
    if (!sms_codec::parseStatusReportPdu(pduHex, report))
    {
        Serial.print("DeliveryReportHandler: failed to parse status report PDU: ");
        Serial.println(pduHex);
        return;
    }

    Serial.print("DeliveryReportHandler: MR=");
    Serial.print((int)report.messageRef);
    Serial.print(" status=");
    Serial.print(report.statusText);
    Serial.print(" delivered=");
    Serial.println(report.delivered ? "yes" : "no");

    String phone;
    uint32_t nowMs = clock_();
    if (!map_.lookup(report.messageRef, phone, nowMs))
    {
        Serial.print("DeliveryReportHandler: MR=");
        Serial.print((int)report.messageRef);
        Serial.println(" not found in map (expired or unknown); ignoring");
        return;
    }

    // Build Telegram notification message.
    String msg;
    if (report.delivered)
    {
        msg = String("Delivered to ") + phone;
    }
    else if (report.status >= 0x20 && report.status <= 0x2F)
    {
        // Temporary failure, SMSC still trying — informational.
        msg = String("Delivery in progress to ") + phone
            + String(" (") + report.statusText + String(")");
    }
    else
    {
        // Permanent failure or stopped trying.
        msg = String("Delivery failed to ") + phone
            + String(": ") + report.statusText;
    }

#ifdef ESP_PLATFORM
    esp_task_wdt_reset(); // bot_.sendMessage() can block ~23 s; kick WDT so
                          // multiple back-to-back delivery reports don't time out
#endif
    if (!bot_.sendMessage(msg))
    {
        Serial.println("DeliveryReportHandler: failed to send Telegram notification");
    }
}
