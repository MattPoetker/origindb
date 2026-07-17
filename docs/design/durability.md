# Design: Durable WAL with group commit + fsync

Status: **IMPLEMENTED** (2026-07-17). The write path in `src/storage/` now uses
a dedicated WAL writer thread with opportunistic group commit and per-batch
`fdatasync` / `F_FULLFSYNC`. Default durability is `fsync` (power-loss safe).

Measured on the dev Mac (F_FULLFSYNC ≈ 21ms/sync — deliberately slow, it really
waits for the drive):

| Benchmark | ops/s | Note |
|---|---|---|
| `wal_commit_fsync_1t` | 46 | one durable commit ≈ one fsync (p99 ~42ms) |
| `wal_commit_fsync_8t` | 185 | **4.0× the 1t number** — group commit working |
| `wal_commit_flush_1t` | 8574 | flush control |
| `storage_insert` (flush) | 8187 | unchanged vs flush-era baseline — no regression |

The 8t/1t ratio meets the design gate (≥4×), proving concurrent commits coalesce
behind one fsync. Crash-recovery is covered by `WalDurability.*` unit tests
(concurrent-commit replay + a SIGKILL-mid-write test asserting every acked
commit survives). On Linux with `fdatasync` and a battery-backed controller the
absolute numbers are far higher; F_FULLFSYNC is the worst realistic case.

What follows is the original design (all of it landed).

## Problem

Two defects in the current commit path (`StorageEngine::Impl::Commit`,
`WALImpl::Append`):

1. **No fsync — "committed" writes can vanish on power loss.** `WALImpl::Append`
   does `wal_file_ << json << "\n"` and, when `sync_wal` is set,
   `wal_file_.flush()`. `flush()` only pushes the C++ `ofstream` buffer into
   the OS page cache; it does **not** call `fsync`/`fdatasync`. A power cut or
   kernel panic between the write and the OS flushing its page cache loses
   data the server already acknowledged as committed. This is a correctness
   bug, not a performance one.

2. **Serial, per-write IO under a global lock caps throughput at ~9k writes/s.**
   `Commit` holds `tables_mutex_` (one global write lock for the whole engine)
   across the entire write set, and calls `wal_->Append` — which does its own
   file write + flush — once per write, inline. Every committing thread
   therefore serializes on both the lock and its own synchronous IO. The
   benchmark suite shows `wasm_invoke_write_commit` and `storage_insert` both
   pinned near 9k/s regardless of concurrency (multi-module scaling flatlines
   at ~4 threads). Adding a real `fsync` per commit here would make it
   *dramatically worse* — a single spinning disk fsync is ~10ms; even an SSD
   is ~0.1–1ms, which would drop us to hundreds of commits/s.

The two fixes are coupled: real durability requires fsync, and fsync is only
affordable if we **amortize one fsync across many concurrent commits** —
group commit.

## Goals

- A configurable durability level, defaulting to **truly durable** (fsync per
  group).
- `fsync` cost amortized across all commits that arrive within a small window,
  so throughput *rises* with concurrency instead of flatlining.
- WAL IO moved **out** of `tables_mutex_` so memtable application and disk IO
  no longer serialize each other.
- No change to the on-disk WAL format (recovery stays compatible).
- Crash-consistency: after recovery, the database reflects exactly the set of
  transactions whose commit was acknowledged to the caller — no torn writes,
  no partially-applied transactions.

## Non-goals (this phase)

- Replication / multi-node durability (Raft). Separate effort.
- WAL encryption or compression. (The JSON-lines format has since been
  replaced with a compact binary codec — see row_codec.{h,cpp} — which cut WAL
  size ~57% and sped recovery ~4x; encryption/compression remain future work.)
- Checksums per record (worth doing eventually to detect torn tails; noted as
  a follow-up, not required for group commit).

## Durability levels (config)

`StorageConfig::sync_mode`, replacing the current boolean `sync_wal`:

