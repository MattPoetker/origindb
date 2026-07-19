// cube.io — an agar.io-style game whose authoritative simulation runs entirely
// as OriginDB WASM reducers. All game state lives in OriginDB tables, so it
// persists through the WAL and streams to every player over the changefeed.
//
// Players are cubes. They roam a huge arena, eat glowing food cubes to grow,
// and can eat smaller players (absorbing their entire mass). A speed boost
// makes them faster but sheds mass as dropped cubes — an escape hatch.
//
// Identity is the session id: joining with the same session returns the SAME
// player row, so a reconnecting player keeps their identity (and best score).
//
// Tables:
//   players  key = session id   {id,name,color,x,y,mass,dirx,diry,boost,alive,score,dropAcc,updated}
//   food     key = food id       {id,x,y,v}
//
// Reducers:
//   __init                                seed the arena with food
//   join(session,name,color)              create or revive a player (by session)
//   input(session,dirx,diry,boost)        set movement intent + boost flag
//   tick(dtMs)                            advance the whole simulation one step
//   leave(session)                        mark a player gone

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

setModuleInfo("cubeio", "2.2.0");
declareTable("players");
declareTable("food");

// ---- world constants (keep in sync with public/game.js) --------------------

const WORLD: f64 = 12000.0;      // arena is WORLD x WORLD units — huge
const FOOD_TARGET: i32 = 1000;   // glowing cubes kept alive in the arena
                                 // (bounded so a full-scan tick stays < ~60ms)
const FOOD_VALUE: f64 = 2.0;     // mass per food cube
const FOOD_R: f64 = 10.0;        // food radius (visual + pickup)
const BASE_MASS: f64 = 12.0;     // starting / respawn mass
const SPEED_BASE: f64 = 340.0;   // units/sec for a base-mass cube
const SPEED_MIN: f64 = 55.0;
const BOOST_MULT: f64 = 1.9;     // speed multiplier while boosting
const BOOST_MIN_MASS: f64 = 18.0;      // can't boost below this
const BOOST_DRAIN_PER_SEC: f64 = 0.22; // fraction of mass shed per second boosting
const DROP_CHUNK: f64 = 2.0;     // shed accumulates into food cubes of this size
const EAT_RATIO: f64 = 1.10;     // must be 10% heavier to eat another player
const RADIUS_K: f64 = 4.0;       // radius = sqrt(mass) * RADIUS_K
const SCAN_LIMIT: i32 = 100000;  // scan ALL rows (SDK default of 0 caps at 1000!)
const REFILL_MAX: i32 = 40;      // cap food respawned per tick (bounds write cost)

function radius(mass: f64): f64 {
  return Math.sqrt(mass) * RADIUS_K;
}

// Server-authoritative food reward. Growth per food cube is decided HERE by the
// tick reducer at eat time — not stored on the food row — so tuning FOOD_VALUE
// (or hot-swapping a new module) changes every cube in the arena instantly, no
// data migration needed. `fd` is passed so a future reward could depend on the
// cube's size/position.
function foodReward(fd: F): f64 {
  return FOOD_VALUE;
}

// ---- tiny deterministic PRNG (xorshift, seeded from the host clock) ---------

let g_seed: u64 = 0;
function rnd(): f64 {
  if (g_seed == 0) g_seed = (<u64>nowMs()) ^ 0x9e3779b97f4a7c15 ^ (<u64>generateId());
  let x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_seed = x;
  return <f64>(x >> 11) / 9007199254740992.0;  // [0,1)
}
function randPos(): f64 {
  return rnd() * WORLD;
}

// ---- player struct ----------------------------------------------------------

class P {
  id: string = "";
  name: string = "";
  color: string = "#7cf";
  x: f64 = 0;
  y: f64 = 0;
  mass: f64 = BASE_MASS;
  dirx: f64 = 0;
  diry: f64 = 0;
  boost: bool = false;
  alive: bool = true;
  score: f64 = BASE_MASS;
  dropAcc: f64 = 0;
  dirty: bool = false;   // needs write-back this tick

  static from(key: string, v: JsonValue): P {
    const p = new P();
    p.id = key;
    p.name = v.getString("name", "cube");
    p.color = v.getString("color", "#7cf");
    p.x = v.getNumber("x", 0);
    p.y = v.getNumber("y", 0);
    p.mass = v.getNumber("mass", BASE_MASS);
    p.dirx = v.getNumber("dirx", 0);
    p.diry = v.getNumber("diry", 0);
    p.boost = v.getBool("boost", false);
    p.alive = v.getBool("alive", true);
    p.score = v.getNumber("score", BASE_MASS);
    p.dropAcc = v.getNumber("dropAcc", 0);
    return p;
  }

