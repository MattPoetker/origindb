// Marble Clash stress harness. Spawns `arenas * per` bot marbles directly over
// gRPC (spread evenly across arenas) and steers each with a wandering direction
// at STEER_HZ. The TICK load is driven by the bridge (server.js) at 60 Hz; the
// bridge's own "[health]" line (maxTickMs / slow ratio) is the real signal for
// where 60 Hz breaks down. This process just manufactures the population + input.
//
// Usage: node bots.js [--grpc localhost:50053] [--arenas 8] [--per 60]
//        [--steer-hz 5] [--seconds 0(forever)]

import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };

const GRPC = flag("--grpc", "localhost:50053");
const ARENAS = parseInt(flag("--arenas", "8"), 10);
const PER = parseInt(flag("--per", "60"), 10);
const STEER_HZ = parseInt(flag("--steer-hz", "5"), 10);
const SECONDS = parseInt(flag("--seconds", "0"), 10);
const RAMP = parseInt(flag("--ramp", "0"), 10);   // spawns/sec (0 = all at once)
const TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");

const proto = grpc.loadPackageDefinition(
  protoLoader.loadSync(resolve(__dirname, "../../proto/origindb.proto"), { keepCase: true, longs: String, defaults: true })
).origindb.grpc;
const wasm = new proto.WasmService(GRPC, grpc.credentials.createInsecure());
function meta() { const m = new grpc.Metadata(); if (TOKEN) m.set("authorization", `Bearer ${TOKEN}`); return m; }
function val(v) {
  if (typeof v === "boolean") return { bool_value: v };
  if (typeof v === "number") return Number.isInteger(v) ? { int64_value: v } : { double_value: v };
  return { string_value: String(v) };
}
function exec(mod, reducer, args, timeoutMs = 5000) {
  return new Promise((res, rej) => wasm.ExecuteReducer(
    { module_name: mod, reducer_name: reducer, sender_identity: "bots", args: args.map(val) },
    meta(), { deadline: Date.now() + timeoutMs }, (e, r) => (e ? rej(e) : res(r))));
}

const COLORS = ["#4da3ff", "#ff6b6b", "#ffd166", "#5ee6a8", "#c792ff", "#ff9f6b", "#4ee1e1", "#f078c8"];
const bots = [];   // {session, mod, arena, dx, dy}
let spawnErrs = 0, steerErrs = 0, steerOk = 0;

async function spawnAll() {
  const total = ARENAS * PER;
  process.stdout.write(`spawning ${total} bots (${ARENAS} arenas x ${PER})...\n`);
  let done = 0;
  for (let a = 0; a < ARENAS; a++) {
    const mod = `marble_${a}`;
    // spawn this arena's bots in small concurrent batches
    for (let i = 0; i < PER; i += 20) {
      const batch = [];
      for (let j = i; j < Math.min(i + 20, PER); j++) {
        const session = `bot_${a}_${j}`;
        const color = COLORS[(a + j) % COLORS.length];
        batch.push(exec(mod, "spawn", [session, `bot${a}-${j}`, color, a])
          .then(() => { bots.push({ session, mod, arena: a, dx: Math.random() * 2 - 1, dy: Math.random() * 2 - 1 }); done++; })
          .catch(() => { spawnErrs++; }));
      }
      await Promise.all(batch);
    }
    process.stdout.write(`  arena ${a}: ${done} spawned\n`);
  }
  process.stdout.write(`spawned=${done} errs=${spawnErrs}\n`);
}

function steerLoop() {
  const period = Math.max(20, Math.floor(1000 / STEER_HZ));
  setInterval(() => {
    for (const b of bots) {
      // wander: occasionally pick a new heading
      if (Math.random() < 0.25) { b.dx = Math.random() * 2 - 1; b.dy = Math.random() * 2 - 1; }
      exec(b.mod, "steer", [b.session, b.dx, b.dy], 3000).then(() => steerOk++).catch(() => steerErrs++);
    }
  }, period);
}

setInterval(() => {
  process.stdout.write(`[bots] pop=${bots.length} steerOk/s≈${Math.round(steerOk / 3)} steerErr=${steerErrs}\n`);
  steerOk = 0;
}, 3000);

// Trickle mode: a steady stream of arrivals at RAMP spawns/sec, round-robin
// across arenas, up to arenas*per, so you watch the arenas fill in real time.
function trickle() {
  const total = ARENAS * PER;
  const perArena = new Array(ARENAS).fill(0);
  let launched = 0, seq = 0;
  process.stdout.write(`trickling ${total} bots @ ${RAMP}/s across ${ARENAS} arenas...\n`);
  const period = Math.max(20, Math.floor(1000 / RAMP));
  const timer = setInterval(() => {
    if (launched >= total) { clearInterval(timer); process.stdout.write(`stream complete: ${total} launched\n`); return; }
    // pick next arena with room (round-robin)
    let a = -1;
    for (let k = 0; k < ARENAS; k++) { const idx = (seq + k) % ARENAS; if (perArena[idx] < PER) { a = idx; break; } }
    if (a < 0) { clearInterval(timer); return; }
    seq = (a + 1) % ARENAS;
    const j = perArena[a]++; launched++;
    const session = `bot_${a}_${j}`;
    const color = COLORS[(a + j) % COLORS.length];
    exec(`marble_${a}`, "spawn", [session, `bot${a}-${j}`, color, a])
      .then(() => bots.push({ session, mod: `marble_${a}`, arena: a, dx: Math.random() * 2 - 1, dy: Math.random() * 2 - 1 }))
      .catch(() => { spawnErrs++; });
  }, period);
}

async function main() {
  steerLoop();                       // start steering immediately; bots join it as they arrive
  if (RAMP > 0) trickle();
  else await spawnAll();
  if (SECONDS > 0) setTimeout(() => { process.stdout.write("done.\n"); process.exit(0); }, SECONDS * 1000);
}
main();
