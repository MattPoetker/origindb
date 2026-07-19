# Territories — a persistent realtime RTS MMO on OriginDB

Status: **planned** (not started). Third OriginDB demo, after NIGHTBOARD
(collab) and cube.io (agar-style). This doc is the implementation plan; build it
in phases, each leaving a playable/testable increment.

---

## 1. Vision

A single **persistent, huge, shared world** where up to **1000 concurrent
players** run RTS empires — command units, harvest resources, build bases,
claim territory, and fight — all simulated authoritatively inside OriginDB as
WASM reducers, streamed to clients over the changefeed, and durably persisted in
the WAL. Your empire survives logout and server restarts; the world keeps
evolving.

This demo exists to prove three things cube.io did not:

1. **Area-of-interest (AOI) at scale** — 1000 clients each subscribed only to
   their viewport via WHERE-filtered changefeed subscriptions, not the whole
   world.
2. **A durable, evolving world** — the WAL is the canonical world state over
   days, not an ephemeral arena.
3. **Transactional gameplay** — resource spend, construction, and combat as
   atomic commits under concurrency.

### Design pillars

- **Scale is the headline requirement.** Every design choice is evaluated
  against "does this hold at 1000 players?" The three levers are: (a) AOI so
  each client handles only its neighborhood, (b) **active-chunk ticking** so sim
  cost is proportional to active battles, not world size, and (c) **lazy
  economy** so idle production costs nothing until collected.
- **RTS, not action.** Select units, issue move/attack/gather/build orders,
  manage an economy and tech. Real-time but command-driven; ~5–10 commands/sec
  per active player, not per-frame input like cube.io.
- **Authoritative & persistent.** All rules run in WASM reducers. Clients render
  and issue orders; they never decide outcomes. The world is the WAL.

---

## 2. World model & spatial scheme

The single most important structural decision: **the world is chunked, and
entity row keys are chunk-prefixed** so that both AOI subscriptions and regional
ticks become cheap *prefix/range scans* — exactly what the storage engine's
ordered memtable + memoized-scan optimizations (see `docs/design/durability.md`
history and `row_codec`) make fast.

### Size (tunable; starting values sized for 1000 players)

```
WORLD_TILES     = 6000 x 6000     # 36,000,000 tiles — huge
CHUNK_TILES     = 100 x 100       # a chunk is 100x100 tiles
CHUNKS          = 60 x 60         # 3,600 chunks
TILE_PX         = 32              # render scale
```

At 1000 players that's ~3.6 chunks of headroom **per player** and 36k tiles
each — players spawn spread across the frontier and rarely share a viewport
except at contested borders. Bump `WORLD_TILES` to 8000–10000 if density feels
high in load tests. Keep `CHUNK_TILES` around 64–128: smaller chunks = finer AOI
and cheaper per-chunk ticks but more chunk rows and more subscription churn as
the camera pans.

### Terrain is procedural, not stored

**Do not materialize 36M tile rows.** Base terrain (grass/forest/mountain/water)
is derived deterministically from a world `seed` via noise (simplex/perlin), the
**same function on client and server**, so neither stores it. Only *modified*
tiles (claimed, built-on, resource-depleted) become rows. This makes row count
proportional to *activity*, not world area — the key to a "huge" world that
stays cheap.

- Server: a `terrain(seed, x, y)` reducer-side function for validation (can I
  build here? is this water?).
- Client: the identical `terrain()` in JS renders the base map for any viewport
  with zero server round-trips.
- Ship the seed via `/api/config`.

### Chunk-prefixed key schema

Row keys embed chunk coords so a viewport or a chunk tick is a prefix scan:

