# JackBus

JackBus is JACK2 1.9.22 reworked for real-time audio on Linux. It stays
real-time safe under load, keeps the graph running when a client dies or stalls
or a card misbehaves, does less work per cycle, and joins any number of sound
cards to the graph. Desktop applications written for PipeWire, PulseAudio or
ALSA run on it directly, each one a JACK client in the graph, with no sound
server in between. It installs in place of a distribution's `jack2` +
`jack2-dbus`, and every JACK application keeps working.

JACK2's last release, 1.9.22, came out in February 2023, and the fixes on
upstream's `develop` branch never shipped. JackBus carries them, fixes what
1.9.22 gets wrong, and keeps a change only if the running engine shows it helps.

**[Architecture document](https://nine7nine.github.io/JackBus/)**: how it works,
every change with diagrams and measurements ·
**[PatchBox](https://github.com/nine7nine/PatchBox)**: its patchbay, settings
page and session manager

## At a glance

| | |
|---|---|
| **Compatible** | Same client API/ABI, shared memory layout and wire protocol as 1.9.22 (protocol 9). A stock libjack, 64- or 32-bit, works with this server, and this libjack works with a stock server. The additions (a side file for detecting dead clients, the JackBus socket, the node API) are optional: a stock libjack never uses them. |
| **Real-time safe** | No system calls on the driver thread's side channels, priority-inheriting locks, all memory locked, RT helper threads that never sleep in the API. |
| **Survives clients** | A client that dies is out of the graph in ~9 ms with no xrun; one that stalls lags alone instead of holding up the server. |
| **Interrupt or timer** | The ALSA driver wakes on the card's period interrupt, on a timer that reads the card's position, or both. By default it trusts the interrupt and adds the timer once it sees interrupts come late. JACK only ever waited for the interrupt. |
| **Survives hardware** | Under jackdbus, an unplugged master card hands over to the dummy driver and takes over again when it is back; bridged cards come back by themselves when replugged. |
| **Several sound cards** | Sound card bridges join cards on their own clocks to the graph, resampled in the master's cycle, with each card's delay measured on the card. Unplug and replug survive. |
| **Lighter engine** | Per-cycle work follows the clients in use, not compile-time maxima: 3.4× fewer instructions per cycle, 6.7× faster connects. |
| **Desktop apps on JackBus** | Shims for PipeWire (`jackbus-pw`), PulseAudio (`jackbus-pa`) and ALSA (`jackbus-alsa`): browsers, players, conferencing clients and games become JACK clients at JackBus's rate and period. PulseAudio and ALSA applications go to PipeWire while JackBus isn't running. |
| **Native API** | `jack/jackbus.h`: nodes over the JackBus socket, which hands the client the server's objects as file descriptors; a cycle record with exact timing; graph events on a file descriptor. |
| **jackdbus for tools** | Bridges, buffer size and CPU wake-up latency changed live, detach-before-stop for session managers, per-client timings, device lists that follow hotplug, a bounded log. |

## What's fixed

Measured on the running engine against stock 1.9.22.

### Real-time safety

| JACK2 1.9.22 | JackBus | Measured |
|---|---|---|
| The driver thread writes notifications and log lines to sockets and files | lock-free queues drained by non-RT threads; a futex event count instead of a condvar | `write()` calls in churn and xruns: **1 945 → 0** |
| JACK's own locks don't inherit priority | PI mutexes, timed waits on `CLOCK_MONOTONIC` | |
| `mlockall()` is dead code | all server memory locked; clients prefault shared memory and their RT stack | page faults in callbacks, 64 KB memlock: **18 → 0** |
| RT helper threads (`jack_client_create_thread`) can sleep in API calls | treated like the process thread | longest call: **2 945 µs → 1 µs** |
| An xrun restarts the whole driver on the RT thread | only the PCM streams restart | |
| Late HDA period interrupts overrun the cycle | a wake timer on the card's position, adaptive by default (`wake-timer`) | HDA 128×2: **499 xruns → 0** |
| Deep CPU idle states wake the CPU late | optional `cpu-dma-latency` hold while the server runs | idle xruns, 128×2: **410–546 → 7–10** |

### Clients that die, stall or fall behind

| JACK2 1.9.22 | JackBus | Measured |
|---|---|---|
| A dead client stalls every cycle until the kernel closes its sockets | a robust mutex per client RT thread; the server learns of the death at once (`FUTEX_OWNER_DIED`, `futex_waitv`) | 1 GB client killed 6×: **346–402 xruns → 0**, removed in ~9 ms |
| A child process holding a dead client's sockets keeps it in the graph | checked through `/proc` | **4.7 s stall → 0** |
| Teardown floods broken pipes; ports are announced gone twice | one graph edit, nothing sent to the dead client | |
| A late client: repeated xruns and log lines | one xrun per failing cycle, one line naming the late clients | |
| A client busy in a callback blocks notifications up to 2 s each, under the engine lock | 500 ms budget, then queued; the server never blocks on its socket | others' worst call: **2.9 s → 0.4 ms**; 1 500 ports: **6.2 s → 0.66 s** |

### Engine cost

| JACK2 1.9.22 | JackBus | Measured |
|---|---|---|
| Per-cycle loops walk all 256 client slots | they stop at the highest in use | instructions per cycle, 2 clients: **9 553 → 2 833** |
| Each connect copies a 19 MB connection manager | only the rows in use | connect round trip: **1 221 → 182 µs** |
| Sample conversion calls libm | no libm calls, bit-identical output | d32 / d16: **4.79 / 2.05 → 0.89 / 0.91 ns** per sample |
| Thread state in pthread keys | `__thread` TLS | `jack_port_connected()`: **5.29 → 2.94 ns** |
| `-X seq` reads the queue time every cycle | only when an event needs it | idle ioctls per cycle: **1 → 0** |
| LTO builds crash every client | shared memory index fixed (upstream #485) | builds and runs with LTO |

### Correctness

| Area | Fixed |
|---|---|
| Shared memory | A server never unlinks another server's live segments; clients that outlive a server can't steal a new one's wake-ups. |
| Timing | The frame timer is re-seeded after an xrun (error **7 985 → 107 µs**); MIDI timing is safe across the 2³² frame wrap. |
| ALSA driver | Poll timeout in the right unit (it was 16 s at 512 frames); S24_LE SSE tail; newly connected ports get fresh capture in the same cycle. |
| `-X seq` MIDI | 150+ sequencer ports no longer abort the server; MIDI ports carry the real ALSA names. |
| libjack | Concurrent requests serialized (**1 599 errors and a hang → 0**); `jack_activate()` vs `Init()`, port-list and callback races; a sync callback set on an active client is called. |
| Data races | Atomic accesses where threads share state, with the layout unchanged: TSan **0 reports**. |
| jackdbus | Clean exit on SIGTERM; async-signal-safe crash handler; patchbay callbacks locked; a failed master switch is recoverable; device reservations hold 20 cards, counted and locked (1.9.22: 2, never freed). |
| jackdbus log | Rotates at `log-max-size` (4 MiB); `log-level` (default `server`) leaves out a line per client and connection. |
| Metadata | Built against Berkeley DB 5.3, like Arch's `jack2`, so every client's libjack reads the same database. |

## New in JackBus

### Interrupt or timer wakeups

On some cards, Intel HDA at small buffer sizes above all, the period interrupt
comes late or is dropped while the card's position is right all along: a server
that waits only for the interrupt wakes late and overruns. JackBus works out
when the next period is due from the position and timestamp the card's last
update left in the status page (no system call). If the interrupt hasn't come
by then plus a margin, it asks the card for its position (`hwsync`) and runs the
cycle anyway. An interrupt that comes first still wakes it, as before.

| Mode | Master card: `wake-timer` (`-w`) | Bridged card: `@wake` |
|---|---|---|
| Interrupt, timer when needed | `adaptive` (default): the interrupt alone until it is seen late three times, then the timer too | `auto` (default) |
| Interrupt and timer | `on`, or a margin in µs (default an eighth of a period, 50–500 µs) | `timer` |
| Interrupt only, as stock JACK | `off` | `irq` |

Intel HDA, 2 periods: **499 → 0 xruns** at 128 frames, **5 574 → 2 773** at 64.
USB (batch) cards stay on the interrupt: their position only moves when a
transfer completes, so a timer has nothing earlier to read.

### Sound card bridges

A second card runs on its own crystal and drifts from the master's. Stock JACK
offers a slave driver (the cards clocked together) or a resampling client with a
fixed margin. A bridge joins the card the way the master's own card is joined:
its ports are physical ports (`U44:capture_1`), filled and emptied by the
master in its own cycle, while a thread of the card's own moves its frames
through a lock-free FIFO and a resampler follows the two clocks.

| | |
|---|---|
| **Delay** | Measured on the card: its worst case, plus 0.75 ms, reached by a 20 ppm slide (inaudible). ZOOM U-44 at 64×3 with a 256-frame master: capture **10.6 → 9.3 ms**, 8.1 ms at 32×3. |
| **USB** | snd-usb takes playback frames a buffer ahead and delivers capture in chunks: the lead is measured, and cards at ≥256 frames get a third period. U-44 playback: **23 % silence → 0**. |
| **Unplugged** | Ports and connections stay; the card is reopened when it returns (under a new index too), its reservation held meanwhile. A new master buffer size or rate is followed. |
| **Set up** | `jackd -B U44 -B Headset/64/3@timer` (`card[/period[/nperiods]][@auto\|timer\|irq]`), jackdbus's `bridges` parameter (live, saved, bridged when plugged in), or PatchBox's Devices tab. |

### Applications on JackBus: the shims

An application's own work (decoding, mixing, effects) stays in its own threads,
as with any sound server; in JackBus's cycle a shim only moves one cycle of
frames between the application's buffer and its ports, with no system call but
JACK's futex. Each application is one JACK client, named after it, routed and
saved like any other. The application's rate is JackBus's when it can use it
(the direct route); one that insists on another rate is resampled, a fallback
that can be switched off.

| Shim | For | How |
|---|---|---|
| `jackbus-pw` | PipeWire applications: ossia score, mpv, SDL3, OpenAL, fluidsynth | `jackbus-pw <program>`: its own `libpipewire-0.3`. Filters run in JackBus's cycle, sample-accurate; the JACK graph is the registry; links are JACK connections. |
| `jackbus-pa` | PulseAudio applications: Firefox, Chrome, Teams, mpv, games | Every program loading libpulse gets JackBus's dispatcher (`/etc/ld.so.conf.d`). With the setting on (`pulse = yes`, PatchBox's Shims tab) it uses JackBus while JackBus runs and PulseAudio's own libpulse otherwise; off by default. `jackbus-pa <program>` forces JackBus. |
| `jackbus-alsa` | ALSA applications | The `jackbus` device (`aplay -D jackbus`), or ALSA's default device with a `fallback` used while JackBus isn't running. |

Settings live in `~/.config/jackbus/compat.conf`, a section per shim
(`pa.output = U44`, `alsa.resample = no`): resampling and its quality, connecting
new streams, the output and input device (`system` or a bridged card), and a cap
on how much a PulseAudio stream buffers. With PatchBox's "PipeWire leaves audio
to JackBus", PipeWire's JACK bridge isn't loaded: while JackBus runs PipeWire
has no audio work, and video and screen sharing are untouched.

The desktop's own sound goes the same way: its volume applet, volume keys,
settings pages and event sounds (libcanberra) get `jackbus-pa`. A device's
volume and mute are the card's own mixer (Master, Capture: alsamixer's
scale, a capture control's 100 % at 0 dB), so every process and alsamixer
show one value. For the desktop's processes to start on JackBus, JackBus has
to start first: `jackbus.service` does that at login (below).

### The native API

`jack/jackbus.h` is a client API beside libjack's, in the same library. A
**node** is a JACK client opened over the **JackBus socket**
(`$XDG_RUNTIME_DIR/jackbus/<server>`), which hands it the server's shared
objects as file descriptors. Its process callback gets a cycle record (frame,
time in ns, the next cycle's time, the period, the rate), its ports are added
and removed at any time, and graph changes (clients, ports, connections,
renames, buffer size, the server going away) arrive on an event file
descriptor for the application's own loop. The shims are built on it.

### jackdbus calls

```text
org.jackaudio.JackControl.AddBridge(s spec)            → s name
org.jackaudio.JackControl.RemoveBridge(s bridge)
org.jackaudio.JackControl.GetBridges()                 → a(sssssuuuuudddttt)
org.jackaudio.JackControl.GetClientTimings()           → a(suu)
org.jackaudio.JackControl.ResetClientTimings()
org.jackaudio.JackPatchbay.DetachClient(s client_name) → u detached
org.jackaudio.JackPatchbay.DetachProcess(x process_id) → u detached
org.jackaudio.Configure.ConstraintsChanged(as container)  (signal)
```

- **Detach before stop:** a session manager asks jackdbus to take an
  application's clients out of the graph before stopping it, so the stop costs
  the graph nothing, whatever libjack the application uses.
- **Client timings:** per client, its late cycles and longest cycle, measured
  only within a minute of a request (27 instructions per cycle when nobody asks).
- **Hotplug:** jackdbus watches `/dev/snd` and refreshes the ALSA device list a
  second after a card comes or goes (`ConstraintsChanged`).
- **Master card unplugged:** the dummy driver stands in at the same rate, buffer
  size and port counts; clients keep running and keep their connections, and
  the card takes over again when it is back.

Older jackdbus versions answer `UnknownMethod`, and tools carry on as before.

### Also

- **From upstream `develop`:** 19 commits, including complete-packet request
  I/O, EAGAIN handling, the metadata API on libjackserver, waf 2.0.26 and JACK
  MIDI port names (#944).
- **Tried and not kept:** an AVX2 mixdown (faster in a hot loop, slower in the
  engine), AVX2/AVX-512 sample conversion, `SCHED_DEADLINE`, pidfd, io_uring,
  `futex_waitv` on the RT path, and late clients skipping a cycle (it loses
  input).

## Validation

- **Isolated suite** (`nspa/tests`): checks in a private
  user/mount/IPC/network namespace with its own `/dev/shm` and runtime
  directory, never near a running server. It covers fork↔stock interoperability
  both ways with 32-bit libjack, clients killed and stalled every way the tests
  can, the jackdbus calls, bridges on snd-aloop, the JackBus socket and nodes.
- **Shims with real applications** (`nspa/pw-shim/test`, `nspa/pa-shim/test`,
  `nspa/jackbus-alsa/test`): PipeWire's tools, mpv, SDL3, OpenAL, fluidsynth,
  ossia score, paplay/parecord, Chrome and Firefox with their sandboxes on,
  aplay/arecord, libcanberra's event sounds; each application's ports recorded
  and checked as a continuous sine; every realtime thread traced (no system
  call but futex); device volume against a test card's mixer.
- **Engine measurements:** an opt-in cycle trace (the master's cycle in ns) and
  an A/B harness that runs two builds in turn, isolated and muted.
- **Bridge tests** (`nspa/tests/bridge`): simulated cards (drift, jitter, xruns,
  a card gone), the resampler, snd-aloop in real time, and real cards through
  jackd or jackdbus.
- **Sanitizers:** ASan/UBSan and TSan builds of the server and clients.
- **Hardware tests** (`nspa/tests/hw`) against both the fork and the stock
  server: load, capture freshness, `-X seq`, low latency, sync mode, MIDI timing,
  clients killed mid-graph. Every playback output stays muted throughout.

```sh
nspa/build.sh                                    # Arch flags, into ./build
nspa/run-isolated.sh nspa/tests/run.sh [quick]   # isolated suite
nspa/tests/hw/run-hw.sh [hw:PCH]                 # real hardware, outputs muted
```

## Building and installing

Arch Linux: `nspa/pkg/jackbus` builds the `jackbus` package, which replaces
`jack2` and `jack2-dbus` (it provides `jack2=1.9.22`, so `lib32-jack2` stays
installable):

```sh
cd nspa/pkg/jackbus
makepkg -si                 # or NSPA_REPO=/path/to/clone makepkg -si
jack_control exit           # end the old jackdbus; D-Bus starts the new one
```

`jack_control stop` is not enough after an upgrade: it stops the server but
leaves the old daemon running.

The package also installs the shims. Nothing changes for an application until
it is run through one (`jackbus-pw`, `jackbus-pa`, `aplay -D jackbus`) or the
switches in PatchBox's Shims tab are on.

To start JackBus at login, before the desktop (through jackdbus, with the
settings PatchBox and `jack_control` save):

```sh
systemctl --user enable jackbus.service
```

It takes the place of an autostart entry running `jack_control start`, which
runs after the desktop has started: hide or remove one.

Elsewhere, build with waf as for upstream JACK2:

```sh
./waf configure --prefix=/usr --classic --dbus
./waf build
sudo ./waf install
```

## About JACK2

JACK2 (jackdmp) is the C++ implementation of the
[JACK](https://jackaudio.org/) low-latency audio server. Clients wake their
successors directly through futexes and run in parallel where the graph allows;
the connection state is double-buffered, so graph changes never interrupt the
audio. JackBus is based on upstream's `v1.9.22` tag.

## License

As upstream: the server is GPL-2.0-or-later and libjack is LGPL-2.1-or-later.
See `COPYING` and the headers of the individual source files.
