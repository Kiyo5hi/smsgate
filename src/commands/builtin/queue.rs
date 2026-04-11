use crate::commands::{Command, CommandContext};

pub struct QueueCommand;

impl Command for QueueCommand {
    fn name(&self) -> &'static str { "queue" }
    fn description(&self) -> &'static str { "Inspect the outbound SMS queue" }

    fn handle(&self, _args: &str, ctx: &CommandContext) -> String {
        let entries = ctx.send_queue.snapshot();
        if entries.is_empty() {
            return "Outbound queue is empty.".to_string();
        }
        let mut out = format!("{} pending:\n", entries.len());
        for e in &entries {
            out.push_str(&format!(
                "#{} → {} | attempt {} | {}s old | \"{}\"\n",
                e.id, e.phone, e.attempts, e.age_secs, e.body_preview
            ));
        }
        out
    }
}
