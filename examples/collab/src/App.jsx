// NIGHTBOARD — realtime collaborative wall on OriginDB.
//
// All state lives in OriginDB tables (cursors, notes, presence, activity,
// chat), written only through the collab module's reducers and streamed back
// over WHERE-filtered websocket subscriptions. This app is a pure subscriber
// with optimistic local echoes for the user's own actions.

import React, {
  useCallback, useEffect, useMemo, useRef, useState,
} from "react";
import {
  DndContext, PointerSensor, TouchSensor, useDraggable, useSensor, useSensors,
} from "@dnd-kit/core";

const PALETTE = ["#ff6b57", "#4adcb1", "#ffc24b", "#b39dff", "#5ec8f2", "#ff8fd3"];
const TABLES = ["cursors", "notes", "activity", "presence", "chat"];

const colorFor = (name) => {
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return PALETTE[h % PALETTE.length];
};
const hash = (s) => {
  let h = 0;
  for (const c of String(s)) h = (h * 33 + c.charCodeAt(0)) >>> 0;
  return h;
};

const call = (reducer, args) =>
  fetch("/api/call", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ reducer, args }),
  }).then((r) => r.json()).catch(() => ({ success: false }));

// Fires at most once per `ms`; trailing call flushes the last value.
function useThrottle(fn, ms) {
  const last = useRef(0);
  const pending = useRef(null);
  const timer = useRef(null);
  return useCallback((...args) => {
    const now = performance.now();
    if (now - last.current >= ms) {
      last.current = now;
      fn(...args);
    } else {
      pending.current = args;
      if (!timer.current) {
        timer.current = setTimeout(() => {
          timer.current = null;
          last.current = performance.now();
          if (pending.current) fn(...pending.current);
          pending.current = null;
        }, ms);
      }
    }
  }, [fn, ms]);
}

// ---------------------------------------------------------------------------

