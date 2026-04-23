from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse, HTMLResponse
from pydantic import BaseModel
import asyncio
import random
import string
import time
import sqlite3
import os
import re
from typing import Optional

app = FastAPI()
DB_PATH = os.path.join(os.path.dirname(__file__), "queue.db")

# ── in-memory state ────────────────────────────────────────────────────────────
# key: "region_mode"  e.g. "NAE_3s"
queues: dict[str, list[dict]] = {}

# active matches: match_id -> match info dict
matches: dict[str, dict] = {}

# player_id -> match_id (so heartbeat can find "match_found" quickly)
player_match: dict[str, str] = {}

# bakkesmod_id -> real_id (session only, updated on every queue join)
real_id_map: dict[str, str] = {}

# Recently-cancelled matches: match_id -> {reason, at}
# Kept for 5 minutes so polling clients can read the cancellation reason.
cancelled_matches: dict[str, dict] = {}

# Priority set: players who should be placed at the front on next join
# (set when their match was cancelled because someone else declined)
victim_priority: set[str] = set()

# Decline rate-limiting: player_id -> list of decline timestamps
decline_log: dict[str, list[float]] = {}

# ── timing constants ──────────────────────────────────────────────────────────
DECLINE_WINDOW_SECS  = 600   # 10 min window for counting recent declines
DECLINE_MAX          = 3     # max declines before cooldown
DECLINE_COOLDOWN_SECS = 300  # 5 min cooldown after too many declines

LOBBY_CREATE_TIMEOUT = 180   # 3 min: host must click "Lobby ready" after all accept
DRAW_TIMEOUT_SECS    = 3600  # 1 hour after lobby_ready with no result = auto-draw

PLAYERS_NEEDED = {"1s": 2, "2s": 4, "3s": 6}

# ── security helpers ───────────────────────────────────────────────────────────
VALID_MODES = {"1s", "2s", "3s"}

def validate_mode(mode: str) -> str:
    if mode not in VALID_MODES:
        raise HTTPException(400, f"Invalid mode '{mode}'. Must be one of: 1s, 2s, 3s")
    return mode

_SAFE_FILENAME_RE = re.compile(r'[^A-Za-z0-9_\-]')

def safe_filename(s: str) -> str:
    return _SAFE_FILENAME_RE.sub('', s)

# ── username validation ────────────────────────────────────────────────────────
_USERNAME_RE = re.compile(r'^[A-Za-z0-9_]{3,20}$')
_LEET_TABLE  = str.maketrans("013@$!|4", "oleasiia")
BLOCKLIST: list[str] = []

def _normalise(s: str) -> str:
    return re.sub(r'[^a-z]', '', s.lower().translate(_LEET_TABLE))

def load_blocklist():
    global BLOCKLIST
    path = os.path.join(os.path.dirname(__file__), "blocklist.txt")
    if os.path.exists(path):
        with open(path) as f:
            BLOCKLIST = [line.strip().lower() for line in f
                         if line.strip() and not line.startswith('#')]
    print(f"[blocklist] loaded {len(BLOCKLIST)} entries")

def validate_username(username: str):
    if not _USERNAME_RE.match(username):
        raise HTTPException(400,
            "Username must be 3–20 characters: letters, numbers, underscores only.")
    norm = _normalise(username)
    for word in BLOCKLIST:
        if _normalise(word) in norm:
            raise HTTPException(400, "Username contains disallowed words.")

# ── database ───────────────────────────────────────────────────────────────────
def init_db():
    conn = sqlite3.connect(DB_PATH)

    conn.execute("""
        CREATE TABLE IF NOT EXISTS players (
            player_id TEXT PRIMARY KEY,
            real_id   TEXT DEFAULT '',
            username  TEXT DEFAULT '',
            mmr_1s    REAL DEFAULT 1000,
            mmr_2s    REAL DEFAULT 1000,
            mmr_3s    REAL DEFAULT 1000,
            wins      INTEGER DEFAULT 0,
            losses    INTEGER DEFAULT 0
        )
    """)
    # migrate older schemas
    for col_def in [
        "username TEXT DEFAULT ''",
        "real_id  TEXT DEFAULT ''",
        "disconnect_wins INTEGER DEFAULT 0",
        "decline_count   INTEGER DEFAULT 0",
        "last_decline_at INTEGER DEFAULT 0",
    ]:
        try:
            conn.execute(f"ALTER TABLE players ADD COLUMN {col_def}")
            conn.commit()
        except Exception:
            pass

    conn.execute("""
        CREATE TABLE IF NOT EXISTS match_history (
            match_id       TEXT PRIMARY KEY,
            mode           TEXT,
            region         TEXT,
            winner_ids     TEXT,
            loser_ids      TEXT,
            timestamp      INTEGER,
            disputed       INTEGER DEFAULT 0,
            mmr_delta_win  REAL    DEFAULT 0,
            mmr_delta_loss REAL    DEFAULT 0,
            outcome        TEXT    DEFAULT 'normal'
        )
    """)
    # migrate old schema
    for col in [
        "disputed INTEGER DEFAULT 0",
        "mmr_delta_win REAL DEFAULT 0",
        "mmr_delta_loss REAL DEFAULT 0",
        "outcome TEXT DEFAULT 'normal'",
    ]:
        try:
            conn.execute(f"ALTER TABLE match_history ADD COLUMN {col}")
            conn.commit()
        except Exception:
            pass

    # match_results now stores "outcome" (win/loss/draw) instead of a boolean won flag
    conn.execute("""
        CREATE TABLE IF NOT EXISTS match_results (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id     TEXT,
            player_id    TEXT,
            outcome      TEXT,
            submitted_at INTEGER,
            UNIQUE(match_id, player_id)
        )
    """)
    # migrate old match_results table that had a 'won' column
    try:
        conn.execute("ALTER TABLE match_results ADD COLUMN outcome TEXT")
        conn.commit()
    except Exception:
        pass

    conn.execute("""
        CREATE TABLE IF NOT EXISTS reports (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id     TEXT,
            reporter_id  TEXT,
            replay_path  TEXT,
            submitted_at INTEGER,
            status       TEXT DEFAULT 'pending'
        )
    """)
    conn.commit()
    conn.close()

def get_mmr(player_id: str, mode: str) -> float:
    validate_mode(mode)
    col = f"mmr_{mode}"
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        f"SELECT {col} FROM players WHERE player_id=?", (player_id,)
    ).fetchone()
    conn.close()
    return row[0] if row else 1000.0

