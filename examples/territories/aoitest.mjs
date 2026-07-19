// AOI verification (Node v22 native WebSocket). Does a viewport WHERE
// subscription's initial_state return ONLY the viewport rows, not the world?
const WS = process.argv[2] || "ws://localhost:8789";

function sub(sql, timeoutMs = 4000) {
  return new Promise((res) => {
    const ws = new WebSocket(WS);
    let count = null, subId = null;
    const done = () => { try { ws.close(); } catch {} res({ count, subId }); };
    ws.onopen = () => ws.send(JSON.stringify({ type: "sql_subscribe", sql }));
    ws.onmessage = (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.type === "sql_subscription_created") subId = m.subscription_id;
      if (m.type === "initial_state") { count = (m.rows || []).length; done(); }
      if (m.type === "initial_state_error") { count = "ERR"; done(); }
    };
    ws.onerror = () => done();
    setTimeout(done, timeoutMs);
  });
}

const whole = await sub("SELECT * FROM units");
console.log(`whole-table units:              ${whole.count} rows`);
const view = await sub("SELECT * FROM units WHERE cx >= 10 AND cx <= 18 AND cy >= 18 AND cy <= 26");
console.log(`AOI viewport (cx10-18,cy18-26): ${view.count} rows`);
const tiny = await sub("SELECT * FROM units WHERE cx >= 0 AND cx <= 2 AND cy >= 0 AND cy <= 2");
console.log(`AOI corner   (cx0-2,cy0-2):     ${tiny.count} rows`);
console.log(typeof view.count === "number" && view.count < whole.count
  ? "\n✓ AOI initial_state filtered to viewport" : "\n✗ not filtering");
