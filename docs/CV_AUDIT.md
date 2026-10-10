# Final CV claim audit

Target roles: **Quant/HFT · Systems/SDE · Performance-oriented C++**

Every claim is audited against what the repository actually contains. Nothing
here is aspirational, and no number appears that was not produced by a benchmark
in `benchmarks/` on the machine recorded in `results/bench_run.log`.

Reproduce everything with `./scripts/run_all.sh`.

**Measurement environment (recorded automatically in every result file):**
13th Gen Intel Core i5-13500H · 16 logical CPUs (12 cores) · 4700 MHz ·
`powersave` governor · Ubuntu 25.10 · Linux 6.17.0-41 · g++ 15.2.0 ·
C++20 · `-O3 -DNDEBUG`.

**Verdicts:** ✅ fully defensible · ⚠️ defensible only with a stated
qualification · ❌ do not claim.

---

## 1. Claim-by-claim audit

### 1.1 Selective Repeat ARQ ✅

| | |
|---|---|
| **Implementation** | `src/sender.cpp`: `Slot` carries its own `rto_deadline_us`; `tick()` retransmits **only** slots whose individual deadline expired. `src/receiver.cpp`: out-of-order packets stored in a `recv_window` ring at `seq & (recv_window-1)`, delivered on gap fill. |
| **Test evidence** | Scenario 03 drops packets 5, 17, 40 of a 200-packet transfer and asserts `retransmits == 3` **exactly**. Scenario 15 asserts out-of-order data is buffered (`out_of_order > 0`) while the delivered stream stays ordered. |
| **Benchmark evidence** | 5 % injected loss → **4.92 %** retransmission. Go-Back-N would retransmit a multiple of the loss rate. |
| **Conditions** | 4 MiB payload, 1200 B packets, `send_window=64`, `recv_window=128`, 9 reps, seeds 1000+rep, simulated link. |
| **Verdict** | ✅ Fully defensible. |
| **Interviewer will demand** | "Prove it isn't Go-Back-N." Answer with the per-slot deadline field *and* scenario 03's exact count of 3 — the structural and the empirical proof. |

### 1.2 SACK / D-SACK ✅

| | |
|---|---|
| **Implementation** | `SrReceiver::build_sacks()` emits up to 8 merged, non-overlapping, ascending half-open `[start,end)` blocks. `note_duplicate()` prepends an RFC 2883 D-SACK block. `is_dsack()` in `src/sender.cpp` detects both D-SACK shapes (below cumulative ACK, or subset of a later block). |
| **Test evidence** | `refmodel` compares **every emitted block list** against an independently written model across 200 k randomised arrivals. Scenarios 08, 16 cover duplicate ACKs and duplicate data. |
| **Benchmark evidence** | Indirect: the near-zero spurious retransmission in 1.1 is only achievable with working selective acknowledgement. |
| **Conditions** | Randomised ISN, window, payload size, loss and reordering; deterministic master seed. |
| **Verdict** | ✅ Fully defensible as **real** RFC 2018-style SACK, not "SACK-like". |
| **Interviewer will demand** | The deviation, which you should volunteer: RFC 2018 recommends *most-recent-block-first* so ACK loss degrades gracefully; mine are ascending, because every DATA packet is ACKed immediately and 8 blocks cover the whole receive window, so the ordering carries no extra information here. |

### 1.3 RACK-style loss detection ✅

| | |
|---|---|
| **Implementation** | `detect_sack_losses()` gates fast retransmit on a **send-time** comparison: `if (reorder_window_us > 0 && sl.last_send_us + reorder_window_us >= rack_xmit_us_) continue;`. Window starts at SRTT/4, grows one step per D-SACK up to 16×, capped at one SRTT, active only once `reordering_seen_`. |
| **Test evidence** | Scenario 06b is a direct A/B on the same workload with `reorder_window_divisor` 0 vs 4. |
| **Benchmark evidence** | 200 KiB, window 32, 5 ms link, **30 % reordering**, +3 ms reorder delay: divisor 0 → 52 retx (**23.32 %**); divisor 4 → 25 retx (**12.76 %**). |
| **Conditions** | Virtual clock, deterministic seed, single workload. |
| **Verdict** | ✅ Defensible as a **reduction**. |
| **Interviewer will demand** | Two things you must state unprompted: (a) 12.76 % is still high — reordering cost was halved, not solved; (b) **completion time was identical (146 ms) in both runs** — this buys bandwidth, not latency. Any speedup claim from RACK would be false. |