def ensure_player(player_id: str, real_id: str = "", username: str = ""):
    """Upsert a player record, transferring stats if real_id matches an old account."""
    conn = sqlite3.connect(DB_PATH)
    if real_id:
        old = conn.execute(
            "SELECT player_id, mmr_1s, mmr_2s, mmr_3s, wins, losses "
            "FROM players WHERE real_id=? AND player_id!=?",
            (real_id, player_id)
        ).fetchone()
        if old:
            old_pid, m1, m2, m3, w, l = old
            conn.execute("INSERT OR IGNORE INTO players (player_id) VALUES (?)", (player_id,))
            conn.execute(
                "UPDATE players SET mmr_1s=?, mmr_2s=?, mmr_3s=?, wins=?, losses=? "
                "WHERE player_id=?",
                (m1, m2, m3, w, l, player_id)
            )
            conn.execute("UPDATE players SET real_id='' WHERE player_id=?", (old_pid,))
            print(f"[account] transferred {old_pid} → {player_id} via real_id {real_id[:8]}…")

    conn.execute("INSERT OR IGNORE INTO players (player_id) VALUES (?)", (player_id,))
    if real_id:
        conn.execute("UPDATE players SET real_id=? WHERE player_id=?", (real_id, player_id))
    if username:
        conn.execute("UPDATE players SET username=? WHERE player_id=?", (username, player_id))
    conn.commit()
    conn.close()

def update_mmr(winner_ids: list, loser_ids: list, mode: str):
    validate_mode(mode)
    K   = 32
    col = f"mmr_{mode}"
    conn = sqlite3.connect(DB_PATH)
    conn.execute("BEGIN IMMEDIATE")
    winner_mmrs = [conn.execute(f"SELECT {col} FROM players WHERE player_id=?", (p,)).fetchone() for p in winner_ids]
    loser_mmrs  = [conn.execute(f"SELECT {col} FROM players WHERE player_id=?", (p,)).fetchone() for p in loser_ids]
    w_avg = sum(r[0] if r else 1000 for r in winner_mmrs) / len(winner_ids)
    l_avg = sum(r[0] if r else 1000 for r in loser_mmrs)  / len(loser_ids)
    expected_w = 1 / (1 + 10 ** ((l_avg - w_avg) / 400))
    expected_l = 1 - expected_w
    delta_w = K * (1 - expected_w)
    delta_l = K * (0 - expected_l)
    for pid in winner_ids:
        conn.execute(f"UPDATE players SET {col}={col}+?, wins=wins+1 WHERE player_id=?", (delta_w, pid))
    for pid in loser_ids:
        conn.execute(f"UPDATE players SET {col}={col}+?, losses=losses+1 WHERE player_id=?", (delta_l, pid))
    conn.commit()
    conn.close()
    return round(delta_w, 1), round(delta_l, 1)

# ── helpers ────────────────────────────────────────────────────────────────────
def rand_str(n: int) -> str:
    return "".join(random.choices(string.ascii_uppercase + string.digits, k=n))

def make_teams(players: list, mode: str):
    random.shuffle(players)
    half = len(players) // 2
    return players[:half], players[half:]

def cancel_match(match_id: str, reason: str):
    m = matches.pop(match_id, None)
    if m:
        for pid in m["players"]:
            player_match.pop(pid, None)
    cancelled_matches[match_id] = {"reason": reason, "at": time.time()}
    print(f"[cancel] {match_id}: {reason}")

def _award_match(match_id: str, m: dict,
                 winner_ids: list, loser_ids: list, outcome: str = "normal"):
    delta_w, delta_l = update_mmr(winner_ids, loser_ids, m["mode"])
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO match_history "
        "(match_id, mode, region, winner_ids, loser_ids, "
        " timestamp, disputed, mmr_delta_win, mmr_delta_loss, outcome) "
        "VALUES (?,?,?,?,?,?,0,?,?,?)",
        (match_id, m["mode"], m["region"],
         ",".join(winner_ids), ",".join(loser_ids),
         int(time.time()), delta_w, delta_l, outcome)
    )
    conn.commit()
    conn.close()
    for pid in m["players"]:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    print(f"[match] {match_id} → {outcome}: winners={winner_ids}")

def _record_draw(match_id: str, m: dict, outcome: str = "draw"):
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO match_history "
        "(match_id, mode, region, winner_ids, loser_ids, "
        " timestamp, disputed, mmr_delta_win, mmr_delta_loss, outcome) "
        "VALUES (?,?,?,?,?,?,0,0,0,?)",
        (match_id, m["mode"], m["region"], "", "", int(time.time()), outcome)
    )
    conn.commit()
    conn.close()
    for pid in m["players"]:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    print(f"[match] {match_id} → {outcome}")

