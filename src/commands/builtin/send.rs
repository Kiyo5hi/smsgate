//! /send <number> <text>
//!
//! Enqueues an outbound SMS. The queue is shared via a Mutex in the main loop;
//! commands receive a read-only snapshot, and the actual enqueue call is
//! handled by the main loop after the command returns.
//!
//! Because commands only have read access to `send_queue`, /send returns
//! a description of what it *would* do; the main loop owns the actual enqueue.
//! To keep it simple, we embed the phone+body in the response and let the
//! caller parse it — but in practice the main loop will just call `enqueue`
//! directly if the command is `/send`.
//!
//! Design: /send is special — the main loop detects it and calls enqueue.
//! We return a structured sentinel response that poller.rs parses.

use crate::commands::{Command, CommandContext};
use crate::sms::codec::count_sms_parts;

pub struct SendCommand;

/// Sentinel prefix that poller.rs recognises as an enqueue request.
pub const SEND_SENTINEL: &str = "__SEND__:";

impl Command for SendCommand {
    fn name(&self) -> &'static str { "send" }
    fn description(&self) -> &'static str { "Send an SMS: /send <number> <text>" }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        let args = args.trim();
        let Some((phone_raw, body)) = args.split_once(|c: char| c.is_whitespace()) else {
            return "Usage: /send <number> <message text>".to_string();
        };
        let phone = crate::sms::codec::normalize_phone(phone_raw);
        if phone.is_empty() {
            return "Invalid phone number".to_string();
        }
        if body.trim().is_empty() {
            return "Message body is empty".to_string();
        }
        let body = body.trim();
        let parts = count_sms_parts(body, 10);
        if parts == 0 {
            return "Message too long (> 10 SMS parts)".to_string();
        }
        let preview: String = body.chars().take(50).collect();
        // Encode newlines/CRs so the sentinel stays on a single line.
        // apply_sentinels decodes these back before passing to enqueue.
        let body_encoded = body.replace('\\', "\\\\").replace('\n', "\\n").replace('\r', "\\r");
        format!("{}{}|{}\nQueued: {} → \"{}…\" ({} part(s))",
            SEND_SENTINEL, phone, body_encoded,
            phone, preview, parts)
    }
}