### 1.4 Adaptive RTO ✅

| | |
|---|---|
| **Implementation** | `include/reliable_udp/rtt.hpp`, RFC 6298: first sample `SRTT=R, RTTVAR=R/2`; then α=1/8, β=1/4; `RTO = clamp(SRTT + max(G, 4·RTTVAR), min, max) << backoff`, G=1 ms, backoff capped at 64×. **Karn's algorithm**: `rtt_eligible` cleared on retransmission. |
| **Test evidence** | 5 `rtt` tests: first sample, subsequent gains, convergence, exponential backoff and restoration, clamping. |
| **Benchmark evidence** | Final RTO per scenario ranges **5.0 ms** (clean link) → **1012.6 ms** (20 % loss + delay + reorder). The estimator demonstrably tracks the path. |
| **Conditions** | Reliability matrix, 9 reps/cell. |
| **Verdict** | ✅ Fully defensible as adaptive, not a fixed timeout relabelled. |
| **Interviewer will demand** | Why `min_rto_us = 20 ms` when RFC 6298 specifies a 1 s floor. Answer: deliberate measurement choice for sub-100 ms paths, documented in PROTOCOL.md §8, and it **flatters recovery latency** relative to an internet-tuned stack. |

### 1.5 Sliding window + flow control ✅ (with a hard boundary)

| | |
|---|---|
| **Implementation** | Power-of-two ring, `seq & (send_window-1)`; send while `snd_next - snd_base < min(send_window, rwnd_)`. `window` field advertises `recv_window - buffered_`. Persist timer armed on *Established, unfinished, no RTO pending, no send room*. |
| **Test evidence** | Scenario 13 (sender stalls, no overrun), 14/14b/14c (receive window driven to zero; explicit zero-window probe; window update deliberately dropped). |
| **Benchmark evidence** | Window sweep, §1.9. |
| **Verdict** | ✅ for **flow control**. ❌ for anything implying congestion control. |
| **Interviewer will demand** | The flow/congestion distinction. Flow control protects the *receiver*; nothing here protects the *network*. |

### 1.6 Loss / reordering / duplication handling ✅

| | |
|---|---|
| **Implementation** | Three-tier detection: per-packet RTO → RFC 6675 fast retransmit (3 SACKed above a hole, once per episode) → RACK gate. Duplicates dropped and D-SACKed; `max_retx_per_packet = 20` ends the session rather than retransmitting forever. |
| **Test evidence** | Scenarios 02–11, 23 cover single/multiple/burst loss, duplication, reordering, delay, duplicate ACKs, delayed ACKs, lost ACKs, repeated loss, and their combinations. |
| **Benchmark evidence** | 15-cell matrix incl. 5 %+10 % dup, 5 %+30 % reorder, and 5/10/20 % + dup + reorder. |
| **Verdict** | ✅ Defensible. |
| **Interviewer will demand** | "What stops infinite retransmission?" — a counter (20), not an assumption. |

### 1.7 Deterministic network simulation ✅

