#pragma once

#include <Arduino.h>
#include <functional>

#include "ibot_client.h"
#include "delivery_report_map.h"
#include "sms_codec.h"

// Handles incoming +CDS SMS-STATUS-REPORT PDUs (RFC-0011).
// Parses the PDU, looks up the TP-MR in the DeliveryReportMap, and
// posts a delivery notification to Telegram via IBotClient.
//
// Usage in the main loop():
//   if (waitingCdsPdu && pduLine received):
//       deliveryReportHandler.onStatusReport(pduLine);
//
// Only compiled / active when -DENABLE_DELIVERY_REPORTS is defined.
// If that flag is not set, this class is still defined but its
// onStatusReport() does nothing — guarded by #ifdef in main.cpp.
class DeliveryReportHandler
{
public:
    using ClockFn = std::function<uint32_t()>;

    DeliveryReportHandler(IBotClient &bot, DeliveryReportMap &map,
                          ClockFn clock)
        : bot_(bot), map_(map), clock_(clock)
    {
    }

    // Called when the second line of a +CDS URC has been read.
    // `pduHex` is the raw hex PDU from the modem.
    void onStatusReport(const String &pduHex);

private:
    IBotClient &bot_;
    DeliveryReportMap &map_;
    ClockFn clock_;
};