  toJson(): string {
    return JsonValue.newObject()
      .setString("id", this.id)
      .setString("name", this.name)
      .setString("color", this.color)
      .setNumber("x", this.x)
      .setNumber("y", this.y)
      .setNumber("mass", this.mass)
      .setNumber("dirx", this.dirx)
      .setNumber("diry", this.diry)
      .setBool("boost", this.boost)
      .setBool("alive", this.alive)
      .setNumber("score", this.score)
      .setNumber("dropAcc", this.dropAcc)
      .setNumber("updated", <f64>nowMs())
      .toString();
  }

  write(): void {
    writeTable("players", this.id, this.toJson());
  }
}

// ---- food helpers -----------------------------------------------------------

function spawnFood(x: f64, y: f64, v: f64): void {
  const id = generateId().toString();
  const row = JsonValue.newObject()
    .setString("id", id)
    .setNumber("x", x)
    .setNumber("y", y)
    .setNumber("v", v);
  writeTable("food", id, row.toString());
}

function spawnFoodRandom(): void {
  spawnFood(randPos(), randPos(), FOOD_VALUE);
}

// ---- reducers ---------------------------------------------------------------

registerReducer(
  "__init",
  (_args: Array<JsonValue>): JsonValue | null => {
    // Seed the arena. On a fresh DB the food table is empty; top it up.
    const existing = countFood();
    for (let i = existing; i < FOOD_TARGET; i++) spawnFoodRandom();
    return JsonValue.newObject().setNumber("food", <f64>FOOD_TARGET);
  },
  []
);

registerReducer(
  "join",
  (args: Array<JsonValue>): JsonValue | null => {
    const session = requireString(args, 0, "session");
    const name = args.length > 1 ? args[1].asString() : "cube";
    const color = args.length > 2 ? args[2].asString() : "#7cf";

    const raw = readTable("players", session);
    const p = new P();
    p.id = session;
    p.name = name.length > 0 ? (name.length > 16 ? name.substring(0, 16) : name) : "cube";
    p.color = color;

    if (raw != null) {
      const prev = JsonValue.parse(raw);
      p.score = prev.getNumber("score", BASE_MASS);
      if (prev.getBool("alive", false)) {
        // Reconnecting while still alive: resume exactly where they were.
        p.x = prev.getNumber("x", randPos());
        p.y = prev.getNumber("y", randPos());
        p.mass = prev.getNumber("mass", BASE_MASS);
      } else {
        // Rejoining after death: same identity, fresh small cube.
        p.x = randPos();
        p.y = randPos();
        p.mass = BASE_MASS;
      }
    } else {
      p.x = randPos();
      p.y = randPos();
      p.mass = BASE_MASS;
    }
    p.alive = true;
    p.boost = false;
    p.dirx = 0;
    p.diry = 0;
    p.write();

    return JsonValue.newObject()
      .setString("id", p.id)
      .setNumber("x", p.x)
      .setNumber("y", p.y)
      .setNumber("mass", p.mass)
      .setNumber("world", WORLD);
  },
  ["session", "name", "color"]
);

registerReducer(
  "input",
  (args: Array<JsonValue>): JsonValue | null => {
    const session = requireString(args, 0, "session");
    const raw = readTable("players", session);
    if (raw == null) return null;
    const prev = JsonValue.parse(raw);
    if (!prev.getBool("alive", false)) return null;

    let dx = args.length > 1 ? args[1].asNumber() : 0;
    let dy = args.length > 2 ? args[2].asNumber() : 0;
    const boost = args.length > 3 ? args[3].asBool() : false;
    const len = Math.sqrt(dx * dx + dy * dy);
    if (len > 0.0001) { dx /= len; dy /= len; } else { dx = 0; dy = 0; }

    const p = P.from(session, prev);
    p.dirx = dx;
    p.diry = dy;
    p.boost = boost;
    p.write();
    return null;
  },
  ["session", "dirx", "diry", "boost"]
);

registerReducer(
  "leave",
  (args: Array<JsonValue>): JsonValue | null => {
    const session = requireString(args, 0, "session");
    const raw = readTable("players", session);
    if (raw == null) return null;
    const p = P.from(session, JsonValue.parse(raw));
    p.alive = false;
    p.boost = false;
    p.write();
    return null;
  },
  ["session"]
);

