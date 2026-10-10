# reliable-udp

A reliable, ordered file-transfer engine built on UDP in C++20 — Selective
Repeat ARQ with SACK, D-SACK, RACK-style loss detection, RFC 6298 adaptive RTO,
and receiver flow control, together with a deterministic network simulator used
to test and measure it.

**No TCP is used anywhere in the transport.** The only sockets opened are
`SOCK_DGRAM`.

Everything in this README is produced by code in this repository. Every number
comes from a benchmark in `benchmarks/`, was run on the machine described in
`results/bench_run.log`, and is reproducible with `scripts/run_all.sh`.

---

## 1. The problem

UDP gives you datagrams and nothing else: packets can be lost, duplicated,
reordered, delayed, or corrupted, and there is no flow control. To move a file
across it intact you have to rebuild the guarantees yourself:

| UDP gives you | You must add |
|---|---|
| unreliable datagrams | acknowledgement + retransmission |
| arbitrary ordering | sequence numbers + reassembly |
| duplicates | duplicate detection |
| weak/optional 16-bit checksum | a real integrity check |
| no flow control | an advertised window |
| no notion of a connection | handshake, session identity, termination |

This project implements those in a way that can be tested and measured rather
than asserted.

---

## 2. Architecture

The central design decision: **the protocol engines contain no I/O and no clock
of their own.**

```
                     ┌───────────────────────────────────────┐
                     │  SrSender            SrReceiver       │   pure logic:
                     │    tick(now_us)        tick(now_us)   │   no syscalls,
                     │    on_datagram(buf,len,now_us)        │   no threads,
                     │    next_deadline_us()                 │   no allocation
                     └────────────────┬──────────────────────┘
                                      │  DatagramSink (virtual)
                     ┌────────────────┴──────────────────────┐
                     │                                       │
              ┌──────▼───────┐                       ┌───────▼────────┐
              │   UdpSink    │                       │    SimLink     │
              │ real sockets │                       │ virtual clock  │
              │ CLOCK_MONO   │                       │ seeded RNG     │
              │ sendmsg/ppoll│                       │ discrete event │
              └──────────────┘                       └────────────────┘
```

`SrSender` and `SrReceiver` take the current time as a parameter and emit
datagrams through a `DatagramSink` interface. That single decision is what makes
the rest of the project possible:

- **The same protocol code** runs under a real socket and under a simulated link.
  The simulator is not a mock of the protocol — it is a mock of the *network*.
- **Tests are deterministic.** Under the simulator, time is a `uint64_t` the
  harness advances, so a seed reproduces a run exactly, bit for bit
  (test scenario 22).
- **Tests are fast.** The reliability benchmark simulates 25-second transfers at
  50 ms RTT in milliseconds of real time, because nothing ever sleeps.
- **Zero allocation is verifiable.** With no I/O in the engine, a test can
  replace global `operator new` and assert the count is exactly zero across a
  full windowed transfer (`tests/test_alloc.cpp`).

### Layout

```
reliable-udp/
├── CMakeLists.txt          Release / Debug / ASan+UBSan / TSan, -Werror
├── include/reliable_udp/
│   ├── seq.hpp             RFC 1982 serial arithmetic
│   ├── crc32c.hpp          constexpr CRC-32C table
│   ├── config.hpp          all tunables in one struct
│   ├── packet.hpp          wire format, encode/parse
│   ├── rtt.hpp             RFC 6298 estimator
│   ├── sender.hpp          Selective Repeat sender
│   ├── receiver.hpp        Selective Repeat receiver
│   ├── stats.hpp           counters (never used for control flow)
│   ├── datagram_sink.hpp   the I/O seam
│   ├── net/                udp_socket, session_runner, server   ← production
│   └── sim/                net_sim, sim_harness                 ← test-only
├── src/                    matching implementations
├── tests/                  83 tests incl. an independent reference model
├── benchmarks/             reliability, throughput, window, concurrency
├── examples/               ru_send, ru_recv, ru_server
├── scripts/                build_all, run_tests, run_benchmarks, run_all
├── docs/                   PROTOCOL.md, INTERVIEW.md, CV_AUDIT.md
└── results/                benchmark CSV + logs (committed, reproducible)
```

