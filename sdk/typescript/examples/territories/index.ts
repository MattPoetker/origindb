// Territories — a persistent realtime RTS MMO whose entire authoritative
// simulation runs as OriginDB WASM reducers. Empires, units, buildings and
// resources all live in OriginDB tables, so the world persists through the WAL
// and streams to every client over the changefeed.
//
// Design highlights (see docs/design/territories.md):
//   * The world is HUGE (6000x6000 tiles) but terrain is PROCEDURAL and
//     DETERMINISTIC — derived from a shared seed by the identical terrain()
//     function on client and server, so the 36M tiles are never stored. Only
//     modified state (units, buildings, claimed resources) becomes rows.
//   * Row keys are CHUNK-PREFIXED (u:<cx>:<cy>:<id>) so a per-chunk tick is a
//     cheap ordered prefix scan.
//   * ACTIVE-CHUNK TICKING: only chunks flagged active (moving units, combat,
//     construction) are simulated. Idle territory costs nothing. Sim cost is
//     proportional to active gameplay, not world size.
//   * LAZY ECONOMY: producer buildings (mine/farm) never tick — they accrue
//     output against wall-clock and realize it on collect().
//   * Identity is the session id: rejoining returns the SAME empire.
//
// Tables:
//   players    key = <session>            one empire per account
//   chunks     key = c:<cx>:<cy>          per-chunk metadata: active flag, owner
//   units      key = u:<cx>:<cy>:<id>     workers + soldiers (carry cx,cy cols)
//   buildings  key = b:<cx>:<cy>:<id>     hq / barracks / mine / farm
//   resources  key = r:<cx>:<cy>:<id>     gold / wood harvest nodes (depletable)

