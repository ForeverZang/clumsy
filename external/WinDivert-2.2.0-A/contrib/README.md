# contrib — upstream reference samples (not built)

Vendored for attribution and as the authority for FLOW-layer semantics
used by `src/flowmap.c` / `src/flowwatch.c`. **Nothing here is compiled**
(the build globs `src/*.c` only).

| File | Source | Why kept |
|---|---|---|
| `netfilter.c` | WinDivert v2.2.0, `examples/netfilter/netfilter.c` | Packet filtering / reinjection patterns |
| `flowtrack.c` | WinDivert v2.2.0, `examples/flowtrack/flowtrack.c` | Per-flow PID attribution: FLOW handle flags (`WINDIVERT_FLAG_SNIFF \| WINDIVERT_FLAG_RECV_ONLY`), `FLOW_ESTABLISHED`/`FLOW_DELETED` handling |
| `LICENSE-LGPL` | https://www.gnu.org/licenses/lgpl-3.0.txt | LGPL v3 plain text |

Provenance: official WinDivert source by basil00 (Ben Baker), GitHub tag
`v2.2.0` of <https://github.com/basil00/WinDivert> (mirror of the
reqrypt.org distribution), fetched 2026-10-20. Each `.c` file carries
its own dual-license header: GNU LGPL v3 **or** GNU GPL v2.

Modifications: none; files are verbatim upstream.
