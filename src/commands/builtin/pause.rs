//! /pause and /resume commands.

use crate::commands::{Command, CommandContext};
use crate::persist::{keys, load_bool};

pub const PAUSE_SENTINEL: &str = "__PAUSE__:";
pub const RESUME_SENTINEL: &str = "__RESUME__";

pub struct PauseCommand;
pub struct ResumeCommand;

impl Command for PauseCommand {
    fn name(&self) -> &'static str { "pause" }
    fn description(&self) -> &'static str { "Pause SMS forwarding (default 60 min)" }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        let mins: u32 = args.trim().parse().unwrap_or(60);
        format!("{}{}  \nForwarding paused for {} min.", PAUSE_SENTINEL, mins, mins)
    }
}

impl Command for ResumeCommand {
    fn name(&self) -> &'static str { "resume" }
    fn description(&self) -> &'static str { "Resume SMS forwarding immediately" }

    fn handle(&self, _args: &str, ctx: &CommandContext) -> String {
        let enabled = load_bool(ctx.store, keys::FWD_ENABLED).unwrap_or(true);
        if enabled {
            "Forwarding is already active.".to_string()
        } else {
            format!("{}  \nForwarding resumed.", RESUME_SENTINEL)
        }
    }
}