| | |
|---|---|
| **Implementation** | `src/sim/net_sim.cpp` — `SimLink` *is* a `DatagramSink`; seeded `std::mt19937_64`; per-direction loss/dup/reorder/corrupt probabilities, delay, jitter, `rate_bps` serialisation, drop-tail `queue_limit_bytes`. Delivery via priority queue on `(deliver_us, insertion_order)` so ties break deterministically. `ScriptFn` forces exact faults. |
| **Test evidence** | 9 `sim` tests validate the *simulator itself*: rates within 4σ, identical event stream from the same seed, delay/jitter band, rate limiting, queue overflow, genuine out-of-transmit-order delivery, single-bit corruption. Scenario 22 asserts bit-identical replay. |
| **Verdict** | ✅ Fully defensible, and stronger than it sounds: **testing the test harness** is the part most projects skip. |
| **Interviewer will demand** | "Isn't a simulator just marking your own homework?" Answer: it is kept in a **separate static library** (`rudp_sim`) so it cannot link into production; it is independently tested; and the reference model (1.14) is the actual guard against self-confirmation. |

### 1.8 135/135 successful transfers ✅

| | |
|---|---|
| **Evidence** | `bench_reliability` prints `0 failing repetition(s) out of 135`. 15 scenarios × 9 reps, every one SHA-256 verified against the source. |
| **Conditions** | 4 MiB, 1200 B packets, window 64/128, seeds 1000+rep. Loss 0/1/5/10/20 %, each with and without 50 ms RTT, plus dup and reorder combinations. |
| **Verdict** | ✅ Defensible. |
| **Interviewer will demand** | "Did you tune timeouts until it passed?" **No** — and this is a strong answer. Two genuine failures found during development were fixed as *protocol bugs* with dedicated regression tests: a `Closed` receiver ignoring retransmitted FINs (→ 17b, plus the TIME_WAIT-role re-ACK edge) and a zero-window deadlock (→ 14b, 14c). Say "simulated network conditions", never "on a 20 %-loss network". |

### 1.9 Window-size experiment (10.4×) ✅

| | |
|---|---|
| **Evidence** | Simulated 25 ms one-way delay (50 ms RTT), 100 Mbit/s, 1 % loss, 8 MiB, 9 reps, **BDP = 520.8 packets**: window 8→128 gives 1.44→**15.03 Mb/s**. Real loopback, 20 MiB, 9 reps: 716.6 / 705.8 / 747.7 / 746.4 / 712.2 Mb/s — **flat**, spread smaller than the 13–27 Mb/s within-window standard deviations. |
| **Verdict** | ✅ Defensible, and the *pair* is what makes it good. |
| **Interviewer will demand** | "Is 128 optimal?" **Never claim it is.** It is the largest window measured, still ~4× below the BDP, and already paying **11.5 %** retransmission versus 1.36 % at window 8 — because with no congestion control the window doubles as a rate limiter and larger bursts overflow the drop-tail queue. Correct framing: *goodput is window-limited below the BDP and unaffected above it.* |

### 1.10 Retransmission tracking within 0.3 pp ⚠️

| | |
|---|---|
| **Evidence** | No-delay rows: 1 %→**1.103 %**, 5 %→**4.922 %**, 10 %→**9.850 %**, 20 %→**20.274 %**. |
| **Verdict** | ⚠️ Defensible **only** with the zero-delay qualification. |
| **Mandatory qualification** | On the 50 ms-RTT rows overhead rises above the loss rate — 20 % loss → **27.0 %** — because with a long RTT more recovery is driven by timeout than SACK inference, and Karn's algorithm suppresses RTT samples precisely when the path is worst. Quoting only the zero-delay row without saying so is cherry-picking. Also: independent per-packet Bernoulli loss is **not** real burst loss. |

### 1.11 716 Mb/s loopback goodput ⚠️

| | |
|---|---|
| **Evidence** | 100 MB, median **715.9 Mb/s**, [min 686.9 \| p25 711.8 \| p75 722.6 \| max 726.1], mean 712.7, sd 15.47, n=5, 1 warm-up discarded. |
| **Conditions** | `send_window=256`, `recv_window=512`, 32 MB socket buffers requested, real UDP over 127.0.0.1. |
| **Verdict** | ⚠️ Weak as a headline. Two mandatory caveats. |
| **Mandatory qualification** | (1) **Loopback is not a network** — no NIC, no propagation delay, no competing traffic, 64 KB MTU; this measures the CPU/syscall path. (2) That run carried a **16.45 % retransmission rate** because a 256-packet window with no congestion control overruns the kernel receive buffer — the kernel drop counter (16 600) closely tracks the retransmit count (17 205). Quoting 716 Mb/s while silently retransmitting one packet in six invites exactly the challenge it deserves. Also 1 MB transfers reach only 310 Mb/s. |

