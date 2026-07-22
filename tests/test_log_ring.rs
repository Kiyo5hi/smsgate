//! LogRing tests.

use smsgate::log_ring::{FlashLogRing, MemFlashLogStorage, FLASH_LOG_RECORD_SIZE};
use smsgate::log_ring::{LogEntry, LogKind, LogRing};

fn entry(sender: &str) -> LogEntry {
    LogEntry {
        kind: LogKind::Sms,
        sender: sender.to_string(),
        body_preview: "body".to_string(),
        timestamp: "ts".to_string(),
        forwarded: true,
    }
}

fn flash_entry(sender: &str, body: &str, forwarded: bool) -> LogEntry {
    LogEntry {
        kind: LogKind::Sms,
        sender: sender.to_string(),
        body_preview: body.to_string(),
        timestamp: "ts".to_string(),
        forwarded,
    }
}

#[test]
fn flash_log_survives_remount() {
    let storage = MemFlashLogStorage::new(4096, 4096);
    let mut flash = FlashLogRing::mount(storage).unwrap();
    flash.append(&flash_entry("+1", "persisted", true)).unwrap();
    let storage = flash.into_storage();
    let mut remounted = FlashLogRing::mount(storage).unwrap();
    assert_eq!(remounted.last_n(1).unwrap()[0].body_preview, "persisted");
}

#[test]
fn flash_log_ignores_corrupt_record_on_remount() {
    let storage = MemFlashLogStorage::new(4096, 4096);
    let mut flash = FlashLogRing::mount(storage).unwrap();
    flash
        .append(&flash_entry("+1", "incomplete", true))
        .unwrap();
    let mut storage = flash.into_storage();
    storage.corrupt_byte(20);
    let mut remounted = FlashLogRing::mount(storage).unwrap();
    assert!(remounted.last_n(10).unwrap().is_empty());
}

#[test]
fn flash_log_wraps_by_erase_sector() {
    let storage = MemFlashLogStorage::new(4096, 4096);
    let mut flash = FlashLogRing::mount(storage).unwrap();
    let slots = 4096 / FLASH_LOG_RECORD_SIZE;
    for i in 0..slots + 2 {
        flash
            .append(&flash_entry("+1", &format!("message-{i}"), true))
            .unwrap();
    }
    let entries = flash.last_n(slots).unwrap();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0].body_preview, format!("message-{slots}"));
}

#[test]
fn flash_log_truncates_long_unicode_record_without_panicking() {
    let storage = MemFlashLogStorage::new(4096, 4096);
    let mut flash = FlashLogRing::mount(storage).unwrap();
    flash
        .append(&flash_entry(&"发".repeat(100), &"信".repeat(100), false))
        .unwrap();
    let entry = &flash.last_n(1).unwrap()[0];
    assert!(entry.sender.is_char_boundary(entry.sender.len()));
    assert!(entry
        .body_preview
        .is_char_boundary(entry.body_preview.len()));
}

#[test]
fn empty_ring_returns_no_entries() {
    let r = LogRing::new();
    assert!(r.last_n(5).is_empty());
    assert!(r.is_empty());
    assert_eq!(r.len(), 0);
}

#[test]
fn single_push_retrieval() {
    let mut r = LogRing::new();
    r.push(entry("A"));
    assert_eq!(r.len(), 1);
    assert_eq!(r.last_n(1)[0].sender, "A");
}

#[test]
fn last_n_capped_at_available() {
    let mut r = LogRing::new();
    r.push(entry("A"));
    r.push(entry("B"));
    let result = r.last_n(10);
    assert_eq!(result.len(), 2);
}

#[test]
fn last_n_returns_most_recent_first() {
    let mut r = LogRing::new();
    for c in ['A', 'B', 'C'] {
        r.push(entry(&c.to_string()));
    }
    let result = r.last_n(2);
    assert_eq!(result[0].sender, "B");
    assert_eq!(result[1].sender, "C");
}

#[test]
fn ring_evicts_oldest_sector_when_full() {
    let mut r = LogRing::new();
    for i in 0..66 {
        r.push(entry(&i.to_string()));
    }
    assert_eq!(r.len(), 50); // one 16-record sector was erased on wrap
    let entries = r.last_n(64);
    assert_eq!(entries[0].sender, "16");
    assert_eq!(entries[49].sender, "65");
}