import {
  JsonValue,
  abortCall,
  declareTable,
  deleteTable,
  generateId,
  nowMs,
  readTable,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "../../assembly/index";

export {
  origindb_alloc,
  origindb_free,
  origindb_describe,
  origindb_invoke,
  __origindb_abort,
} from "../../assembly/index";

setModuleInfo("territories", "2.1.0");
declareTable("players");
declareTable("chunks");
declareTable("active");
declareTable("units");
declareTable("buildings");
declareTable("resources");

// ---- world constants (keep in sync with public/game.js) --------------------

const SEED: i32 = 1337;          // world seed — same on client & server
const WORLD: f64 = 6000.0;       // WORLD x WORLD tiles
const CHUNK: f64 = 100.0;        // tiles per chunk side
const NCHUNKS: i32 = 60;         // WORLD / CHUNK
const SCAN_LIMIT: i32 = 100000;  // scan ALL rows (SDK default of 0 caps at 1000!)

// starting stockpile
const START_GOLD: f64 = 120.0;
const START_WOOD: f64 = 150.0;
const START_FOOD: f64 = 100.0;

// unit stats
const WORKER_SPEED: f64 = 7.0;   // tiles / sec
const SOLDIER_SPEED: f64 = 6.0;
const WORKER_HP: f64 = 40.0;
const SOLDIER_HP: f64 = 90.0;
const SOLDIER_DMG: f64 = 22.0;   // per second in range
const ATTACK_RANGE: f64 = 2.2;   // tiles
const GATHER_RANGE: f64 = 1.6;
const GATHER_RATE: f64 = 14.0;   // resource units / sec into stockpile
const REACH: f64 = 0.4;          // "arrived at destination" epsilon

// building stats
const HQ_HP: f64 = 600.0;
const BARRACKS_HP: f64 = 320.0;
const PROD_HP: f64 = 200.0;
const BUILD_SECONDS: f64 = 6.0;
const MINE_RATE: f64 = 6.0;      // gold / sec (lazy)
const FARM_RATE: f64 = 7.0;      // food / sec (lazy)
const PROD_CAP: f64 = 400.0;     // max un-collected accrual

// ---- deterministic terrain (MUST be byte-identical to client) --------------
// Integer value-noise. All intermediate math is f64 (IEEE754 double) and the
// hash stays in u32 space, so client (JS number) and server (AS f64) agree.

function hash2(ix: i32, iy: i32): f64 {
  let h: u32 = <u32>(ix * 374761393) + <u32>(iy * 668265263) + <u32>(SEED * 1442695040);
  h = (h ^ (h >> 13)) * 1274126177;
  h = h ^ (h >> 16);
  return <f64>(h & 0xffffff) / 16777216.0;   // [0,1)
}

function smooth(t: f64): f64 {
  return t * t * (3.0 - 2.0 * t);
}

function valueNoise(x: f64, y: f64): f64 {
  const x0 = <i32>Math.floor(x);
  const y0 = <i32>Math.floor(y);
  const fx = smooth(x - <f64>x0);
  const fy = smooth(y - <f64>y0);
  const a = hash2(x0, y0);
  const b = hash2(x0 + 1, y0);
  const c = hash2(x0, y0 + 1);
  const d = hash2(x0 + 1, y0 + 1);
  const top = a + (b - a) * fx;
  const bot = c + (d - c) * fx;
  return top + (bot - top) * fy;
}

// Terrain classes: 0 water, 1 sand, 2 grass, 3 forest, 4 mountain.
function terrain(tx: f64, ty: f64): i32 {
  const e =
    valueNoise(tx / 260.0, ty / 260.0) * 0.6 +
    valueNoise(tx / 90.0, ty / 90.0) * 0.3 +
    valueNoise(tx / 30.0, ty / 30.0) * 0.1;
  const m = valueNoise(tx / 200.0 + 100.0, ty / 200.0 - 40.0);
  if (e < 0.34) return 0;              // water
  if (e < 0.38) return 1;              // sand
  if (e > 0.80) return 4;              // mountain
  if (m > 0.62 && e < 0.72) return 3;  // forest
  return 2;                            // grass
}

function buildable(tx: f64, ty: f64): bool {
  const t = terrain(tx, ty);
  return t == 1 || t == 2 || t == 3;   // not water, not mountain
}

// ---- chunk helpers ----------------------------------------------------------

function cx_of(x: f64): i32 { return <i32>Math.floor(x / CHUNK); }
function cy_of(y: f64): i32 { return <i32>Math.floor(y / CHUNK); }
function chunkKey(cx: i32, cy: i32): string { return "c:" + cx.toString() + ":" + cy.toString(); }
function activeKey(cx: i32, cy: i32): string { return "a:" + cx.toString() + ":" + cy.toString(); }
function unitPrefix(cx: i32, cy: i32): string { return "u:" + cx.toString() + ":" + cy.toString() + ":"; }
function bldgPrefix(cx: i32, cy: i32): string { return "b:" + cx.toString() + ":" + cy.toString() + ":"; }

// The active-set is its OWN small table: a chunk gets a row while it has motion/
// combat/construction and the row is DELETED when it settles. The tick scans
// only this table, so its cost tracks *live* activity — not the number of chunks
// ever touched. (The old code scanned the ever-growing `chunks` table every
// tick, which eventually overflowed a scan and crashed the server.)
function markActive(cx: i32, cy: i32): void {
  writeTable("active", activeKey(cx, cy), JsonValue.newObject()
    .setNumber("cx", <f64>cx).setNumber("cy", <f64>cy).toString());
}
function clearActive(cx: i32, cy: i32): void { deleteTable("active", activeKey(cx, cy)); }

// Persistent territory ownership lives in `chunks` (for the minimap); it is
// written only when an owner is set, and never grows per-tick.
function writeChunkOwner(cx: i32, cy: i32, owner: string): void {
  writeTable("chunks", chunkKey(cx, cy), JsonValue.newObject()
    .setNumber("cx", <f64>cx)
    .setNumber("cy", <f64>cy)
    .setString("owner", owner)
    .setNumber("updated", <f64>nowMs())
    .toString());
}

function activateChunk(cx: i32, cy: i32, owner: string): void {
  if (cx < 0 || cy < 0 || cx >= NCHUNKS || cy >= NCHUNKS) return;
  markActive(cx, cy);
  if (owner.length > 0) writeChunkOwner(cx, cy, owner);
}

function claimChunk(cx: i32, cy: i32, owner: string): void {
  writeChunkOwner(cx, cy, owner);
  markActive(cx, cy);
}

// ---- deterministic PRNG (seeded per call from host clock) -------------------

let g_seed: u64 = 0;
function rnd(): f64 {
  if (g_seed == 0) g_seed = (<u64>nowMs()) ^ 0x9e3779b97f4a7c15 ^ (<u64>generateId());
  let x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_seed = x;
  return <f64>(x >> 11) / 9007199254740992.0;
}

// ---- unit struct ------------------------------------------------------------

class U {
  id: string = "";
  owner: string = "";
  kind: string = "worker";   // worker | soldier
  x: f64 = 0;
  y: f64 = 0;
  hp: f64 = WORKER_HP;
  maxHp: f64 = WORKER_HP;
  dx: f64 = 0;               // destination
  dy: f64 = 0;
  cmd: string = "idle";      // idle | move | gather | attack
  targ: string = "";         // resource id (gather) or unit id (attack)
  cx: i32 = 0;
  cy: i32 = 0;
  dirty: bool = false;
  dead: bool = false;
  migrated: bool = false;

  static from(cx: i32, cy: i32, key: string, v: JsonValue): U {
    const u = new U();
    u.id = v.getString("id", key);
    u.owner = v.getString("owner", "");
    u.kind = v.getString("kind", "worker");
    u.x = v.getNumber("x", 0);
    u.y = v.getNumber("y", 0);
    u.maxHp = v.getNumber("maxHp", WORKER_HP);
    u.hp = v.getNumber("hp", u.maxHp);
    u.dx = v.getNumber("dx", u.x);
    u.dy = v.getNumber("dy", u.y);
    u.cmd = v.getString("cmd", "idle");
    u.targ = v.getString("targ", "");
    u.cx = cx;
    u.cy = cy;
    return u;
  }

  key(): string { return unitPrefix(this.cx, this.cy) + this.id; }

  toJson(): string {
    return JsonValue.newObject()
      .setString("id", this.id)
      .setString("owner", this.owner)
      .setString("kind", this.kind)
      .setNumber("x", this.x)
      .setNumber("y", this.y)
      .setNumber("hp", this.hp)
      .setNumber("maxHp", this.maxHp)
      .setNumber("dx", this.dx)
      .setNumber("dy", this.dy)
      .setString("cmd", this.cmd)
      .setString("targ", this.targ)
      .setNumber("cx", <f64>this.cx)
      .setNumber("cy", <f64>this.cy)
      .setNumber("updated", <f64>nowMs())
      .toString();
  }

  write(): void { writeTable("units", this.key(), this.toJson()); }
}

function speedOf(u: U): f64 { return u.kind == "soldier" ? SOLDIER_SPEED : WORKER_SPEED; }

function newUnit(owner: string, kind: string, x: f64, y: f64): void {
  const u = new U();
  u.id = generateId().toString();
  u.owner = owner;
  u.kind = kind;
  u.x = x;
  u.y = y;
  u.dx = x;
  u.dy = y;
  u.maxHp = kind == "soldier" ? SOLDIER_HP : WORKER_HP;
  u.hp = u.maxHp;
  u.cx = cx_of(x);
  u.cy = cy_of(y);
  u.write();
  activateChunk(u.cx, u.cy, owner);
}

// ---- player helpers ---------------------------------------------------------

function loadPlayer(session: string): JsonValue | null {
  const raw = readTable("players", session);
  return raw == null ? null : JsonValue.parse(raw);
}

// HQ destroyed → the empire is eliminated (alive=false). The client sees this on
// its own player row and shows the defeat screen; the empire can redeploy fresh.
function eliminate(owner: string): void {
  const p = loadPlayer(owner);
  if (p == null) return;
  writePlayer(owner, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    p.getNumber("gold", 0), p.getNumber("wood", 0), p.getNumber("food", 0),
    p.getNumber("hqx", 0), p.getNumber("hqy", 0), false, p.getNumber("score", 0));
}

function writePlayer(session: string, name: string, faction: string, color: string,
                     gold: f64, wood: f64, food: f64, hqx: f64, hqy: f64,
                     alive: bool, score: f64): void {
  writeTable("players", session, JsonValue.newObject()
    .setString("id", session)
    .setString("name", name)
    .setString("faction", faction)
    .setString("color", color)
    .setNumber("gold", gold)
    .setNumber("wood", wood)
    .setNumber("food", food)
    .setNumber("hqx", hqx)
    .setNumber("hqy", hqy)
    .setBool("alive", alive)
    .setNumber("score", score)
    .setNumber("updated", <f64>nowMs())
    .toString());
}

// Deterministic-ish spawn: hash the session to a frontier tile on buildable
// land, spread across the world; probe outward until a valid spot is found.
function pickSpawn(session: string): Array<f64> {
  let h: u32 = 2166136261;
  for (let i = 0; i < session.length; i++) { h = (h ^ <u32>session.charCodeAt(i)) * 16777619; }
  let tx = <f64>((h % 5200) + 400);
  let ty = <f64>(((h / 5200) % 5200) + 400);
  for (let ring = 0; ring < 400; ring++) {
    const px = tx + <f64>((ring * 37) % 800) - 400.0;
    const py = ty + <f64>((ring * 53) % 800) - 400.0;
    if (px > 200 && px < WORLD - 200 && py > 200 && py < WORLD - 200 &&
        buildable(px, py) && buildable(px + 3, py) && buildable(px, py + 3)) {
      return [px, py];
    }
  }
  return [tx, ty];
}

// ---- reducers ---------------------------------------------------------------

registerReducer("__init", (_a: Array<JsonValue>): JsonValue | null => {
  // Terrain is procedural and empires seed their own resources — nothing to do
  // on a fresh DB but announce the world.
  return JsonValue.newObject()
    .setNumber("world", WORLD)
    .setNumber("chunk", CHUNK)
    .setNumber("seed", <f64>SEED);
}, []);

registerReducer("spawnEmpire", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const name = args.length > 1 && args[1].asString().length > 0 ? clampName(args[1].asString()) : "Commander";
  const faction = args.length > 2 && args[2].asString().length > 0 ? args[2].asString() : "azure";
  const color = args.length > 3 && args[3].asString().length > 0 ? args[3].asString() : "#41d6ff";

  const existing = loadPlayer(session);
  if (existing != null && existing.getBool("alive", false)) {
    // Already have an empire — just return where the HQ is (reconnect).
    return JsonValue.newObject()
      .setNumber("hqx", existing.getNumber("hqx", 0))
      .setNumber("hqy", existing.getNumber("hqy", 0))
      .setNumber("world", WORLD)
      .setNumber("chunk", CHUNK)
      .setNumber("seed", <f64>SEED)
      .setBool("resumed", true);
  }

  const spawn = pickSpawn(session);
  const hx = spawn[0];
  const hy = spawn[1];
  const hcx = cx_of(hx);
  const hcy = cy_of(hy);

  writePlayer(session, name, faction, color, START_GOLD, START_WOOD, START_FOOD, hx, hy, true, 0);
  claimChunk(hcx, hcy, session);

  // HQ building.
  newBuilding(session, "hq", hx, hy);
  // Starting workers around the HQ.
  newUnit(session, "worker", hx - 2.0, hy - 1.0);
  newUnit(session, "worker", hx + 2.0, hy - 1.0);
  newUnit(session, "worker", hx, hy + 2.0);

  // Seed resource nodes nearby (deterministic scatter on buildable land).
  seedResources(session, hx, hy);

  return JsonValue.newObject()
    .setNumber("hqx", hx)
    .setNumber("hqy", hy)
    .setNumber("world", WORLD)
    .setNumber("chunk", CHUNK)
    .setNumber("seed", <f64>SEED)
    .setBool("resumed", false);
}, ["session", "name", "faction", "color"]);

