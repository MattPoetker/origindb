// Subscription-scale probe: open N concurrent ws clients, each with its OWN
// viewport AOI subscription (units+buildings+resources), like N browsers. Report
// how many connected, got their initial_state, and the avg rows delivered per
// client (should be a small viewport, not the whole world).
const WS = process.argv[2] || "ws://localhost:8789";
const N = parseInt(process.argv[3] || "250", 10);

let connected = 0, gotInitial = 0, totalRows = 0, errors = 0;
const socks = [];

function spawn(i) {
  return new Promise((res) => {
    const ws = new WebSocket(WS);
    socks.push(ws);
    // random viewport somewhere in the 60x60 chunk world (~8x8 chunks)
    const cx0 = (i * 7) % 52, cy0 = (i * 13) % 52;
    let got = 0;
    ws.onopen = () => {
      connected++;
      for (const t of ["units", "buildings", "resources"])
        ws.send(JSON.stringify({ type: "sql_subscribe",
          sql: `SELECT * FROM ${t} WHERE cx >= ${cx0} AND cx <= ${cx0 + 8} AND cy >= ${cy0} AND cy <= ${cy0 + 8}` }));
      res();
    };
    ws.onmessage = (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.type === "initial_state") { totalRows += (m.rows || []).length; if (++got === 3) gotInitial++; }
    };
    ws.onerror = () => { errors++; res(); };
  });
}

const t0 = Date.now();
// ramp connections in small batches to avoid a thundering herd
for (let i = 0; i < N; i += 25) {
  await Promise.all(Array.from({ length: Math.min(25, N - i) }, (_, j) => spawn(i + j)));
  await new Promise((r) => setTimeout(r, 120));
}
await new Promise((r) => setTimeout(r, 3000));   // let initial_states arrive

console.log(`clients=${N}  connected=${connected}  gotInitialState=${gotInitial}  errors=${errors}`);
console.log(`avg rows/client (viewport) = ${(totalRows / Math.max(1, gotInitial)).toFixed(1)}  (whole-world would be ~900+ each)`);
console.log(`elapsed ${((Date.now() - t0) / 1000).toFixed(1)}s`);
for (const s of socks) { try { s.close(); } catch {} }
process.exit(0);