| Mode | Semantics | Use |
|---|---|---|
| `none` | append to WAL, never flush explicitly (OS decides) | tests, throwaway data |
| `flush` | flush the userspace buffer to the OS per group (current behavior, no fsync) | today's default; survives process crash, not power loss |
| `fsync` | `fdatasync` the WAL per group before acknowledging | **new default**; survives power loss |

`fdatasync` (not `fsync`) because the WAL is append-only to an existing file —
we don't need to persist inode metadata changes like mtime on every commit,
only the data blocks and the file size. On Linux `fdatasync` still persists
size changes. On macOS, `fdatasync` maps to `fsync`; for true power-loss
durability there we additionally need `fcntl(fd, F_FULLFSYNC)` (plain fsync on
macOS does not guarantee the drive flushed its cache) — expose that as a
build/runtime detail, defaulting to `F_FULLFSYNC` on Darwin.

## Design: a dedicated WAL writer with group commit

### Shape

Replace the inline `wal_->Append(...)` calls in `Commit` with a **submit +
wait on a durable batch** protocol against a single background WAL writer
thread.

```
commit thread(s)                     WAL writer thread
----------------                     -----------------
apply writes to memtable
  (under tables_mutex_, no IO)
build WALEntry vector for txn
enqueue {entries, promise} ----->    loop:
wait on future  <-------.              drain all queued submissions
                        |              write every entry to the WAL file
                        |              ONE fdatasync for the whole batch
                        '------------  fulfil every waiter's promise
acknowledge commit to caller
```

Key points:

- **One fsync per batch, not per commit or per write.** Under load, dozens of
  commits coalesce into a single `fdatasync`. This is the classic group-commit
  win: fsync cost is paid once per ~batch, so effective commit throughput is
  `batch_size / fsync_latency` instead of `1 / fsync_latency`.
- **WAL IO leaves the tables lock.** Memtable application still happens under
  `tables_mutex_` (fast, in-memory), but the durability wait happens *after*
  releasing it. The writer thread does all file IO; commit threads never touch
  the file. (Ordering caveat below.)
- **Batching is opportunistic, not timer-padded.** The writer drains whatever
  is already queued, fsyncs, then drains again. Under low load a lone commit
  is fsync'd immediately (latency ≈ one fsync). Under high load the queue
  naturally fills while the previous fsync is in flight, so batches grow
  exactly as much as contention demands — no artificial delay. (An optional
  `group_commit_window_us` can force a tiny wait to enlarge batches on very
  fast storage; default 0.)

### Ordering & the visibility question

This is the subtle part. Today, a write is visible in the memtable the instant
`Commit` applies it, and the changefeed event fires right after — both *before*
any durability guarantee. If we keep that, a reader (or subscriber) can observe
a write that a subsequent crash then loses ("read-your-writes" of non-durable
data). Options, in order of preference:

1. **Apply-then-wait, publish-after-durable (recommended).** Apply to the
   memtable under the lock (so in-process reads see it immediately and the
   global offset order is fixed there), enqueue the WAL entries, release the
   lock, then block the committing thread until the batch is durable. Only
   after the durability wait returns do we (a) return success to the caller and
   (b) publish changefeed events. This means: external observers (API callers,
   subscribers) only ever see durable data, while the brief in-process window
   where the memtable is ahead of the WAL is invisible to them because the
   commit call hasn't returned yet. Crash in that window = the memtable state
   is discarded on restart (we recover from the WAL), and no one was told the
   commit succeeded. This is correct and is what most embedded engines do.

2. Write-ahead strictly (WAL durable *before* memtable apply). Stronger, but
   forces the fsync inside the critical path before the row is visible even
   in-process, hurting latency and complicating the global-offset assignment.
   Not worth it for a single node.

Recommendation: option 1. It preserves the current global-offset ordering
(assigned under the lock), keeps in-memory reads fast, and makes the only
externally-visible data durable.

### Assigning the WAL sequence / changefeed offset