// Orders take full chunk-prefixed ROW KEYS (u:cx:cy:id), not bare ids, so the
// unit lookup is a direct O(1) readTable instead of a full-table scan. The
// client/bridge already know each entity's cx:cy, so they build the key.
registerReducer("moveUnits", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const keys = splitCsv(args.length > 1 ? args[1].asString() : "");
  const wx = clamp(args.length > 2 ? args[2].asNumber() : 0, 0, WORLD);
  const wy = clamp(args.length > 3 ? args[3].asNumber() : 0, 0, WORLD);
  commandUnits(session, keys, "move", wx, wy, "");
  return null;
}, ["session", "keys", "x", "y"]);

registerReducer("gather", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const keys = splitCsv(args.length > 1 ? args[1].asString() : "");
  const resKey = args.length > 2 ? args[2].asString() : "";
  const rr = readResource(resKey);
  if (rr == null) return null;
  commandUnits(session, keys, "gather", rr!.x, rr!.y, resKey);   // targ = resource KEY
  return null;
}, ["session", "keys", "resKey"]);

registerReducer("attack", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const keys = splitCsv(args.length > 1 ? args[1].asString() : "");
  const targKey = args.length > 2 ? args[2].asString() : "";   // unit (u:) OR building (b:)
  let tx: f64 = 0, ty: f64 = 0;
  if (targKey.length > 0 && targKey.charCodeAt(0) == 0x62) {    // 'b' — enemy base/building
    const tb = readBuilding(targKey);
    if (tb != null) { tx = tb!.x; ty = tb!.y; activateChunk(tb!.cx, tb!.cy, ""); }
  } else {
    const tu = readUnit(targKey);
    if (tu != null) { tx = tu!.x; ty = tu!.y; }
  }
  commandUnits(session, keys, "attack", tx, ty, targKey);
  return null;
}, ["session", "keys", "targetKey"]);

