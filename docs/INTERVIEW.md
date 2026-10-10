# Interview questions

Thirty-two questions with answers grounded in **this** implementation. Where the
honest answer is "this project doesn't do that", the answer says so — those are
the questions most likely to be asked, and a confident wrong answer is worse than
a clear limitation.

---

## UDP vs TCP

### 1. Why build reliability on UDP when TCP already does this?

Two legitimate reasons and one for this project specifically. Legitimately: you
want reliability *semantics you choose* — partial reliability, message
boundaries, or per-stream loss isolation (QUIC exists for exactly this: TCP's
single byte-stream means one lost packet head-of-line-blocks every multiplexed
stream above it). Or you need to control the retransmission and pacing policy,
which TCP hides in the kernel.

For this project, the reason is that implementing it is the only way to actually
understand it. I would not ship this instead of TCP for a file transfer — it has
no congestion control, which §7.2 of the README shows causing a 16–29 %
retransmission rate on loopback.

### 2. What does TCP give you that this does not?

Congestion control (the big one), bidirectional streams, path MTU discovery,
Nagle/delayed-ACK interaction tuning, PAWS, window scaling, decades of
middlebox compatibility, and kernel-side zero-copy paths. Also: TCP does not
require the length to be known up front, and it segments by bytes so it can
re-segment on retransmission. This implementation numbers *packets*, so it
cannot.

### 3. Why is UDP's checksum not enough?

It is 16-bit ones-complement — weak against multi-bit errors and burst errors —
and over IPv4 it is *optional*; a sender may transmit zero to disable it
entirely. For a file transfer that is unacceptable, so every datagram carries a
CRC-32C over the whole packet with the checksum field zeroed. The test
`packet :: every single-bit flip is detected` verifies that exhaustively over
representative packets.

---

## Selective Repeat vs Go-Back-N

### 4. What actually distinguishes Selective Repeat from Go-Back-N?

Not the ACK format — what a *timeout* retransmits. Go-Back-N has one timer for
the oldest unacknowledged packet and retransmits the entire window on expiry;
the receiver discards anything out of order. Selective Repeat gives each packet
its own timer and retransmits only that packet; the receiver buffers out-of-order
data.

### 5. Prove yours is Selective Repeat.

Three ways. **Structurally:** `Slot` carries a per-packet `rto_deadline_us`, and
`tick()` retransmits only slots whose own deadline expired. **By test:**
scenario 03 drops packets 5, 17 and 40 of a 200-packet transfer and asserts
`retransmits == 3` exactly — Go-Back-N would retransmit roughly 135. **By
measurement:** in the reliability matrix, 5 % injected loss produces a 4.92 %
retransmission rate. Go-Back-N's would be a multiple of the loss rate.

### 6. What does Selective Repeat cost you?

Receiver memory — you must buffer out-of-order data, so the receive window is a
real allocation (`recv_window` packets, 128 by default). More sender state: a
per-packet timer and ACK status rather than a single base pointer. And more
complexity in loss detection, since "the base didn't advance" is no longer a
sufficient signal — which is where SACK and the reordering window come in.

---

## Sequence numbers

### 7. Bytes or packets, and why?

Packets. The payload length is known up front and the stream is cut into
fixed-size pieces, so a packet index is sufficient, one SACK block covers a run
of packets in 8 bytes regardless of their sizes, and the receiver's ring can be
indexed directly by `seq & mask`.

The cost: I cannot re-segment on retransmission. TCP can coalesce two small
segments into one larger retransmission because it numbers bytes; I must
retransmit the identical packet. For fixed-size segmentation that costs nothing.

### 8. How do you compare sequence numbers across the 32-bit wrap?

RFC 1982 serial arithmetic — subtract in unsigned, interpret the result as
signed:

```cpp
inline std::int32_t seq_diff(seq_t a, seq_t b) {
  return static_cast<std::int32_t>(a - b);
}
```

`a` is "before" `b` iff `seq_diff(a,b) < 0`. Nothing in the codebase compares
sequence numbers with a bare `<`. Scenarios 12 and 12b start the ISN just below
2³²−1 so the transfer crosses zero mid-flight, and the reference-model tests
randomise the ISN across the whole space.

### 9. What breaks if the window exceeds half the sequence space?

Serial arithmetic becomes ambiguous — "before" and "after" stop being
distinguishable, because the signed difference wraps to the wrong sign. With a
32-bit space and a window of at most a few hundred packets, this is ~7 orders of
magnitude away. It is a real constraint, not a live risk.

