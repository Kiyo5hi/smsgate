# Architecture

Board startup constructs a shared ModemPort. Runtime bridge logic uses traits.

| Trait | Definition | Responsibility |
| --- | --- | --- |
| Board | [boards/mod.rs](../src/boards/mod.rs) | Pins, power sequence, modem construction |
| AtTransport / ModemPort | [modem/mod.rs](../src/modem/mod.rs) | AT transport and modem operations |
| MessageSink / MessageSource | [im/mod.rs](../src/im/mod.rs) | Outbound delivery and inbound commands |
| Store | [persist/mod.rs](../src/persist/mod.rs) | Persistent key/value access |
| Command | [commands/mod.rs](../src/commands/mod.rs) | Bot command handlers |

[main.rs](../src/main.rs) wires boards, workers, modem polling and recovery.
FanoutSink delivers to multiple sinks; the primary message ID supports reply
routing. SMS notifications go to the configured primary chat; command replies
target the calling trusted conversation.

Large implementations live in modem/a76xx/, im/telegram/, commands/builtin/.
Keep hardware/backend types outside business logic.

## State

- Compile-time defaults: ignored config.toml, schema in
  [config.toml.example](../config.toml.example).
- Runtime credentials: seven smsgcfg NVS keys (see AGENTS.md).
- Runtime bridge state: four smsgate NVS keys (see AGENTS.md).
- Event history: separate flash log_ring partition, not a fifth smsgate key.
- Transient timers, in-memory queues and multipart assembly are not a durable
  record of deployment state.

Software-only OTA preserves existing NVS credentials; with-config applies
compiled credential defaults. This distinction concerns provisioning behavior,
not proof that an artifact contains no secrets.