def _flag_disputed(match_id: str, m: dict):
    conn = sqlite3.connect(DB_PATH)
    already = conn.execute(
        "SELECT 1 FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    if not already:
        conn.execute(
            "INSERT OR IGNORE INTO match_history "
            "(match_id, mode, region, winner_ids, loser_ids, "
            " timestamp, disputed, mmr_delta_win, mmr_delta_loss) "
            "VALUES (?,?,?,?,?,?,1,0,0)",
            (match_id, m["mode"], m["region"], "", "", int(time.time()))
        )
        conn.commit()
    conn.close()
    for pid in m["players"]:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    print(f"[match] {match_id} → disputed (conflicting reports)")

def _get_total_players(match_id: str) -> int | None:
    m = matches.get(match_id)
    if m:
        return len(m["players"])
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT mode FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    conn.close()
    return PLAYERS_NEEDED.get(row[0]) if row else None

# ── models ─────────────────────────────────────────────────────────────────────
class JoinRequest(BaseModel):
    player_id: str
    real_id:   str = ""
    username:  str = ""
    region:    str
    mode:      str

class RegisterRequest(BaseModel):
    player_id: str
    real_id:   str = ""
    username:  str = ""

class LeaveRequest(BaseModel):
    player_id: str

class AcceptRequest(BaseModel):
    player_id: str
    match_id:  str

class MatchResultRequest(BaseModel):
    player_id: str
    match_id:  str
    outcome:   str   # "win", "loss", or "draw"

class ForfeitRequest(BaseModel):
    player_id: str
    match_id:  str

# ── startup ────────────────────────────────────────────────────────────────────
@app.on_event("startup")
async def startup():
    load_blocklist()
    init_db()
    asyncio.create_task(cleanup_loop())

async def cleanup_loop():
    """
    Background task that runs every 10 seconds.
    Handles:
      1. Stale players in queues (no heartbeat for >60s)
      2. Match acceptance timeout (>60s, not all accepted)
      3. Host lobby creation timeout (>3min after all accepted, no lobby_ready)
      4. Draw timeout (>1h after lobby_ready, no result)
      5. Safety net: matches open >2h
      6. Expired cancelled-match records
    """
    while True:
        await asyncio.sleep(10)
        now = time.time()

        # ── Queue cleanup ─────────────────────────────────────────────────────
        for key in list(queues.keys()):
            before = len(queues[key])
            queues[key] = [
                p for p in queues[key]
                if now - p.get("last_heartbeat", p["joined_at"]) < 60
            ]
            removed = before - len(queues[key])
            if removed:
                print(f"[cleanup] removed {removed} stale player(s) from {key}")

        # ── Match cleanup ─────────────────────────────────────────────────────
        for mid in list(matches.keys()):
            m = matches[mid]
            age = now - m["created_at"]

            # 1. Acceptance window expired (60s)
            if age > 60 and len(m["accepted"]) < len(m["players"]):
                cancel_match(mid, "Not all players accepted in time.")
                continue

            # 2. Host never created the lobby (3 min after all accepted)
            all_acc_at = m.get("all_accepted_at")
            if all_acc_at and not m.get("lobby_ready") \
                    and (now - all_acc_at) > LOBBY_CREATE_TIMEOUT:
                cancel_match(mid, "Host did not create the lobby in time.")
                continue

            # 3. Draw timeout (1 hour after lobby_ready with no result)
            lobby_ready_at = m.get("lobby_ready_at")
            if lobby_ready_at and (now - lobby_ready_at) > DRAW_TIMEOUT_SECS:
                conn = sqlite3.connect(DB_PATH)
                already = conn.execute(
                    "SELECT 1 FROM match_history WHERE match_id=?", (mid,)
                ).fetchone()
                conn.close()
                if not already:
                    _record_draw(mid, m, outcome="draw_timeout")
                else:
                    for pid in m["players"]:
                        player_match.pop(pid, None)
                    matches.pop(mid, None)
                continue

            # 4. Safety net: match open >2 hours with no result
            if age > 7200:
                cancel_match(mid, "Match expired (2-hour safety limit).")

        # ── Cancelled-match record expiry ─────────────────────────────────────
        for mid in list(cancelled_matches.keys()):
            if now - cancelled_matches[mid]["at"] > 300:
                cancelled_matches.pop(mid, None)

# ── routes: health + stats ─────────────────────────────────────────────────────
@app.get("/health")
def health():
    return {"status": "ok"}

@app.get("/queue/stats")
def queue_stats():
    buckets = {key: len(players) for key, players in queues.items() if players}
    buckets["total_searching"] = sum(buckets.values())
    return buckets

# ── routes: homepage (leaderboard) ────────────────────────────────────────────
@app.get("/", response_class=HTMLResponse)
def homepage():
    return HTMLResponse(content="""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RL Custom Queue</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0d0d0d; color: #e0e0e0; font-family: 'Segoe UI', sans-serif; min-height: 100vh; }
  header {
    background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
    border-bottom: 2px solid #2a4a7f;
    padding: 18px 40px;
    display: flex; align-items: center; gap: 16px;
  }
  header h1 { font-size: 1.6rem; color: #5fa8ff; letter-spacing: 1px; }
  header span { color: #888; font-size: 0.9rem; }
  .container { max-width: 920px; margin: 36px auto; padding: 0 20px; }
  .card {
    background: #161616; border: 1px solid #2a2a2a; border-radius: 10px;
    padding: 26px 30px; margin-bottom: 30px;
  }
  .card h2 { font-size: 1.05rem; color: #5fa8ff; margin-bottom: 6px; }
  .card > p { color: #888; font-size: 0.88rem; margin-bottom: 16px; }
  .profile-form { display: flex; gap: 10px; flex-wrap: wrap; }
  .profile-form input {
    flex: 1; min-width: 180px; background: #222; border: 1px solid #333;
    border-radius: 6px; color: #e0e0e0; padding: 10px 14px; font-size: 0.95rem;
    outline: none; transition: border-color .2s;
  }
  .profile-form input:focus { border-color: #5fa8ff; }
  .profile-form input::placeholder { color: #555; }
  .profile-form button {
    background: #2a5298; color: #fff; border: none; border-radius: 6px;
    padding: 10px 22px; font-size: 0.95rem; cursor: pointer; transition: background .2s;
  }
  .profile-form button:hover { background: #3a6bc4; }
  #profile-result { margin-top: 14px; font-size: 0.92rem; }
  .profile-card {
    background: #1a1a2e; border: 1px solid #2a4a7f; border-radius: 8px;
    padding: 16px 20px; display: flex; gap: 28px; flex-wrap: wrap; align-items: center;
  }
  .profile-card .pname { font-size: 1.15rem; font-weight: 700; color: #5fa8ff; }
  .profile-card .pstat { font-size: 0.88rem; color: #aaa; }
  .profile-card .pstat span { color: #e0e0e0; font-weight: 600; }
  .success { color: #4cff91; } .error { color: #ff5f5f; }
  .lb-controls { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; margin-bottom: 18px; }
  #lb-mode-select {
    background: #1e1e1e; border: 1px solid #333; border-radius: 6px;
    color: #e0e0e0; padding: 7px 14px; font-size: 0.9rem; cursor: pointer; outline: none;
  }
  .search-wrap { position: relative; margin-left: auto; }
  .search-wrap input {
    background: #1e1e1e; border: 1px solid #333; border-radius: 6px;
    color: #e0e0e0; padding: 7px 14px 7px 34px; font-size: 0.88rem;
    outline: none; width: 200px; transition: border-color .2s, width .3s;
  }
  .search-wrap input:focus { border-color: #5fa8ff; width: 240px; }
  .search-wrap input::placeholder { color: #555; }
  .search-wrap::before {
    content: '🔍'; position: absolute; left: 10px; top: 50%;
    transform: translateY(-50%); font-size: 0.75rem; pointer-events: none;
  }
  table { width: 100%; border-collapse: collapse; font-size: 0.9rem; }
  thead tr { border-bottom: 1px solid #2a2a2a; }
  th { text-align: left; padding: 8px 12px; color: #5fa8ff; font-weight: 600; }
  td { padding: 9px 12px; border-bottom: 1px solid #1e1e1e; }
  tr:hover td { background: #1a1a1a; }
  .rank-1 td:first-child { color: #ffd700; font-weight: bold; }
  .rank-2 td:first-child { color: #c0c0c0; font-weight: bold; }
  .rank-3 td:first-child { color: #cd7f32; font-weight: bold; }
  .mmr-val { color: #5fa8ff; font-weight: 600; }
  .win { color: #4cff91; } .loss { color: #ff5f5f; }
  .empty { color: #555; text-align: center; padding: 30px 0; }
  #no-search { display: none; color: #555; text-align: center; padding: 24px 0; }
</style>
</head>
<body>
<header>
  <h1>⚡ RL Custom Queue</h1>
  <span>Competitive matchmaking for Rocket League</span>
</header>
<div class="container">
  <div class="card">
    <h2>Look up a player</h2>
    <p>Search by username to view their rating and stats.</p>
    <div class="profile-form">
      <input id="inp-search" type="text" placeholder="Username" maxlength="32"
             onkeydown="if(event.key==='Enter') lookupProfile()">
      <button onclick="lookupProfile()">Search</button>
    </div>
    <div id="profile-result"></div>
  </div>
  <div class="card">
    <h2>Leaderboard</h2>
    <p>Sorted by the selected mode. All ratings start at 1000.</p>
    <div class="lb-controls">
      <select id="lb-mode-select" onchange="loadLB(this.value)">
        <option value="1s">1s</option>
        <option value="2s" selected>2s</option>
        <option value="3s">3s</option>
      </select>
      <div class="search-wrap">
        <input id="lb-search" type="text" placeholder="Search player…" oninput="filterLB()">
      </div>
    </div>
    <table>
      <thead>
        <tr><th>#</th><th>Player</th><th id="th-rating">Rating</th><th>W</th><th>L</th><th>Win %</th></tr>
      </thead>
      <tbody id="lb-body">
        <tr><td colspan="6" class="empty">Loading…</td></tr>
      </tbody>
    </table>
    <div id="no-search">No players match your search.</div>
  </div>
</div>
<script>
let lbData = [], currentMode = '2s';
async function lookupProfile() {
  const q = document.getElementById('inp-search').value.trim();
  const result = document.getElementById('profile-result');
  if (!q) return;
  result.innerHTML = 'Searching…';
  try {
    const r = await fetch('/player/search?q=' + encodeURIComponent(q));
    const data = await r.json();
    if (!data.length) { result.innerHTML = '<span class="error">No players found.</span>'; return; }
    result.innerHTML = data.map(p => {
      const total = p.wins + p.losses;
      const winpct = total ? Math.round(p.wins / total * 100) + '%' : '—';
      return `<div class="profile-card">
        <div class="pname">${p.username}</div>
        <div class="pstat">1s <span>${p.mmr_1s}</span></div>
        <div class="pstat">2s <span>${p.mmr_2s}</span></div>
        <div class="pstat">3s <span>${p.mmr_3s}</span></div>
        <div class="pstat">W <span style="color:#4cff91">${p.wins}</span></div>
        <div class="pstat">L <span style="color:#ff5f5f">${p.losses}</span></div>
        <div class="pstat">Win% <span>${winpct}</span></div>
      </div>`;
    }).join('<br>');
  } catch(e) { result.innerHTML = '<span class="error">Could not reach server.</span>'; }
}
function renderRows(rows, mode) {
  const query = document.getElementById('lb-search').value.trim().toLowerCase();
  const noSearch = document.getElementById('no-search');
  if (!rows.length) {
    document.getElementById('lb-body').innerHTML = '<tr><td colspan="6" class="empty">No players yet.</td></tr>';
    noSearch.style.display = 'none'; return;
  }
  const filtered = query ? rows.filter(p => (p.username || p.player_id).toLowerCase().includes(query)) : rows;
  noSearch.style.display = (filtered.length === 0 && query) ? 'block' : 'none';
  const mmrKey = 'mmr_' + mode;
  document.getElementById('lb-body').innerHTML = filtered.map(p => {
    const total = p.wins + p.losses;
    const winpct = total ? Math.round(p.wins / total * 100) + '%' : '—';
    const name = p.username || '<span style="color:#555">' + p.player_id.slice(0,14) + '…</span>';
    const rowCls = p.rank <= 3 ? 'rank-' + p.rank : '';
    return `<tr class="${rowCls}"><td>${p.rank}</td><td>${name}</td><td class="mmr-val">${p[mmrKey]}</td><td class="win">${p.wins}</td><td class="loss">${p.losses}</td><td>${winpct}</td></tr>`;
  }).join('');
}
function filterLB() { renderRows(lbData, currentMode); }
async function loadLB(mode) {
  currentMode = mode;
  document.getElementById('th-rating').textContent = mode + ' Rating';
  document.getElementById('lb-body').innerHTML = '<tr><td colspan="6" class="empty">Loading…</td></tr>';
  try {
    const r = await fetch('/leaderboard/' + mode);
    lbData = await r.json();
    renderRows(lbData, mode);
  } catch(e) {
    document.getElementById('lb-body').innerHTML = '<tr><td colspan="6" class="empty">Failed to load.</td></tr>';
  }
}
loadLB('2s');
</script>
</body>
</html>""")

# ── routes: account ────────────────────────────────────────────────────────────
@app.post("/account/register")
def account_register(req: RegisterRequest):
    if req.username:
        validate_username(req.username)
    ensure_player(req.player_id, req.real_id, req.username)
    conn = sqlite3.connect(DB_PATH)
    row  = conn.execute(
        "SELECT username, mmr_1s, mmr_2s, mmr_3s FROM players WHERE player_id=?",
        (req.player_id,)
    ).fetchone()
    conn.close()
    return {
        "status":   "ok",
        "username": row[0] if row else "",
        "mmr_1s":   round(row[1], 1) if row else 1000,
        "mmr_2s":   round(row[2], 1) if row else 1000,
        "mmr_3s":   round(row[3], 1) if row else 1000,
    }

# ── routes: queue ──────────────────────────────────────────────────────────────
@app.post("/queue/join")
def queue_join(req: JoinRequest):
    existing_mid = player_match.get(req.player_id)
    if existing_mid and existing_mid in matches:
        raise HTTPException(409, "Already in an active match — finish or decline it first")

    ensure_player(req.player_id, req.real_id, req.username)
    if req.real_id:
        real_id_map[req.player_id] = req.real_id

    # Decline rate-limit check
    now_check = time.time()
    recent = [t for t in decline_log.get(req.player_id, [])
              if now_check - t < DECLINE_WINDOW_SECS]
    decline_log[req.player_id] = recent
    if len(recent) >= DECLINE_MAX:
        wait = int(DECLINE_COOLDOWN_SECS - (now_check - min(recent)))
        if wait > 0:
            raise HTTPException(429,
                f"Too many declines. Wait {wait}s before queuing again.")

    priority = 100 if req.player_id in victim_priority else 50
    victim_priority.discard(req.player_id)

    key = f"{req.region}_{req.mode}"
    if key not in queues:
        queues[key] = []

    # Remove duplicates (same player_id or same real_id)
    queues[key] = [
        p for p in queues[key]
        if p["player_id"] != req.player_id
        and not (req.real_id and p.get("real_id") == req.real_id)
    ]
    now = time.time()
    queues[key].append({
        "player_id":      req.player_id,
        "real_id":        req.real_id,
        "region":         req.region,
        "mode":           req.mode,
        "joined_at":      now,
        "last_heartbeat": now,
        "priority":       priority,
    })
    queues[key].sort(key=lambda p: p["priority"], reverse=True)

    needed = PLAYERS_NEEDED.get(req.mode, 2)
    if len(queues[key]) >= needed:
        players = queues[key][:needed]

        # Never match two slots with the same real_id
        real_ids_in_match = [p["real_id"] for p in players if p.get("real_id")]
        if len(real_ids_in_match) != len(set(real_ids_in_match)):
            pos = next((i + 1 for i, p in enumerate(queues[key])
                        if p["player_id"] == req.player_id), 0)
            return {"status": "queued", "position": pos}

        queues[key] = queues[key][needed:]

        match_id       = f"{req.region}_{req.mode}_{rand_str(6)}"
        lobby_name     = f"RLCQ_{rand_str(4)}"
        lobby_password = rand_str(6)
        host           = random.choice(players)
        team_a, team_b = make_teams([p["player_id"] for p in players], req.mode)

        match_info = {
            "match_id":       match_id,
            "region":         req.region,
            "mode":           req.mode,
            "players":        [p["player_id"] for p in players],
            "team_a":         team_a,
            "team_b":         team_b,
            "host_id":        host["player_id"],
            "lobby_name":     lobby_name,
            "lobby_password": lobby_password,
            "accepted":       [],
            "created_at":     time.time(),
            "lobby_ready":    False,
            "lobby_ready_at": None,
        }
        matches[match_id] = match_info
        for p in players:
            player_match[p["player_id"]] = match_id

    pos = next((i + 1 for i, p in enumerate(queues[key])
                if p["player_id"] == req.player_id), 0)
    return {"status": "queued", "position": pos}


@app.post("/queue/leave")
def queue_leave(req: LeaveRequest):
    for key in list(queues.keys()):
        queues[key] = [p for p in queues[key] if p["player_id"] != req.player_id]
    player_match.pop(req.player_id, None)
    return {"status": "left"}


LONG_POLL_SECS = 20.0
LONG_POLL_TICK = 0.25

def _match_found_payload(player_id: str, mid: str) -> dict:
    m = matches[mid]
    other_real_ids = [
        real_id_map.get(pid, "")
        for pid in m["players"]
        if pid != player_id and real_id_map.get(pid, "")
    ]
    team = 0 if player_id in m["team_a"] else 1
    return {
        "status":         "match_found",
        "match_id":       mid,
        "lobby_name":     m["lobby_name"],
        "lobby_password": m["lobby_password"],
        "is_host":        m["host_id"] == player_id,
        "mode":           m["mode"],
        "region":         m["region"],
        "real_ids":       other_real_ids,
        "team":           team,
    }

@app.post("/queue/heartbeat")
async def queue_heartbeat(req: LeaveRequest):
    mid = player_match.get(req.player_id)
    if mid and mid in matches:
        return _match_found_payload(req.player_id, mid)

    in_queue = False
    queue_count = 0
    queue_position = 0
    for key in queues:
        for i, p in enumerate(queues[key]):
            if p["player_id"] == req.player_id:
                p["last_heartbeat"] = time.time()
                in_queue       = True
                queue_count    = len(queues[key])
                queue_position = i + 1
                break
        if in_queue:
            break

    if not in_queue:
        return {"status": "not_in_queue"}

    deadline = time.time() + LONG_POLL_SECS
    while time.time() < deadline:
        await asyncio.sleep(LONG_POLL_TICK)
        mid = player_match.get(req.player_id)
        if mid and mid in matches:
            return _match_found_payload(req.player_id, mid)
        still_queued = any(
            p["player_id"] == req.player_id
            for key in queues for p in queues[key]
        )
        if not still_queued:
            return {"status": "not_in_queue"}

    return {"status": "queued", "queue_count": queue_count, "queue_position": queue_position}

# ── routes: match ──────────────────────────────────────────────────────────────
@app.post("/match/accept")
def match_accept(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    if req.player_id not in m["accepted"]:
        m["accepted"].append(req.player_id)
    if len(m["accepted"]) >= len(m["players"]) and "all_accepted_at" not in m:
        m["all_accepted_at"] = time.time()
    return {"status": "accepted", "accepted_count": len(m["accepted"]), "total": len(m["players"])}


@app.post("/match/decline")
def match_decline(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    decline_log.setdefault(req.player_id, []).append(time.time())
    for pid in m["players"]:
        if pid != req.player_id:
            victim_priority.add(pid)
    cancel_match(req.match_id, "A player declined the match.")
    return {"status": "declined"}


@app.get("/match/status/{match_id}")
def match_status(match_id: str):
    m = matches.get(match_id)
    if not m:
        # Check if it was just resolved (in history)
        conn = sqlite3.connect(DB_PATH)
        hist = conn.execute(
            "SELECT outcome, winner_ids FROM match_history WHERE match_id=?",
            (match_id,)
        ).fetchone()
        conn.close()
        if hist:
            return {"status": "resolved", "outcome": hist[0] or "normal"}
        c = cancelled_matches.get(match_id)
        if c:
            return {"status": "cancelled", "reason": c["reason"]}
        return {"status": "not_found"}

    elapsed        = time.time() - m["created_at"]
    time_remaining = max(0, 30 - int(elapsed))
    all_accepted   = len(m["accepted"]) >= len(m["players"])

    if time_remaining <= 0 and not all_accepted:
        for pid in m["players"]:
            player_match.pop(pid, None)
        matches.pop(match_id, None)
        return {"status": "expired"}

    # Draw countdown — starts from lobby_ready_at
    draw_in = None
    lobby_ready_at = m.get("lobby_ready_at")
    if lobby_ready_at:
        remaining = int(DRAW_TIMEOUT_SECS - (time.time() - lobby_ready_at))
        draw_in   = max(0, remaining)

    forfeits = m.get("forfeits", {})
    return {
        "status":         "accepting",
        "accepted_count": len(m["accepted"]),
        "total":          len(m["players"]),
        "all_accepted":   all_accepted,
        "time_remaining": time_remaining,
        "lobby_ready":    m.get("lobby_ready", False),
        "draw_in":        draw_in,
        "forfeits_a":     len(forfeits.get("team_a", [])),
        "forfeits_b":     len(forfeits.get("team_b", [])),
    }


@app.post("/match/lobby_ready")
def match_lobby_ready(req: AcceptRequest):
    """Host calls this once they've created the lobby in-game."""
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    if m["host_id"] != req.player_id:
        raise HTTPException(403, "Only the host can signal lobby ready")
    m["lobby_ready"]    = True
    m["lobby_ready_at"] = time.time()
    return {"status": "lobby_ready"}


@app.post("/match/result")
def submit_match_result(req: MatchResultRequest):
    """
    Called when a player presses Win / Loss / Draw in the plugin UI.
    No game hooks are used — this is the only result detection mechanism.

    Consensus rules:
    - All say "draw"                     → draw, no MMR change
    - team_a all say "win", team_b "loss" → award team_a
    - team_b all say "win", team_a "loss" → award team_b
    - Any conflict                        → flag disputed, admin review
    """
    if req.outcome not in ("win", "loss", "draw"):
        raise HTTPException(400, "outcome must be 'win', 'loss', or 'draw'")

    m = matches.get(req.match_id)
    if not m:
        return {"status": "recorded", "note": "match not active"}

    if req.player_id not in m["players"]:
        raise HTTPException(400, "Player not in this match")

    # Store this player's result
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR REPLACE INTO match_results (match_id, player_id, outcome, submitted_at) "
        "VALUES (?,?,?,?)",
        (req.match_id, req.player_id, req.outcome, int(time.time()))
    )
    conn.commit()

    rows = conn.execute(
        "SELECT player_id, outcome FROM match_results WHERE match_id=?",
        (req.match_id,)
    ).fetchall()
    conn.close()

    submitted = {r[0]: r[1] for r in rows}   # player_id -> outcome
    total     = len(m["players"])

    # Wait until all players have submitted
    if len(submitted) < total:
        return {"status": "recorded", "waiting": total - len(submitted)}

    team_a = m["team_a"]
    team_b = m["team_b"]

    a_outcomes = [submitted.get(p) for p in team_a if p in submitted]
    b_outcomes = [submitted.get(p) for p in team_b if p in submitted]

    # All say draw
    if all(o == "draw" for o in a_outcomes + b_outcomes):
        _record_draw(req.match_id, m, outcome="draw")
        return {"status": "draw_recorded"}

    # Consensus win for team_a
    if all(o == "win"  for o in a_outcomes) and \
       all(o == "loss" for o in b_outcomes):
        _award_match(req.match_id, m, team_a, team_b, outcome="normal")
        return {"status": "awarded", "winners": team_a}

    # Consensus win for team_b
    if all(o == "win"  for o in b_outcomes) and \
       all(o == "loss" for o in a_outcomes):
        _award_match(req.match_id, m, team_b, team_a, outcome="normal")
        return {"status": "awarded", "winners": team_b}

    # Conflict — flag for admin
    _flag_disputed(req.match_id, m)
    return {"status": "disputed",
            "note": "conflicting reports — admin review required"}


@app.post("/match/forfeit")
def match_forfeit(req: ForfeitRequest):
    """
    A player presses Forfeit.
    When the whole team forfeits, the other side wins.
    """
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    if req.player_id not in m["players"]:
        raise HTTPException(400, "Player not in this match")

    if req.player_id in m.get("team_a", []):
        my_team_key = "team_a"
        their_team  = m["team_b"]
        my_team     = m["team_a"]
    elif req.player_id in m.get("team_b", []):
        my_team_key = "team_b"
        their_team  = m["team_a"]
        my_team     = m["team_b"]
    else:
        raise HTTPException(400, "Player not on any team")

    forfeits = m.setdefault("forfeits", {"team_a": [], "team_b": []})
    if req.player_id not in forfeits[my_team_key]:
        forfeits[my_team_key].append(req.player_id)

    forfeited_count = len(forfeits[my_team_key])
    team_size       = len(my_team)
    whole_team_out  = forfeited_count >= team_size

    if whole_team_out:
        _award_match(req.match_id, m,
                     winner_ids=their_team, loser_ids=my_team,
                     outcome="forfeit")

    print(f"[forfeit] {req.player_id} in {req.match_id} "
          f"({forfeited_count}/{team_size})")
    return {
        "status":          "forfeited",
        "whole_team_out":  whole_team_out,
        "forfeited_count": forfeited_count,
        "team_size":       team_size,
    }

# ── routes: admin ──────────────────────────────────────────────────────────────
ADMIN_PASSWORD = "admin1234"   # change this before shipping

@app.get("/admin", response_class=HTMLResponse)
def admin_page():
    return HTMLResponse(content="""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RL Queue — Admin</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0d0d0d; color: #e0e0e0; font-family: 'Segoe UI', sans-serif; min-height: 100vh; }
  header {
    background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
    border-bottom: 2px solid #7f2a2a;
    padding: 18px 40px; display: flex; align-items: center; gap: 16px;
  }
  header h1 { font-size: 1.4rem; color: #ff6b6b; letter-spacing: 1px; }
  header span { color: #888; font-size: 0.9rem; }
  .container { max-width: 860px; margin: 40px auto; padding: 0 20px; }
  .card { background: #161616; border: 1px solid #2a2a2a; border-radius: 10px; padding: 28px 32px; margin-bottom: 24px; }
  .card h2 { font-size: 1.05rem; color: #ff6b6b; margin-bottom: 16px; }
  #login-card p { color: #888; font-size: 0.9rem; margin-bottom: 18px; }
  .login-row { display: flex; gap: 10px; }
  .login-row input {
    flex: 1; background: #222; border: 1px solid #333; border-radius: 6px;
    color: #e0e0e0; padding: 10px 14px; font-size: 0.95rem; outline: none;
  }
  .login-row input:focus { border-color: #ff6b6b; }
  .btn { border: none; border-radius: 6px; padding: 10px 22px; font-size: 0.95rem; cursor: pointer; color: #fff; }
  .btn-red   { background: #7f2a2a; } .btn-red:hover   { background: #a03535; }
  .btn-green { background: #1a5c1a; } .btn-green:hover { background: #228022; }
  .btn-grey  { background: #333;    } .btn-grey:hover  { background: #444;    }
  #login-error { margin-top: 12px; color: #ff5f5f; font-size: 0.9rem; min-height: 18px; }
  #admin-panel { display: none; }
  .report-card { background: #1a1a1a; border: 1px solid #2d2d2d; border-radius: 8px; padding: 18px 22px; margin-bottom: 16px; }
  .report-card .mid { font-size: 0.8rem; color: #666; margin-bottom: 6px; }
  .report-card .reporter { font-size: 1rem; color: #5fa8ff; font-weight: 600; margin-bottom: 4px; }
  .report-card .meta { font-size: 0.85rem; color: #888; margin-bottom: 14px; }
  .badge { display: inline-block; padding: 2px 10px; border-radius: 20px; font-size: 0.75rem; font-weight: 600; margin-left: 8px; }
  .badge-pending { background: #3d3000; color: #ffd040; }
  .badge-accepted { background: #003d10; color: #40ff80; }
  .badge-cancelled { background: #3d0000; color: #ff6060; }
  .actions { display: flex; gap: 10px; flex-wrap: wrap; }
  .status-msg { color: #4cff91; font-size: 0.88rem; margin-top: 10px; min-height: 18px; }
  .empty { color: #555; text-align: center; padding: 30px 0; }
  .refresh-row { display: flex; align-items: center; gap: 14px; margin-bottom: 20px; }
  #panel-status { color: #888; font-size: 0.88rem; }
</style>
</head>
<body>
<header>
  <h1>🔒 RL Queue — Admin</h1>
  <span>Restricted access</span>
</header>
<div class="container">
  <div class="card" id="login-card">
    <h2>Admin Login</h2>
    <p>Enter the admin password to access the report review panel.</p>
    <div class="login-row">
      <input id="inp-pass" type="password" placeholder="Admin password"
             onkeydown="if(event.key==='Enter') login()">
      <button class="btn btn-red" onclick="login()">Unlock</button>
    </div>
    <div id="login-error"></div>
  </div>
  <div id="admin-panel">
    <div class="card">
      <h2>Reported Matches</h2>
      <div class="refresh-row">
        <button class="btn btn-grey" onclick="loadReports()">↻ Refresh</button>
        <span id="panel-status"></span>
        <button class="btn btn-grey" style="margin-left:auto" onclick="logout()">Lock</button>
      </div>
      <div id="reports-list"><div class="empty">Loading…</div></div>
    </div>
  </div>
</div>
<script>
let adminPassword = '';
async function login() {
  const pass = document.getElementById('inp-pass').value.trim();
  const err  = document.getElementById('login-error');
  if (!pass) { err.textContent = 'Enter a password.'; return; }
  err.textContent = '';
  try {
    const r = await fetch('/admin/reports?password=' + encodeURIComponent(pass));
    if (r.status === 403) { err.textContent = '❌ Wrong password.'; return; }
    const data = await r.json();
    adminPassword = pass;
    document.getElementById('login-card').style.display  = 'none';
    document.getElementById('admin-panel').style.display = 'block';
    renderReports(data);
  } catch(e) { err.textContent = '❌ Could not reach server.'; }
}
function logout() {
  adminPassword = '';
  document.getElementById('login-card').style.display  = 'block';
  document.getElementById('admin-panel').style.display = 'none';
  document.getElementById('inp-pass').value = '';
}
async function loadReports() {
  document.getElementById('panel-status').textContent = 'Loading…';
  try {
    const r = await fetch('/admin/reports?password=' + encodeURIComponent(adminPassword));
    const data = await r.json();
    renderReports(data);
    document.getElementById('panel-status').textContent = 'Updated ' + new Date().toLocaleTimeString();
  } catch(e) { document.getElementById('panel-status').textContent = 'Failed to load.'; }
}
function renderReports(data) {
  const el = document.getElementById('reports-list');
  if (!data.length) { el.innerHTML = '<div class="empty">No reports yet.</div>'; return; }
  el.innerHTML = data.map(r => {
    const date = r.submitted_at ? new Date(r.submitted_at * 1000).toLocaleString() : '—';
    const badgeCls = r.status === 'accepted' ? 'badge-accepted'
                   : r.status === 'cancelled' ? 'badge-cancelled' : 'badge-pending';
    return `<div class="report-card" id="rc-${r.id}">
      <div class="mid">Match ID: ${r.match_id}</div>
      <div class="reporter">Reported by: ${r.reporter_username}<span class="badge ${badgeCls}">${r.status}</span></div>
      <div class="meta">Submitted: ${date}</div>
      <div class="actions">
        <button class="btn btn-grey" onclick="downloadReplay(${r.id})">⬇ Download Replay</button>
        <button class="btn btn-green" onclick="adminAction('accept','${r.match_id}',${r.id})">✅ Accept Result</button>
        <button class="btn btn-red"   onclick="adminAction('cancel','${r.match_id}',${r.id})">✖ Cancel Match</button>
      </div>
      <div class="status-msg" id="msg-${r.id}"></div>
    </div>`;
  }).join('');
}
function downloadReplay(id) {
  window.open('/admin/replay/' + id + '?password=' + encodeURIComponent(adminPassword), '_blank');
}
async function adminAction(action, matchId, reportId) {
  const msg = document.getElementById('msg-' + reportId);
  msg.textContent = 'Processing…';
  try {
    const r = await fetch('/admin/match/' + action + '/' + matchId
                          + '?password=' + encodeURIComponent(adminPassword),
                          { method: 'POST' });
    const data = await r.json();
    msg.textContent = action === 'accept'
      ? (data.note === 'no_history' ? '✅ Report closed.' : '✅ Result accepted — MMR awarded.')
      : (data.note === 'no_history' ? '✖ Report closed.' : '✖ Match cancelled — MMR reversed.');
    setTimeout(loadReports, 1500);
  } catch(e) { msg.textContent = 'Request failed.'; }
}
</script>
</body>
</html>""")

@app.get("/admin/reports")
def admin_reports(password: str = ""):
    if password != ADMIN_PASSWORD:
        raise HTTPException(403, "Unauthorized")
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        "SELECT r.id, r.match_id, r.reporter_id, r.submitted_at, r.status, "
        "p.username, COALESCE(mh.mode, '') AS mode "
        "FROM reports r "
        "LEFT JOIN players p ON r.reporter_id = p.player_id "
        "LEFT JOIN match_history mh ON mh.match_id = r.match_id "
        "ORDER BY r.submitted_at DESC LIMIT 50"
    ).fetchall()
    conn.close()
    results = []
    for r in rows:
        results.append({
            "id":                r[0],
            "match_id":          r[1],
            "reporter_id":       r[2],
            "submitted_at":      r[3],
            "status":            r[4],
            "reporter_username": r[5] or r[2],
        })
    return results

@app.get("/admin/replay/{report_id}")
def admin_get_replay(report_id: int, password: str = ""):
    if password != ADMIN_PASSWORD:
        raise HTTPException(403, "Unauthorized")
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT replay_path, match_id FROM reports WHERE id=?", (report_id,)
    ).fetchone()
    conn.close()
    if not row or not row[0] or not os.path.exists(row[0]):
        raise HTTPException(404, "Replay not found")
    from fastapi.responses import FileResponse
    return FileResponse(row[0], filename=f"{row[1]}_report.replay",
                        media_type="application/octet-stream")