### 10. Why an initial sequence number instead of always starting at zero?

So a delayed datagram from a previous session on the same port cannot be
misinterpreted as current data. It is the same reasoning as TCP's ISN, minus the
security dimension — mine is not randomised for anti-spoofing, and I would not
claim it is.

---

## Acknowledgement

### 11. Exactly what does your ACK carry?

A cumulative ACK (`ack` = lowest sequence number not yet received in order) plus
up to 8 SACK blocks, plus RFC 2883 D-SACK. Blocks are half-open `[start, end)`
ranges of packet sequence numbers, ascending from the lowest, merged so they are
non-overlapping and non-adjacent.

### 12. Is that "SACK-like" or real SACK?

Real, in the RFC 2018 sense: the sender learns precisely which packets arrived
and retransmits only the gaps. The strongest evidence is
`tests/test_refmodel.cpp` — an independently written reference receiver using a
`std::map` and a signed int64 index space derives the expected block list, and
200 000 randomised arrivals assert the production receiver's blocks match it
exactly.

One deviation I would volunteer: RFC 2018 recommends putting the most recently
received block *first* so ACK loss degrades gracefully. Mine are ascending,
because every DATA packet is ACKed immediately and 8 blocks cover the receive
window, so the ordering carries no extra information here.

### 13. What is D-SACK and why did you implement it?

RFC 2883: reporting a block the receiver *already had*, so the sender learns a
duplicate arrived. Two shapes — a block entirely below the cumulative ACK, or a
block that is a subset of a later block.

I implemented it because it is the signal that distinguishes **reordering from
loss**. Without it, a reordered packet looks identical to a lost one, and the
sender retransmits for nothing. It drives the RACK reordering window (Q19).

### 14. Why ACK every packet instead of every other packet?

`ack_every` is configurable and the delayed-ACK path exists (scenario 09 tests
it), but the default is 1. For a unidirectional bulk transfer the ACK stream is
the cheap direction, and immediate ACKs give the RTT estimator the maximum number
of samples — which matters because Karn's algorithm already discards samples from
retransmitted packets.

### 15. What happens if ACKs are lost?

Very little, which is the point of cumulative acknowledgement: the next ACK
supersedes the lost one entirely. A later ACK carrying a higher cumulative value
retires everything the lost ACK would have. Only if *every* ACK in an RTO
interval is lost does the sender retransmit — scenario 10 runs a lossy ACK path
and completes. The dangerous case is a lost *window update* on a closed window,
which is what the persist timer exists for (Q22).

---

## Retransmission and timers

### 16. Walk me through your three loss-detection mechanisms.

1. **Per-packet RTO.** Each slot gets `now + RTO` on transmission; expiry
   retransmits that one packet, doubles the backoff, and marks the slot
   RTT-ineligible.
2. **SACK fast retransmit (RFC 6675).** If ≥3 packets *above* an unacknowledged
   packet have been SACKed, infer loss and retransmit immediately without waiting
   for the RTO. Once per loss episode per slot (`fast_rexmitted`), so a burst of
   ACKs can't retransmit the same packet repeatedly.
3. **RACK reordering window (RFC 8985).** Gates (2) — see Q19.

### 17. How is your RTO computed?

RFC 6298 exactly. First sample: `SRTT = R`, `RTTVAR = R/2`. Then with α=1/8,
β=1/4:

```
RTTVAR = (1-β)·RTTVAR + β·|SRTT - R|
SRTT   = (1-α)·SRTT   + α·R
RTO    = clamp(SRTT + max(G, 4·RTTVAR), min_rto, max_rto) << backoff
```

G is the clock granularity, 1 ms. Backoff doubles per timeout, capped at 64×, and
resets on a valid sample. Before the first sample, `init_rto_us` = 200 ms.

I want to be clear this is *adaptive*, not a fixed timeout dressed up: the
reliability benchmark reports the final RTO per scenario, and it ranges from
5 ms on a clean zero-delay link to 1012.6 ms on 20 % loss + delay + reordering.
The estimator tracks the path.

### 18. What is retransmission ambiguity, and how do you handle it?

If a packet was sent twice and you get an ACK, you cannot tell which
transmission it acknowledges. Sampling the wrong one corrupts SRTT — badly, in
the direction that makes the timer too short, which causes more spurious
retransmission and more ambiguity.

**Karn's algorithm:** a slot is `rtt_eligible` when first transmitted and the
flag is cleared on retransmission, so an ACK covering a retransmitted packet
contributes no sample. The exponential backoff supplies the timer adaptation the
missing samples would otherwise provide.