registerReducer(
  "tick",
  (args: Array<JsonValue>): JsonValue | null => {
    let dt = (args.length > 0 ? args[0].asNumber() : 0) / 1000.0;
    if (dt <= 0) return null;
    if (dt > 0.1) dt = 0.1;  // clamp huge gaps (tab was backgrounded)

    // Load everything once.
    const players = loadPlayers();
    const food = loadFood();

    // 1) Move + boost drain.
    for (let i = 0; i < players.length; i++) {
      const p = players[i];
      if (!p.alive) continue;
      let speed = SPEED_BASE * Math.pow(BASE_MASS / p.mass, 0.30);
      if (speed < SPEED_MIN) speed = SPEED_MIN;

      if (p.boost && p.mass > BOOST_MIN_MASS) {
        speed *= BOOST_MULT;
        const drain = p.mass * BOOST_DRAIN_PER_SEC * dt;
        p.mass -= drain;
        if (p.mass < BASE_MASS) p.mass = BASE_MASS;
        // Shed accumulates into discrete food cubes dropped behind the player.
        p.dropAcc += drain;
        while (p.dropAcc >= DROP_CHUNK) {
          const bx = p.x - p.dirx * (radius(p.mass) + 6.0);
          const by = p.y - p.diry * (radius(p.mass) + 6.0);
          spawnFood(clamp(bx, 0, WORLD), clamp(by, 0, WORLD), DROP_CHUNK);
          p.dropAcc -= DROP_CHUNK;
          p.dirty = true;
        }
      }

      if (p.dirx != 0 || p.diry != 0) {
        p.x = clamp(p.x + p.dirx * speed * dt, 0, WORLD);
        p.y = clamp(p.y + p.diry * speed * dt, 0, WORLD);
        p.dirty = true;
      } else if (p.boost) {
        p.dirty = true;  // boost changed mass even while standing still
      }
    }

    // 2) Eat food.
    for (let i = 0; i < players.length; i++) {
      const p = players[i];
      if (!p.alive) continue;
      const r = radius(p.mass) + FOOD_R;   // food has its own radius
      const r2 = r * r;
      for (let f = 0; f < food.length; f++) {
        const fd = food[f];
        if (fd.eaten) continue;
        const ddx = p.x - fd.x;
        const ddy = p.y - fd.y;
        if (ddx * ddx + ddy * ddy <= r2) {
          // Server-authoritative growth: the reducer decides the reward at eat
          // time (see foodReward), so it's not baked into the food row and a
          // code/hot-swap change re-values the whole arena instantly.
          p.mass += foodReward(fd);
          p.dirty = true;
          fd.eaten = true;
          deleteTable("food", fd.id);
        }
      }
      if (p.mass > p.score) p.score = p.mass;
    }

    // 3) Eat players. Bigger absorbs the smaller's ENTIRE mass.
    for (let i = 0; i < players.length; i++) {
      const a = players[i];
      if (!a.alive) continue;
      for (let j = 0; j < players.length; j++) {
        if (i == j) continue;
        const b = players[j];
        if (!b.alive) continue;
        if (a.mass <= b.mass * EAT_RATIO) continue;  // not big enough
        const ra = radius(a.mass);
        const ddx = a.x - b.x;
        const ddy = a.y - b.y;
        const dist = Math.sqrt(ddx * ddx + ddy * ddy);
        // A engulfs B when B's center is well inside A.
        if (dist < ra - radius(b.mass) * 0.35) {
          a.mass += b.mass;         // absorb all their mass
          if (a.mass > a.score) a.score = a.mass;
          a.dirty = true;
          b.alive = false;
          b.mass = 0;
          b.boost = false;
          b.dirty = true;
        }
      }
    }

    // 4) Persist changed players.
    for (let i = 0; i < players.length; i++) {
      if (players[i].dirty) players[i].write();
    }

    // 5) Keep the arena stocked with food. Respawn only what's missing, capped
    //    per tick so a tick never turns into thousands of writes.
    let liveFood = 0;
    for (let f = 0; f < food.length; f++) if (!food[f].eaten) liveFood++;
    let refill = FOOD_TARGET - liveFood;
    if (refill > REFILL_MAX) refill = REFILL_MAX;
    for (let i = 0; i < refill; i++) spawnFoodRandom();

    return null;
  },
  ["dtMs"]
);

// ---- loaders ----------------------------------------------------------------

class F {
  id: string = "";
  x: f64 = 0;
  y: f64 = 0;
  v: f64 = FOOD_VALUE;
  eaten: bool = false;
}

function loadPlayers(): Array<P> {
  const out = new Array<P>();
  const arr = JsonValue.parse(scanTable("players", "", SCAN_LIMIT));
  for (let i = 0; i < arr.length; i++) {
    const e = arr.at(i);
    out.push(P.from(e.getString("key", ""), e.get("value")));
  }
  return out;
}

function loadFood(): Array<F> {
  const out = new Array<F>();
  const arr = JsonValue.parse(scanTable("food", "", SCAN_LIMIT));
  for (let i = 0; i < arr.length; i++) {
    const e = arr.at(i);
    const v = e.get("value");
    const f = new F();
    f.id = e.getString("key", "");
    f.x = v.getNumber("x", 0);
    f.y = v.getNumber("y", 0);
    f.v = v.getNumber("v", FOOD_VALUE);
    out.push(f);
  }
  return out;
}

function countFood(): i32 {
  return JsonValue.parse(scanTable("food", "", SCAN_LIMIT)).length;
}

// ---- utils ------------------------------------------------------------------

function clamp(v: f64, lo: f64, hi: f64): f64 {
  return v < lo ? lo : (v > hi ? hi : v);
}

function requireString(args: Array<JsonValue>, index: i32, name: string): string {
  const v = index < args.length ? args[index].asString() : "";
  if (v.length == 0) abortCall("missing required argument: " + name);
  return v;
}