@app.post("/admin/match/accept/{match_id}")
def admin_accept_match(match_id: str, password: str = ""):
    if password != ADMIN_PASSWORD:
        raise HTTPException(403, "Unauthorized")
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT winner_ids, loser_ids, mode, mmr_delta_win, mmr_delta_loss, disputed "
        "FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    note = ""
    if not row:
        note = "no_history"
    elif row[5] == 1:
        winner_ids = [p for p in row[0].split(",") if p]
        loser_ids  = [p for p in row[1].split(",") if p]
        validate_mode(row[2])
        col = f"mmr_{row[2]}"
        delta_w, delta_l = row[3] or 0.0, row[4] or 0.0
        for pid in winner_ids:
            conn.execute(f"UPDATE players SET {col}={col}+? WHERE player_id=?", (delta_w, pid))
        for pid in loser_ids:
            conn.execute(f"UPDATE players SET {col}={col}+? WHERE player_id=?", (delta_l, pid))
        conn.execute("UPDATE match_history SET disputed=0 WHERE match_id=?", (match_id,))
    conn.execute("UPDATE reports SET status='accepted' WHERE match_id=?", (match_id,))
    conn.commit()
    conn.close()
    return {"status": "accepted", "note": note}

@app.post("/admin/match/cancel/{match_id}")
def admin_cancel_match(match_id: str, password: str = ""):
    if password != ADMIN_PASSWORD:
        raise HTTPException(403, "Unauthorized")
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT winner_ids, loser_ids, mode, mmr_delta_win, mmr_delta_loss, disputed "
        "FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    note = ""
    if not row:
        note = "no_history"
    elif row[5] == 0:
        winner_ids = [p for p in row[0].split(",") if p]
        loser_ids  = [p for p in row[1].split(",") if p]
        validate_mode(row[2])
        col = f"mmr_{row[2]}"
        delta_w, delta_l = row[3] or 0.0, row[4] or 0.0
        for pid in winner_ids:
            conn.execute(f"UPDATE players SET {col}={col}-? WHERE player_id=?", (delta_w, pid))
        for pid in loser_ids:
            conn.execute(f"UPDATE players SET {col}={col}-? WHERE player_id=?", (delta_l, pid))
        conn.execute("UPDATE match_history SET disputed=1 WHERE match_id=?", (match_id,))
    conn.execute("UPDATE reports SET status='cancelled' WHERE match_id=?", (match_id,))
    conn.commit()
    conn.close()
    return {"status": "cancelled", "note": note}