| Table            | Key                              | Notes |
|------------------|----------------------------------|-------|
| `players`        | `p:<session>`                    | one row per account (session identity, like cube.io) |
| `chunks`         | `c:<cx>:<cy>`                    | per-chunk metadata: active flag, unit/building counts, owner tallies, last_tick |
| `units`          | `u:<cx>:<cy>:<uid>`             | chunk-prefixed; carries `cx,cy` as columns too (for WHERE) |
| `buildings`      | `b:<cx>:<cy>:<bid>`             | chunk-prefixed |
| `tiles`          | `t:<cx>:<cy>:<lx>:<ly>`         | only claimed/modified tiles; `lx,ly` local-to-chunk |
| `resources`      | `r:<cx>:<cy>:<rid>`             | harvest nodes (placed procedurally, row stores depletion state) |
| `player_index`   | `pi:<session>:<kind>:<id>`      | secondary index: "my empire" without a full scan (see §5) |

Every spatial row also stores `cx` and `cy` as **columns** so clients can
subscribe `WHERE cx BETWEEN a AND b AND cy BETWEEN c AND d`.

**Cross-chunk movement:** when a unit crosses a chunk boundary its key changes
(`u:<oldcx>:<oldcy>:<uid>` → `u:<newcx>:<newcy>:<uid>`). That's a delete +
insert in the same tick transaction — atomic. Handle it explicitly in the
movement step.

---

## 3. Simulation model

### 3.1 Active-chunk ticking (core scaling mechanism)

A single global tick that scans every entity every frame does **not** scale (we
measured the cost in cube.io: full-table scan dominates). Instead:

- The `chunks` table marks a chunk **active** when it contains moving units, an
  ongoing build, or combat. Idle chunks (settled territory, static buildings)
  are **not** ticked.
- The bridge drives ticks **per active chunk**, round-robin, budgeting N chunks
  per `tick` call so each active chunk advances every ~100–150 ms. Signature:
  `tick(chunkKeys: string[], dtMs)` — the reducer scans only those chunks
  (`u:<cx>:<cy>:` prefix scans — cheap, ordered) and simulates them.
- A chunk deactivates itself when everything in it goes idle (no moving units,
  no build queue, no enemy contact) — it stops costing anything.

Sim cost is therefore proportional to **active gameplay**, not world size or
player count. 1000 players who are mostly managing settled economies produce far
fewer active chunks than 1000 players all fighting.

### 3.2 Lazy economy (idle production costs nothing)

Buildings that produce resources (farm→food, mine→gold, sawmill→wood) do **not**
tick. Each stores `last_collected_ts` and a rate. When the player collects (or a
cap is checked), compute `accrued = rate * (now - last_collected_ts)` capped at
storage. This means:

- Offline empires "produce" for free — no ticking while you're logged out.
- Reduces active-chunk load massively (economy chunks stay idle).

### 3.3 Command reducers (player orders)

Clients issue RTS orders as cheap reducers (authoritative, validated, one commit
each). These mark affected chunks active:

- `spawnEmpire(session, name, faction)` — place HQ in an unclaimed frontier
  chunk, seed starting resources + a few workers. Idempotent by session
  (persistent identity).
- `moveUnits(session, unitIds[], destX, destY)` — set unit destinations; compute
  path (or straight-line + local avoidance first, A* later); mark chunks active.
- `buildStructure(session, type, x, y)` — validate terrain + resource cost
  (atomic spend), place a `constructing` building with a completion timer.
- `trainUnit(session, buildingId, unitType)` — validate cost, enqueue in the
  building's train queue.
- `attack(session, unitIds[], targetId)` — set units' attack-target; mark chunk
  active.
- `gather(session, unitIds[], resourceId)` — assign workers to a node.
- `collect(session, buildingId)` — realize lazy production into the player's
  stockpile (atomic).
- `claimTile` / territory spread — see §6.

Every order is one transaction: validation + spend + state change commit
together, or roll back. This is where OriginDB's ACID commit earns its keep.

### 3.4 Movement & combat (in active-chunk tick)