registerReducer("build", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const kind = args.length > 1 ? args[1].asString() : "";
  const wx = clamp(args.length > 2 ? args[2].asNumber() : 0, 0, WORLD);
  const wy = clamp(args.length > 3 ? args[3].asNumber() : 0, 0, WORLD);

  const p = loadPlayer(session);
  if (p == null || !p.getBool("alive", false)) return err("no empire");
  if (!buildable(wx, wy)) return err("cannot build there");

  let cg: f64 = 0, cw: f64 = 0;
  if (kind == "barracks") { cw = 120; cg = 40; }
  else if (kind == "mine") { cw = 80; cg = 20; }
  else if (kind == "farm") { cw = 70; }
  else return err("unknown building");

  let gold = p.getNumber("gold", 0);
  let wood = p.getNumber("wood", 0);
  if (wood < cw || gold < cg) return err("not enough resources");

  // Atomic spend + place: both land in the same transaction.
  gold -= cg; wood -= cw;
  writePlayer(session, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    gold, wood, p.getNumber("food", 0), p.getNumber("hqx", 0), p.getNumber("hqy", 0), true, p.getNumber("score", 0));
  newBuilding(session, kind, wx, wy);
  return JsonValue.newObject().setBool("ok", true).setNumber("gold", gold).setNumber("wood", wood);
}, ["session", "kind", "x", "y"]);

