// Territories stress harness.
//
// Spawns a growing fleet of bot empires and keeps their units CONSTANTLY moving
// (and slowly training more), so active-chunk ticking can't rest and the
// authoritative tick + order calls pile up behind the single per-module mutex.
//
// It prints live metrics every 2s: bot count, unit count, active chunks, and the
// p50/p95 latency of a moveUnits order (which contends with the tick — rising
// latency = the sim is choking). Ramps gently so degradation is felt, not sudden.
//
// Usage: node bots.js [--grpc localhost:50053] [--max 250] [--start 15]
//        [--ramp 5] [--ramp-ms 6000] [--move-ms 1500] [--train-ms 20000]

import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const flag = (n, d) => { const i = args.indexOf(n); return i >= 0 && args[i + 1] ? args[i + 1] : d; };

const GRPC   = flag("--grpc", "localhost:50053");
const MAX    = parseInt(flag("--max", "250"), 10);
const START  = parseInt(flag("--start", "15"), 10);
const RAMP    = parseInt(flag("--ramp", "5"), 10);
const RAMP_MS = parseInt(flag("--ramp-ms", "6000"), 10);
const MOVE_MS = parseInt(flag("--move-ms", "1500"), 10);
const TRAIN_MS = parseInt(flag("--train-ms", "20000"), 10);
const WORLD = 6000, CHUNK = 100, NCHUNKS = 60;
const RS = parseInt(flag("--region-size", "20"), 10);          // must match the bridge
const REGS = Math.ceil(NCHUNKS / RS);
const HOME = "terr_0_0";
const clampR = (v) => (v < 0 ? 0 : v >= REGS ? REGS - 1 : v) | 0;
const moduleFor = (cx, cy) => `terr_${clampR(Math.floor(cx / RS))}_${clampR(Math.floor(cy / RS))}`;

const proto = grpc.loadPackageDefinition(
  protoLoader.loadSync(resolve(__dirname, "../../proto/origindb.proto"),
    { keepCase: true, longs: String, defaults: true })
).origindb.grpc;
const wasm = new proto.WasmService(GRPC, grpc.credentials.createInsecure());
const sql  = new proto.SQLService(GRPC, grpc.credentials.createInsecure());

const wv = (v) => typeof v === "number" ? (Number.isInteger(v) ? { int64_value: v } : { double_value: v })
                : typeof v === "boolean" ? { bool_value: v } : { string_value: String(v) };

function call(moduleName, reducer, a, timeoutMs = 8000) {
  return new Promise((res, rej) => {
    wasm.ExecuteReducer({ module_name: moduleName, reducer_name: reducer, sender_identity: "stress", args: a.map(wv) },
      { deadline: Date.now() + timeoutMs }, (e, r) => e ? rej(e) : res(r));
  });
}
function query(s, timeoutMs = 15000) {
  return new Promise((res, rej) => {
    sql.Execute({ sql: s }, { deadline: Date.now() + timeoutMs }, (e, r) => e ? rej(e) : res(r));
  });
}

// ---- bot fleet --------------------------------------------------------------
const bots = new Map();   // session -> { hqx, hqy, units:[id], hqId }
let spawned = 0;

// deterministic-ish rng per bot so moves vary
function rnd() { return Math.random(); }

async function spawnBot(i) {
  const session = `bot_${i}`;
  try {
    const r = await call(HOME, "spawnEmpire", [session, `Bot${i}`, "azure", "#3b82f6"]);
    let hqx = WORLD / 2, hqy = WORLD / 2;
    const b = r.results && r.results[0];
    if (b && b.bytes_value) { try { const j = JSON.parse(Buffer.from(b.bytes_value).toString()); hqx = j.hqx ?? hqx; hqy = j.hqy ?? hqy; } catch {} }
    bots.set(session, { hqx, hqy, units: [], hqKey: null });   // units: [{key,cx,cy}]
  } catch (e) { errs++; }
}

