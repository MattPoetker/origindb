// Thin OriginDB websocket client for OriginLands.
// One socket does both jobs: subscribe to AoI changefeeds AND call reducers
// (createCharacter / login / move) via call_reducer.
export class Net {
  constructor(url, module) {
    this.url = url;
    this.module = module;
    this.ws = null;
    this.nextId = 1;
    this.pending = new Map();       // call id -> {resolve}
    this.subId = null;              // current AoI subscription id
    this._subResolvers = [];        // queued resolvers for sql_subscription_created
    this.onEvent = () => {};        // (msg) for initial_state / sql_changefeed_event
    this.onClose = () => {};
  }

  connect() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.url);
      this.ws = ws;
      ws.onopen = () => resolve();
      ws.onerror = (e) => reject(e);
      ws.onclose = () => this.onClose();
      ws.onmessage = (e) => {
        let m; try { m = JSON.parse(e.data); } catch { return; }
        this._handle(m);
      };
    });
  }

  _handle(m) {
    switch (m.type) {
      case "call_result": {
        const p = this.pending.get(m.id);
        if (p) { this.pending.delete(m.id); p(m); }
        break;
      }
      case "sql_subscription_created": {
        this.subId = m.subscription_id;
        const r = this._subResolvers.shift();
        if (r) r(m.subscription_id);
        break;
      }
      case "initial_state":
      case "sql_changefeed_event":
        this.onEvent(m);
        break;
      default: break; // sql_unsubscribed, pong, error
    }
  }

  // Call a reducer. wantResult=true awaits the call_result (create/login).
  call(reducer, args, wantResult = false) {
    const msg = { type: "call_reducer", module: this.module, reducer, args };
    if (!wantResult) { this.ws.send(JSON.stringify(msg)); return Promise.resolve(); }
    const id = String(this.nextId++);
    msg.id = id;
    const p = new Promise((resolve) => this.pending.set(id, resolve));
    this.ws.send(JSON.stringify(msg));
    return p;
  }

  subscribe(sql) {
    const p = new Promise((resolve) => this._subResolvers.push(resolve));
    this.ws.send(JSON.stringify({ type: "sql_subscribe", sql }));
    return p;
  }

  unsubscribe(id) {
    if (id) this.ws.send(JSON.stringify({ type: "sql_unsubscribe", subscription_id: id }));
  }
}