# ── routes: dispute replay upload ─────────────────────────────────────────────
MAX_REPLAY_BYTES = 50 * 1024 * 1024   # 50 MB

@app.post("/match/report/{match_id}")
async def report_match(match_id: str, request: Request, reporter: str = ""):
    """
    Upload a replay as evidence of a disputed result.
    This is for disagreements only — normal results use the Win/Loss/Draw buttons.
    """
    replay_bytes = await request.body()
    if not replay_bytes:
        raise HTTPException(400, "Replay body is empty")
    if len(replay_bytes) > MAX_REPLAY_BYTES:
        raise HTTPException(413, f"Replay too large (max {MAX_REPLAY_BYTES // 1024 // 1024} MB)")

    safe_reporter = safe_filename(reporter)

    # Validate reporter is a player in this match (active or completed)
    m = matches.get(match_id)
    if m:
        if safe_reporter not in m["players"]:
            raise HTTPException(403, "Reporter is not a player in this match")
    else:
        conn_check = sqlite3.connect(DB_PATH)
        row_check = conn_check.execute(
            "SELECT winner_ids, loser_ids FROM match_history WHERE match_id=?",
            (match_id,)
        ).fetchone()
        conn_check.close()
        if row_check:
            all_ids = set((row_check[0] + "," + row_check[1]).split(","))
            if safe_reporter not in all_ids:
                raise HTTPException(403, "Reporter is not a player in this match")
        else:
            raise HTTPException(404, "Match not found")

    reports_dir = os.path.join(os.path.dirname(__file__), "reports")
    os.makedirs(reports_dir, exist_ok=True)
    safe_mid    = safe_filename(match_id)
    replay_path = os.path.join(reports_dir, f"{safe_mid}_report_{safe_reporter}.replay")
    with open(replay_path, "wb") as f:
        f.write(replay_bytes)

    conn = sqlite3.connect(DB_PATH)

    # 1-hour report window
    hist_row = conn.execute(
        "SELECT timestamp FROM match_history WHERE match_id=?", (safe_mid,)
    ).fetchone()
    if hist_row and time.time() - hist_row[0] > 3600:
        conn.close()
        raise HTTPException(410, "Report window has expired (1 hour after match).")

    existing = conn.execute(
        "SELECT id FROM reports WHERE match_id=? AND reporter_id=?",
        (match_id, safe_reporter)
    ).fetchone()
    if not existing:
        conn.execute(
            "INSERT INTO reports (match_id, reporter_id, replay_path, submitted_at, status) "
            "VALUES (?,?,?,?,?)",
            (match_id, safe_reporter, replay_path, int(time.time()), "pending")
        )
        conn.commit()

    report_count = conn.execute(
        "SELECT COUNT(DISTINCT reporter_id) FROM reports WHERE match_id=?", (match_id,)
    ).fetchone()[0]
    conn.commit()
    conn.close()
    return {"status": "reported", "match_id": match_id, "report_count": report_count}