// Refresh unit ids + HQ building ids for every bot (one scan each). Cheap enough
// on an interval; keeps up with trained units.
async function refresh() {
  try {
    const u = await query("SELECT * FROM units");
    const byOwner = new Map();
    for (const row of u.rows || []) {
      const owner = row.columns?.owner?.string_value;
      if (!owner || !bots.has(owner)) continue;
      const cx = Number(row.columns?.cx?.int64_value ?? row.columns?.cx?.double_value ?? 0);
      const cy = Number(row.columns?.cy?.int64_value ?? row.columns?.cy?.double_value ?? 0);
      (byOwner.get(owner) || byOwner.set(owner, []).get(owner)).push({ key: row.key, cx, cy });
    }
    for (const [s, b] of bots) b.units = byOwner.get(s) || b.units;
    totalUnits = (u.rows || []).length;
  } catch (e) { errs++; }
  try {
    const bq = await query("SELECT * FROM buildings");
    for (const row of bq.rows || []) {
      const owner = row.columns?.owner?.string_value;
      const kind = row.columns?.kind?.string_value;
      if (kind === "hq" && owner && bots.has(owner)) {
        const cx = Number(row.columns?.cx?.int64_value ?? row.columns?.cx?.double_value ?? 0);
        const cy = Number(row.columns?.cy?.int64_value ?? row.columns?.cy?.double_value ?? 0);
        bots.get(owner).hqKey = { key: row.key, cx, cy };
      }
    }
  } catch (e) { errs++; }
  try {
    const c = await query("SELECT * FROM chunks");
    activeChunks = (c.rows || []).filter(r => r.columns?.active?.bool_value === true).length;
  } catch (e) { errs++; }
}

// ---- metrics ----------------------------------------------------------------
let lat = [], errs = 0, totalUnits = 0, activeChunks = 0;
function pct(a, p) { if (!a.length) return 0; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; }

// ---- per-bot activity: keep units moving ------------------------------------
async function moveBot(b) {
  if (!b.units.length) return;
  const x = Math.max(2, Math.min(WORLD - 2, b.hqx + (rnd() * 80 - 40)));
  const y = Math.max(2, Math.min(WORLD - 2, b.hqy + (rnd() * 80 - 40)));
  // group this bot's units by owning region shard, one moveUnits per shard
  const groups = new Map();
  for (const u of b.units.slice(0, 8)) {
    const m = moduleFor(u.cx, u.cy);
    (groups.get(m) || groups.set(m, []).get(m)).push(u.key);
  }
  for (const [m, keys] of groups) {
    const t0 = Date.now();
    try { await call(m, "moveUnits", [b.session ?? "", keys.join(","), x, y]); lat.push(Date.now() - t0); }
    catch { errs++; }
  }
}

// ---- schedulers -------------------------------------------------------------
async function boot() {
  console.log(`[stress] target ${MAX} bots · ramp +${RAMP}/${RAMP_MS}ms · move ${MOVE_MS}ms · train ${TRAIN_MS}ms`);
  // initial batch
  for (let i = 0; i < START && spawned < MAX; i++) await spawnBot(spawned++);
  await refresh();

  // ramp
  const ramp = setInterval(async () => {
    if (spawned >= MAX) { clearInterval(ramp); console.log(`[stress] fleet at max ${MAX}`); return; }
    const n = Math.min(RAMP, MAX - spawned);
    for (let i = 0; i < n; i++) await spawnBot(spawned++);
  }, RAMP_MS);

  // move loop — every MOVE_MS, issue a move for every bot (spread within the window)
  setInterval(() => {
    const list = [...bots.entries()];
    list.forEach(([s, b], i) => { b.session = s; setTimeout(() => moveBot(b), (i / Math.max(1, list.length)) * MOVE_MS); });
  }, MOVE_MS);

  // train loop — grow the population so load escalates
  setInterval(async () => {
    for (const [s, b] of bots) {
      if (b.hqKey) { try { await call(moduleFor(b.hqKey.cx, b.hqKey.cy), "train", [s, b.hqKey.key, "worker"]); } catch { errs++; } }
    }
  }, TRAIN_MS);

  // discovery refresh
  setInterval(refresh, 8000);

  // metrics
  setInterval(() => {
    const p50 = pct(lat, 0.5), p95 = pct(lat, 0.95), n = lat.length;
    console.log(`[stress] bots=${bots.size}/${MAX} units=${totalUnits} activeChunks=${activeChunks} ` +
      `move/s=${(n / 2).toFixed(0)} lat.p50=${p50}ms lat.p95=${p95}ms errs=${errs}`);
    lat = []; errs = 0;
  }, 2000);
}

boot().catch(e => { console.error(e); process.exit(1); });