### 1.12 Zero heap allocation per packet ✅

| | |
|---|---|
| **Implementation** | Fixed `head_buf_[kMaxOverhead]` member; ring buffers sized once at construction; `sendmsg` with 2 iovecs referencing the source buffer directly. |
| **Test evidence** | `tests/test_alloc.cpp` replaces **global `operator new`/`operator delete`** with counting versions and asserts **exactly zero** allocations across a full windowed transfer, including under loss and reordering. Includes a self-test proving the probe detects allocation (using `::operator new` through a `void* volatile` so the optimizer cannot elide it). |
| **Benchmark evidence** | CPU/MB falls 38.63 ms (1 MB) → 16.80 ms (100 MB) as fixed costs amortise. |
| **Verdict** | ✅ Fully defensible — and the **highest-value claim for HFT**, because it is proven by mechanism rather than asserted. |
| **Interviewer will demand** | Scope. Say "steady-state hot path": construction allocates, and error paths build `std::string` messages. The claim is no allocation *per packet*. |

### 1.13 83 tests ⚠️

| | |
|---|---|
| **Evidence** | 83 tests across 9 groups, 0 failures under all four build configurations. |
| **Verdict** | ⚠️ The **count** is a weak claim — test count is not test quality. |
| **Better framing** | Lead with what is distinctive: the differential reference model, 20 k-iteration fuzzing of both endpoints, exhaustive single-bit-flip detection, deterministic seeded replay, and the fact that the *simulator itself* is tested. Use 83 as supporting detail, never as the headline. |

### 1.14 Independent reference model ✅

| | |
|---|---|
| **Implementation** | `tests/ref_model.hpp` built from **deliberately different structures**: ordered `std::map`/`std::set` keyed by a **signed int64** index from the ISN, versus the production power-of-two ring and wrapping serial arithmetic. |
| **Test evidence** | 200 k randomised arrivals; receiver state, delivered stream and every SACK block compared per event. |
| **Verdict** | ✅ Fully defensible, and among the strongest signals in the project. |
| **Interviewer will demand** | "Did it ever actually catch anything?" **Yes** — tell the story: the model's originally *unsigned* index aliased sequence numbers below the ISN to the far end of the space, while the production receiver's serial arithmetic correctly classified them as recent duplicates. Resolving it required deciding which side was right. That answer is worth more than "200 k iterations passed". |

### 1.15 ASan / UBSan ✅

| | |
|---|---|
| **Evidence** | `-fsanitize=address,undefined -fno-sanitize-recover=all`, `ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1`. All 83 tests pass, **0 reports, 0 leaks**. |
| **Verdict** | ✅ Defensible. |
| **Interviewer will demand** | Whether the suite exercises anything interesting under it — it does: 20 k-iteration fuzzing of both endpoints and 200 k random garbage datagrams run under ASan/UBSan. |

### 1.16 TSan ✅

| | |
|---|---|
| **Evidence** | `-fsanitize=thread`, all 83 tests, **0 races**. |
| **Verdict** | ✅ Defensible, **but only because it is meaningful here** — this is the claim most often faked. |
| **Interviewer will demand** | "Did TSan actually run a thread?" Volunteer the answer before being asked: the `realnet` group runs the threaded `Server` — 1 rx thread, 1 timer thread, a `std::shared_mutex` session table, per-session mutexes — including the **8-concurrent-session** test, under TSan; the binary genuinely links `libtsan`. A TSan run over single-threaded code proves nothing. |

### 1.17 Concurrency / 8 sessions ⚠️

