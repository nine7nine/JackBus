# jack2dbus-nspa

JACK2 1.9.22 with real-time safety, correctness and performance work on the
server engine, the ALSA driver, `jackdbus` and libjack's internals. It builds as
a drop-in replacement for a distribution's `jack2` + `jack2-dbus`: same files,
same paths, same configure options.

**No client API/ABI change and no wire-protocol change.** The shared-memory
layout, the futex activation protocol, the request encoding and the time base
are exactly 1.9.22's (protocol 9). Applications built against any JACK2 keep
working; a stock 1.9.22 libjack (64- or 32-bit) works against this server, and
this libjack works against a stock server.

## Why this fork

JACK2's last release, 1.9.22, came out in February 2023. Upstream development
since has been sporadic, and fixes that did land on `develop` never shipped in a
release. JACK is still the better fit for low-latency work on a PREEMPT_RT
system. In this setup a real JACK server owns the card, and PipeWire runs on top
of it as an ordinary JACK client for desktop audio.

Stock 1.9.22 has real problems: system calls and non-PI locks on the RT thread,
per-cycle work that scales with compile-time maxima instead of the actual graph,
races in libjack and jackdbus, and asserts that take down the whole server. This
fork fixes those, carries the useful upstream fixes, and keeps JACK2 from
bit-rotting. Every change is measured on the running engine, and a change that
doesn't hold up under measurement is reverted.

## What's changed

### Real-time safety

- **No system calls on the driver thread's side channels.** Client
  notifications and log messages go through lock-free bounded queues drained by
  non-RT threads. The per-cycle "a cycle has passed" signal is a futex event
  count instead of a glibc condvar. `write()` calls from the driver thread
  during graph churn and xruns: **1 945 → 0**.
- **Priority-inheriting mutexes** for JACK's own locks, and timed waits on
  `CLOCK_MONOTONIC`.
- **Memory locked.** The server's `mlockall()` was dead code in 1.9.22; it now
  locks all of the server's memory (105 MB → 225 MB locked). Clients prefault
  shared memory and their RT stack when `mlock` is refused. Page faults in
  process callbacks with a 64 KB memlock limit: **18 → 0**.
- **RT helper threads never sleep in the API.** Threads created with
  `jack_client_create_thread()` are treated like the process thread. Longest
  API call from such a thread during graph churn: **2 945 µs → 1 µs**
  (period 2 667 µs).
- **Lightweight xrun recovery.** An xrun restarts only the PCM streams, not the
  whole driver on the RT thread.
- **Optional PM QoS.** The `cpu-dma-latency` engine parameter
  (`jackd --cpu-dma-latency`, `jack_control eps cpu-dma-latency 0`) holds a
  request on `/dev/cpu_dma_latency` while the server runs. Idle xruns over 20 s
  at 128×2: **410–546 → 7–10**. Off by default.

### Performance

- **Per-cycle loops bounded by the clients in use.** Engine loops stop at the
  highest client refnum instead of walking all 256 slots every cycle. Server
  instructions per cycle (2 clients): **9 553 → 2 833**.
- **Graph edits copy only what's used.** Each connect or disconnect copied a
  whole 19 MB connection manager; now only the used rows are copied. Connect
  round trip, median: **1 221 µs → 182 µs**.
- **Sample conversion without libm calls.** d32 / d16 conversion:
  **4.79 / 2.05 ns → 0.89 / 0.91 ns** per sample, bit-identical output.
- **Thread-local state with TLS descriptors** (`__thread`) instead of pthread
  keys. `jack_port_connected()` from a callback: **5.29 → 2.94 ns**.
- **No idle ioctls for MIDI.** With `-X seq`, the sequencer queue time is read
  only when an event needs it: **1 → 0** ioctls per idle cycle.
- **LTO.** Builds and runs with LTO (it crashed every client in 1.9.22).

### Correctness

