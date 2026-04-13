use crate::commands::{Command, CommandContext, UPDATE_CONFIRM_SENTINEL, UPDATE_SENTINEL};
use crate::ota;

pub struct UpdateCommand;

impl Command for UpdateCommand {
    fn name(&self) -> &'static str { "update" }
    fn description(&self) -> &'static str { crate::i18n::desc_update() }

    fn handle(&self, args: &str, _ctx: &CommandContext) -> String {
        match args.trim() {
            "confirm" => {
                if !ota::is_manual_confirm() {
                    return crate::i18n::update_confirm_not_manual().to_string();
                }
                format!("{}\n{}", UPDATE_CONFIRM_SENTINEL, crate::i18n::update_confirming())
            }
            "" => {
                if !ota::is_enabled() {
                    return crate::i18n::update_disabled().to_string();
                }
                format!("{}\n{}", UPDATE_SENTINEL, crate::i18n::update_starting())
            }
            _ => crate::i18n::update_usage().to_string(),
        }
    }
}
