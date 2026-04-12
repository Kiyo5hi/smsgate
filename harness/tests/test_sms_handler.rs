//! Tests for bridge::sms_handler — CMTI processing and boot-time sweep.

use harness::mocks::{RecordingMessenger, ScriptedModem};
use smsgate::bridge::reply_router::ReplyRouter;
use smsgate::bridge::sms_handler::{handle_new_sms, process_pdu_hex, sweep_one_storage};
use smsgate::log_ring::LogRing;
use smsgate::persist::mem::MemStore;
use smsgate::sms::concat::ConcatReassembler;

// Minimal valid SMS-DELIVER PDU: sender "+8613800138000", body "Hello"
// SCA=00, FO=04, OA=0D918136001380F0, PID=00, DCS=00, SCTS=..., UDL=05, UD=C8329BFD06
const HELLO_PDU: &str = "00040D91683108108300F0000062400110000000 05C8329BFD06";

fn pdu(hex: &str) -> String {
    hex.chars().filter(|c| !c.is_whitespace()).collect()
}

// ---------------------------------------------------------------------------
// process_pdu_hex
// ---------------------------------------------------------------------------

#[test]
fn process_pdu_hex_forwards_valid_sms() {
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    let result = process_pdu_hex(
        &pdu(HELLO_PDU), 1,
        &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    assert!(result, "valid PDU should return true (delete slot)");
    assert_eq!(messenger.sent_count(), 1);
    assert!(messenger.contains_sent("Hello"));
}

#[test]
fn process_pdu_hex_invalid_hex_returns_true() {
    // Unparseable PDU → delete slot (no point retaining garbage)
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    let result = process_pdu_hex(
        "DEADBEEF", 5,
        &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    assert!(result, "unparseable PDU should return true (delete slot)");
    assert_eq!(messenger.sent_count(), 0); // nothing forwarded
}

#[test]
fn process_pdu_hex_records_log_entry() {
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    process_pdu_hex(
        &pdu(HELLO_PDU), 1,
        &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    assert_eq!(log.len(), 1);
    assert!(log.last_n(1)[0].forwarded);
}

// ---------------------------------------------------------------------------
// handle_new_sms
// ---------------------------------------------------------------------------

#[test]
fn handle_new_sms_reads_and_forwards() {
    // AT+CPMS="ME" → OK
    // AT+CMGR=1 → "+CMGR: 0,,18\n<pdu>" OK
    // (after forward) AT+CMGD=1 → OK
    let modem = ScriptedModem::new()
        .expect("+CPMS=\"ME\"", "", true)
        .expect("+CMGR=1", &format!("+CMGR: 0,,18\n{}", pdu(HELLO_PDU)), true)
        .expect("+CMGD=1", "", true);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    handle_new_sms(
        "ME", 1,
        &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 1);
    assert!(messenger.contains_sent("Hello"));
}

#[test]
fn handle_new_sms_cmgr_error_does_not_forward() {
    // AT+CPMS="ME" → OK, AT+CMGR=2 → ERROR
    let modem = ScriptedModem::new()
        .expect("+CPMS=\"ME\"", "", true)
        .expect("+CMGR=2", "+CMS ERROR: 321", false);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    handle_new_sms(
        "ME", 2,
        &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 0);
}

#[test]
fn handle_new_sms_invalid_pdu_deletes_slot() {
    // Unparseable PDU → should still delete the slot
    let modem = ScriptedModem::new()
        .expect("+CPMS=\"ME\"", "", true)
        .expect("+CMGR=3", "+CMGR: 0,,2\nDEAD", true)
        .expect("+CMGD=3", "", true);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    handle_new_sms(
        "ME", 3,
        &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed(); // CMGD must have been called
    assert_eq!(messenger.sent_count(), 0);
}

// ---------------------------------------------------------------------------
// sweep_one_storage
// ---------------------------------------------------------------------------

#[test]
fn sweep_empty_storage_no_forwards() {
    // AT+CMGL=4 returns OK with empty body
    let modem = ScriptedModem::new()
        .expect("+CMGL=4", "", true);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    sweep_one_storage(
        "ME", &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 0);
}

#[test]
fn sweep_finds_and_forwards_sms() {
    // AT+CMGL=4 returns one entry at slot 1
    let cmgl_body = format!("+CMGL: 1,0,,18\n{}", pdu(HELLO_PDU));
    let modem = ScriptedModem::new()
        .expect("+CMGL=4", &cmgl_body, true)
        .expect("+CMGD=1", "", true); // deleted after forwarding

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    sweep_one_storage(
        "ME", &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 1);
    assert!(messenger.contains_sent("Hello"));
}

#[test]
fn sweep_multiple_sms() {
    // Two SMS in storage
    let cmgl_body = format!(
        "+CMGL: 1,0,,18\n{}\n+CMGL: 2,0,,18\n{}",
        pdu(HELLO_PDU), pdu(HELLO_PDU)
    );
    let modem = ScriptedModem::new()
        .expect("+CMGL=4", &cmgl_body, true)
        .expect("+CMGD=1", "", true)
        .expect("+CMGD=2", "", true);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    sweep_one_storage(
        "ME", &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 2);
}

#[test]
fn sweep_cmgl_error_is_silent() {
    // AT+CMGL=4 returns error (e.g. storage not supported)
    let modem = ScriptedModem::new()
        .expect("+CMGL=4", "+CMS ERROR: 302", false);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    sweep_one_storage(
        "SM", &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 0);
}

#[test]
fn sweep_invalid_pdu_deletes_slot() {
    // Bad PDU at slot 7 — should delete but not forward
    let cmgl_body = "+CMGL: 7,0,,4\nDEAD";
    let modem = ScriptedModem::new()
        .expect("+CMGL=4", cmgl_body, true)
        .expect("+CMGD=7", "", true);

    let mut modem = modem;
    let mut router = ReplyRouter::new();
    let mut log = LogRing::new();
    let mut concat = ConcatReassembler::new();
    let mut messenger = RecordingMessenger::new();
    let mut store = MemStore::new();

    sweep_one_storage(
        "ME", &mut modem, &mut router, &mut log, &mut concat,
        &mut messenger, &mut store,
    );

    modem.check_consumed();
    assert_eq!(messenger.sent_count(), 0);
}
