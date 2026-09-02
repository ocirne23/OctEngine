# NetFuzz
Documentation for `Code/NetFuzz`. Read `.claude/CLAUDE.md` first (rules, building, style, dependency direction).

* `Code/NetFuzz` (console exe, Core + Network; VS target `run-netfuzz`): protocol fuzzer, the regression gate for wire-format changes. `NetFuzz reader|host|game|all [iterations] [seed]` — deterministic seed printed
* `reader`: any view NetReader returns must lie inside the input buffer. `host`: drives a real in-process NetHost (cold-start, pre-auth garbage, authenticated payloads via real handshake; also smoke-tests the encrypted path). `game <ip[:port]>`: the only mode reaching NetworkManager's handlers — hostile client vs a live `App --server --headless` run with `--no-encrypt` (it speaks the handshake by hand and mirrors `GameProtocolId`)
* Harness constraints that silently neuter it if broken: pump the in-process server between handshake polls, drain the socket first, raise the rate limits for the fuzz phases