| | |
|---|---|
| **Evidence** | 1→590.1, 2→781.2, 4→864.2, 8→**873.3 Mb/s** aggregate; per-session 798.5→139.4 Mb/s; retx 19 %→49 %; 5/5 reps ok at every level, each payload independently SHA-256 verified. |
| **Verdict** | ⚠️ Claim **correctness and state isolation**, never scalability. |
| **Mandatory qualification** | Aggregate improves only **1.48×** from 1→8 sessions, bounded by the single rx thread on one socket. If asked why: `SO_REUSEPORT` with multiple rx threads and `recvmmsg` batching — as the fix, not as a claim that it already scales. |

---

## 2. Wording to reject outright

| Never say | Why |
|---|---|
| ❌ anything implying **congestion control** | Not implemented. Flow control ≠ congestion control; conflating them is the fastest way to lose an interviewer's trust. |
| ❌ **production-ready** / production transport | No congestion control, no security, no PMTU discovery, unidirectional, known-length only. |
| ❌ **real-WAN performance** / "on a lossy network" | Every measurement is loopback or simulated. No NIC, no WAN, no competing traffic, no middleboxes. |
| ❌ **faster than TCP** | Never measured against TCP, and the comparison would be unfair in this project's favour — TCP has congestion control and this does not. |
| ❌ **zero-copy** | Accurate only as "zero-copy **send path**" (`sendmsg` iovec). The receive path copies from the kernel buffer into the reassembly ring. |
| ❌ **lock-free** | The server uses a `std::shared_mutex` plus per-session `std::mutex`. |
| ❌ **tolerates 20 % real-world packet loss** | Measured against independent per-packet Bernoulli loss in a simulator. Real loss is bursty and correlated; only scenario 04 covers burst loss. Always say "simulated". |
| ❌ **eliminates spurious retransmission** | Reduced 23.3 % → 12.8 % on a heavy-reorder link. Reduced, not eliminated. |
| ❌ any **latency/speedup** claim from RACK | Completion time was identical (146 ms) with and without the reordering window. |
| ❌ **line-rate / multi-Gb/s** | Peak measured is 873 Mb/s aggregate, on loopback. |

---

## 3. The three strongest CV bullets

**1 — Protocol / algorithmic sophistication** (130 chars)

> Built a Selective Repeat ARQ transport over UDP in C++20 with SACK, D-SACK, RFC 8985 RACK loss detection and RFC 6298 adaptive RTO

**2 — Measurable reliability** (127 chars)

> Held retransmit overhead within 0.3 pp of injected loss (0-20%, zero-delay sim link); 135/135 seeded runs byte-exact by SHA-256

**3 — Systems / testing / performance engineering** (125 chars)

> Validated by 83 tests, a 200k-arrival independent reference model and ASan/UBSan/TSan; proved zero heap allocation per packet

---

## 4. Benchmark selection — what earns its place

**Selected:**

| Metric | Why it is defensible and meaningful |
|---|---|
| **0.3 pp retransmission tracking** | The single most *meaningful* number here. It is not a magnitude, it is a **signature**: retransmission tracking injected loss almost exactly is what working selective repeat looks like, and it would be impossible under Go-Back-N or with sloppy loss inference. Needs the zero-delay caveat. |
| **135/135 byte-exact** | A completion-and-correctness metric, not a performance one, so it is nearly impossible to attack on methodology. SHA-256 verification makes "correct" a single assertion rather than a claim. |
| **Zero heap allocation per packet** | Highest-value claim for HFT specifically, and unusually strong because it is proven **by mechanism** — global `operator new` replaced and counted — rather than asserted or inferred from a profile. |
| **200,000 reference-model arrivals** | Signals a testing technique most candidates have never used. Differential testing against a deliberately different implementation is the reason to trust everything else in the repo. |
| **10.4× window improvement** | Good *because it comes with a negative result*: flat on loopback. It demonstrates BDP reasoning and the discipline to report the null finding, which reads as more credible than the 10.4× alone. |