# ── routes: player data ────────────────────────────────────────────────────────
@app.get("/player/search")
def player_search(q: str = ""):
    if not q or len(q) < 2:
        return []
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        "SELECT player_id, username, mmr_1s, mmr_2s, mmr_3s, wins, losses "
        "FROM players WHERE username LIKE ? AND username != '' LIMIT 10",
        (f"%{q}%",)
    ).fetchall()
    conn.close()
    return [{"player_id": r[0], "username": r[1],
             "mmr_1s": round(r[2], 1), "mmr_2s": round(r[3], 1), "mmr_3s": round(r[4], 1),
             "wins": r[5], "losses": r[6]} for r in rows]

@app.get("/player/{player_id}/mmr")
def player_mmr(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT username, mmr_1s, mmr_2s, mmr_3s, wins, losses FROM players WHERE player_id=?",
        (player_id,)
    ).fetchone()
    conn.close()
    if not row:
        return {"player_id": player_id, "username": "", "mmr_1s": 1000,
                "mmr_2s": 1000, "mmr_3s": 1000, "wins": 0, "losses": 0}
    return {"player_id": player_id, "username": row[0] or "",
            "mmr_1s": round(row[1], 1), "mmr_2s": round(row[2], 1), "mmr_3s": round(row[3], 1),
            "wins": row[4], "losses": row[5]}