- **Shared memory:** segment-index corruption under LTO fixed (upstream #485).
  A server never unlinks another server's live segments. Client processes that
  outlive their server can no longer steal wake-ups from a new one.
- **Frame timer:** the DLL is re-seeded after an xrun. Timer error after an
  xrun: **7 985 µs → 107 µs**.
- **Wrap-safe MIDI timing** across the 2³² frame-counter wrap.
- **ALSA driver:**
  - The poll timeout is now computed in milliseconds; 1.9.22 used the wrong
    unit (16 s at 512 frames).
  - The S24_LE SSE tail is fixed.
  - Newly connected ports get fresh capture audio in the same cycle instead of
    stale data.
- **-X seq MIDI:** 150+ sequencer ports no longer abort the server; overflow is
  dropped and counted. JACK MIDI ports carry the real ALSA port names.
- **libjack:**
  - Concurrent API requests are serialized: 1 599 errors and a hang → 0.
  - `jack_activate()` no longer races `Init()`.
  - Fixed port-list and callback races; a sync callback set on an active client
    is now invoked.
- **jackdbus:**
  - Exits cleanly on SIGTERM; it used to need SIGKILL and left its segments
    behind.
  - Async-signal-safe crash handler.
  - Patchbay callbacks are locked.
  - A failed master switch is recoverable.
  - Control operations are serialized.
  - Device-reservation bookkeeping is bounded.
  - Log-file writes are serialized.
- **Metadata:** builds against Berkeley DB 5.3, like Arch's `jack2`, so the
  shared metadata database stays readable by every client's libjack.

### From upstream `develop`

19 commits picked from upstream's unreleased `develop` branch, including:

- complete-packet request I/O (`JackRequest` rework);
- EAGAIN handling on blocking sockets;
- startup/shutdown without a running server;
- mutex owner on `TimedWait()` timeout;
- the metadata API on libjackserver;
- waf 2.0.26;
- JACK MIDI port names (#944).

### Evaluated and not kept

- An AVX2 input-port mixdown was faster in a hot loop but slower in the running
  engine (432–509 ns vs 326–369 ns), so it was reverted.
- Also evaluated and not adopted: AVX2/AVX-512 sample conversion,
  `SCHED_DEADLINE`, `futex_waitv`, pidfd and io_uring. None showed a measurable
  gain or fit JACK's activation model.

## Validation

- **Isolated test suite** (`nspa/tests`). It runs inside a private
  user/mount/IPC/network namespace with its own `/dev/shm`, so tests never touch
  a running server. It covers fork↔stock interoperability in both directions,
  including 32-bit libjack.
- **ASan/UBSan and TSan builds** of the server and clients.
- **Hardware tests** (`nspa/tests/hw`) on a real ALSA device, against both the
  fork and the stock server. They cover load, capture freshness, `-X seq`
  startup and bursts, low-latency configurations, sync mode and MIDI timing.
  Every playback output stays muted for the whole run; a watchdog re-mutes when
  PipeWire re-applies its volumes.

```sh
nspa/build.sh                                    # Arch flags, into ./build
nspa/run-isolated.sh nspa/tests/run.sh [quick]   # isolated suite
nspa/tests/hw/run-hw.sh [hw:PCH]                 # real hardware, outputs muted
```

## Building and installing

Arch Linux: `nspa/pkg/jack2dbus-nspa` contains a PKGBUILD that replaces `jack2`
and `jack2-dbus` (it provides `jack2=1.9.22`, so `lib32-jack2` stays
installable):

```sh
cd nspa/pkg/jack2dbus-nspa
makepkg -si                 # or NSPA_REPO=/path/to/clone makepkg -si
jack_control exit           # end the old jackdbus; D-Bus starts the new one
```

`jack_control stop` is not enough after an upgrade: it stops the server but
leaves the old daemon running.

Elsewhere, build with waf as for upstream JACK2:

```sh
./waf configure --prefix=/usr --classic --dbus
./waf build
sudo ./waf install
```

## About JACK2

JACK2 (jackdmp) is the C++ implementation of the
[JACK](https://jackaudio.org/) low-latency audio server:

- **Data-flow activation.** Clients run in parallel on SMP machines when the
  graph allows it. Each client wakes its successors directly through futexes,
  without going back through the server.
- **Lock-free graph access.** The connection state is double-buffered, so
  connections and disconnections never interrupt the audio stream.
- **Synchronous or asynchronous mode.** In synchronous mode the server waits for
  every client each cycle. In asynchronous mode it uses the previous cycle's
  output, so one late client doesn't stall the rest.

This fork is based on upstream's `v1.9.22` tag.

## License

As upstream: the server is GPL-2.0-or-later and libjack is LGPL-2.1-or-later.
See `COPYING` and the headers of the individual source files.
