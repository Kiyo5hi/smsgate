//! Timer arithmetic tests — specifically wraparound safety.

use smsgate::timer::{elapsed_since, is_past};

#[test]
fn elapsed_normal() {
    assert_eq!(elapsed_since(1000, 2000), 1000);
}

#[test]
fn elapsed_wraparound() {
    // now wrapped around past 0
    let snapshot = u32::MAX - 100;
    let now = 100u32;
    // true elapsed = 201
    assert_eq!(elapsed_since(snapshot, now), 201);
}

#[test]
fn is_past_not_yet() {
    assert!(!is_past(2000, 1000));
}

#[test]
fn is_past_exactly() {
    assert!(is_past(1000, 1000));
}

#[test]
fn is_past_after() {
    assert!(is_past(1000, 1001));
}

#[test]
fn is_past_wraparound_fires_correctly() {
    // target = u32::MAX - 100, now has wrapped to 100
    // is_past should return true (201 elapsed > 0)
    let target = u32::MAX - 100;
    let now = 100u32;
    assert!(is_past(target, now));
}

#[test]
fn is_past_far_future_not_past() {
    // target is 1 billion ms in the future (never fires within half-cycle)
    let now = 1000u32;
    let target = now.wrapping_add(1_000_000_000);
    assert!(!is_past(target, now));
}
