# Sharing Across Machines

## Ownership

| Information | Location | Transport |
| --- | --- | --- |
| Code, rules, procedures, redacted findings | This repository | Git |
| Device identities, phone/chat mapping, deployments | Separate private records | Private Git or encrypted storage |
| Tokens, WiFi passwords, SIM PINs | Encrypted credential storage | Encrypted sync |
| Local config, raw serial logs, generated images | Ignored config.toml and .local/ | Only intentional private transfer |
| COM ports, installed tools, build caches | Current host | Rediscover/rebuild |

A private Git repository is not encryption. Store secret references in device
records instead of token/password values. The public device template is blank.

## Resume on Another Host

1. Commit and push intended code/docs from the source machine; an ordinary
   clone does not carry dirty files. Use a private patch transfer only when
   intentionally handing off uncommitted work, and record its base commit.
2. Clone/pull on the destination, inspect git status and read handoff.md.
3. Restore config.toml from encrypted storage, or populate the example locally.
   Build tools read root config.toml; private per-device records are not
   automatically consumed by firmware.
4. Install/check Rust, ESP toolchain and espflash using development.md. Discover
   the attached device and port; do not assume another host's COM number.
5. Select board pins, target, SDK defaults and layout explicitly. Run the
   relevant checks, then record any deployment using device.example.md.

Do not sync target/, ESP-IDF build caches, or the entire working tree through a
file-sync service while building. Git carries source; each host owns its caches.

## Local Files

.local/ is ignored and intended for captures, private working notes and images.
config.toml remains separately ignored. Ignoring a file does not encrypt it or
remove data already tracked in Git. Check staged diffs before publication.

No new private repository or credential service is configured by this layout.
Choose the existing private storage used by the device owner and keep only
nonsecret navigation instructions in shared docs.

## Knowledge Maintenance

Project-specific facts from user-level skills should be maintained here so a
fresh clone is sufficient to understand the project. Generic reusable skills
may live separately, but should link to project procedures instead of duplicating
board state and host-specific paths.

Promote confirmed conclusions from chats into troubleshooting/decisions; record
unconfirmed explanations as hypotheses. Keep handoff short and current.