export default function App() {
  const [me, setMe] = useState(null);
  const [connected, setConnected] = useState(false);
  const [cursors, setCursors] = useState({});   // user -> row + lastSeen
  const [notes, setNotes] = useState({});       // id -> row
  const [activity, setActivity] = useState([]); // newest first
  const [chat, setChat] = useState([]);         // oldest first
  const [presence, setPresence] = useState({});
  const boardRef = useRef(null);
  const meRef = useRef(null);
  meRef.current = me;

  // Notes being interacted with locally; remote updates to them are ignored
  // so the server echo can't fight the pointer.
  const holding = useRef(new Set());

  // ---- websocket subscriptions --------------------------------------------
  useEffect(() => {
    let ws;
    let closed = false;

    const applyRow = (table, key, cols) => {
      if (!key || String(key).startsWith("__")) return;
      if (table === "cursors") {
        if (meRef.current && cols.user === meRef.current.user) return;
        setCursors((c) => ({
          ...c,
          [cols.user]: { ...cols, lastSeen: performance.now() },
        }));
      } else if (table === "notes") {
        if (holding.current.has(key)) return;
        setNotes((n) => ({ ...n, [key]: { ...cols, id: key } }));
      } else if (table === "activity") {
        setActivity((a) =>
          a.some((e) => e.id === key) ? a
            : [{ ...cols, id: key }, ...a].slice(0, 80));
      } else if (table === "chat") {
        setChat((c) =>
          c.some((m) => m.id === key) ? c
            : [...c, { ...cols, id: key }].sort((x, y) => (x.ts || 0) - (y.ts || 0)));
      } else if (table === "presence") {
        setPresence((p) => ({ ...p, [cols.user]: cols }));
      }
    };

    const connect = async () => {
      let wsPort = 8787, token = "";
      try {
        const cfg = await fetch("/api/config").then((r) => r.json());
        wsPort = cfg.wsPort;
        token = cfg.token || "";
      } catch { /* default */ }
      if (closed) return;

      const q = token ? `?token=${encodeURIComponent(token)}` : "";
      ws = new WebSocket(`ws://${location.hostname}:${wsPort}${q}`);
      ws.onopen = () => {
        setConnected(true);
        for (const t of TABLES)
          ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM ${t}` }));
      };
      ws.onclose = () => {
        setConnected(false);
        if (!closed) setTimeout(connect, 1500);
      };
      ws.onmessage = (e) => {
        let msg;
        try { msg = JSON.parse(e.data); } catch { return; }
        if (msg.type === "initial_state") {
          const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1];
          for (const row of msg.rows || []) applyRow(table, row.key, row.data);
        } else if (msg.type === "sql_changefeed_event") {
          const table = msg.table;
          if (msg.operation === "DELETE") {
            if (table === "notes")
              setNotes((n) => { const c = { ...n }; delete c[msg.key]; return c; });
            return;
          }
          try {
            const parsed = JSON.parse(msg.new_value);
            applyRow(table, msg.key, parsed.columns ?? parsed);
          } catch { /* ignore unparseable */ }
        }
      };
    };

    connect();
    return () => { closed = true; ws?.close(); };
  }, []);

  // ---- my cursor (position + '/' message) -----------------------------------
  const cursorMsg = useRef("");
  const lastPos = useRef([0.5, 0.5]);
  const pushCursor = useThrottle((x, y) => {
    const m = meRef.current;
    if (m) call("moveCursor", [m.user, m.color, x, y, cursorMsg.current]);
  }, 80);

  const boardXY = (clientX, clientY) => {
    const r = boardRef.current.getBoundingClientRect();
    return [
      Math.min(1, Math.max(0, (clientX - r.left) / r.width)),
      Math.min(1, Math.max(0, (clientY - r.top) / r.height)),
    ];
  };

  const onPointerMove = (e) => {
    const [x, y] = boardXY(e.clientX, e.clientY);
    lastPos.current = [x, y];
    pushCursor(x, y);
    if (chatAt) setChatAt((c) => ({ ...c, x, y }));
  };

  // ---- '/' cursor chat --------------------------------------------------------
  const [chatAt, setChatAt] = useState(null); // {x, y} while typing
  const cursorInputRef = useRef(null);
  useEffect(() => {
    const onKey = (e) => {
      if (!meRef.current) return;
      const typing = ["INPUT", "TEXTAREA"].includes(document.activeElement?.tagName);
      if (e.key === "/" && !typing && !chatAt) {
        e.preventDefault();
        const [x, y] = lastPos.current;
        setChatAt({ x, y });
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [chatAt]);

  useEffect(() => {
    if (chatAt) cursorInputRef.current?.focus();
  }, [chatAt != null]);

  const endCursorChat = () => {
    cursorMsg.current = "";
    const m = meRef.current;
    const [x, y] = lastPos.current;
    if (m) call("moveCursor", [m.user, m.color, x, y, ""]);
    setChatAt(null);
  };

  // ---- notes ---------------------------------------------------------------------
  const [editingId, setEditingId] = useState(null);
  const pushEdit = useThrottle((id, user, text) => call("editNote", [id, user, text]), 300);
  const pushNoteMove = useThrottle((id, x, y) => call("moveNote", [id, x, y]), 80);

  const createNote = async (x, y) => {
    const m = meRef.current;
    if (!m) return;
    const result = await call("addNote", [m.user, m.color, x, y, "…"]);
    // The changefeed will deliver the note; open it for editing on arrival.
    if (result.success) setTimeout(() => {
      setNotes((n) => {
        const added = Object.values(n).find(
          (note) => note.user === m.user && note.text === "…" &&
                    Math.abs(note.x - x) < 0.01 && Math.abs(note.y - y) < 0.01);
        if (added) setEditingId(added.id);
        return n;
      });
    }, 350);
  };

  const onBoardDoubleClick = (e) => {
    if (e.target.closest(".note") || e.target.closest(".cursor-chat")) return;
    createNote(...boardXY(e.clientX, e.clientY));
  };

  // long-press = mobile note creation
  const pressTimer = useRef(null);
  const onTouchStart = (e) => {
    if (e.target.closest(".note")) return;
    const t = e.touches[0];
    pressTimer.current = setTimeout(() => createNote(...boardXY(t.clientX, t.clientY)), 480);
  };
  const onTouchMove = (e) => {
    clearTimeout(pressTimer.current);
    const t = e.touches[0];
    const [x, y] = boardXY(t.clientX, t.clientY);
    lastPos.current = [x, y];
    pushCursor(x, y);
  };

  // ---- dnd-kit ---------------------------------------------------------------------
  const sensors = useSensors(
    useSensor(PointerSensor, { activationConstraint: { distance: 5 } }),
    useSensor(TouchSensor, { activationConstraint: { delay: 150, tolerance: 8 } }),
  );

  const dragOrigin = useRef(null);
  const onDragStart = ({ active }) => {
    holding.current.add(active.id);
    const n = notes[active.id];
    dragOrigin.current = n ? { x: n.x, y: n.y } : { x: 0.5, y: 0.5 };
  };
  const dragPosition = ({ active, delta }) => {
    const r = boardRef.current.getBoundingClientRect();
    const x = Math.min(1, Math.max(0, dragOrigin.current.x + delta.x / r.width));
    const y = Math.min(1, Math.max(0, dragOrigin.current.y + delta.y / r.height));
    return [x, y];
  };
  const onDragMove = (ev) => {
    const [x, y] = dragPosition(ev);
    setNotes((n) => ({ ...n, [ev.active.id]: { ...n[ev.active.id], x, y } }));
    pushNoteMove(ev.active.id, x, y);   // everyone else sees the drag live
  };
  const onDragEnd = (ev) => {
    const [x, y] = dragPosition(ev);
    setNotes((n) => ({ ...n, [ev.active.id]: { ...n[ev.active.id], x, y } }));
    call("moveNote", [ev.active.id, x, y]);
    holding.current.delete(ev.active.id);
    dragOrigin.current = null;
  };

  // ---- render ---------------------------------------------------------------------
  const noteList = useMemo(
    () => Object.values(notes).sort((a, b) => (a.created_at || 0) - (b.created_at || 0)),
    [notes]);

  return (
    <>
      <DndContext sensors={sensors} onDragStart={onDragStart}
                  onDragMove={onDragMove} onDragEnd={onDragEnd}>
        <div className="board" ref={boardRef}
             onPointerMove={onPointerMove}
             onDoubleClick={onBoardDoubleClick}
             onTouchStart={onTouchStart} onTouchMove={onTouchMove}
             onTouchEnd={() => clearTimeout(pressTimer.current)}>
          {noteList.map((n) => (
            <NoteCard key={n.id} note={n} me={me}
                      editing={editingId === n.id}
                      onStartEdit={() => setEditingId(n.id)}
                      onEdit={(text) => {
                        setNotes((s) => ({ ...s, [n.id]: { ...s[n.id], text } }));
                        pushEdit(n.id, me.user, text);
                      }}
                      onEndEdit={(text) => {
                        setEditingId(null);
                        call("editNote", [n.id, me.user, text]);
                      }}
                      onRemove={() => call("removeNote", [n.id, me?.user || "someone"])}
                      holding={holding} />
          ))}
          {Object.values(cursors).map((c) => <RemoteCursor key={c.user} c={c} />)}
          {chatAt && me && (
            <div className="cursor-chat"
                 style={{ left: `${chatAt.x * 100}%`, top: `${chatAt.y * 100}%` }}>
              <input ref={cursorInputRef} maxLength={140}
                     placeholder="say something…"
                     onChange={(e) => {
                       cursorMsg.current = e.target.value;
                       const [x, y] = lastPos.current;
                       pushCursor(x, y);
                     }}
                     onKeyDown={(e) => {
                       if (e.key === "Escape" || e.key === "Enter") endCursorChat();
                     }}
                     onBlur={endCursorChat} />
              <div className="esc">enter / esc to dismiss</div>
            </div>
          )}
        </div>
      </DndContext>

      <header>
        <h1>NIGHT<em>BOARD</em></h1>
        <span className="sub">shared wall · origindb</span>
        <div className={`conn ${connected ? "live" : ""}`}>
          <span className="dot" />
          <span>{connected ? "live" : "reconnecting"}</span>
        </div>
      </header>

      <div className="hint">
        <span><b>drag</b> a card to move it</span>
        <span><b>double-click</b> board = new card · card = edit</span>
        <span><b>/</b> to talk at your cursor</span>
      </div>
      <button className="wipe" onClick={() => me && call("clearBoard", [me.user])}>
        wipe board
      </button>

      <SidePanel activity={activity} chat={chat} presence={presence} me={me} />

      {!me && <JoinOverlay onJoin={(user) => {
        const joined = { user, color: colorFor(user) };
        document.documentElement.style.setProperty("--me-color", joined.color);
        setMe(joined);
        call("join", [joined.user, joined.color]);
      }} />}
    </>
  );
}

// ---------------------------------------------------------------------------

function NoteCard({ note, me, editing, onStartEdit, onEdit, onEndEdit, onRemove, holding }) {
  const { attributes, listeners, setNodeRef, isDragging } =
    useDraggable({ id: note.id, disabled: editing });
  const taRef = useRef(null);
  const entered = useRef(false);
  useEffect(() => { entered.current = true; }, []);
  useEffect(() => {
    if (editing && taRef.current) {
      taRef.current.focus();
      taRef.current.select();
      holding.current.add(note.id);   // don't let remote echoes stomp typing
    } else {
      holding.current.delete(note.id);
    }
  }, [editing]);

  const tilt = `${((hash(note.id) % 50) - 25) / 10}deg`;
  return (
    <div ref={setNodeRef}
         className={`note ${isDragging ? "dragging" : ""} ${entered.current ? "" : "entering"}`}
         style={{
           left: `${note.x * 100}%`, top: `${note.y * 100}%`,
           "--note-color": note.color || "#ffc24b", "--tilt": tilt,
         }}
         onDoubleClick={(e) => { e.stopPropagation(); me && onStartEdit(); }}
         {...listeners} {...attributes}>
      <span className="pin-dot" />
      <div className="who">
        <span>{note.user || ""}</span>
        {note.edited_by && note.edited_by !== note.user &&
          <span className="edited">✎ {note.edited_by}</span>}
      </div>
      {editing ? (
        <textarea ref={taRef} defaultValue={note.text === "…" ? "" : note.text}
                  maxLength={280}
                  onChange={(e) => onEdit(e.target.value)}
                  onBlur={(e) => onEndEdit(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter" && !e.shiftKey) {
                      e.preventDefault();
                      onEndEdit(e.target.value);
                    }
                    if (e.key === "Escape") onEndEdit(e.target.value);
                  }}
                  onPointerDown={(e) => e.stopPropagation()} />
      ) : (
        <div className="txt">{note.text}</div>
      )}
      <button className="x" title="remove"
              onPointerDown={(e) => e.stopPropagation()}
              onClick={(e) => { e.stopPropagation(); onRemove(); }}>✕</button>
    </div>
  );
}

function RemoteCursor({ c }) {
  const [, tick] = useState(0);
  useEffect(() => {
    const t = setInterval(() => tick((n) => n + 1), 2000);
    return () => clearInterval(t);
  }, []);
  const age = performance.now() - (c.lastSeen || 0);
  const cls = age > 20000 ? "gone" : age > 6000 ? "stale" : "";
  return (
    <div className={`cursor ${cls}`}
         style={{ transform: `translate(${c.x * 100 * 0.01 * (boardWidth())}px, ${c.y * boardHeight()}px)` }}>
      <svg width="18" height="20" viewBox="0 0 18 20">
        <path d="M1 1 L1 15 L5.2 11.5 L8 18 L10.6 16.9 L7.9 10.6 L13 10.2 Z"
              fill={c.color || "#fff"} stroke="#0e0e12" strokeWidth="1.2" />
      </svg>
      <span className="tag" style={{ background: c.color || "#fff" }}>{c.user}</span>
      {c.msg && <div className="bubble" style={{ "--c": c.color }}>{c.msg}</div>}
    </div>
  );
}
// Board metrics for cursor placement (board is fixed-position, full height).
const boardWidth = () =>
  document.querySelector(".board")?.getBoundingClientRect().width ?? window.innerWidth;
const boardHeight = () =>
  document.querySelector(".board")?.getBoundingClientRect().height ?? window.innerHeight;

function SidePanel({ activity, chat, presence, me }) {
  const [tab, setTab] = useState("chat");
  const [open, setOpen] = useState(false);
  const [unread, setUnread] = useState(0);
  const scrollRef = useRef(null);
  const inputRef = useRef(null);
  const chatLen = useRef(chat.length);

  useEffect(() => {
    if (tab === "chat")
      scrollRef.current?.scrollTo(0, scrollRef.current.scrollHeight);
    if (chat.length > chatLen.current && tab !== "chat") {
      setUnread((u) => u + (chat.length - chatLen.current));
    }
    chatLen.current = chat.length;
  }, [chat, tab]);

  const send = () => {
    const text = inputRef.current.value.trim();
    if (!text || !me) return;
    call("sendChat", [me.user, me.color, text]);
    inputRef.current.value = "";
  };

  return (
    <aside className={`panel ${open ? "open" : ""}`}>
      <div className="tabs" onClick={() => setOpen(true)}>
        <button className={tab === "chat" ? "active" : ""}
                onClick={() => { setTab("chat"); setUnread(0); }}>
          CHAT{unread > 0 && <span className="badge">{unread}</span>}
        </button>
        <button className={tab === "tape" ? "active" : ""}
                onClick={() => setTab("tape")}>TAPE ▮▶</button>
      </div>

      <div className="scroll" ref={scrollRef}>
        {tab === "tape" && activity.map((a) => (
          <div key={a.id} className="evt"
               style={{ "--evt-color": a.color || "#e8e6df", borderLeftColor: a.color || "transparent" }}>
            <b>{a.user}</b> {a.action || ""}
            {a.detail ? <> <span className="detail">“{a.detail}”</span></> : null}
            <time>{a.ts ? new Date(Number(a.ts)).toLocaleTimeString() : ""}</time>
          </div>
        ))}
        {tab === "chat" && chat.map((m) => (
          <div key={m.id} className="msg" style={{ "--msg-color": m.color || "#e8e6df" }}>
            <div className="head"><b>{m.user}</b>
              <time>{m.ts ? new Date(Number(m.ts)).toLocaleTimeString() : ""}</time>
            </div>
            <div className="body">{m.text}</div>
          </div>
        ))}
      </div>

      {tab === "chat" && (
        <div className="chat-input">
          <input ref={inputRef} maxLength={500}
                 placeholder={me ? "message the room…" : "join first"}
                 disabled={!me}
                 onKeyDown={(e) => e.key === "Enter" && send()} />
          <button onClick={send} disabled={!me}>send</button>
        </div>
      )}

      <div className="roster">
        {Object.values(presence).map((p) => (
          <span key={p.user} className="chip" style={{ "--chip-color": p.color || "#e8e6df" }}>
            {p.user}
          </span>
        ))}
      </div>
    </aside>
  );
}

function JoinOverlay({ onJoin }) {
  const ref = useRef(null);
  useEffect(() => ref.current?.focus(), []);
  const go = () => {
    const user = ref.current.value.trim();
    if (user) onJoin(user);
  };
  return (
    <div className="join">
      <div className="card">
        <h1>NIGHT<span className="a">BO</span><span className="b">ARD</span></h1>
        <p>every cursor · every card · one wall</p>
        <input ref={ref} maxLength={18} placeholder="your name" autoComplete="off"
               onKeyDown={(e) => e.key === "Enter" && go()} />
        <button onClick={go}>Step up to the wall</button>
      </div>
    </div>
  );
}
