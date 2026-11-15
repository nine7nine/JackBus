# JackBus

JACK2 1.9.22 with real-time safety, correctness and performance work on the
server engine, the ALSA driver, `jackdbus` and libjack's internals. It builds as
a drop-in replacement for a distribution's `jack2` + `jack2-dbus`: same files,
same paths, same configure options.

**No client API/ABI change and no wire-protocol change.** The shared-memory
layout, the futex activation protocol, the request encoding and the time base
are exactly 1.9.22's (protocol 9). Applications built against any JACK2 keep
working; a stock 1.9.22 libjack (64- or 32-bit) works against this server, and
this libjack works against a stock server. The one addition is optional: a small
side file in which each client's realtime thread holds a robust mutex, so the
server knows the moment a client dies. A stock libjack never opens it, and this
libjack does without it on a stock server.

It also adds what JACK never had: **sound card bridges** that join other cards
to the graph, each resampled in the master's own cycle with the delay it needs
measured on the card; a master card that can be unplugged and plugged back under
`jackdbus` without clients noticing; and the D-Bus calls a settings page and a
session manager need. [PatchBox](https://github.com/nine7nine/PatchBox) is its
patchbay, settings page and session manager.

**[Architecture document](https://nine7nine.github.io/JackBus/)**: how the
server, libjack, shared memory and the activation graph work, every change with
diagrams, and the measurements behind them (source: [`docs/index.html`](docs/index.html)).

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
- **A wake timer for late period interrupts.** On Intel HDA at small buffer
  sizes the period interrupt can come late or be dropped while the card's
  position is right. The ALSA driver works out when the period is due from the
  status page (no system call) and, if the interrupt hasn't come by then plus a
  margin, wakes on the position. `wake-timer` (`-w`): `adaptive` (the default:
  on once notifications are seen late three times), `on`, `off`, or a margin in
  µs. HDA at 128×2, 30 s: **499 xruns → 0**. Batch (USB) cards are left alone.
- **Optional PM QoS.** The `cpu-dma-latency` engine parameter
  (`jackd --cpu-dma-latency`, `jack_control eps cpu-dma-latency 0`) holds a
  request on `/dev/cpu_dma_latency` while the server runs. Idle xruns over 20 s
  at 128×2: **410–546 → 7–10**. Off by default.

### Clients that die, run late or fall behind

- **A dead client is out of the graph at once.** 1.9.22 only noticed when the
  client's sockets closed, and the kernel closes a dying process's files after
  it has torn down its memory; every cycle waited until then. Now each client's
  realtime thread holds a robust mutex in a shared slot, the kernel marks it
  (`FUTEX_OWNER_DIED`) at the start of the thread's exit, and the server's
  notifier, waiting on all slots with `futex_waitv`, wakes within microseconds.
  A 1 GB client killed 6 times: **346–402 xruns, stalls up to 78 cycles → 0
  xruns, 0 missed cycles**, removed in ~9 ms. It works whichever way the thread
  ends: kill, crash, `exit()`, or a process thread that returns.
- **Any libjack.** A client without a slot (a stock libjack) is checked through
  `/proc` when the graph doesn't finish: 1 xrun per kill. A child process that
  inherited a client's sockets no longer keeps a dead client in the graph:
  **4.7 s stall → 0**.
- **Removed in one step.** All of a client's disconnections and its activation
  edges go into one graph edit, switched at the next cycle. Nothing is sent to a
  dead client: no broken-pipe floods, and its ports are announced gone once
  instead of twice.
- **One xrun per failing cycle**, with one log line naming the unfinished
  clients, and `jack_get_xrun_delayed_usecs()` reports that xrun. A late client
  still catches up as before: making it skip its next cycle (CoreAudio-style)
  was designed and dropped, because it loses a block of that client's input
  (MIDI events, recorded audio).
- **A slow notification client lags alone.** 1.9.22 waited up to 2 s per
  notification for a client busy in a callback, under the engine lock, and
  blocked for good on one that stopped reading. Now it gets a 500 ms budget,
  then its notifications are queued in order and sent as it catches up; the
  server never blocks on its socket, and late replies are no longer mistaken
  for later ones. Other clients' worst operation with a 1.5 s callback:
  **2.9 s → 0.41 s once, then 0.4 ms**. 1 500 port registrations behind a hung
  client: **6.2 s → 0.66 s**.

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
  - Patchbay callbacks are locked; a destroyed client's leftover ports and
    their connections are removed, and teardown holds the lock.
  - A failed master switch is recoverable.
  - Control operations are serialized.
  - Device-reservation bookkeeping is bounded.
  - Log-file writes are serialized.
  - The log no longer grows without end: past `log-max-size` MiB (default 4)
    `jackdbus.log` moves to `jackdbus.log.1`, replacing the one there. The
    `log-level` engine parameter sets what is written: `off`, `errors`,
    `server` (the default: errors and messages, without a line for every
    client and connection) or `all` (upstream's). Both are saved in conf.xml.
- **Metadata:** builds against Berkeley DB 5.3, like Arch's `jack2`, so the
  shared metadata database stays readable by every client's libjack.

### Sound card bridges

Two sound cards run on two crystals that never agree. Stock JACK offers a slave
driver, which needs the cards clocked together, or a resampling client
(`zita-a2j`, `audioadapter`) with a fixed margin. A **bridge** joins another
card the way the master's own is joined: its ports are physical ports (`U44:capture_1`),
filled and emptied by the master in its own cycle, while a thread of the card's
own moves its frames through a lock-free FIFO and a resampler follows the two
clocks. Neither side ever waits on the other.

- **The delay is measured, not guessed.** The card side notes its worst case
  (how late capture comes, how far ahead playback takes frames); after 20 s
  locked the delay slides to that worst case plus 0.75 ms, at 20 ppm (inaudible),
  and the ports report it. A ZOOM U-44 bridged at 64×3 to a 256-frame master:
  capture **10.6 → 9.3 ms**, 8.1 ms at 32×3.
- **USB cards handled as they are.** snd-usb takes playback frames a whole
  buffer ahead, and delivers capture in chunks that don't follow the period; the
  bridge measures the first and gives cards at ≥256 frames a third period for the
  second. U-44 playback: **23 % of frames played as silence → 0**.
- **Unplugged and back.** The ports stay, connections are kept, the card is
  reopened when it returns (under a new index too) and placed again; its device
  reservation is held meanwhile. A new master buffer size or sample rate is
  followed.
- **Set up anywhere.** `jackd --bridge U44 -B Headset/64/3@timer`
  (`card[/period[/nperiods]][@auto|timer|irq]`), jackdbus's `bridges` engine
  parameter (applied live, kept in conf.xml; a listed card is bridged when it is
  plugged in), the D-Bus calls below, or PatchBox's Devices tab.

### jackdbus for session managers and monitors

```text
org.jackaudio.JackPatchbay.DetachClient(s client_name)  → u detached
org.jackaudio.JackPatchbay.DetachProcess(x process_id)  → u detached
org.jackaudio.JackControl.GetClientTimings()          → a(suu)
org.jackaudio.JackControl.ResetClientTimings()
org.jackaudio.JackControl.AddBridge(s spec)           → s name
org.jackaudio.JackControl.RemoveBridge(s bridge)
org.jackaudio.JackControl.GetBridges()                → a(sssssuuuuudddttt)
org.jackaudio.Configure.ConstraintsChanged(as container)  (signal)
```

- **Detach before stop.** A session manager that is about to stop an
  application asks jackdbus first, by client name or by the pid it started. The
  application's clients leave the graph before the signal, so stopping it costs
  the graph nothing, with any libjack inside the application.
- **Client timings.** Per client in the graph: the cycles it hadn't finished
  when the next one began, and its longest cycle in µs. A client waiting for one
  of its inputs isn't counted late; the one it waits for is. Late cycles are
  counted on cycles that didn't finish, at no cost to the others. Cycle lengths
  are measured only within a minute of a request (about 50 instructions per
  client per cycle), so a monitor polls. With nobody asking, the server costs
  27 instructions per cycle more than before.

- **Sound cards plugged in or removed.** Stock jackdbus lists the ALSA devices
  once, at start: a USB interface plugged in later is never offered until
  jackdbus restarts. This one watches `/dev/snd` and, about a second after a card
  comes or goes, reads the ALSA driver's device list again and sends
  `ConstraintsChanged` with `["drivers", "alsa"]`, so a settings page refreshes
  its list. That covers whether a card is fully set up (its control node and
  permissions), and a burst of changes gives one refresh. It runs in jackdbus's
  main loop: no new thread, nothing near the server's real-time thread.
- **The master's card unplugged.** In 1.9.22 the server stops and every client
  is told it shut down. Under jackdbus the dummy driver stands in at the same
  rate, buffer size and port counts, clients keep running, and the card takes
  over again when it is back, with its connections. jackd on its own behaves as
  before.
- **Device reservations** hold twenty cards (two in 1.9.22), are counted and
  locked: a bridge on another device of the master's card shares its
  reservation, and removing the bridge no longer releases the master's.

Older jackdbus versions answer `UnknownMethod`, and a tool carries on as before
(the signal simply never comes).

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
  `SCHED_DEADLINE`, pidfd and io_uring. None showed a measurable gain or fit
  JACK's activation model. `futex_waitv` gains nothing on the RT path; it is
  used off it, where the notifier waits on all the clients' slots at once.
- A late client skipping its next cycle, as above.

## Validation

- **Isolated test suite** (`nspa/tests`). It runs inside a private
  user/mount/IPC/network namespace with its own `/dev/shm`, so tests never touch
  a running server. 97 checks, including fork↔stock interoperability in both
  directions with 32-bit libjack, clients killed and stalled in every way the
  tests can produce, the jackdbus calls, and bridges on snd-aloop.
- **Bridge tests** (`nspa/tests/bridge`): whole bridges against simulated cards
  (drift, jitter, xruns, a card gone), the resampler, real time on snd-aloop,
  and any real cards through jackd or jackdbus, with every playback control
  muted.
- **ASan/UBSan and TSan builds** of the server and clients.
- **Hardware tests** (`nspa/tests/hw`) on a real ALSA device, against both the
  fork and the stock server. They cover load, capture freshness, `-X seq`
  startup and bursts, low-latency configurations, sync mode, MIDI timing and
  clients killed while the graph runs.
  Every playback output stays muted for the whole run; a watchdog re-mutes when
  PipeWire re-applies its volumes.

```sh
nspa/build.sh                                    # Arch flags, into ./build
nspa/run-isolated.sh nspa/tests/run.sh [quick]   # isolated suite
nspa/tests/hw/run-hw.sh [hw:PCH]                 # real hardware, outputs muted
```

## Building and installing

Arch Linux: `nspa/pkg/jackbus` contains a PKGBUILD that replaces `jack2`
and `jack2-dbus` (it provides `jack2=1.9.22`, so `lib32-jack2` stays
installable):

```sh
cd nspa/pkg/jackbus
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
