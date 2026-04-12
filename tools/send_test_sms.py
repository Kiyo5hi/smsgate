#!/usr/bin/env python3
"""
Send a test SMS to the device via Infobip to trigger the CMTI receive pipeline.

Setup:
  1. Register at https://www.infobip.com (free trial ~$20 credit)
  2. Grab your Base URL and API Key from the portal
  3. Fill in the three variables below, or set them as env vars

Usage:
  python tools/send_test_sms.py
  python tools/send_test_sms.py "custom message body"
"""

import os
import sys
import json
import urllib.request
import urllib.error

# ── Config ────────────────────────────────────────────────────────────────────
BASE_URL = os.environ.get("INFOBIP_BASE_URL", "FILL_IN.api.infobip.com")
API_KEY  = os.environ.get("INFOBIP_API_KEY",  "FILL_IN_YOUR_API_KEY")
TO_NUMBER = os.environ.get("DEVICE_PHONE",    "+86XXXXXXXXXXX")   # device SIM number
# ──────────────────────────────────────────────────────────────────────────────

BODY = sys.argv[1] if len(sys.argv) > 1 else "smsgate test 123"

payload = json.dumps({
    "messages": [{
        "destinations": [{"to": TO_NUMBER}],
        "from": "InfoSMS",
        "text": BODY,
    }]
}).encode()

req = urllib.request.Request(
    url=f"https://{BASE_URL}/sms/2/text/advanced",
    data=payload,
    headers={
        "Authorization": f"App {API_KEY}",
        "Content-Type": "application/json",
        "Accept": "application/json",
    },
    method="POST",
)

try:
    with urllib.request.urlopen(req, timeout=15) as resp:
        result = json.loads(resp.read())
        msg = result["messages"][0]
        status = msg["status"]["groupName"]
        msg_id = msg["messageId"]
        print(f"Sent: {status}  id={msg_id}")
        print(f"Body: {BODY!r}")
        if status not in ("PENDING", "SENT"):
            print(f"Warning: unexpected status — {msg['status']}")
            sys.exit(1)
except urllib.error.HTTPError as e:
    print(f"HTTP {e.code}: {e.read().decode()}", file=sys.stderr)
    sys.exit(1)