registerReducer("train", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const bId = args.length > 1 ? args[1].asString() : "";
  const kind = args.length > 2 ? args[2].asString() : "worker";

  const p = loadPlayer(session);
  if (p == null || !p.getBool("alive", false)) return err("no empire");
  const b = readBuilding(bId);   // bId is now the building's row key
  if (b == null) return err("no such building");
  if (!b!.done) return err("building not finished");

  let cf: f64 = 0, cg: f64 = 0;
  if (kind == "worker") { if (b!.kind != "hq") return err("train workers at the HQ"); cf = 25; }
  else if (kind == "soldier") { if (b!.kind != "barracks") return err("train soldiers at a barracks"); cf = 30; cg = 20; }
  else return err("unknown unit");

  let gold = p.getNumber("gold", 0);
  let food = p.getNumber("food", 0);
  if (food < cf || gold < cg) return err("not enough resources");
  gold -= cg; food -= cf;
  writePlayer(session, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    gold, p.getNumber("wood", 0), food, p.getNumber("hqx", 0), p.getNumber("hqy", 0), true, p.getNumber("score", 0));
  // Spawn just outside the building with a small rally offset.
  newUnit(session, kind, b!.x + (rnd() * 4.0 - 2.0), b!.y + 2.0 + rnd() * 2.0);
  return JsonValue.newObject().setBool("ok", true).setNumber("gold", gold).setNumber("food", food);
}, ["session", "buildingId", "kind"]);

registerReducer("collect", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const bId = args.length > 1 ? args[1].asString() : "";
  const b = readBuilding(bId);   // bId is now the building's row key
  if (b == null || b!.owner != session) return err("not your building");
  if (!b!.done || (b!.kind != "mine" && b!.kind != "farm")) return err("nothing to collect");

  const now = <f64>nowMs();
  const rate = b!.kind == "mine" ? MINE_RATE : FARM_RATE;
  let accrued = rate * (now - b!.lastColl) / 1000.0;
  if (accrued > PROD_CAP) accrued = PROD_CAP;
  if (accrued < 0) accrued = 0;

  const p = loadPlayer(session);
  if (p == null) return err("no empire");
  let gold = p.getNumber("gold", 0);
  let food = p.getNumber("food", 0);
  if (b!.kind == "mine") gold += accrued; else food += accrued;
  writePlayer(session, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    gold, p.getNumber("wood", 0), food, p.getNumber("hqx", 0), p.getNumber("hqy", 0), true, p.getNumber("score", 0));

  b!.lastColl = now;
  b!.write();
  return JsonValue.newObject().setBool("ok", true).setNumber("collected", accrued)
    .setNumber("gold", gold).setNumber("food", food);
}, ["session", "buildingId"]);

registerReducer("leave", (args: Array<JsonValue>): JsonValue | null => {
  const session = requireString(args, 0, "session");
  const p = loadPlayer(session);
  if (p == null) return null;
  // Keep the empire persistent; just note the departure timestamp.
  writePlayer(session, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    p.getNumber("gold", 0), p.getNumber("wood", 0), p.getNumber("food", 0),
    p.getNumber("hqx", 0), p.getNumber("hqy", 0), p.getBool("alive", true), p.getNumber("score", 0));
  return null;
}, ["session"]);

// ---- the tick: active-chunk authoritative simulation ------------------------