**Rejected, and why — these are large numbers that do not earn a bullet:**

| Metric | Why not |
|---|---|
| **716 Mb/s loopback** | The biggest number in the project and the weakest claim. Loopback measures the syscall path, not a transport, and the same run carried 16.45 % self-inflicted retransmission. Keep it in the README with caveats; do not lead a CV with it. |
| **83 tests** | Test *count* signals nothing about test quality. What is distinctive is the reference model and the fuzzing — which is why bullet 3 names those and lets 83 ride along as supporting detail. |
| **8 concurrent sessions** | 1.48× aggregate scaling is a mediocre result. Correctness and isolation are real, but this is not a highlight; it is an answer to have ready, not a bullet. |
| **"up to 20 % loss"** alone | Invites "real loss is bursty" immediately. Only meaningful welded to 135/135 or to the tracking result, and only with "simulated". |

---

## 5. Interview defensibility per bullet

### Bullet 1 — Selective Repeat / SACK / RACK / RTO

**Q1. "Why is this actually Selective Repeat and not Go-Back-N?"**
The distinguishing property is not the ACK format, it is **what a timeout
retransmits**. Go-Back-N keeps one timer for the oldest unacknowledged packet and
retransmits the whole window on expiry, with the receiver discarding out-of-order
data. Here every `Slot` carries its own `rto_deadline_us` and `tick()`
retransmits only that packet; the receiver buffers out-of-order data in a ring.
Empirical proof: scenario 03 loses packets 5, 17 and 40 of a 200-packet transfer
and asserts **exactly 3** retransmissions — Go-Back-N would issue ~135.

**Q2. "How do SACK and D-SACK work, and why do you need D-SACK at all?"**
The cumulative `ack` is the lowest sequence not yet received in order; up to 8
SACK blocks are merged, non-overlapping, ascending half-open ranges of packets
held out of order, so the sender retransmits only the gaps. D-SACK (RFC 2883)
reports a packet the receiver **already had**, in one of two shapes — a block
entirely below the cumulative ACK, or a block that is a subset of a later block.
It is the signal that distinguishes **reordering from loss**: without it a
reordered packet and a lost packet look identical, and the sender retransmits for
nothing.

**Q3. "Why does RACK help under reordering, and how is your RTO computed?"**
RFC 6675 declares loss on 3 SACKed packets above a hole — but 3-deep reordering
produces exactly that pattern. So fast retransmit is gated on a **send-time**
comparison against the newest packet known to have arrived: a packet sent too
recently relative to `rack_xmit_us_` is not yet declared lost. The window starts
at SRTT/4, grows per D-SACK to 16×, caps at one SRTT, and activates only once
reordering has been observed. Measured 23.32 % → 12.76 % spurious retransmission,
**with identical completion time**.
RTO is RFC 6298: `SRTT + max(G, 4·RTTVAR)`, α=1/8, β=1/4, clamped to [20 ms, 2 s],
shifted by an exponential backoff capped at 64×. Karn's algorithm is implemented
— a retransmitted packet is marked RTT-ineligible, since its ACK is ambiguous.
Be ready to defend the 20 ms floor as a deliberate deviation from RFC 6298's 1 s.

### Bullet 2 — reliability under simulated loss

**Q1. "What happens when ACKs are lost?"**
Usually nothing, and that is the point of cumulative acknowledgement: the next
ACK supersedes the lost one entirely, retiring everything the lost ACK would
have. Only if every ACK within an RTO interval is lost does the sender
retransmit — scenario 10 runs a lossy ACK path to completion. The genuinely
dangerous case is a lost **window update** on a closed window, which is the
persist timer's job.

**Q2. "How does the persist timer prevent deadlock?"**
If the receive window closes and the reopening ACK is lost, both sides wait
forever. The subtle part is the arming condition — it is **not** "window == 0",
it is *Established, unfinished, no RTO pending, and no room to send*. A debug
harness during development showed the sender with 32 packets in flight and
`next_deadline_us() == UINT64_MAX`, because every in-flight packet had been
SACKed so no RTO was armed, while the window was closed. One lost window update
would have hung the connection permanently. Tests 14b and 14c are the
regressions.