- **Movement:** integrate unit positions toward destinations at unit speed;
  handle chunk-boundary key migration; simple separation to avoid stacking.
  Pathfinding: start with straight-line + obstacle stop; add flow-field or A*
  over the chunk grid in a later phase.
- **Combat:** units with an attack-target in range deal damage each tick; HP to
  0 → destroy (delete row, drop loot/rubble). Buildings have HP; destroying an
  HQ eliminates that empire's presence in the chunk. Deterministic, resolved by
  the single authoritative tick that owns the chunk (no cross-tick races).
- **Rock-paper-scissors** unit roster (e.g. spearman > cavalry > archer >
  spearman) to give tactical depth.

---

## 4. Area-of-interest (AOI) — how 1000 clients stay cheap

Each client subscribes **only to its viewport**, per table, with a spatial
WHERE, and **re-subscribes when the camera pans** across chunk boundaries
(debounced; snap bounds to the chunk grid to avoid thrash):

```
SELECT * FROM units      WHERE cx BETWEEN a AND b AND cy BETWEEN c AND d
SELECT * FROM buildings  WHERE cx BETWEEN a AND b AND cy BETWEEN c AND d
SELECT * FROM tiles      WHERE cx BETWEEN a AND b AND cy BETWEEN c AND d
SELECT * FROM resources  WHERE cx BETWEEN a AND b AND cy BETWEEN c AND d
```

- **initial_state** on subscribe loads everything currently in view (the
  changefeed already returns a snapshot on `sql_subscribe`).
- Live `sql_changefeed_event`s stream only entities whose `cx/cy` fall in the
  window (PredicateEvaluator does the WHERE match server-side).
- **Viewport handoff:** when a unit moves out of a client's window, that client
  simply stops receiving its updates. The client prunes entities whose
  last-known position is outside the viewport on each camera update. When it
  moves back in, the next subscription's `initial_state` (or the next update in
  range) reintroduces it.
- **Minimap** uses the `chunks` table (aggregate ownership/activity per chunk) —
  a coarse, cheap global subscription (`SELECT * FROM chunks`, ~3600 small
  rows), or a periodic poll.

### The changefeed scaling caveat (drives an engine enhancement)

Today `SqlSubscriptionManager` indexes subscriptions **by table**
(`table_index_`), so an event only checks subs on that table — good. But with
1000 clients all subscribed to `units`, every unit update still evaluates up to
1000 viewport predicates → O(subs) per event. AOI limits *delivery* but not
*predicate evaluation*. To truly hit 1000 players we add a **spatial
subscription index** (bucket subscriptions by chunk) so a unit update in chunk C
only checks subs whose window covers C. This is a **universal engine
improvement** (any spatial workload benefits) and is a Phase-7 task (§9).

---

## 5. "My empire" queries without full scans