Production code (`reliable_udp` target) and simulator code (`rudp_sim` target)
are **separate static libraries**, so no simulator or test scaffolding can be
linked into the shipped examples.

---

## 3. Protocol summary

Full normative spec: **[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

24-byte big-endian header, 0–8 SACK blocks, payload:

```
 0        1        2        3
+--------+--------+--------+--------+
|version | type   |sackcnt |reserved|  0
+--------+--------+--------+--------+
|           session_id              |  4
+-----------------------------------+
|              seq                  |  8
+-----------------------------------+
|              ack                  | 12
+-----------------+-----------------+
|   payload_len   |     window      | 16
+-----------------+-----------------+
|      checksum (CRC-32C)           | 20
+-----------------------------------+
|  SACK block 0 start / end  ...    | 24
+-----------------------------------+
|            payload                |
+-----------------------------------+
```

Seven packet types: `DATA SYN SYN_ACK ACK FIN FIN_ACK RST`. Sequence numbers
count **packets**, compared with wrapping serial arithmetic. Integrity is
CRC-32C over the whole datagram — UDP's optional 16-bit checksum is too weak to
be the only defence for a file transfer.

Each header field is justified individually in
[PROTOCOL.md §1](docs/PROTOCOL.md#why-each-field-exists), along with the fields
deliberately *not* included (no timestamp option, no congestion field, no TLV
area).

---

## 4. Selective Repeat design

### Why this is genuinely Selective Repeat

The difference between Go-Back-N and Selective Repeat is not the ACK format — it
is what a timeout retransmits. Here **every slot carries its own deadline**:

```cpp
struct Slot {
  std::size_t   offset;            // bytes in the source buffer, never copied
  std::uint64_t last_send_us;
  std::uint64_t rto_deadline_us;   // ← per packet, not per connection
  std::uint16_t len;
  std::uint32_t retx;
  bool in_use, acked, rtt_eligible, fast_rexmitted;
};
```

A timeout retransmits *that one packet*. The receiver buffers out-of-order data
in a ring rather than discarding it. Both properties are asserted, not assumed:

- **scenario 03** loses packets 5, 17 and 40 of a 200-packet transfer and asserts
  `retransmits == 3` exactly. Go-Back-N would retransmit ~135.
- **scenario 15** asserts out-of-order packets are buffered and
  `out_of_order > 0` while the delivered stream stays in order.
- The reliability benchmark shows the same thing at scale: at 5 % injected loss
  the retransmission rate is **4.92 %**, not the multiple of the loss rate that
  Go-Back-N would produce.

### Sender state machine

```
Idle ──start()──► SynSent ──SYN_ACK──► Established ──all data ACKed──► FinSent
                     │                      │                            │
                     │ retries exhausted    │ RST                        │ FIN_ACK
                     ▼                      ▼                            ▼
                  Failed                 Failed                       Closed
```

Established is where the work happens, driven by three rules evaluated on every
`tick()` and every inbound datagram:

1. **fill_window** — send while `snd_next - snd_base < min(send_window, rwnd)`.
2. **handle_ack** — retire everything below the cumulative ACK, mark SACKed
   slots, feed RTT samples, run loss detection.
3. **timers** — per-packet RTO, plus the persist timer if the window is closed.

### Receiver state machine

```
Listen ──SYN──► Established ──FIN (stream complete)──► Closing ──linger──► Closed
                                                          │                  │
                                                          └──FIN──┬──────────┘
                                                    restart linger,  re-ACK
                                                                 (TIME_WAIT role)
```

That last edge is not decoration. Without a `Closed` receiver still answering a
retransmitted FIN, a lost FIN_ACK made the sender exhaust its retry budget and
report failure on a transfer that had completed byte-perfectly. It was found by
randomised scenario 21, not by inspection.

### ACK mechanism — stated precisely

**Cumulative ACK + up to 8 SACK blocks + RFC 2883 D-SACK.** The SACK support is
real: blocks are ascending, merged, non-overlapping half-open ranges of packet
sequence numbers, and the sender retransmits only the gaps between them.

The strongest evidence that it is real is `tests/test_refmodel.cpp`: an
**independently written reference receiver** — `std::map`/`std::set` keyed by a
signed 64-bit index from the ISN, versus the production ring buffer and wrapping
serial arithmetic — derives the expected block list, and 200 000 randomised
arrivals assert the two agree block for block.

D-SACK reports duplicates so the sender can distinguish *reordering* from *loss*.
That distinction drives the reordering window below.

### Retransmission and RTO

Three mechanisms (detail in [PROTOCOL.md §5](docs/PROTOCOL.md#5-loss-detection-and-retransmission)):

1. **Per-packet RTO** — RFC 6298 timer per slot, exponential backoff, hard cap of
   20 retransmissions per packet so the session fails instead of retrying forever.
2. **SACK fast retransmit** (RFC 6675) — 3 SACKed packets above a hole infers
   loss, retransmit immediately, once per loss episode.
3. **RACK reordering window** (RFC 8985) — gate (2) on a *send-time* comparison
   against the newest SACKed packet, so reordering is not misread as loss. The
   window starts at SRTT/4, grows per D-SACK up to 16×, caps at one SRTT, and
   only activates once reordering has actually been seen.

RTO is textbook RFC 6298 — `SRTT + max(G, 4·RTTVAR)`, α=1/8, β=1/4 — clamped to
[20 ms, 2 s] and shifted by the backoff. **Karn's algorithm is implemented**: a
retransmitted packet is marked ineligible and contributes no RTT sample, because
its ACK is ambiguous.

The 20 ms RTO floor is a deliberate deviation from RFC 6298's 1 s floor, made
because every path here is loopback or a sub-100 ms simulated link.
[It is stated wherever recovery latency is reported](docs/PROTOCOL.md#8-deliberate-deviations).

---

## 5. Network simulator

`src/sim/net_sim.cpp` — a `SimLink` that *is* a `DatagramSink`, sitting between
two endpoints. Deterministic and seeded (`std::mt19937_64`), configurable per
direction:

```cpp
struct LinkConfig {
  double loss_prob, dup_prob, reorder_prob, corrupt_prob;
  std::uint64_t delay_us, jitter_us, reorder_extra_us;
  std::uint64_t rate_bps;              // serialisation delay
  std::size_t   queue_limit_bytes;     // drop-tail
};
```

Delivery is a priority queue ordered by `(deliver_us, insertion_order)`, so ties
break deterministically. A `ScriptFn` hook can override the probabilistic model
to force an exact fault ("drop the 7th DATA packet"), which is how the
single-loss and lost-ACK tests are made precise rather than statistical.

The simulator is itself tested (9 tests): loss/duplication/reordering rates land
within 4σ of the configured probability, the same seed produces an identical
event stream, delay+jitter land in the configured band, rate limiting actually
serialises, the drop-tail queue drops the overflow, reordering genuinely
delivers out of transmit order, and corruption flips exactly one bit.

The harness (`run_sim_transfer`) is a discrete-event loop that jumps directly to
`min(fwd.next, rev.next, sender.next_deadline, receiver.next_deadline)` and
detects deadlock when nothing has a deadline and nothing is in flight.

---

## 6. Correctness testing

**83 tests, 0 failures, under all four build configurations.**

| Group | n | What it covers |
|---|---:|---|
| `packet` | 9 | CRC-32C and SHA-256 published vectors, big-endian layout, **every single-bit flip detected**, truncation, bad version/type/reserved/sack_count, length inconsistency, 200 k random garbage datagrams |
| `seq` | 3 | serial arithmetic across the 2³² wrap, range tests, window normalisation |
| `rtt` | 5 | RFC 6298 first sample, subsequent gains, convergence, backoff, clamping |
| `sim` | 9 | the simulator's own fidelity (above) |
| `transfer` | 31 | the 22 required scenarios plus variants |
| `corrupt` | 12 | corrupted header/payload/checksum, wrong session, out-of-window seq, oversized payload, pre-handshake data, duplicate SYN, **20 k-iteration fuzzing of both endpoints**, 5 % on-wire corruption still delivering intact |
| `refmodel` | 3 | differential testing against an independent implementation |
| `alloc` | 3 | zero heap allocation on the hot path, including under loss and reordering |
| `realnet` | 8 | real loopback sockets: 4 MB byte-exact, empty transfer, 10 % injected loss, idle timeouts both directions, threaded server with 1 and 8 concurrent sessions, RST for unknown session |

Every transfer test verifies **SHA-256 of the delivered bytes against the
source**, so "byte-for-byte correct, correctly ordered, no duplication, no
corruption" is one assertion rather than a claim.

### Coverage of the required scenarios

zero loss (01) · single loss (02) · multiple loss (03) · burst loss (04) ·
duplication (05) · reordering (06, 06b) · delayed packets (07) · duplicate ACKs
(08) · delayed ACKs (09) · lost ACKs (10) · retransmission (11) · sequence
wraparound (12, 12b) · sender window exhaustion (13) · receiver window
exhaustion (14, 14b, 14c) · out-of-order buffering (15) · duplicate handling
(16) · termination (17, 17b, 17c, 17d) · empty transfer (18) · very small
transfer (19, 19b) · large transfer (20) · random fault injection (21) ·
deterministic replay (22) · combinations (23).

### The reference model

`tests/ref_model.hpp` is a second implementation written to **different data
structures on purpose**, so it cannot reproduce the same bug:

| | production | reference model |
|---|---|---|
| buffer | power-of-two ring, `seq & mask` | ordered `std::map` |
| SACKed set | flags in ring slots | `std::set` |
| index space | wrapping `uint32` serial arithmetic | signed `int64` offset from the ISN |

This caught a real divergence during development: the model's originally
*unsigned* index aliased sequence numbers below the ISN to the top of the space,
while the production receiver's serial arithmetic correctly classified them as
recent duplicates. Fixing it required deciding which behaviour was right — the
production one — and that is exactly the value of a differential test.

### Sanitizers

| Config | Flags | Result |
|---|---|---|
| Release | `-O3 -DNDEBUG` | 83/83 pass |
| Debug | `-O0 -g` | 83/83 pass |
| ASan + UBSan | `-fsanitize=address,undefined -fno-sanitize-recover=all` | 83/83 pass, **0 reports, 0 leaks** (`detect_leaks=1`) |
| TSan | `-fsanitize=thread` | 83/83 pass, **0 races** |

TSan is meaningful here rather than vacuous: the `realnet` group runs the
threaded `Server` — one rx thread, one timer thread, a `std::shared_mutex`
session table and per-session mutexes — including the **8-concurrent-session**
test, all under TSan. The binary genuinely links `libtsan`.

Compiler warnings are errors, with `-Wconversion`, `-Wshadow`, `-Wold-style-cast`,
`-Wcast-align`, `-Wdouble-promotion`, `-Wnull-dereference` and more. The build is
clean.

---

## 7. Benchmarks

Machine (captured automatically into every result file):

```
13th Gen Intel Core i5-13500H · 16 logical CPUs (12 cores) · 4700 MHz · powersave
Ubuntu 25.10 · Linux 6.17.0-41-generic x86_64
g++ 15.2.0 · -std=c++20 (__cplusplus=202002) · Release · -O3 -DNDEBUG
```

Deterministic seeds, warm-up runs discarded and separated from measured runs,
median reported with the full distribution, 5–9 repetitions per cell.

### 7.1 Reliability matrix — `bench_reliability`

4 MiB payload, 1200 B packets, window 64/128, **9 repetitions per cell, seeds
1000+rep**. Every run verified by SHA-256.

| Scenario | ok | time (ms) | goodput (Mb/s) | pkts | retx | retx % | meanRTT (µs) | final RTO (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 % loss | 9/9 | 100.0 | 335.54 | 3496 | 0 | 0.000 | 1 | 5.0 |
| 1 % loss | 9/9 | 100.0 | 335.54 | 3535 | 39 | 1.103 | 1 | 5.0 |
| 5 % loss | 9/9 | 140.0 | 239.67 | 3677 | 181 | 4.922 | 1 | 5.0 |
| 10 % loss | 9/9 | 245.0 | 136.96 | 3878 | 382 | 9.850 | 1 | 10.0 |
| 20 % loss | 9/9 | 440.0 | 76.26 | 4385 | 889 | 20.274 | 1 | 20.0 |
| 0 % + 50 ms RTT | 9/9 | 2991.7 | 11.22 | 3586 | 90 | 2.510 | 46522 | 64.9 |
| 1 % + 50 ms RTT | 9/9 | 3951.2 | 8.49 | 3600 | 104 | 2.889 | 46907 | 64.3 |
| 5 % + 50 ms RTT | 9/9 | 5857.2 | 5.73 | 3728 | 232 | 6.223 | 47372 | 121.1 |
| 10 % + 50 ms RTT | 9/9 | 7982.7 | 4.20 | 3999 | 503 | 12.578 | 47653 | 130.7 |
| 20 % + 50 ms RTT | 9/9 | 14183.0 | 2.37 | 4790 | 1294 | 27.015 | 48908 | 251.2 |
| 5 % + 10 % dup | 9/9 | 5623.7 | 5.97 | 3730 | 234 | 6.273 | 47132 | 120.1 |
| 5 % + 30 % reorder | 9/9 | 9461.5 | 3.55 | 3738 | 242 | 6.474 | 60362 | 287.8 |
| 5 % + dup + reorder | 9/9 | 9403.9 | 3.57 | 3753 | 257 | 6.848 | 58845 | 225.5 |
| 10 % + dup + reorder | 9/9 | 13226.2 | 2.54 | 3971 | 475 | 11.962 | 59511 | 273.5 |
| 20 % + dup + reorder | 9/9 | 25609.4 | 1.31 | 4634 | 1138 | 24.558 | 62701 | 1012.6 |

**135 of 135 repetitions completed with matching SHA-256. Zero failures.**

The result worth reading closely is the retransmission column on the no-delay
rows: **1 % loss → 1.103 % retx, 5 % → 4.922 %, 10 % → 9.850 %, 20 % → 20.274 %.**
Retransmission overhead tracks injected loss to within ~0.3 percentage points.
That is the quantitative signature of working selective repeat — near-zero
spurious retransmission.

On the 50 ms-RTT rows the overhead rises above the loss rate (20 % loss →
27.0 % retx), because with a long RTT more recovery is driven by timeout than by
SACK inference, and Karn's algorithm suppresses RTT samples exactly when the path
is worst. The 30 %-reorder rows cost only ~1.5 pp over the equivalent
non-reordered row (4.92 % → 6.47 %), which is what the RACK window buys.

### 7.2 Throughput — `bench_throughput`

Real UDP over loopback, `send_window=256`, `recv_window=512`, 32 MB socket
buffers requested, 1 warm-up discarded, 9/9/5 repetitions.

| Size | runs | time (ms) | goodput | pkts | retx | retx % | kernel drops | mean RTT (µs) | CPU/MB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 MB | 9 | 27.1 | **310.0 Mb/s** | 1229 | 355 | 28.885 | 253 | 2671 | 38.63 ms |
| 10 MB | 9 | 130.9 | **640.9 Mb/s** | 10634 | 1895 | 17.820 | 1739 | 1382 | 18.78 ms |
| 100 MB | 5 | 1171.8 | **715.9 Mb/s** | 104587 | 17205 | 16.450 | 16600 | 1299 | 16.80 ms |

Distribution for 100 MB: median 715.9, [min 686.9 | p25 711.8 | p75 722.6 |
max 726.1], mean 712.7, sd 15.47, n=5.

**The 16–29 % retransmission rate here is real and it is my protocol's fault.**
The `kernel drops` column (16 600 drops vs 17 205 retransmissions at 100 MB)
shows what is happening: with no congestion control, a 256-packet window on a
zero-latency path lets the sender outrun the receiver and overflow the kernel's
UDP receive buffer. The protocol recovers every one of those losses — the SHA-256
still matches — but it *caused* them. This is the single most important
limitation in the project and the numbers make it visible rather than hiding it.

Smaller transfers are slower per byte (310 vs 716 Mb/s) because handshake and
window ramp are a fixed cost amortised over fewer bytes.

### 7.3 Window size — `bench_window`

The question asked was whether increasing the window actually improves
throughput. **The measured answer is "only when the window is the binding
constraint", and on loopback it is not.**

**(A) Simulated: 25 ms one-way delay (50 ms RTT), 100 Mbit/s, 1 % loss, 8 MiB,
9 reps.** Bandwidth-delay product = **520.8 packets**.

| window | time (ms) | goodput | pkts | retx | retx % | mean RTT (µs) | stalls |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 | 46460.1 | 1.44 Mb/s | 7087 | 96 | 1.355 | 49881 | 21128 |
| 16 | 24372.0 | 2.75 Mb/s | 7100 | 109 | 1.535 | 49675 | 21128 |
| 32 | 13247.6 | 5.07 Mb/s | 7130 | 139 | 1.950 | 49579 | 21156 |
| 64 | 7446.5 | 9.01 Mb/s | 7246 | 255 | 3.519 | 49878 | 21400 |
| 128 | 4464.3 | **15.03 Mb/s** | 7900 | 909 | 11.506 | 51161 | 22986 |

Goodput scales almost linearly with window — **10.4× from window 8 to 128** —
because every one of these windows is far below the 521-packet BDP, so the
window, not the link, is the limit. Even at 128 the transfer achieves 15 Mb/s of
a 100 Mbit/s link.

The honest caveat is the retx column: **1.36 % → 11.51 %**. Larger bursts against
the simulated bottleneck queue cause drop-tail losses. With no congestion
control, the window is doing double duty as a rate limiter, and raising it trades
wasted bandwidth for completion time. **128 is not "optimal"** — it is simply the
largest window measured, and it is still under-provisioned relative to the BDP
while already paying an 11 % retransmission tax.

**(B) Real loopback, 20 MiB, no artificial delay or loss, 9 reps:**

| window | time (ms) | goodput | pkts | retx | kernel drops |
|---:|---:|---:|---:|---:|---:|
| 8 | 234.1 | 716.58 Mb/s | 17477 | 0 | 0 |
| 16 | 237.7 | 705.82 Mb/s | 17477 | 0 | 0 |
| 32 | 224.4 | 747.67 Mb/s | 17477 | 0 | 0 |
| 64 | 224.8 | 746.40 Mb/s | 17477 | 0 | 0 |
| 128 | 235.6 | 712.20 Mb/s | 17477 | 0 | 0 |

**Flat.** The spread (706–748 Mb/s) is smaller than the within-window standard
deviations (13–27 Mb/s), so there is no significant effect. On a path with
~0.1 ms RTT the BDP is under one packet; the bottleneck is per-packet syscall and
copy cost, not the window. Anyone who benchmarks window size only on loopback
will conclude window size does not matter, and they will be wrong for every real
network — which is precisely why experiment (A) exists.

(Note windows 8–128 here produce **zero** retransmissions, while §7.2 at window
256 produced 16–29 %. That is the same overrun effect, isolated.)

### 7.4 Concurrency — `bench_concurrency`

Threaded `Server`: 1 UDP socket, 1 rx thread, 1 timer thread, 8 MiB per session,
one client thread + socket each, 5 measured reps + 1 warm-up.

| sessions | ok | wall (ms) | aggregate | per-session | retx % | mean RTT (µs) | CPU/MB |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5/5 | 113.7 | 590.1 Mb/s | 798.5 Mb/s | 18.954 | 1475 | 22.23 ms |
| 2 | 5/5 | 171.8 | 781.2 Mb/s | 476.9 Mb/s | 35.504 | 1475 | 21.24 ms |
| 4 | 5/5 | 310.6 | 864.2 Mb/s | 253.7 Mb/s | 42.779 | 1622 | 23.28 ms |
| 8 | 5/5 | 614.7 | **873.3 Mb/s** | 139.4 Mb/s | 49.341 | 1682 | 25.65 ms |

Every session's payload is SHA-256-verified independently, so state isolation is
demonstrated rather than asserted.

**Aggregate throughput improves only 1.48× going from 1 to 8 sessions** — far
from linear, and per-session throughput collapses from 798 to 139 Mb/s. The
bottleneck is the single rx thread demultiplexing one socket, plus rising
contention on the receive path: retransmission climbs from 19 % to 49 % as eight
senders overrun a receive buffer no one is rate-limiting. This is a modest
result and it is reported as one.

Concurrency exists here because a server must handle multiple clients, not to
generate a benchmark number. State isolation: the session table is a `std::map`
under a `std::shared_mutex` (read-mostly on the datagram path), and each session
holds its own `std::mutex` guarding its own sender/receiver — the engines
themselves have no shared state at all.

---

## 8. Building and running

```bash
git clone <repo> && cd reliable-udp
./scripts/build_all.sh        # release, debug, asan, tsan
./scripts/run_tests.sh        # all 83 tests under all four configs
./scripts/run_benchmarks.sh   # writes results/*.csv
```

Or by hand:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)
./build/release/rudp_tests                 # --list, --filter <substring>
```

Requires CMake ≥ 3.20, a C++20 compiler (tested: g++ 15.2.0), Linux.

Example programs:

```bash
./build/release/ru_recv --port 9000 --out received.bin
./build/release/ru_send --host 127.0.0.1 --port 9000 --file input.bin
./build/release/ru_server --port 9000 --dir ./incoming    # multi-session
```

---

## 9. Resource management

- **Zero heap allocation on the packet hot path**, enforced by a test that
  replaces global `operator new`/`operator delete` with counting versions and
  asserts exactly zero allocations across a full windowed transfer — including
  under loss and reordering. Buffers are sized once at construction; the header
  is built in a fixed `head_buf_[kMaxOverhead]` member.
- **No payload copying on send.** `sendmsg` with two iovecs — header and a slice
  of the source buffer — so payload bytes go from the application's buffer to the
  kernel without an intermediate copy.
- **RAII throughout.** `UdpSocket` is move-only and closes in its destructor;
  there are no raw owning pointers and no manual `delete` in production code.
- **Syscall failures handled explicitly:** `EINTR` retried, `EAGAIN`/`ENOBUFS`
  surfaced as would-block rather than treated as fatal, short/truncated sends
  detected, `MSG_TRUNC` handled on receive, and both endpoints time out instead
  of hanging when the peer disappears (two `realnet` tests cover exactly that).
- Verified by ASan+UBSan (no leaks, no UB) and TSan (no races).

---

## 10. Limitations

Stated plainly, because they bound every claim above:

1. **No congestion control.** The largest gap. The sender is limited only by its
   window; it will happily overrun a bottleneck, which §7.2 and §7.4 measure
   directly. A production transport needs slow start and a congestion window.
2. **Unidirectional, known-length transfers.** One sender, one receiver, size
   known at `start()`. Not a general bidirectional stream.
3. **All measurements are loopback or simulated.** No real WAN, no real NIC, no
   competing traffic, no middleboxes. Loopback has no propagation delay, no
   physical loss, and an MTU of 65 536 — the simulated-link results exist because
   loopback alone would be misleading.
4. **The simulator is a model.** Independent per-packet Bernoulli loss is not
   real burst loss; a drop-tail queue is not a real router.
5. **20 ms RTO floor**, not RFC 6298's 1 s. Flatters recovery latency relative to
   an internet-tuned stack.
6. **No security.** `session_id` is a demultiplexer, not authentication. No
   encryption, no anti-spoofing.
7. **No path MTU discovery**; `max_payload` is configuration.
8. **Concurrency scales poorly** — 1.48× aggregate from 1→8 sessions, bounded by
   a single rx thread.
9. **Linux/IPv4 only.** `ppoll`, `CLOCK_MONOTONIC`, `sockaddr_in`.

---
<!-- 
## 11. Further reading in this repo

- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — normative wire format, state
  machines, ACK/SACK/D-SACK rules, RTO and RACK rules, persist timer, deliberate
  deviations.
- **[docs/INTERVIEW.md](docs/INTERVIEW.md)** — 32 questions and answers, every
  one grounded in what this code actually does.
- **[docs/CV_AUDIT.md](docs/CV_AUDIT.md)** — every claim I could make about this
  project, with its evidence, its exact benchmark configuration, and an honest
  verdict on whether it survives an interviewer pushing back. -->