### 19. How do you avoid mistaking reordering for loss?

Pure RFC 6675 counts 3 SACKed packets above a hole as loss — but 3-deep
reordering produces exactly that pattern. So the fast-retransmit decision is
gated on a **send-time** comparison, RACK-style:

```cpp
if (reorder_window_us > 0 && sl.last_send_us + reorder_window_us >= rack_xmit_us_)
    continue;   // too recently sent relative to what we know arrived
```

where `rack_xmit_us_` is the send time of the most recently transmitted packet
known to have arrived. The window starts at SRTT/4, grows by that step on each
D-SACK up to 16×, caps at one SRTT, and only activates once reordering has
actually been observed.

**Measured, on 200 KiB / window 32 / 5 ms link / 30 % reordering:** spurious
retransmission fell from **23.32 % to 12.76 %** (52 → 25 retransmissions).
Completion time was identical at 146 ms in both — this buys bandwidth, not
latency. And 12.76 % is still high; I have not solved reordering, I have halved
its cost.

### 20. What stops infinite retransmission?

`max_retx_per_packet` = 20. Past that the session fails with an explicit error
rather than retrying forever. Handshake packets have their own budget
(`handshake_retries` = 10) — scenario 17d asserts the handshake gives up rather
than hanging. Every "no infinite retransmission" property in this project is a
counter, not an assumption.

---

## Windows and flow control

### 21. How does the sender's window work?

A power-of-two ring of `send_window` slots indexed by `seq & (send_window-1)`.
`snd_base_` is the oldest unacknowledged sequence, `snd_next_` the next to send;
the sender may transmit while
`snd_next_ - snd_base_ < min(send_window, rwnd_)`. Power-of-two sizing (enforced
by `Config::normalize()`) makes indexing a mask rather than a modulo.

### 22. What is the persist timer and what bug did it fix?

If the receive window closes and the ACK that reopens it is lost, both sides wait
forever — a permanent deadlock. TCP solves this with a persist timer; so do I.

The subtle part is the arming condition. It is **not** "the window is zero" — it
is *Established, unfinished, no RTO armed, and no room to send*. A debug harness
during development showed the sender with 32 packets in flight and
`next_deadline_us() == UINT64_MAX`, because every in-flight packet had been
SACKed, so no RTO was pending — and the window was closed. One lost window update
would have hung it permanently. Test 14b unit-tests a SYN_ACK advertising window
0; test 14c deliberately drops the window update.

### 23. Flow control vs congestion control — which do you have?

Flow control only. The `window` field advertises `recv_window - buffered_`, i.e.
genuine free space in packets, and the sender clamps to it. That protects the
*receiver*.

There is **no congestion control** — nothing protects the *network*. No
congestion window, no slow start, no AIMD, no loss-triggered rate reduction. The
consequence is measured, not hypothetical: on loopback with a 256-packet window,
the sender overruns the kernel's UDP receive buffer and drives a 16–29 %
retransmission rate, with the kernel drop counter (16 600) closely matching the
retransmission count (17 205) at 100 MB. This is the largest gap between this
and a production transport.

### 24. Does a bigger window make it faster?

**It depends entirely on the bandwidth-delay product, and I measured both sides.**

On a simulated 50 ms-RTT, 100 Mbit/s link (BDP = 520.8 packets), goodput scaled
almost linearly — 1.44 → 15.03 Mb/s from window 8 to 128, a 10.4× improvement —
because every window tested was far below the BDP.

On real loopback with ~0.1 ms RTT, it was **flat**: 706–748 Mb/s across windows
8 through 128, a spread smaller than the within-window standard deviation. The
BDP is under one packet; the bottleneck is per-packet syscall and copy cost.

I would not claim 128 is optimal. It was the largest I measured, it is still
under-provisioned relative to that BDP, and it already pays an 11.5 %
retransmission tax from drop-tail losses versus 1.36 % at window 8 — because
with no congestion control the window is doubling as a rate limiter.

### 25. How does the receiver handle out-of-order data?

A ring of `recv_window` reassembly slots indexed by `seq & (recv_window-1)`. An
in-window packet is stored there; when `rcv_base` becomes filled, the receiver
walks forward delivering every contiguous packet and advancing `rcv_base`.
Out-of-window sequence numbers are counted (`out_of_window`) and dropped;
duplicates within a window below the base are counted and D-SACKed.

---

## Integrity and robustness

### 26. What happens to a malformed packet?