@app.get("/player/{player_id}/history")
def player_history(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        """SELECT match_id, mode, region, winner_ids, loser_ids,
                  timestamp, mmr_delta_win, mmr_delta_loss,
                  COALESCE(outcome, 'normal') as outcome
           FROM match_history
           WHERE winner_ids LIKE ? OR loser_ids LIKE ?
           ORDER BY timestamp DESC LIMIT 10""",
        (f"%{player_id}%", f"%{player_id}%")
    ).fetchall()
    conn.close()
    history = []
    for mid, mode, region, winner_ids, loser_ids, ts, dw, dl, outcome in rows:
        won = player_id in (winner_ids or "").split(",")
        history.append({
            "match_id":   mid,
            "mode":       mode,
            "region":     region,
            "won":        won,
            "outcome":    outcome,
            "mmr_change": round(dw if won else dl, 1),
            "timestamp":  ts,
        })
    return history

@app.get("/account/status/{player_id}")
def account_status(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT username, mmr_1s, mmr_2s, mmr_3s FROM players WHERE player_id=?",
        (player_id,)
    ).fetchone()
    conn.close()
    if row:
        return {"registered": True, "username": row[0] or "",
                "mmr_1s": round(row[1], 1), "mmr_2s": round(row[2], 1),
                "mmr_3s": round(row[3], 1)}
    return {"registered": False, "username": ""}

@app.get("/leaderboard/{mode}")
def leaderboard(mode: str):
    validate_mode(mode)
    col = f"mmr_{mode}"
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        f"SELECT player_id, username, mmr_1s, mmr_2s, mmr_3s, wins, losses "
        f"FROM players WHERE username != '' ORDER BY {col} DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return [{"rank": i + 1, "player_id": r[0], "username": r[1] or "",
             "mmr_1s": round(r[2], 1), "mmr_2s": round(r[3], 1), "mmr_3s": round(r[4], 1),
             "wins": r[5], "losses": r[6]}
            for i, r in enumerate(rows)]