**Q3. "Why no congestion control, and isn't 20 % loss unrealistic?"**
No congestion control is a scope decision, and I report it as the project's
largest gap rather than hiding it — it is directly visible as the 16–29 %
retransmission rate on loopback, where a 256-packet window overruns the kernel
receive buffer. On the loss model: it is **independent per-packet Bernoulli
loss in a simulator**, which is not real burst loss; the only burst coverage is
scenario 04's contiguous drop. The honest claim is "recovers correctly under
simulated loss up to 20 %", not "tolerates 20 % real-world loss".

### Bullet 3 — testing and performance engineering

**Q1. "How did you verify zero allocation per packet?"**
`tests/test_alloc.cpp` replaces **global `operator new` and `operator delete`**
with counting versions and asserts the count is exactly zero across a full
windowed transfer, including under loss and reordering. It contains a self-test
proving the probe actually detects allocation — via `::operator new` through a
`void* volatile`, because the optimizer elided a naive `new int(5)`. Buffers are
sized once at construction, the header is built in a fixed
`head_buf_[kMaxOverhead]` member, and `sendmsg` takes two iovecs so payload bytes
are never copied before the kernel. Scope it to the steady-state path.

**Q2. "Was TSan meaningful, or did it just never run a thread?"**
Meaningful, and worth volunteering. The `realnet` group runs the threaded server
— one rx thread, one timer thread, a `std::shared_mutex` session table, per-session
mutexes — including the 8-concurrent-session test, under TSan. That is genuinely
shared mutable state across two threads. The binary links `libtsan`.

**Q3. "Why is loopback throughput not representative, and why does concurrency
only scale 1.48×?"**
Loopback has no NIC, no propagation delay, no competing traffic and a 64 KB MTU,
so its BDP is under one packet — it measures per-packet syscall and copy cost,
not transport performance. That is exactly why the window sweep was run **twice**:
flat on loopback, 10.4× on a 50 ms-RTT simulated link. Concurrency is bounded by
a single rx thread demultiplexing one socket; retransmission climbs 19 %→49 % as
eight senders overrun an unrate-limited receive path. The fix would be
`SO_REUSEPORT` with multiple rx threads plus `recvmmsg` batching.

---

## 6. Final CV wording

### A. Three bullets (full)

1. Built a Selective Repeat ARQ transport over UDP in C++20 with SACK, D-SACK, RFC 8985 RACK loss detection and RFC 6298 adaptive RTO
2. Held retransmit overhead within 0.3 pp of injected loss (0-20%, zero-delay sim link); 135/135 seeded runs byte-exact by SHA-256
3. Validated by 83 tests, a 200k-arrival independent reference model and ASan/UBSan/TSan; proved zero heap allocation per packet

### B. Shorter versions

1. Built a Selective Repeat ARQ transport over UDP in C++20: SACK/D-SACK, RACK loss detection, RFC 6298 adaptive RTO
2. Held retransmit overhead within 0.3 pp of injected loss (0-20% sim); 135/135 seeded runs verified byte-exact
3. Validated by 83 tests, a 200k-arrival reference model and ASan/UBSan/TSan; proved zero heap allocation per packet

### C. Character counts

| Bullet | Full | Short |
|---|---:|---:|
| 1 | **130** | **113** |
| 2 | **127** | **108** |
| 3 | **125** | **113** |

No factual content was altered to hit the range. The short forms drop only
qualifiers that are recoverable in conversation — except bullet 2's short form,
which loses "zero-delay"; if you use it, **be ready to volunteer that caveat
immediately**, because it is the one an interviewer can legitimately call
cherry-picking.

### D. LaTeX

