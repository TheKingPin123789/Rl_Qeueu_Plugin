from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel
import random
import string
import time
import sqlite3
import os
from typing import Optional

app = FastAPI()
DB_PATH = os.path.join(os.path.dirname(__file__), "queue.db")

# ── in-memory queues ───────────────────────────────────────────────────────────
# key: "region_mode"  e.g. "NAE_3s"
queues: dict[str, list[dict]] = {}

# active matches: match_id -> match info
matches: dict[str, dict] = {}

# player -> match_id (for polling)
player_match: dict[str, str] = {}

# bakkesmod_id -> real_id (session only, updated on every queue join)
real_id_map: dict[str, str] = {}

PLAYERS_NEEDED = {"1s": 2, "2s": 4, "3s": 6}

# ── database ───────────────────────────────────────────────────────────────────
def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS players (
            player_id TEXT PRIMARY KEY,
            mmr_1s    REAL DEFAULT 1000,
            mmr_2s    REAL DEFAULT 1000,
            mmr_3s    REAL DEFAULT 1000,
            wins      INTEGER DEFAULT 0,
            losses    INTEGER DEFAULT 0
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS match_history (
            match_id   TEXT PRIMARY KEY,
            mode       TEXT,
            region     TEXT,
            winner_ids TEXT,
            loser_ids  TEXT,
            timestamp  INTEGER
        )
    """)
    conn.commit()
    conn.close()

def get_mmr(player_id: str, mode: str) -> float:
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        f"SELECT mmr_{mode} FROM players WHERE player_id=?", (player_id,)
    ).fetchone()
    conn.close()
    return row[0] if row else 1000.0

def ensure_player(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO players (player_id) VALUES (?)", (player_id,)
    )
    conn.commit()
    conn.close()

def update_mmr(winner_ids: list, loser_ids: list, mode: str):
    K = 32
    conn = sqlite3.connect(DB_PATH)
    col = f"mmr_{mode}"

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

# ── models ─────────────────────────────────────────────────────────────────────
class JoinRequest(BaseModel):
    player_id: str
    real_id: str = ""   # current RL account ID — session only, can change on account switch
    region: str
    mode: str

class LeaveRequest(BaseModel):
    player_id: str

class AcceptRequest(BaseModel):
    player_id: str
    match_id: str

# ── routes ─────────────────────────────────────────────────────────────────────
@app.on_event("startup")
def startup():
    init_db()

@app.post("/queue/join")
def queue_join(req: JoinRequest):
    ensure_player(req.player_id)

    # always update real ID mapping — covers account switches
    if req.real_id:
        real_id_map[req.player_id] = req.real_id

    key = f"{req.region}_{req.mode}"
    if key not in queues:
        queues[key] = []

    # remove if already queued
    queues[key] = [p for p in queues[key] if p["player_id"] != req.player_id]
    queues[key].append({
        "player_id": req.player_id,
        "region": req.region,
        "mode": req.mode,
        "joined_at": time.time()
    })

    needed = PLAYERS_NEEDED.get(req.mode, 2)
    if len(queues[key]) >= needed:
        players = queues[key][:needed]
        queues[key] = queues[key][needed:]

        match_id = f"{req.region}_{req.mode}_{rand_str(6)}"
        lobby_name = f"RLCQ_{rand_str(4)}"
        lobby_password = rand_str(6)
        host = random.choice(players)

        team_a, team_b = make_teams([p["player_id"] for p in players], req.mode)

        match_info = {
            "match_id": match_id,
            "region": req.region,
            "mode": req.mode,
            "players": [p["player_id"] for p in players],
            "team_a": team_a,
            "team_b": team_b,
            "host_id": host["player_id"],
            "lobby_name": lobby_name,
            "lobby_password": lobby_password,
            "accepted": [],
            "created_at": time.time()
        }
        matches[match_id] = match_info
        for p in players:
            player_match[p["player_id"]] = match_id

    return {"status": "queued"}

@app.post("/queue/leave")
def queue_leave(req: LeaveRequest):
    for key in queues:
        queues[key] = [p for p in queues[key] if p["player_id"] != req.player_id]
    player_match.pop(req.player_id, None)
    return {"status": "left"}

@app.get("/queue/status/{player_id}")
def queue_status(player_id: str):
    mid = player_match.get(player_id)
    if not mid or mid not in matches:
        return {"status": "queued"}

    m = matches[mid]
    # real IDs of all OTHER players (host uses these to send party invites)
    other_real_ids = [
        real_id_map.get(pid, "")
        for pid in m["players"]
        if pid != player_id and real_id_map.get(pid, "")
    ]
    return {
        "status": "match_found",
        "match_id": mid,
        "lobby_name": m["lobby_name"],
        "lobby_password": m["lobby_password"],
        "is_host": m["host_id"] == player_id,
        "mode": m["mode"],
        "region": m["region"],
        "real_ids": other_real_ids
    }

@app.post("/match/accept")
def match_accept(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    if req.player_id not in m["accepted"]:
        m["accepted"].append(req.player_id)
    return {"status": "accepted", "accepted_count": len(m["accepted"]), "total": len(m["players"])}

@app.post("/match/decline")
def match_decline(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    player_match.pop(req.player_id, None)
    return {"status": "declined"}

@app.post("/match/replay/{match_id}")
async def upload_replay(match_id: str, request: Request):
    m = matches.get(match_id)
    if not m:
        raise HTTPException(404, "Match not found")

    replay_bytes = await request.body()
    replay_dir = os.path.join(os.path.dirname(__file__), "replays")
    os.makedirs(replay_dir, exist_ok=True)
    replay_path = os.path.join(replay_dir, f"{match_id}.replay")

    with open(replay_path, "wb") as f:
        f.write(replay_bytes)

    # parse replay to determine winner
    result = parse_replay(replay_path, m)
    if result:
        winner_ids, loser_ids = result
        delta_w, delta_l = update_mmr(winner_ids, loser_ids, m["mode"])

        conn = sqlite3.connect(DB_PATH)
        conn.execute(
            "INSERT OR IGNORE INTO match_history VALUES (?,?,?,?,?,?)",
            (match_id, m["mode"], m["region"],
             ",".join(winner_ids), ",".join(loser_ids), int(time.time()))
        )
        conn.commit()
        conn.close()

        matches.pop(match_id, None)
        for pid in m["players"]:
            player_match.pop(pid, None)

        return {
            "status": "processed",
            "winners": winner_ids,
            "losers": loser_ids,
            "mmr_change_win": f"+{delta_w}",
            "mmr_change_loss": str(delta_l)
        }

    return {"status": "replay_saved", "note": "could not parse result"}

@app.get("/player/{player_id}/mmr")
def player_mmr(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT mmr_1s, mmr_2s, mmr_3s, wins, losses FROM players WHERE player_id=?",
        (player_id,)
    ).fetchone()
    conn.close()
    if not row:
        return {"player_id": player_id, "mmr_1s": 1000, "mmr_2s": 1000, "mmr_3s": 1000, "wins": 0, "losses": 0}
    return {
        "player_id": player_id,
        "mmr_1s": round(row[0], 1),
        "mmr_2s": round(row[1], 1),
        "mmr_3s": round(row[2], 1),
        "wins": row[3],
        "losses": row[4]
    }

@app.get("/leaderboard/{mode}")
def leaderboard(mode: str):
    col = f"mmr_{mode}"
    conn = sqlite3.connect(DB_PATH)
    rows = conn.execute(
        f"SELECT player_id, {col}, wins, losses FROM players ORDER BY {col} DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return [{"rank": i+1, "player_id": r[0], "mmr": round(r[1], 1), "wins": r[2], "losses": r[3]}
            for i, r in enumerate(rows)]

# ── replay parsing ─────────────────────────────────────────────────────────────
def parse_replay(path: str, match: dict):
    """
    Reads the RL replay binary header to extract Team0Score and Team1Score.
    No external dependencies needed — scores are stored as plain UTF-8 strings
    in the unreal property list near the start of the file.
    """
    try:
        with open(path, "rb") as f:
            data = f.read(8192)  # scores appear in first 8KB

        text = data.decode("latin-1")

        def find_score(key: str) -> int:
            idx = text.find(key)
            if idx == -1:
                return -1
            # score is a 4-byte little-endian int 8 bytes after the key
            offset = idx + len(key) + 8
            if offset + 4 > len(data):
                return -1
            return int.from_bytes(data[offset:offset+4], "little")

        score0 = find_score("Team0Score")
        score1 = find_score("Team1Score")

        if score0 < 0 or score1 < 0:
            return None

        team_a = match["team_a"]
        team_b = match["team_b"]

        if score0 > score1:
            return team_a, team_b
        elif score1 > score0:
            return team_b, team_a
    except Exception:
        pass

    return None
