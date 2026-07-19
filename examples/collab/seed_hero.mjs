// Keeps the origindb.org hero board feeling alive: a few welcome notes plus one
// gentle "OriginDB" cursor drifting in a slow figure-8. Everything is a real
// reducer call through the collab bridge — no fakery, it's the live board.
//
// Usage: node seed_hero.mjs [--api http://localhost:9090]
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
const API = flag("--api", "http://localhost:9090") + "/api/call";

const call = (reducer, args) =>
  fetch(API, { method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ reducer, args }) }).catch(() => {});

// Welcome notes, kept on the right half so they never cover the headline.
const NOTES = [
  ["welcome 👋 you're on a live OriginDB board", 0.60, 0.28, "#4da3ff"],
  ["move your cursor — everyone here sees it", 0.74, 0.46, "#5ee6a8"],
  ["double-click anywhere to pin a note", 0.62, 0.64, "#ffd166"],
  ["every pixel here is one row + one changefeed", 0.80, 0.80, "#c792ff"],
];

async function seedNotes() {
  await call("clearBoard", ["origindb"]);
  for (const [text, x, y, color] of NOTES) await call("addNote", ["OriginDB", color, x, y, text]);
  console.log(`seeded ${NOTES.length} welcome notes`);
}
await seedNotes();
// re-seed periodically in case a visitor clears the board
setInterval(seedNotes, 90_000);

// one gentle cursor drifting forever (keeps its ts fresh so it always renders)
let t = 0;
setInterval(() => {
  t += 0.05;
  const x = 0.70 + Math.cos(t) * 0.16;
  const y = 0.50 + Math.sin(t * 1.4) * 0.20;
  call("moveCursor", ["OriginDB", "#4da3ff", x, y, ""]);
}, 180);
console.log("hero cursor drifting…");