registerReducer("tick", (args: Array<JsonValue>): JsonValue | null => {
  let dt = (args.length > 0 ? args[0].asNumber() : 0) / 1000.0;
  if (dt <= 0) return null;
  if (dt > 0.25) dt = 0.25;   // clamp long stalls

  // Region sharding: when the bridge passes (rx, ry, regionSize) this instance
  // only simulates chunks inside its region, so N module copies tick disjoint
  // slices of the world in parallel (each has its own mutex → its own core).
  // With no region args it runs the whole world (single-module fallback).
  const sharded = args.length > 3;
  const rrx = sharded ? <i32>args[1].asNumber() : 0;
  const rry = sharded ? <i32>args[2].asNumber() : 0;
  const rs = sharded ? <i32>args[3].asNumber() : 1;

  // 1) Find active chunks — scan ONLY the bounded active-set table, region-filtered.
  const chunkArr = JsonValue.parse(scanTable("active", "a:", SCAN_LIMIT));
  const acx = new Array<i32>();
  const acy = new Array<i32>();
  for (let i = 0; i < chunkArr.length; i++) {
    const v = chunkArr.at(i).get("value");
    const cx = <i32>v.getNumber("cx", 0), cy = <i32>v.getNumber("cy", 0);
    if (sharded && (cx / rs != rrx || cy / rs != rry)) continue;   // not my region
    acx.push(cx); acy.push(cy);
  }
  if (acx.length == 0) return null;

  // 2) Load every unit AND building in the active chunks (prefix scans).
  const units = new Array<U>();
  const blds = new Array<B>();
  for (let c = 0; c < acx.length; c++) {
    const ua = JsonValue.parse(scanTable("units", unitPrefix(acx[c], acy[c]), SCAN_LIMIT));
    for (let i = 0; i < ua.length; i++) {
      const e = ua.at(i);
      units.push(U.from(acx[c], acy[c], e.getString("key", ""), e.get("value")));
    }
    const ba = JsonValue.parse(scanTable("buildings", bldgPrefix(acx[c], acy[c]), SCAN_LIMIT));
    for (let i = 0; i < ba.length; i++) {
      const e = ba.at(i);
      blds.push(B.from(acx[c], acy[c], e.getString("key", ""), e.get("value")));
    }
  }

  // Index by row key for combat target lookup within the loaded set.
  const byKey = new Map<string, U>();
  for (let i = 0; i < units.length; i++) byKey.set(units[i].key(), units[i]);
  const byKeyB = new Map<string, B>();
  for (let i = 0; i < blds.length; i++) byKeyB.set(blds[i].key(), blds[i]);

  // 3) Movement + gather + combat intent.
  for (let i = 0; i < units.length; i++) {
    const u = units[i];
    if (u.dead) continue;

    if (u.cmd == "gather") {
      const r = readResource(u.targ);   // targ = resource row key (O(1))
      if (r == null) { u.cmd = "idle"; u.dirty = true; }
      else {
        u.dx = r!.x; u.dy = r!.y;
        const d = dist(u.x, u.y, r!.x, r!.y);
        if (d <= GATHER_RANGE) {
          harvest(u, r!, dt);
          continue;   // standing on the node, harvesting
        }
      }
    } else if (u.cmd == "attack") {
      // targ = target row key — a unit (u:...) OR a building (b:...). Prefer the
      // LOADED object so damage persists in the write-back pass; else read for
      // position and activate its chunk so it loads next tick.
      const targBldg = u.targ.length > 0 && u.targ.charCodeAt(0) == 0x62;   // 'b'
      let tx: f64 = 0, ty: f64 = 0, talive: bool = false;
      let tu: U | null = null;
      let tb: B | null = null;
      if (targBldg) {
        tb = byKeyB.has(u.targ) ? byKeyB.get(u.targ) : null;
        if (tb != null && !tb!.dead && tb!.hp > 0) { tx = tb!.x; ty = tb!.y; talive = true; }
        else { tb = null; const bf = readBuilding(u.targ); if (bf != null && bf!.hp > 0) { tx = bf!.x; ty = bf!.y; talive = true; activateChunk(bf!.cx, bf!.cy, ""); } }
      } else {
        tu = byKey.has(u.targ) ? byKey.get(u.targ) : null;
        if (tu != null && !tu!.dead && tu!.hp > 0) { tx = tu!.x; ty = tu!.y; talive = true; }
        else { tu = null; const tf = readUnit(u.targ); if (tf != null && tf!.hp > 0) { tx = tf!.x; ty = tf!.y; talive = true; activateChunk(tf!.cx, tf!.cy, ""); } }
      }
      if (!talive) { u.cmd = "idle"; u.dirty = true; }
      else {
        u.dx = tx; u.dy = ty;
        const range = targBldg ? ATTACK_RANGE + 2.5 : ATTACK_RANGE;   // buildings are bigger
        const d = dist(u.x, u.y, tx, ty);
        if (d <= range && u.kind == "soldier") {
          if (tu != null) { tu!.hp -= SOLDIER_DMG * dt; tu!.dirty = true; if (tu!.hp <= 0) tu!.dead = true; }
          else if (tb != null) { tb!.hp -= SOLDIER_DMG * dt; tb!.dirty = true; if (tb!.hp <= 0) tb!.dead = true; }
          markActive(u.cx, u.cy);   // keep this fight's chunk hot so damage persists
          continue;   // in range, attacking (hold position)
        }
      }
    }

    // Integrate toward destination.
    const d = dist(u.x, u.y, u.dx, u.dy);
    if (d > REACH) {
      const sp = speedOf(u) * dt;
      const step = sp >= d ? d : sp;
      u.x += (u.dx - u.x) / d * step;
      u.y += (u.dy - u.y) / d * step;
      u.dirty = true;
      if (dist(u.x, u.y, u.dx, u.dy) <= REACH && u.cmd == "move") u.cmd = "idle";
    }
  }

  // 4) Persist / migrate / destroy, and decide chunk liveness.
  const liveCount = new Map<string, i32>();     // chunkKey -> live units
  const pending = new Map<string, bool>();      // chunkKey -> has motion/combat

  for (let i = 0; i < units.length; i++) {
    const u = units[i];
    const oldKey = u.key();
    if (u.dead) { deleteTable("units", oldKey); continue; }

    const ncx = cx_of(u.x);
    const ncy = cy_of(u.y);
    if (ncx != u.cx || ncy != u.cy) {
      // Cross-chunk migration: delete old key, insert under new chunk key.
      deleteTable("units", oldKey);
      u.cx = ncx; u.cy = ncy;
      u.dirty = true;
      activateChunk(ncx, ncy, "");
    }
    if (u.dirty) u.write();

    const ck = chunkKey(u.cx, u.cy);
    liveCount.set(ck, (liveCount.has(ck) ? liveCount.get(ck) : 0) + 1);
    const busy = u.cmd != "idle" || dist(u.x, u.y, u.dx, u.dy) > REACH;
    if (busy) pending.set(ck, true);
  }

  // 5) Buildings: construction progress + combat damage write-back + destruction.
  for (let i = 0; i < blds.length; i++) {
    const b = blds[i];
    const ck = chunkKey(b.cx, b.cy);
    if (b.dead) {
      deleteTable("buildings", b.key());
      if (b.kind == "hq") eliminate(b.owner);   // base destroyed → empire falls
      pending.set(ck, true);                     // fight was happening here
      continue;
    }
    if (!b.done) {
      if (<f64>nowMs() >= b.buildUntil) { b.done = true; b.lastColl = <f64>nowMs(); b.dirty = true; }
      else pending.set(ck, true);   // still constructing -> keep chunk active
    }
    if (b.dirty) b.write();          // persist damage / completion
  }

  // 6) Deactivate settled chunks — DELETE their active-set row (no busy units,
  //    no construction, no fight). Keeps the active table bounded to live play.
  for (let c = 0; c < acx.length; c++) {
    if (!pending.has(chunkKey(acx[c], acy[c]))) clearActive(acx[c], acy[c]);
  }

  return null;
}, ["dtMs"]);