`parse_packet()` validates before anything is applied: minimum length, version,
type range, `reserved == 0`, `sack_count ≤ 8`, then the exact length identity
`24 + 8·sack_count + payload_len == len`, then CRC-32C. Only then does a caller
read a field. Failures are counted by category (`rx_bad_checksum`,
`rx_bad_version`, `rx_bad_session`, `rx_malformed`) and the datagram dropped.

Verified by fuzzing: 20 000 mutated packets thrown at both endpoints, plus
200 000 random garbage datagrams at the parser, asserting no crash, no
out-of-bounds read, and no state change. All of it also runs under ASan+UBSan.

### 27. Someone injects a packet with a valid checksum but a wrong session ID?

Counted as `rx_bad_session` and dropped; a server replies RST for a session it
does not know. But I want to be precise about what this is *not*: `session_id` is
a demultiplexer, not a security boundary. It is a fixed guessable value, there is
no authentication and no encryption, and anyone who can inject a datagram with
the right id can inject data or reset the session. Making this secure means
DTLS or a QUIC-style authenticated handshake.

---

## Sockets, threading, performance

### 28. How do you get sub-millisecond timer granularity?

`ppoll` rather than `poll` — it takes a `struct timespec`, so timeouts have
nanosecond resolution rather than `poll`'s integer milliseconds. With a 20 ms
minimum RTO and RTTs in the hundreds of microseconds on loopback, millisecond
rounding would be a large fraction of the interval. All timestamps come from
`CLOCK_MONOTONIC`, which is immune to wall-clock adjustments.

### 29. Where does concurrency live, and how is state isolated?

Only in the server, and only because a server must serve multiple clients — I
did not add threads for a benchmark number. One rx thread reads the single
socket and demultiplexes by session id; one timer thread drives per-session
`tick()`. The session table is a `std::map` under a `std::shared_mutex`
(read-mostly on the datagram path); each session holds its own `std::mutex`
guarding its own sender/receiver. The engines themselves have **no** shared
mutable state.

That is genuinely shared state under two threads, which is what makes TSan
meaningful rather than vacuous — the 8-concurrent-session test runs under TSan
and reports no races.

Scaling is honestly mediocre: aggregate throughput goes 590 → 873 Mb/s from 1 to
8 sessions, only 1.48×, bounded by the single rx thread. Fixing it means
`SO_REUSEPORT` with multiple rx threads, or `recvmmsg` to amortise syscalls.

### 30. Where does the CPU actually go, and how do you know you're not allocating?

Per-packet syscall and copy cost dominates — that is the conclusion from the
loopback window sweep being flat at ~700 Mb/s regardless of window, and from
CPU/MB falling from 38.6 ms at 1 MB to 16.8 ms at 100 MB as fixed costs amortise.

Allocation is *proven* zero on the hot path, not assumed: `tests/test_alloc.cpp`
replaces global `operator new`/`operator delete` with counting versions and
asserts exactly zero allocations across a full windowed transfer, including under
loss and reordering. Buffers are sized once at construction and the header is
built in a fixed `head_buf_[kMaxOverhead]` member. Sends use `sendmsg` with two
iovecs — header plus a slice of the source buffer — so payload bytes are never
copied before the kernel.

The next optimisation would be `sendmmsg`/`recvmmsg` to batch syscalls, which is
where the remaining time is.

---

## Methodology

### 31. How do I know your benchmark numbers mean anything?

Each result file records CPU model and topology, max frequency, scaling
governor, OS, kernel, compiler and version, `__cplusplus`, build type, and
optimisation flags, plus payload size, packet size, window sizes, loss/dup/
reorder rates, delay, jitter, seed and repetition count. Seeds are deterministic.
Warm-up runs are discarded and separated from measured runs. Every cell is 5–9
repetitions and I report the **median with min/p25/p75/max/mean/stddev**, not a
best run. Per-repetition data is in `results/*.csv` so the distribution can be
checked rather than taken on trust.

The honest caveats: `powersave` governor with turbo means frequency is not
pinned, and everything is loopback or simulated — no real NIC, no WAN, no
competing traffic. The simulated results model a network; they do not measure
one.

### 32. What would you do next?

In order: (1) congestion control — it is the one thing whose absence corrupts the
throughput numbers; NewReno first, then something delay-based. (2) `sendmmsg`/
`recvmmsg` batching, which the flat loopback window sweep says is where the time
is. (3) Multiple rx threads with `SO_REUSEPORT` to fix the 1.48× concurrency
scaling. (4) Real-network measurement, because everything here is loopback or
simulated.