Chunk-prefixed keys make *spatial* queries cheap but *"list everything I own"*
expensive (owner isn't in the key prefix). Options, in order of preference:

1. **`player_index` mirror table** keyed `pi:<session>:<kind>:<id>` written
   alongside each unit/building. "My empire" = prefix scan `pi:<session>:`.
   Costs an extra small write per entity create/destroy (cheap; one txn).
2. Store rollup counters on the `players` row (unit count, building count,
   resource totals, score) for HUD/leaderboard without scanning.
3. Owned-chunk list on the player row → scan only those chunks.

Use (1) + (2). Keep the mirror consistent by writing/deleting it in the same
transaction as the entity (atomic — the engine guarantees it).

---

## 6. Territory & fog of war

- **Claiming:** a chunk (or tile cluster) is owned by whoever has an HQ/units
  present and uncontested. Store owner tallies on the `chunks` row; claimed
  tiles become `tiles` rows with `owner`. Passive benefits: vision, small
  resource trickle, build permission.
- **Contested chunks:** multiple owners present → `contested`, ownership decays
  toward whoever holds military presence. (Contested writes to the same chunk
  row are serialized by the authoritative tick — no lost-update problem there.
  Direct player-vs-player *tile claim* races are the one spot that could want
  OCC; see risks.)
- **Fog of war (later phase):** restrict a client's AOI subscription to
  owned + adjacent chunks instead of raw viewport, so you literally cannot
  receive entities you shouldn't see. Elegant: fog = subscription scope, not a
  render trick.
- **Minimap** colors chunks by owner faction from the `chunks` table.

---

## 7. Client architecture

- **Renderer: PixiJS (WebGL)**, not raw canvas — thousands of unit/tile sprites
  need batched WebGL. Camera pan/zoom, sprite culling to viewport, a tile layer
  drawn from the procedural `terrain()` + `tiles` overrides, an entity layer,
  and a minimap.
- **Controls (RTS):** drag-box unit selection, right-click move/attack/gather,
  a build palette (click building → ghost placement → confirm), train queues on
  building panels, resource HUD, minimap click-to-pan.
- **Prediction/interpolation:** interpolate unit positions between server
  updates (like cube.io's lerp). Optionally client-side move prediction for
  responsiveness, reconciled to server.
- **Networking:** same shape as cube.io — a Node bridge serves the client and
  bridges HTTP order calls → gRPC reducer calls; the browser subscribes directly
  to the OriginDB websocket for AOI changefeed. Bridge drives the active-chunk
  tick scheduler.
- **Bind `0.0.0.0`** (server + ws) for LAN/remote access; run long-lived procs
  under `setsid`/systemd (learned the hard way — see below).

---

## 8. Asset pipeline & recommended tools

Prioritize **free CC0 asset packs** for speed and consistency; author custom
sprites only where needed.

**Sprites / tilesets (start here):**
- **Kenney.nl** (CC0, free) — "Tiny Battle", "Medieval RTS", "Tower Defense",
  "Topdown Shooter" packs cover terrain tiles, buildings, and unit sprites.
  Ideal starting art, zero attribution required.
- **OpenGameArt.org** and **LPC (Liberated Pixel Cup)** sets for extra units.
- **game-icons.net** (CC BY) for UI/ability icons.

**Custom pixel art + animation** (if Kenney isn't enough):
- **Aseprite** (paid, ~$20, best-in-class) or free **LibreSprite** / **Piskel**
  for unit walk/attack animations and building states.

**Tilemap / terrain authoring:**
- **Tiled** (mapeditor.org, free) — build tilesets, design chunk prefabs and
  terrain templates, author resource-node placement rules; exports JSON.
- **Procedural terrain:** `simplex-noise` (npm) with a shared seed for the
  deterministic `terrain()` function. Prototype/tune the noise with a small
  HTML noise-preview page or Tiled.

**Sprite atlas packing (perf):**
- **free-tex-packer** (free) or **TexturePacker** (paid) → pack sprites into a
  single atlas + JSON for PixiJS (fewer draw calls, big win at scale).

**UI / HUD design:**
- **Figma** (free tier) for HUD/panel mockups before coding.

**Audio (optional, later):**
- Kenney audio packs, **freesound.org** (check licenses), **jfxr/sfxr** for
  retro SFX.

**AI-assisted art:** usable for concepting or one-off icons, but pixel-art
*consistency* across a unit roster is hard to get from image models — Kenney
packs are more reliable for a coherent look. Consider AI for terrain textures or
faction crests, not core unit sprites.

**Recommended minimal stack to start:** Kenney "Tiny Battle" + "Medieval RTS"
(terrain, buildings, units) → Tiled for terrain templates/noise tuning → PixiJS
renderer → free-tex-packer atlases → Aseprite only if a needed sprite is
missing.

---

## 9. Required / motivated OriginDB engine enhancements

Territories pushes the engine harder than cube.io. Some items are prerequisites
for full 1000-player scale; treat them as universal engine work (they benefit
any spatial/high-subscriber workload), landed in Phase 7.

1. **Spatial subscription index (changefeed).** Bucket subscriptions by chunk so
   an entity update only evaluates subs whose window covers its chunk. Removes
   the O(subs)-per-event wall. *(Universal.)*
2. **Range/BETWEEN predicate perf** in `PredicateEvaluator` — ensure
   `cx BETWEEN a AND b AND cy BETWEEN c AND d` is fast; add if missing.
3. **Module-per-region sharding.** The per-module mutex serializes a module's
   ticks. To parallelize the world, deploy **one module per region** (e.g. a
   4×4 = 16-region grid), each owning a disjoint chunk range; the bridge routes
   orders + ticks by region. Cross-region unit handoff at borders is the hard
   part (defer; keep units within a region initially, or do a delete-in-A /
   insert-in-B handshake). This is how one server scales past a single module's
   throughput ceiling.
4. **Optimistic concurrency / conflict handling (optional).** Direct
   player-vs-player tile-claim races currently resolve last-writer-wins. If
   contested-tile correctness matters, add compare-and-set (version check) at
   commit. Most contention is mediated by the authoritative tick, so this may
   not be needed — validate under load first.
5. **Secondary index support (nice-to-have).** The `player_index` mirror table
   (§5) is a userland workaround; a real by-owner index would be cleaner.

Phases 0–6 can be built on the **current** engine (AOI works today via
table-indexed WHERE subscriptions; it just doesn't scale to 1000 subs until #1).
Prove gameplay first, then harden.

---

## 10. Phase plan

Each phase ends with a demoable, tested increment. Don't rush — quality over
speed.

**Phase 0 — Design & scaffolding**
- Lock key schema, world constants, table list. Write the deterministic
  `terrain(seed,x,y)` in both TS (module) and JS (client) and unit-test they
  agree. Stand up `examples/territories/` (bridge + PixiJS client skeleton) and
  `sdk/typescript/examples/territories/` (module skeleton with `join`/tables).
  Asset pipeline: pull Kenney packs, pack an atlas, render one tile layer.
- *Done when:* client renders the procedural terrain of any viewport, camera
  pans/zooms, minimap shows the world; no entities yet.

**Phase 1 — Player & persistent base**
- `spawnEmpire` places an HQ in an unclaimed frontier chunk (spread-out spawn
  selection), seeds starting resources + workers, persists by session. Resource
  HUD. Reconnect returns the same empire.
- *Done when:* join → your HQ appears, survives restart, resources shown.

**Phase 2 — Units & movement (active-chunk tick)**
- Train workers; RTS drag-select + right-click move; authoritative movement in
  the active-chunk tick; chunk-boundary key migration; client interpolation.
  Stand up the bridge's active-chunk scheduler.
- *Done when:* you command units across chunks smoothly; idle chunks stop
  ticking (verify via logs/metrics).

**Phase 3 — Economy**
- Resource nodes (procedural placement + depletion rows), worker gathering,
  economic buildings with **lazy production** + `collect`, build placement with
  atomic cost + construction timers.
- *Done when:* a full harvest→build→grow loop works and offline production
  accrues correctly.

**Phase 4 — Military & combat**
- Combat unit roster (rock-paper-scissors), `attack` orders, deterministic
  combat in tick, unit/building HP + destruction, HQ loss.
- *Done when:* two players can fight a decisive battle; results persist.

**Phase 5 — Territory & fog of war**
- Chunk/tile ownership, contested decay, vision, minimap ownership colors, fog
  via AOI subscription scoping.
- *Done when:* a border war visibly shifts territory on the minimap and fog
  hides unseen chunks.

**Phase 6 — "My empire" & UX polish**
- `player_index` mirror + rollup counters, empire overview panel, leaderboard,
  build/train UI polish, notifications (under attack!), tech tree start.
- *Done when:* managing a multi-chunk empire feels good without full scans.

**Phase 7 — Scale hardening (the 1000-player gate)**
- Land the **spatial subscription index** (§9.1), **module-per-region
  sharding** (§9.3), tune the active-chunk scheduler and group-commit/sync mode.
  Build a **1000-bot load harness** (extend cube.io's capacity-test pattern) and
  run it on the rack server (`10.0.0.4`, 32c/125G, Linux/fdatasync). Profile,
  fix the top bottleneck, repeat.
- *Done when:* 1000 simulated players sustain playable tick rates + AOI latency
  on the rack server, with numbers recorded.

**Phase 8 — Depth & seasonal live-ops**
- Alliances/diplomacy, more unit/building types, a real tech tree, hot-swap
  **seasonal events / balance changes** (showcase live module hot-swap on a
  populated world — like the cube.io food-value swap but game-wide rules),
  sound, final art pass.
- *Done when:* it feels like a game, and a rule change ships live with zero
  downtime.

---

## 11. Reuse from existing demos

- **Module shape:** copy `sdk/typescript/examples/cubeio/index.ts` — SDK imports,
  `setModuleInfo`/`declareTable`, `registerReducer`, `JsonValue`, `scanTable`
  (pass an explicit high `SCAN_LIMIT`, not 0 — the default caps at 1000!),
  `readTable`/`writeTable`/`deleteTable`, PRNG seeded from `nowMs`/`generateId`.
  Build with `npx asc --config examples/<name>/asconfig.json --target release`
  (a **per-example asconfig** to avoid the todo-entry merge bug).
- **Bridge shape:** copy `examples/cubeio/server.js` — `/api/config`,
  `/api/call` allow-list, gRPC `ExecuteReducer`, tick scheduler via
  `setInterval` with an in-flight guard. Symlink `node_modules` from an existing
  example or `npm i @grpc/grpc-js @grpc/proto-loader`.
- **Client shape:** cube.io's `public/game.js` for the changefeed wire
  (`initial_state` rows + `sql_changefeed_event` with `new_value.columns`,
  DELETE by key), `?token=` websocket auth, `/api/config` bootstrap. Swap the
  canvas renderer for PixiJS.
- **Ops:** run the OriginDB server with `-p/-g/--sync-mode flush` (games favor
  flush for tick throughput; the world still survives process restarts — use
  periodic snapshots to bound recovery). Everything binds `0.0.0.0`. Launch
  long-lived server + bridge with **`setsid`** (or systemd), never bare
  `nohup &` over SSH (it gets SIGHUP'd on disconnect).
- **Auth:** admin token deploys modules + runs SQL; the low-privilege client
  token goes to the browser for reducer calls + ws subscribe.

---

## 12. Key risks & open questions

- **Changefeed at 1000 subs** — the spatial subscription index (§9.1) is the
  gating item; validate early with the load harness before over-investing in
  gameplay depth.
- **Cross-region unit handoff** under module sharding — genuinely tricky; keep
  units region-local initially, design the handshake later.
- **Viewport handoff / entity pruning** — get the client's "unit left my view"
  cleanup right or ghosts accumulate.
- **Pathfinding cost** — A* for many units can blow the tick budget; prefer
  flow-fields per destination or coarse chunk-graph paths.
- **Contested-tile races** — measure whether last-writer-wins is acceptable
  before building OCC.
- **Determinism** — the procedural `terrain()` must be byte-identical on client
  and server (watch float precision) or players build on mismatched maps.
- **Snapshot cadence** — a long-lived world needs periodic snapshots (not just
  on shutdown) to bound WAL replay on restart; wire this in during Phase 7.

---

## 13. Success criteria

- A huge, persistent, procedurally-terrained world.
- Full RTS loop: gather → build → train → fight → claim territory, all
  authoritative and durable.
- Each client handles only its viewport via AOI; empires persist by session
  across restarts.
- **1000 simulated players coexist at playable tick + AOI latency on the rack
  server**, with recorded numbers.
- A seasonal rule change ships live via module hot-swap, zero downtime.