Sequence numbers must be assigned in commit order and must match the order
entries hit the file. Assign them **while holding `tables_mutex_`** (as today),
enqueue the pre-sequenced entries, and have the writer preserve enqueue order
(a FIFO). The changefeed offset is likewise assigned under the lock; events are
buffered on the commit thread and only handed to `ChangefeedEngine::PublishEvent`
after the durability wait, preserving "subscribers see only durable, in-order
events."

### Failure handling

- **fdatasync fails** (EIO, ENOSPC): the writer marks the batch failed and
  fulfils every waiter's promise with an error. Those commits return failure to
  the caller; the memtable rows they applied must be rolled back. Because this
  is now *after* memtable application, rollback means re-deriving the previous
  state — which we don't currently retain per row. **Mitigation:** on a WAL
  write/fsync error, treat it as fatal for durability integrity — stop
  accepting writes, log loudly, and require restart (recovery from the
  last-good WAL). A single-node prototype crashing safely on disk failure is
  acceptable; silent divergence is not. (A more graceful design keeps undo
  info; out of scope now.)
- **Writer thread dies**: same — fatal, stop the server.
- **Shutdown**: drain and fsync the final batch before the writer exits (the
  clean-shutdown snapshot path already truncates the WAL after; ensure the
  final group is durable first).

## Config additions

```cpp
enum class SyncMode { None, Flush, Fsync };
struct StorageConfig {
    SyncMode sync_mode = SyncMode::Fsync;   // was: bool sync_wal
    uint32_t group_commit_window_us = 0;    // 0 = opportunistic only
    // ... existing fields
};
```

`sync_wal == true` maps to `Fsync`, `false` to `Flush`, for back-compat.

## Implementation phases

Each phase is independently benchmarkable via `scripts/bench.sh --compare`.

1. **Move WAL IO off the tables lock, keep current durability (`flush`).**
   Introduce the writer thread + submit/wait, no fsync yet. Expected: write
   throughput rises (lock no longer covers IO), latency unchanged. This
   validates the plumbing with zero durability-semantics change.
2. **Add group batching.** Writer drains-fsyncs-drains. Still `flush` mode.
   Expected: multi-thread write scaling stops flatlining at 4.
3. **Add `fsync` mode + make it the default.** Now correctness is fixed.
   Expected: single-thread commit latency rises to ~one fsync; but *aggregate*
   throughput under concurrency stays high because of batching. Record the new
   baseline. This is the honest cost: durable single-writer latency is
   fsync-bound; durable throughput is not.
4. **Darwin `F_FULLFSYNC`, error-is-fatal handling, shutdown drain.**
5. **(Follow-up, optional)** per-record CRC32C to detect torn WAL tails on
   recovery; today a torn final line is skipped by the parser but not
   distinguished from a truncated-but-valid one.

## New benchmarks to add

- `wal_commit_durable_1t` / `_8t` — commit throughput at `fsync` mode, 1 vs 8
  threads. The 8t number demonstrates group commit works (should be many× the
  1t number, unlike today where they're equal).
- `wal_commit_latency_p99` — single-commit p99 under `fsync`, so we can see the
  fsync floor and catch regressions.
- Keep the existing `storage_insert` (which uses the direct helper path) as the
  non-durable-throughput control.

Gate: `fsync`-mode 8-thread throughput must exceed 1-thread by ≥4× (proves
batching), and crash-recovery correctness is covered by a new test that kills
`-9` the server mid-write and asserts every acknowledged commit survives.

## Risk notes

- macOS fsync semantics are weaker than Linux; `F_FULLFSYNC` is slow (it really
  waits for the drive). The benchmark numbers on the dev Mac will look worse
  than a Linux server with a battery-backed controller — document the platform
  with every durability benchmark.
- The apply-then-wait ordering means a disk failure after memtable apply is
  unrecoverable-in-place (see Failure handling). Fatal-stop is the honest
  single-node answer; note it in ops docs.
- Group commit adds a thread and a queue; the shutdown path must drain it
  before the storage snapshot truncates the WAL, or the final commits are lost.
  Wire this into the existing `WriteSnapshot()`-on-shutdown sequence carefully.