// ---- building struct --------------------------------------------------------

class B {
  id: string = "";
  owner: string = "";
  kind: string = "hq";
  x: f64 = 0;
  y: f64 = 0;
  hp: f64 = HQ_HP;
  maxHp: f64 = HQ_HP;
  done: bool = false;
  buildUntil: f64 = 0;
  lastColl: f64 = 0;
  cx: i32 = 0;
  cy: i32 = 0;
  dirty: bool = false;
  dead: bool = false;

  static from(cx: i32, cy: i32, key: string, v: JsonValue): B {
    const b = new B();
    b.id = v.getString("id", key);
    b.owner = v.getString("owner", "");
    b.kind = v.getString("kind", "hq");
    b.x = v.getNumber("x", 0);
    b.y = v.getNumber("y", 0);
    b.maxHp = v.getNumber("maxHp", HQ_HP);
    b.hp = v.getNumber("hp", b.maxHp);
    b.done = v.getBool("done", false);
    b.buildUntil = v.getNumber("buildUntil", 0);
    b.lastColl = v.getNumber("lastColl", 0);
    b.cx = cx;
    b.cy = cy;
    return b;
  }

  key(): string { return bldgPrefix(this.cx, this.cy) + this.id; }

  toJson(): string {
    return JsonValue.newObject()
      .setString("id", this.id)
      .setString("owner", this.owner)
      .setString("kind", this.kind)
      .setNumber("x", this.x)
      .setNumber("y", this.y)
      .setNumber("hp", this.hp)
      .setNumber("maxHp", this.maxHp)
      .setBool("done", this.done)
      .setNumber("buildUntil", this.buildUntil)
      .setNumber("lastColl", this.lastColl)
      .setNumber("cx", <f64>this.cx)
      .setNumber("cy", <f64>this.cy)
      .setNumber("updated", <f64>nowMs())
      .toString();
  }

  write(): void { writeTable("buildings", this.key(), this.toJson()); }
}

function newBuilding(owner: string, kind: string, x: f64, y: f64): void {
  const b = new B();
  b.id = generateId().toString();
  b.owner = owner;
  b.kind = kind;
  b.x = x;
  b.y = y;
  b.cx = cx_of(x);
  b.cy = cy_of(y);
  if (kind == "hq") { b.maxHp = HQ_HP; b.done = true; b.lastColl = <f64>nowMs(); }
  else if (kind == "barracks") { b.maxHp = BARRACKS_HP; b.done = false; b.buildUntil = <f64>nowMs() + BUILD_SECONDS * 1000.0; }
  else { b.maxHp = PROD_HP; b.done = false; b.buildUntil = <f64>nowMs() + BUILD_SECONDS * 1000.0; }
  b.hp = b.maxHp;
  b.write();
  claimChunk(b.cx, b.cy, owner);
}

// ---- resource struct + helpers ----------------------------------------------

class R {
  id: string = "";
  x: f64 = 0;
  y: f64 = 0;
  kind: string = "gold";   // gold | wood
  amt: f64 = 0;
  cx: i32 = 0;
  cy: i32 = 0;
}

function resKey(cx: i32, cy: i32, id: string): string { return "r:" + cx.toString() + ":" + cy.toString() + ":" + id; }

function writeResource(r: R): void {
  writeTable("resources", resKey(r.cx, r.cy, r.id), JsonValue.newObject()
    .setString("id", r.id)
    .setNumber("x", r.x)
    .setNumber("y", r.y)
    .setString("kind", r.kind)
    .setNumber("amt", r.amt)
    .setNumber("cx", <f64>r.cx)
    .setNumber("cy", <f64>r.cy)
    .setNumber("updated", <f64>nowMs())
    .toString());
}

