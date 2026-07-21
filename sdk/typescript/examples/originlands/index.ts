// OriginLands — M1 backend module (accounts + movement + AoI chunking).
//
// The whole server for the persistent world is this one WASM module:
//   createCharacter(name, password, appearance)  -> unique name + salted hash
//   login(name, password)                          -> resume, rebind session
//   move(name, x, z, yaw)                           -> authoritative position
//   leaveWorld(name)                               -> mark offline
//
// Auth model: the password (salted SHA-256) proves identity across sessions;
// on create/login we bind the row's `owner` to the caller's senderIdentity()
// (server-assigned, unspoofable per connection), so move/leave are authorized
// without sending the password on every tick.
import {
  JsonValue, abortCall, declareTable, generateId, nowMs,
  readTable, registerReducer, senderIdentity, setModuleInfo, setResult, writeTable,
} from "../../assembly/index";

export {
  origindb_alloc, origindb_free, origindb_describe, origindb_invoke, __origindb_abort,
} from "../../assembly/index";

setModuleInfo("originlands", "0.1.0");
declareTable("players");

const CHUNK: f64 = 48.0;     // world units per AoI chunk (must match the client)
const SPAWN: f64 = 0.0;      // spawn centre; jittered per-name so players don't stack
const MAX_STEP: f64 = 10.0;  // anti-teleport clamp per move call (metres)

// ======================================================================
// SHA-256 (demo-grade salted password hashing; no host/engine dependency).
// Upgrade path: swap for argon2/bcrypt via a host function later.
// ======================================================================
// prettier-ignore
const K256: u32[] = [
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
];

function rotr(x: u32, n: u32): u32 { return (x >>> n) | (x << (32 - n)); }

function hex32(v: u32): string {
  const d = "0123456789abcdef";
  let s = "";
  for (let i: i32 = 7; i >= 0; i--) s += d.charAt(<i32>((v >>> (<u32>(i * 4))) & 0xf));
  return s;
}

function sha256Hex(input: string): string {
  const src = Uint8Array.wrap(String.UTF8.encode(input));
  const ml = src.length;
  const withOne = ml + 1;
  const pad = ((56 - (withOne % 64)) + 64) % 64;
  const total = withOne + pad + 8;
  const msg = new Uint8Array(total);
  for (let i = 0; i < ml; i++) msg[i] = src[i];
  msg[ml] = 0x80;
  const bitLen: u64 = (<u64>ml) * 8;
  for (let i = 0; i < 8; i++) msg[total - 1 - i] = <u8>((bitLen >> (<u64>(8 * i))) & 0xff);

  let h0: u32 = 0x6a09e667, h1: u32 = 0xbb67ae85, h2: u32 = 0x3c6ef372, h3: u32 = 0xa54ff53a;
  let h4: u32 = 0x510e527f, h5: u32 = 0x9b05688c, h6: u32 = 0x1f83d9ab, h7: u32 = 0x5be0cd19;

  const w = new StaticArray<u32>(64);
  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) {
      const j = off + i * 4;
      w[i] = ((<u32>msg[j]) << 24) | ((<u32>msg[j + 1]) << 16) | ((<u32>msg[j + 2]) << 8) | (<u32>msg[j + 3]);
    }
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = h + S1 + ch + K256[i] + w[i];
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = S0 + maj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
  }
  return hex32(h0) + hex32(h1) + hex32(h2) + hex32(h3) + hex32(h4) + hex32(h5) + hex32(h6) + hex32(h7);
}

// ======================================================================
// helpers
// ======================================================================
function chunkOf(x: f64, z: f64): string {
  const cx = <i32>Math.floor(x / CHUNK);
  const cz = <i32>Math.floor(z / CHUNK);
  return cx.toString() + ":" + cz.toString();
}

function pkey(name: string): string { return "p:" + name; }

// a stable hash of a string -> [0,1) for deterministic spawn jitter
function frac(s: string): f64 {
  let h: u32 = 2166136261;
  for (let i = 0; i < s.length; i++) { h = (h ^ (<u32>s.charCodeAt(i))) * 16777619; }
  return (<f64>(h >>> 8)) / 16777216.0;
}

