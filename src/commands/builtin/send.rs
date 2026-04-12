//! /send <number> <text>
//!
//! Returns a sentinel line for poller.rs to parse + a user-visible confirmation.

use crate::commands::{Command, CommandContext};
use crate::sms::codec::count_sms_parts;

pub struct SendCommand;

pub const SEND_SENTINEL: &str = "__SEND__:";

impl Command for SendCommand {
    fn name(&self) -> &'static str { "send" }
    fn description(&self) -> &'static str { crate::i18n::desc_send() }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        let args = args.trim();
        let Some((phone_raw, body)) = args.split_once(|c: char| c.is_whitespace()) else {
            return crate::i18n::send_usage().to_string();
        };
        let phone = crate::sms::codec::normalize_phone(phone_raw);
        if phone.is_empty() {
            return crate::i18n::send_invalid_number().to_string();
        }
        let body = body.trim();
        if body.is_empty() {
            return crate::i18n::send_empty_body().to_string();
        }
        let parts = count_sms_parts(body, 10);
        if parts == 0 {
            return crate::i18n::send_too_long().to_string();
        }
        let preview: String = body.chars().take(50).collect();
        let body_encoded = body.replace('\\', "\\\\").replace('\n', "\\n").replace('\r', "\\r");
        format!("{}{}|{}\n{}",
            SEND_SENTINEL, phone, body_encoded,
            crate::i18n::send_queued(&phone, &preview, parts))
    }
}
