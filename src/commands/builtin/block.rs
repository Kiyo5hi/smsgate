//! /block and /unblock commands.

use crate::commands::{Command, CommandContext};

pub struct BlockCommand;
pub struct UnblockCommand;

pub const BLOCK_SENTINEL: &str = "__BLOCK__:";
pub const UNBLOCK_SENTINEL: &str = "__UNBLOCK__:";

impl Command for BlockCommand {
    fn name(&self) -> &'static str { "block" }
    fn description(&self) -> &'static str { "Block SMS from a number" }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        let phone = crate::sms::codec::normalize_phone(args.trim());
        if phone.is_empty() {
            return "Usage: /block <number>".to_string();
        }
        format!("{}{}\nBlocked: {}", BLOCK_SENTINEL, phone, phone)
    }
}

impl Command for UnblockCommand {
    fn name(&self) -> &'static str { "unblock" }
    fn description(&self) -> &'static str { "Unblock SMS from a number" }

    fn handle(&self, args: &str, ctx: &CommandContext) -> String {
        let phone = crate::sms::codec::normalize_phone(args.trim());
        if phone.is_empty() {
            return "Usage: /unblock <number>".to_string();
        }
        let is_blocked = crate::bridge::forwarder::is_blocked(&phone, ctx.store);
        if !is_blocked {
            return format!("{} is not in the block list.", phone);
        }
        format!("{}{}\nUnblocked: {}", UNBLOCK_SENTINEL, phone, phone)
    }
}
