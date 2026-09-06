# Handoff

Last reviewed: 2026-09-05. This is a repository/work record, not a live device
health report. Read git status and history again when resuming.

## Current Work

Base HEAD at review: 35b74c1 (log pages reduced to five entries).
Three pre-existing uncommitted firmware files contain SMS recovery work:

- src/bridge/sms_handler.rs: check/restore CNMI; explicitly select sweep storage.
- src/main.rs: run recovery every five minutes, defer while direct CMT PDU pending.
- tests/test_sms_handler.rs: notification checks and storage selection expectations.

These changes are not available to another clone until committed and pushed.
Documentation reorganization is separate work; preserve the firmware diff.

## Historical Verification

The July 26, 2026 session (America/Los_Angeles) recorded an R2/4MB build and
flash on then-COM5 from 35b74c1 plus the dirty changes above:

- Host tests passed with -j 1 after Windows parallel linking failed.
- Clippy and the R2 release build passed.
- Serial capture showed SIM ready, network registered, CNMI=2,1,0,0,0,
  ME empty, WiFi connected and smsgate ready.
- At approximately 326 seconds uptime, CNMI OK and an empty ME sweep were logged.

Evidence source: conversation transcript. No redacted capture artifact or
firmware SHA-256 was preserved here. These results have not been repeated
on September 5 and do not establish current board connectivity.

## Outstanding

- External inbound SMS and /status acceptance for the recovery build.
- Real-hardware recovery from deliberately missing CMTI / changed CNMI.
- Review and commit/push the firmware change when ready.
- Create private device records from confirmed identities; do not infer them
  from old chat IDs, phone numbers or port labels.
- Update this record with the resulting commit and evidence when work advances.

## Maintaining This File

Record date, base commit, dirty files, implemented changes, evidence, gaps and
next actions. Move durable conclusions to their owning docs. Do not store
credentials, raw SMS, phone numbers or real chat IDs here.