function validName(n: string): bool {
  if (n.length < 1 || n.length > 16) return false;
  for (let i = 0; i < n.length; i++) {
    const c = n.charCodeAt(i);
    const ok = (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95 || c == 45;
    if (!ok) return false;
  }
  return true;
}

// public view of a player row (never leaks salt/hash/owner)
function publicRow(row: JsonValue): JsonValue {
  const out = JsonValue.newObject()
    .setString("name", row.getString("name", ""))
    .setNumber("x", row.getNumber("x", 0))
    .setNumber("z", row.getNumber("z", 0))
    .setNumber("yaw", row.getNumber("yaw", 0))
    .setString("chunk", row.getString("chunk", "0:0"))
    .setBool("online", row.getBool("online", false));
  if (row.has("appearance")) out.set("appearance", row.get("appearance"));
  return out;
}

function errResult(msg: string): JsonValue {
  const j = JsonValue.newObject().setBool("ok", false).setString("error", msg);
  setResult(j.toString());
  return j;
}

// ======================================================================
// reducers
// ======================================================================
function createCharacter(args: Array<JsonValue>): JsonValue | null {
  const name = args.length > 0 ? args[0].asString() : "";
  const password = args.length > 1 ? args[1].asString() : "";
  const appearance = args.length > 2 ? args[2] : JsonValue.newObject();

  if (!validName(name)) return errResult("invalid name (1-16 letters/digits/_/-)");
  if (password.length < 4) return errResult("password must be at least 4 characters");
  if (readTable("players", pkey(name)) != null) return errResult("name already taken");

  const salt = generateId().toString();
  const hash = sha256Hex(salt + ":" + password);

  // spawn near the centre, jittered by name so newcomers don't overlap
  const ang = frac(name) * 6.28318;
  const rad = 6.0 + frac(name + "r") * 18.0;
  const x = SPAWN + Math.cos(ang) * rad;
  const z = SPAWN + Math.sin(ang) * rad;
  const now = <f64>nowMs();

  const row = JsonValue.newObject()
    .setString("name", name)
    .setString("salt", salt)
    .setString("hash", hash)
    .setString("owner", senderIdentity())
    .setNumber("x", x).setNumber("z", z).setNumber("yaw", 0)
    .setString("chunk", chunkOf(x, z))
    .setBool("online", true)
    .setNumber("created", now).setNumber("lastSeen", now);
  row.set("appearance", appearance);
  writeTable("players", pkey(name), row.toString());

  const pub = publicRow(row).setBool("ok", true);
  setResult(pub.toString());
  return pub;
}

function login(args: Array<JsonValue>): JsonValue | null {
  const name = args.length > 0 ? args[0].asString() : "";
  const password = args.length > 1 ? args[1].asString() : "";

  const raw = readTable("players", pkey(name));
  if (raw == null) return errResult("no such character");
  const row = JsonValue.parse(raw!);

  const expect = sha256Hex(row.getString("salt", "") + ":" + password);
  if (expect != row.getString("hash", "\x00")) return errResult("wrong password");

  // rebind this session + mark online
  row.setString("owner", senderIdentity());
  row.setBool("online", true);
  row.setNumber("lastSeen", <f64>nowMs());
  writeTable("players", pkey(name), row.toString());

  const pub = publicRow(row).setBool("ok", true);
  setResult(pub.toString());
  return pub;
}

function move(args: Array<JsonValue>): JsonValue | null {
  const name = args.length > 0 ? args[0].asString() : "";
  const nx = args.length > 1 ? args[1].asNumber() : 0;
  const nz = args.length > 2 ? args[2].asNumber() : 0;
  const yaw = args.length > 3 ? args[3].asNumber() : 0;

  const raw = readTable("players", pkey(name));
  if (raw == null) { abortCall("move: no such character"); return null; }
  const row = JsonValue.parse(raw!);

  // authorize: only the session that owns this character may move it
  if (senderIdentity() != row.getString("owner", "\x00")) {
    abortCall("move: not your character");
    return null;
  }

  // server-authoritative clamp: no teleporting past MAX_STEP per call
  const ox = row.getNumber("x", 0), oz = row.getNumber("z", 0);
  let tx = nx, tz = nz;
  const dx = nx - ox, dz = nz - oz;
  const dist = Math.sqrt(dx * dx + dz * dz);
  if (dist > MAX_STEP) { const k = MAX_STEP / dist; tx = ox + dx * k; tz = oz + dz * k; }

  row.setNumber("x", tx).setNumber("z", tz).setNumber("yaw", yaw);
  row.setString("chunk", chunkOf(tx, tz));
  row.setBool("online", true);
  row.setNumber("lastSeen", <f64>nowMs());
  writeTable("players", pkey(name), row.toString());
  return null;
}

function leaveWorld(args: Array<JsonValue>): JsonValue | null {
  const name = args.length > 0 ? args[0].asString() : "";
  const raw = readTable("players", pkey(name));
  if (raw == null) return null;
  const row = JsonValue.parse(raw!);
  if (senderIdentity() != row.getString("owner", "\x00")) return null; // silent
  row.setBool("online", false);
  row.setNumber("lastSeen", <f64>nowMs());
  writeTable("players", pkey(name), row.toString());
  return null;
}

registerReducer("createCharacter", createCharacter, ["name", "password", "appearance"]);
registerReducer("login", login, ["name", "password"]);
registerReducer("move", move, ["name", "x", "z", "yaw"]);
registerReducer("leaveWorld", leaveWorld, ["name"]);
