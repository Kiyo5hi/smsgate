use crate::commands::{Command, CommandContext};

pub struct RestartCommand;

pub const RESTART_SENTINEL: &str = "__RESTART__";

impl Command for RestartCommand {
    fn name(&self) -> &'static str { "restart" }
    fn description(&self) -> &'static str { "Reboot the device" }

    fn handle(&self, _args: &str, _ctx: &CommandContext) -> String {
        format!("{}  \nRebooting…", RESTART_SENTINEL)
    }
}