```latex
\begin{itemize}\itemsep0pt
  \item Built a Selective Repeat ARQ transport over UDP in C++20 with SACK,
        D-SACK, RFC 8985 RACK loss detection and RFC 6298 adaptive RTO
  \item Held retransmit overhead within 0.3\,pp of injected loss (0--20\%,
        zero-delay sim link); 135/135 seeded runs byte-exact by SHA-256
  \item Validated by 83 tests, a 200k-arrival independent reference model and
        ASan/UBSan/TSan; proved zero heap allocation per packet
\end{itemize}
```

Short variant:

```latex
\begin{itemize}\itemsep0pt
  \item Built a Selective Repeat ARQ transport over UDP in C++20: SACK/D-SACK,
        RACK loss detection, RFC 6298 adaptive RTO
  \item Held retransmit overhead within 0.3\,pp of injected loss (0--20\% sim);
        135/135 seeded runs verified byte-exact
  \item Validated by 83 tests, a 200k-arrival reference model and
        ASan/UBSan/TSan; proved zero heap allocation per packet
\end{itemize}
```

*(`\%` and `\,` are escaped; `--` is an en dash. Reflow to your template's
column width.)*

---

## 7. Final verdict

### As a Quant/HFT recruiter would read it

**Strongest:** zero heap allocation per packet, *proven by replacing global
`operator new`*, plus the deterministic seeded simulator. Determinism and
allocation discipline are the two things this industry actually screens for, and
both are demonstrated by mechanism rather than asserted.
**Weakest:** no latency distribution. There is no p99/p99.9 tail-latency
measurement anywhere — throughput and completion time only. For HFT that is the
conspicuous gap.
**Biggest risk:** being asked for tail latency and having only medians.
**Strongest story:** the zero-window deadlock — `next_deadline_us() == UINT64_MAX`
with 32 packets in flight because every one had been SACKed, found with a debug
harness, fixed with a persist timer whose arming condition is deliberately not
"window == 0".

### As a Systems/SDE recruiter would read it

**Strongest:** the differential reference model and the honesty of the
limitations section. Building a second implementation from deliberately different
data structures, and then documenting a 1.48× concurrency result as mediocre, is
senior behaviour.
**Weakest:** unidirectional and known-length-only — it is a file-transfer engine,
not a transport.
**Biggest risk:** "why not just use TCP?" Have the QUIC/head-of-line-blocking
answer ready, and concede you would use TCP for this workload.
**Strongest story:** the reference-model index-aliasing divergence, where fixing
it required deciding which implementation was correct.

### As a C++ performance engineer would read it

**Strongest:** the I/O seam. Pure-logic engines taking `now_us` as a parameter and
emitting through a `DatagramSink` is what makes determinism, fast tests, the
allocation probe, and running identical code on real and simulated links all fall
out of one decision. That is architecture, not plumbing.
**Weakest:** no profiling data. "Per-packet syscall cost dominates" is inferred
from the flat loopback window sweep, not measured with `perf`.
**Biggest risk:** "where does the time actually go?" — you have an inference, not
a flame graph.
**Strongest story:** the RACK/D-SACK work — a measured 23.32 % → 12.76 % reduction
in spurious retransmission **with identical completion time**, and the discipline
to report that it bought bandwidth rather than latency.

### Cross-cutting

The biggest single interview risk across all three audiences is **the absence of
congestion control**, because it is visible in your own throughput numbers as a
16–29 % retransmission rate. Do not let an interviewer discover it — state it
first. Volunteering "my 716 Mb/s run retransmitted one packet in six because I
have no congestion control, and here is the kernel drop counter that proves it"
converts the project's largest weakness into evidence of measurement rigour.

---

## STOP — the project is sufficient for the CV.

No further implementation work is justified. The gaps identified above
(congestion control, tail-latency measurement, `perf` profiling, real-network
validation) are **interview answers, not tasks**. Each is more valuable as a
crisp explanation of a known limitation than as a half-built feature, and adding
any of them would enlarge the surface an interviewer can probe without
strengthening any of the three bullets.
