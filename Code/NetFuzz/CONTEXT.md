# NetFuzz

> Documentation for `Code/NetFuzz`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Console executable (Core + Network; VS target `run-netfuzz`). The protocol fuzzer, and the regression
gate for wire-format changes.

```
NetFuzz reader|host|game|all [iterations] [seed]
```

The seed is printed, so a run is deterministic and repeatable.

## Modes

| Mode | What it does |
|---|---|
| `reader` | Asserts the invariant that any view `NetReader` returns lies inside the input buffer. |
| `host` | Drives a real in-process `NetHost`: cold start, pre-auth garbage, authenticated payloads through the real handshake. Also smoke-tests the encrypted path. |
| `game <ip[:port]>` | The only mode that reaches NetworkManager's handlers — a hostile client against a live `App --server --headless` run with `--no-encrypt`. It speaks the handshake by hand and mirrors `GameProtocolId`. |

## Harness constraints

Break any of these and the fuzzer silently stops testing anything:

* Pump the in-process server between handshake polls.
* Drain the socket first.
* Raise the rate limits for the fuzz phases.
