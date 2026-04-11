//! Wraparound-safe timer primitives.
//!
//! All timer logic uses only these functions — never raw `>=` / `<` on u32 timestamps.

/// Milliseconds elapsed since `snapshot` (safe across u32 wraparound).
#[inline]
pub fn elapsed_since(snapshot: u32, now: u32) -> u32 {
    now.wrapping_sub(snapshot)
}

/// Returns true when `now >= target` within half a u32 cycle.
#[inline]
pub fn is_past(target: u32, now: u32) -> bool {
    now.wrapping_sub(target) < u32::MAX / 2
}