function seedResources(owner: string, hx: f64, hy: f64): void {
  for (let i = 0; i < 8; i++) {
    const ang = rnd() * 6.2831853;
    const rad = 8.0 + rnd() * 34.0;
    const px = clamp(hx + Math.cos(ang) * rad, 2, WORLD - 2);
    const py = clamp(hy + Math.sin(ang) * rad, 2, WORLD - 2);
    if (!buildable(px, py)) continue;
    const r = new R();
    r.id = generateId().toString();
    r.x = px; r.y = py;
    // Forest tiles favour wood; elsewhere gold ore.
    r.kind = terrain(px, py) == 3 ? "wood" : (rnd() < 0.5 ? "wood" : "gold");
    r.amt = 300.0 + rnd() * 400.0;
    r.cx = cx_of(px); r.cy = cy_of(py);
    writeResource(r);
  }
}

// O(1) lookups by full row key (u:cx:cy:id etc). The key carries cx:cy, so no
// scan is needed. A stale key (entity migrated/deleted since the caller last saw
// it) simply misses and returns null — the caller re-issues off the next
// changefeed update. This is what moved the stress knee from ~45 empires up.
function readResource(key: string): R | null {
  if (key.length == 0) return null;
  const raw = readTable("resources", key);
  if (raw == null) return null;
  const v = JsonValue.parse(raw);
  const r = new R();
  r.id = v.getString("id", "");
  r.x = v.getNumber("x", 0);
  r.y = v.getNumber("y", 0);
  r.kind = v.getString("kind", "gold");
  r.amt = v.getNumber("amt", 0);
  r.cx = <i32>v.getNumber("cx", 0);
  r.cy = <i32>v.getNumber("cy", 0);
  return r;
}

function readUnit(key: string): U | null {
  if (key.length == 0) return null;
  const raw = readTable("units", key);
  if (raw == null) return null;
  const v = JsonValue.parse(raw);
  return U.from(<i32>v.getNumber("cx", 0), <i32>v.getNumber("cy", 0), v.getString("id", ""), v);
}

function readBuilding(key: string): B | null {
  if (key.length == 0) return null;
  const raw = readTable("buildings", key);
  if (raw == null) return null;
  const v = JsonValue.parse(raw);
  return B.from(<i32>v.getNumber("cx", 0), <i32>v.getNumber("cy", 0), v.getString("id", ""), v);
}

// Worker drains a node straight into the owner's stockpile (authoritative).
function harvest(u: U, r: R, dt: f64): void {
  let take = GATHER_RATE * dt;
  if (take > r.amt) take = r.amt;
  r.amt -= take;
  if (r.amt <= 0.0001) {
    deleteTable("resources", resKey(r.cx, r.cy, r.id));
    u.cmd = "idle"; u.dirty = true;
  } else {
    writeResource(r);
  }
  const p = loadPlayer(u.owner);
  if (p == null) return;
  let gold = p.getNumber("gold", 0);
  let wood = p.getNumber("wood", 0);
  if (r.kind == "gold") gold += take; else wood += take;
  writePlayer(u.owner, p.getString("name", ""), p.getString("faction", ""), p.getString("color", "#41d6ff"),
    gold, wood, p.getNumber("food", 0), p.getNumber("hqx", 0), p.getNumber("hqy", 0), true, p.getNumber("score", 0));
  activateChunk(u.cx, u.cy, "");
}

// ---- order dispatch (shared by move/gather/attack) --------------------------

function commandUnits(session: string, keys: Array<string>, cmd: string, tx: f64, ty: f64, targ: string): void {
  for (let i = 0; i < keys.length; i++) {
    const u = readUnit(keys[i]);   // O(1) by row key
    if (u == null || u!.owner != session) continue;
    u!.cmd = cmd;
    u!.dx = tx; u!.dy = ty;
    u!.targ = targ;
    u!.write();
    activateChunk(u!.cx, u!.cy, "");
  }
}

// ---- utils ------------------------------------------------------------------

function dist(ax: f64, ay: f64, bx: f64, by: f64): f64 {
  const dx = ax - bx, dy = ay - by;
  return Math.sqrt(dx * dx + dy * dy);
}

function clamp(v: f64, lo: f64, hi: f64): f64 { return v < lo ? lo : (v > hi ? hi : v); }

function clampName(s: string): string { return s.length > 18 ? s.substring(0, 18) : s; }

function splitCsv(s: string): Array<string> {
  const out = new Array<string>();
  if (s.length == 0) return out;
  const parts = s.split(",");
  for (let i = 0; i < parts.length; i++) if (parts[i].length > 0) out.push(parts[i]);
  return out;
}

function err(msg: string): JsonValue {
  return JsonValue.newObject().setBool("ok", false).setString("error", msg);
}

function requireString(args: Array<JsonValue>, index: i32, name: string): string {
  const v = index < args.length ? args[index].asString() : "";
  if (v.length == 0) abortCall("missing required argument: " + name);
  return v;
}
