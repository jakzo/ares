# N64 guest profiler

This fork contains an opt-in guest profiler for GoldenEye performance work.
The same ares build can run normal CI tests or collect profiles. Normal
emulation only pays predictable disabled checks; instruction-level JIT hooks
are emitted only when profiling is configured before the N64 powers on.

Set both environment variables when launching ares:

```sh
ARES_N64_PROFILE_SYMBOLS=/path/to/ge007.u.elf \
ARES_N64_PROFILE_OUTPUT=/path/to/profile/ge007 \
ARES_N64_PROFILE_REPLAY=1 \
  ares --no-file-prompt /path/to/ge007.u.z64
```

The ELF must be the exact ELF used to build the ROM. The profiler reads its
ELF32 big-endian `STT_FUNC` symbols directly; no `nm` sidecar or debug-server
connection is required.

Capturing starts after a non-title `lvlStageLoad` returns and stops when
`lvlUnloadStageTextData` begins. Returning to the title and loading another
stage therefore creates another numbered capture. Each capture writes:

- `*-summary.csv`: total CPU cycles, frame count/average delta, and TLB totals.
- `*-functions.csv`: call counts plus self and inclusive guest CPU cycles.
- `*-tlb.csv`: per-8 KiB virtual-page access and TLB-cache hit/miss counts.
- `*-frames.csv`: VI frame start, stop, and delta cycles.
- `*-game-frames.csv`: when `ARES_N64_PROFILE_REPLAY=1`, GoldenEye rendered
  replay-frame tick cycles and software code-page loads. These are observed at
  normal release-ROM function boundaries without guest instrumentation.
- `*.folded`: guest-cycle weighted call stacks in folded-stack format.

ares exits automatically after writing a capture. It also exits if capture has
not started within 60 seconds, or writes a partial capture and exits if an
active capture has not finished within 10 minutes. The generated file paths are
printed to stderr after they are written.

The profiler records emulated VR4300/CP0 cycles, not host wall time, so results
remain useful when the host is under load. Function call stacks are recovered
from JAL/JALR and return addresses; exception and tail-call entries are
resynchronised at symbol boundaries.

Generate a standalone interactive HTML flame graph from a folded capture with:

```sh
python3 tools/n64-profiler-flamegraph.py profile-001.folded
```
