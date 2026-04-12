//! /block and /unblock commands.

use crate::commands::{Command, CommandContext};

pub struct BlockCommand;
pub struct UnblockCommand;

pub const BLOCK_SENTINEL: &str = "__BLOCK__:";
pub const UNBLOCK_SENTINEL: &str = "__UNBLOCK__:";

impl Command for BlockCommand {
    fn name(&self) -> &'static str { "block" }
    fn description(&self) -> &'static str { crate::i18n::desc_block() }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        let phone = crate::sms::codec::normalize_phone(args.trim());
        if phone.is_empty() {
            return crate::i18n::block_usage().to_string();
        }
        format!("{}{}\n{}", BLOCK_SENTINEL, phone, crate::i18n::block_ok(&phone))
    }
}

impl Command for UnblockCommand {
    fn name(&self) -> &'static str { "unblock" }
    fn description(&self) -> &'static str { crate::i18n::desc_unblock() }

    fn handle(&self, args: &str, ctx: &CommandContext) -> String {
        let phone = crate::sms::codec::normalize_phone(args.trim());
        if phone.is_empty() {
            return crate::i18n::unblock_usage().to_string();
        }
        if !crate::bridge::forwarder::is_blocked(&phone, ctx.store) {
            return crate::i18n::unblock_not_found(&phone);
        }
        format!("{}{}\n{}", UNBLOCK_SENTINEL, phone, crate::i18n::unblock_ok(&phone))
    }
}
