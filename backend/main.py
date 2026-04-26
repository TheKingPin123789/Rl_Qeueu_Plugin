from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse, HTMLResponse, FileResponse
from pydantic import BaseModel
from sse_starlette.sse import EventSourceResponse
import asyncio
import json as _json
import math
import random
import string
import struct
import time
import sqlite3
import os
import re
from collections import Counter
from datetime import datetime, timezone
from itertools import combinations

app = FastAPI()

# ── SSE push infrastructure ───────────────────────────────────────────────────
# player_id → asyncio.Queue of events to push down the SSE stream
sse_queues: dict[str, asyncio.Queue] = {}
_loop: asyncio.AbstractEventLoop | None = None

@app.on_event("startup")
async def _capture_loop():
    global _loop
    _loop = asyncio.get_event_loop()

def _push(player_id: str, event_type: str, data: dict):
    """Push an SSE event to a player from any context (sync or async)."""
    q = sse_queues.get(player_id)
    if q and _loop:
        # Include "event" inside the data JSON so the client can identify the type
        # (the SSE protocol's "event:" header line is not read by the plugin's parser)
        payload = {"event": event_type, **data}
        _loop.call_soon_threadsafe(q.put_nowait, {
            "event": event_type,
            "data":  _json.dumps(payload),
        })

def _push_all(player_ids: list[str], event_type: str, data: dict):
    for rid in player_ids:
        _push(rid, event_type, data)
DB_PATH = os.path.join(os.path.dirname(__file__), "queue.db")

# ── in-memory state ────────────────────────────────────────────────────────────
# key: "region_mode"  e.g. "NAE_3s"
queues: dict[str, list[dict]] = {}

# active matches: match_id -> match info dict
matches: dict[str, dict] = {}

# player_id -> match_id (so heartbeat can find "match_found" quickly)
player_match: dict[str, str] = {}

# system_id -> player_id (session only, updated on every queue join)
system_real_map: dict[str, str] = {}

# Recently-cancelled matches: match_id -> {reason, at}
# Kept for 5 minutes so polling clients can read the cancellation reason.
cancelled_matches: dict[str, dict] = {}

# Priority set: players who should be placed at the front on next join
# (set when their match was cancelled because someone else declined)
victim_priority: set[str] = set()

# Decline rate-limiting: player_id -> list of decline timestamps
decline_log: dict[str, list[float]] = {}

# No-response rate-limiting: player_id -> list of timestamps where they
# failed to accept/decline within the 30s window, or (as host) failed to
# create the lobby in 3 min.  Shares the same 3-strike, 5-min-ban logic.
no_response_log: dict[str, list[float]] = {}

# Signals the matchmaker to wake up immediately when a player joins.
# Set from the sync queue_join thread via _loop.call_soon_threadsafe.
_match_trigger: asyncio.Event = asyncio.Event()

# ── timing constants ──────────────────────────────────────────────────────────
DECLINE_WINDOW_SECS   = 600   # 10 min window for counting recent declines
DECLINE_MAX           = 3     # max declines before cooldown
DECLINE_COOLDOWN_SECS = 300   # 5 min cooldown after too many declines

NO_RESPONSE_WINDOW_SECS = 3600  # 1 hour window for counting no-responses
NO_RESPONSE_MAX         = 3     # strikes before being dequeued + temp ban
NO_RESPONSE_BAN_SECS    = 300   # 5 min ban after 3 no-responses

LOBBY_CREATE_TIMEOUT = 180   # 3 min: host must click "Lobby ready" after all accept
DRAW_TIMEOUT_SECS    = 1800  # 30 min after lobby_ready with no result = auto-draw

# ── matchmaker MMR range expansion ────────────────────────────────────────────
# Each player's search window expands the longer they wait.
# Starts at ±50, grows by 25 every 30s, caps at ±150.
MMR_RANGE_START    = 50
MMR_RANGE_STEP     = 25
MMR_RANGE_EXPAND_S = 30
MMR_RANGE_MAX      = 150

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

    # ── column migrations ─────────────────────────────────────────────────────
    # Legacy DB had: player_id (PK, stored Steam/Epic ID) + real_id (duplicate)
    # Intermediate DB had: real_id (PK) + system_id (BakkesMod install ID)
    # Final schema: player_id (PK, Steam/Epic ID) + system_id (BakkesMod install ID)

    cols_pi = [r[1] for r in conn.execute("PRAGMA table_info(players)").fetchall()]
    # Case 1: legacy DB — has old "player_id" (Steam/Epic) and "real_id" (duplicate)
    if "player_id" not in cols_pi and "real_id" in cols_pi:
        # intermediate state: real_id is the PK, rename it to player_id
        conn.execute("ALTER TABLE players RENAME COLUMN real_id TO player_id")
        conn.commit()
    elif "player_id" in cols_pi and "real_id" in cols_pi:
        # very old DB: both existed as separate columns — drop the redundant real_id
        # (real_id was a duplicate of player_id; system_id column already added separately)
        conn.execute("ALTER TABLE players RENAME COLUMN real_id TO _drop_real_id")
        conn.commit()

    for tbl in ("match_history", "replay_submissions", "reports"):
        cols = [r[1] for r in conn.execute(f"PRAGMA table_info({tbl})").fetchall()]
        if "real_id" in cols and "player_id" not in cols:
            conn.execute(f"ALTER TABLE {tbl} RENAME COLUMN real_id TO player_id")
            conn.commit()

    conn.execute("""
        CREATE TABLE IF NOT EXISTS players (
            player_id   TEXT PRIMARY KEY,
            system_id TEXT DEFAULT '',
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
        "username        TEXT    DEFAULT ''",
        "system_id       TEXT    DEFAULT ''",
        "platform        TEXT    DEFAULT ''",
        "platform_id     TEXT    DEFAULT ''",
        "rl_display_name TEXT    DEFAULT ''",
        "disconnect_wins INTEGER DEFAULT 0",
        "decline_count   INTEGER DEFAULT 0",
        "last_decline_at INTEGER DEFAULT 0",
        "trust_score     REAL    DEFAULT 100.0",
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
    # No player_id column in match_history (winner_ids/loser_ids are TEXT fields)
    # migrate old schema
    for col in [
        "disputed INTEGER DEFAULT 0",
        "mmr_delta_win REAL DEFAULT 0",
        "mmr_delta_loss REAL DEFAULT 0",
        "outcome TEXT DEFAULT 'normal'",
        "replay_status TEXT DEFAULT 'unverified'",
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
            player_id      TEXT,
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

    # Replay verification: one row per match, tracks the collection window and outcome
    conn.execute("""
        CREATE TABLE IF NOT EXISTS replay_collection (
            match_id           TEXT PRIMARY KEY,
            lobby_ready_at     REAL DEFAULT 0,
            resolved_at        REAL DEFAULT 0,
            collection_ends_at REAL DEFAULT 0,
            button_outcome     TEXT DEFAULT '',   -- 'team_a' / 'team_b' / 'draw'
            button_winners     TEXT DEFAULT '',   -- comma-sep player IDs
            button_losers      TEXT DEFAULT '',
            total_players      INTEGER DEFAULT 0,
            replay_status      TEXT DEFAULT 'pending',
            verified_winners   TEXT DEFAULT '',
            verified_losers    TEXT DEFAULT '',
            saved_replay_path  TEXT DEFAULT '',
            match_guid         TEXT DEFAULT '',
            trust_adjusted     INTEGER DEFAULT 0
        )
    """)

    # Replay verification: one row per player per match
    conn.execute("""
        CREATE TABLE IF NOT EXISTS replay_submissions (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id     TEXT,
            player_id      TEXT,
            status       TEXT DEFAULT 'pending',
            match_guid   TEXT DEFAULT '',
            replay_path  TEXT DEFAULT '',
            winner_ids   TEXT DEFAULT '',
            submitted_at INTEGER DEFAULT 0,
            UNIQUE(match_id, player_id)
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

def ensure_player(player_id: str, system_id: str = "", username: str = ""):
    """Upsert a player record.

    player_id    = Steam64 or Epic account ID (primary key)
    system_id  = BakkesMod install ID (machine identifier)
    """
    conn = sqlite3.connect(DB_PATH)

    conn.execute("INSERT OR IGNORE INTO players (player_id) VALUES (?)", (player_id,))
    if system_id:
        conn.execute("UPDATE players SET system_id=? WHERE player_id=?", (system_id, player_id))
    if username:
        conn.execute("UPDATE players SET username=? WHERE player_id=?", (username, player_id))
    conn.commit()
    conn.close()

def update_platform(player_id: str, platform: str):
    if not platform:
        return
    conn = sqlite3.connect(DB_PATH)
    conn.execute("UPDATE players SET platform=? WHERE player_id=?", (platform, player_id))
    conn.commit()
    conn.close()

def update_rl_display_name(player_id: str, rl_display_name: str):
    """Save the player's in-game platform display name for replay verification."""
    if not rl_display_name:
        return
    conn = sqlite3.connect(DB_PATH)
    conn.execute("UPDATE players SET rl_display_name=? WHERE player_id=?",
                 (rl_display_name, player_id))
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

def _penalize_no_response(player_id: str) -> bool:
    """Record a no-response strike for player_id.

    Returns True if the player has now hit the limit and should be dequeued.
    Also removes them from every queue if the limit is reached.
    """
    now    = time.time()
    recent = [t for t in no_response_log.get(player_id, [])
              if now - t < NO_RESPONSE_WINDOW_SECS]
    recent.append(now)
    no_response_log[player_id] = recent

    if len(recent) >= NO_RESPONSE_MAX:
        # Remove from all queues immediately
        for key in list(queues.keys()):
            queues[key] = [p for p in queues[key] if p["player_id"] != player_id]
        print(f"[penalty] {player_id} hit no-response limit — dequeued")
        return True
    return False

def make_balanced_teams(players: list[dict]) -> tuple[list[str], list[str]]:
    """Split players into two teams with the closest possible average MMR.

    players: list of {"player_id": str, "mmr": float}
    Returns (team_a_ids, team_b_ids).

    Brute-forces all C(n, n/2) combinations — at most 20 for a 6-player lobby,
    trivially fast and gives the mathematically optimal split every time.
    """
    n    = len(players)
    half = n // 2

    best_a, best_b = None, None
    best_diff = float("inf")

    for combo in combinations(range(n), half):
        team_a = [players[i] for i in combo]
        team_b = [players[i] for i in range(n) if i not in combo]
        avg_a  = sum(p["mmr"] for p in team_a) / half
        avg_b  = sum(p["mmr"] for p in team_b) / half
        diff   = abs(avg_a - avg_b)
        if diff < best_diff:
            best_diff = diff
            best_a, best_b = team_a, team_b

    return [p["player_id"] for p in best_a], [p["player_id"] for p in best_b]

def cancel_match(match_id: str, reason: str, declining_id: str = ""):
    m = matches.pop(match_id, None)
    if m:
        for pid in m["players"]:
            player_match.pop(pid, None)
            # Tell each player the match was cancelled; victims get priority flag
            _push(pid, "match_cancelled", {
                "reason":   reason,
                "priority": pid != declining_id and bool(declining_id),
            })
    cancelled_matches[match_id] = {"reason": reason, "at": time.time()}
    print(f"[cancel] {match_id}: {reason}")

def _award_match(match_id: str, m: dict,
                 winner_ids: list, loser_ids: list, outcome: str = "normal",
                 replay_status: str = "unverified"):
    player_ids = list(m["players"])   # copy before match is removed
    delta_w, delta_l = update_mmr(winner_ids, loser_ids, m["mode"])
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO match_history "
        "(match_id, mode, region, winner_ids, loser_ids, "
        " timestamp, disputed, mmr_delta_win, mmr_delta_loss, outcome, replay_status) "
        "VALUES (?,?,?,?,?,?,0,?,?,?,?)",
        (match_id, m["mode"], m["region"],
         ",".join(winner_ids), ",".join(loser_ids),
         int(time.time()), delta_w, delta_l, outcome, replay_status)
    )
    conn.commit()
    conn.close()
    for pid in player_ids:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    _push_all(player_ids, "match_resolved", {
        "outcome":  outcome,
        "winners":  winner_ids,
    })
    print(f"[match] {match_id} → {outcome}: winners={winner_ids}")

def _record_draw(match_id: str, m: dict, outcome: str = "draw",
                 replay_status: str = "unverified"):
    player_ids = list(m["players"])
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO match_history "
        "(match_id, mode, region, winner_ids, loser_ids, "
        " timestamp, disputed, mmr_delta_win, mmr_delta_loss, outcome, replay_status) "
        "VALUES (?,?,?,?,?,?,0,0,0,?,?)",
        (match_id, m["mode"], m["region"], "", "", int(time.time()), outcome, replay_status)
    )
    conn.commit()
    conn.close()
    for pid in player_ids:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    _push_all(player_ids, "match_resolved", {"outcome": outcome})
    print(f"[match] {match_id} → {outcome}")

def _flag_disputed(match_id: str, m: dict):
    player_ids = list(m["players"])
    conn = sqlite3.connect(DB_PATH)
    already = conn.execute(
        "SELECT 1 FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    if not already:
        conn.execute(
            "INSERT OR IGNORE INTO match_history "
            "(match_id, mode, region, winner_ids, loser_ids, "
            " timestamp, disputed, mmr_delta_win, mmr_delta_loss, replay_status) "
            "VALUES (?,?,?,?,?,?,1,0,0,'admin_review')",
            (match_id, m["mode"], m["region"], "", "", int(time.time()))
        )
        conn.commit()
    conn.close()
    for pid in player_ids:
        player_match.pop(pid, None)
    matches.pop(match_id, None)
    _push_all(player_ids, "match_resolved", {"outcome": "disputed"})
    print(f"[match] {match_id} → disputed (conflicting reports)")


# ── replay collection helpers ─────────────────────────────────────────────────

REPLAY_COLLECTION_WINDOW = 180   # seconds after resolution to accept replays

def _init_replay_collection(match_id: str, m: dict,
                             button_outcome: str,
                             button_winners: list,
                             button_losers: list,
                             resolved_at: float,
                             player_ids_to_notify: list):
    """
    Called right after a match is resolved via button votes.
    Creates a replay_collection record and pushes 'collect_replay' to all players.
    """
    collection_ends_at = resolved_at + REPLAY_COLLECTION_WINDOW
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "INSERT OR IGNORE INTO replay_collection "
        "(match_id, lobby_ready_at, resolved_at, collection_ends_at, "
        " button_outcome, button_winners, button_losers, total_players, replay_status) "
        "VALUES (?,?,?,?,?,?,?,?,'pending')",
        (match_id,
         m.get("lobby_ready_at", 0),
         resolved_at,
         collection_ends_at,
         button_outcome,
         ",".join(button_winners),
         ",".join(button_losers),
         len(m.get("players", [])))
    )
    conn.commit()
    conn.close()
    _push_all(player_ids_to_notify, "collect_replay", {
        "match_id":            match_id,
        "collection_ends_at":  collection_ends_at,
    })
    print(f"[replay] collection window opened for {match_id} "
          f"until {datetime.utcfromtimestamp(collection_ends_at).strftime('%H:%M:%S')} UTC")


def _adjust_trust(player_id: str, delta: float, reason: str):
    """Increase or decrease a player's trust_score, clamped to [0, 100]."""
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "UPDATE players SET trust_score = MAX(0, MIN(100, trust_score + ?)) "
        "WHERE player_id = ?",
        (delta, player_id)
    )
    conn.commit()
    score_row = conn.execute(
        "SELECT trust_score FROM players WHERE player_id=?", (player_id,)
    ).fetchone()
    conn.close()
    score = score_row[0] if score_row else "?"
    print(f"[trust] {player_id} {delta:+.1f} → {score}  ({reason})")


def _check_replay_resolution(match_id: str):
    """
    After each replay submission, check whether:
      a) A majority of verified replays agree → update replay_status to 'verified'
      b) All players have submitted (replay or no_replay) but no majority
         → update replay_status to 'no_majority'
      c) Otherwise → still waiting

    Also adjusts trust scores for players whose replay contradicts the button result.
    Called from upload_replay and no_replay endpoints.
    """
    conn = sqlite3.connect(DB_PATH)
    coll = conn.execute(
        "SELECT button_outcome, button_winners, button_losers, total_players, "
        "       collection_ends_at, trust_adjusted, replay_status "
        "FROM replay_collection WHERE match_id=?",
        (match_id,)
    ).fetchone()
    if not coll:
        conn.close()
        return

    (button_outcome, button_winners_str, button_losers_str,
     total_players, collection_ends_at, trust_adjusted, rc_status) = coll

    if rc_status not in ("pending",):
        conn.close()
        return   # already finalized

    subs = conn.execute(
        "SELECT player_id, status, winner_ids FROM replay_submissions WHERE match_id=?",
        (match_id,)
    ).fetchall()
    conn.close()

    button_winners = [x for x in (button_winners_str or "").split(",") if x]
    button_losers  = [x for x in (button_losers_str  or "").split(",") if x]

    # Count verified submissions and group by winner set
    vote_counts: Counter = Counter()
    verified_subs: list[tuple] = []   # (player_id, winner_ids_str)
    submitted_count = len(subs)

    for pid, status, winner_ids_str in subs:
        if status == "verified":
            vote_counts[winner_ids_str] += 1
            verified_subs.append((pid, winner_ids_str))

    # Check if majority reached (same threshold as button voting)
    # We need to know mode for threshold — look up from match_history
    conn2 = sqlite3.connect(DB_PATH)
    hist_row = conn2.execute(
        "SELECT mode FROM match_history WHERE match_id=?", (match_id,)
    ).fetchone()
    conn2.close()
    mode       = hist_row[0] if hist_row else "1s"
    threshold  = CONSENSUS_THRESHOLD.get(mode, 1.0)
    min_votes  = math.ceil(threshold * total_players)

    new_status    = None
    final_winners = None
    final_losers  = None

    if vote_counts:
        top_winner_str, top_count = vote_counts.most_common(1)[0]
        if top_count >= min_votes:
            verified_w = [x for x in top_winner_str.split(",") if x]
            # Determine losers: everyone who isn't a winner
            all_pids = button_winners + button_losers
            verified_l = [p for p in all_pids if p not in verified_w]

            new_status    = "verified"
            final_winners = verified_w
            final_losers  = verified_l
            print(f"[replay] {match_id} → verified by replay majority "
                  f"({top_count}/{total_players}): winners={verified_w}")

    if new_status is None and submitted_count >= total_players:
        new_status = "no_majority"
        print(f"[replay] {match_id} → all replays submitted but no majority")

    # ── trust adjustments (only done once per match) ──────────────────────────
    if new_status in ("verified", "no_majority") and not trust_adjusted:
        if new_status == "verified" and final_winners is not None:
            replay_winners_set = set(final_winners)
            button_winners_set = set(button_winners)
            for pid, winner_ids_str in verified_subs:
                sub_w = set(winner_ids_str.split(",")) if winner_ids_str else set()
                if sub_w == replay_winners_set:
                    # Player's replay matches the final verified result
                    if replay_winners_set == button_winners_set:
                        # Replay also confirms button vote → small trust bonus
                        _adjust_trust(pid, +2.0, f"replay confirms button for {match_id}")
                    # If replay disagrees with button but is the majority replay result,
                    # that's a complex case — don't penalize based on replay alone
                else:
                    # Player submitted a replay that contradicts the majority result
                    _adjust_trust(pid, -5.0, f"contradicting replay for {match_id}")

            # Players who clicked the wrong button (button ≠ replay majority)
            # Get their button votes from match_results
            conn3 = sqlite3.connect(DB_PATH)
            votes = conn3.execute(
                "SELECT player_id, outcome FROM match_results WHERE match_id=?",
                (match_id,)
            ).fetchall()
            conn3.close()
            for pid, outcome in votes:
                if outcome == "draw":
                    continue
                # Re-normalise: are they claiming their team won?
                player_is_winner = pid in replay_winners_set
                voted_win = (outcome == "win")
                if player_is_winner != voted_win:
                    # Button vote contradicts verified replay result
                    _adjust_trust(pid, -5.0,
                                  f"button contradicts replay for {match_id}")

        # Mark trust as adjusted so we don't double-apply
        conn4 = sqlite3.connect(DB_PATH)
        conn4.execute(
            "UPDATE replay_collection SET trust_adjusted=1 WHERE match_id=?",
            (match_id,)
        )
        conn4.commit()
        conn4.close()

    # ── persist final replay_status ───────────────────────────────────────────
    if new_status:
        conn5 = sqlite3.connect(DB_PATH)
        conn5.execute(
            "UPDATE replay_collection SET replay_status=?, verified_winners=?, "
            "verified_losers=? WHERE match_id=?",
            (new_status,
             ",".join(final_winners) if final_winners else "",
             ",".join(final_losers)  if final_losers  else "",
             match_id)
        )
        conn5.execute(
            "UPDATE match_history SET replay_status=? WHERE match_id=?",
            (new_status, match_id)
        )
        conn5.commit()
        conn5.close()


# ── replay parsing ────────────────────────────────────────────────────────────
# Parses the Rocket League .replay binary header using the Unreal property format.
# No external dependencies needed — everything is standard struct/bytes operations.

# Set once per parse at the top of _parse_replay_data; read inside _rl_read_prop.
# Safe under asyncio (single-threaded, _parse_replay_data is fully synchronous).
_replay_major: int = 868

def _rl_read_str(data: bytes, pos: int) -> tuple:
    """Read a length-prefixed Unreal string. Returns (str, new_pos)."""
    if pos + 4 > len(data):
        import binascii
        ctx = binascii.hexlify(data[max(0,pos-8):pos+16]).decode() if pos < len(data) else "(past end)"
        raise ValueError(f"EOF at string length  pos={pos}  data_len={len(data)}  context_hex={ctx}")
    length = struct.unpack_from("<i", data, pos)[0]   # signed int32
    pos += 4
    if length == 0:
        return "", pos
    if length < 0:
        # UTF-16LE — length is negative char count including null
        byte_len = (-length) * 2
        if pos + byte_len > len(data):
            raise ValueError(f"EOF reading UTF-16 string  pos={pos}  length={length}  byte_len={byte_len}  data_len={len(data)}")
        s = data[pos:pos + byte_len].decode("utf-16-le", errors="replace").rstrip("\x00")
        return s, pos + byte_len
    else:
        # ASCII — length includes null terminator
        if length > 65536:
            import binascii
            ctx = binascii.hexlify(data[pos-4:pos+16]).decode()
            raise ValueError(f"Implausible string length={length}  pos={pos-4}  hex={ctx}")
        s = data[pos:pos + length - 1].decode("latin-1", errors="replace")
        return s, pos + length

def _rl_read_prop(data: bytes, pos: int) -> tuple:
    """
    Read one Unreal serialised property.
    Returns (name, value, new_pos).
    name == 'None' signals end of property list.
    """
    name, pos = _rl_read_str(data, pos)
    if not name or name == "None":
        return "None", None, pos

    type_name, pos = _rl_read_str(data, pos)

    # 8-byte size block: [0-3] value_size  [4-7] array_index
    # In RL 868.14+, BoolProperty packs its value into the lower bit of
    # array_index (no extra byte follows), so we need both fields.
    if pos + 8 > len(data):
        raise ValueError("EOF at property size block")
    value_size  = struct.unpack_from("<I", data, pos)[0]
    array_index = struct.unpack_from("<I", data, pos + 4)[0]
    pos += 8
    # 'end' is the AUTHORITATIVE position after this property's value bytes.
    # value_size is written by RL's own serialiser and is always exact —
    # so every type handler returns 'end' rather than a hand-counted offset.
    # This makes the parser immune to version-specific quirks (old vs new
    # BoolProperty format, unknown ByteProperty variants, StructProperty
    # type-name prefix bytes, etc.).
    end = pos + value_size

    if type_name == "IntProperty":
        val = struct.unpack_from("<i", data, pos)[0] if end - pos >= 4 else 0
        return name, val, end

    elif type_name in ("StrProperty", "NameProperty"):
        try:
            val, _ = _rl_read_str(data, pos)
        except Exception:
            val = None
        return name, val, end

    elif type_name == "QWordProperty":
        val = struct.unpack_from("<Q", data, pos)[0] if end - pos >= 8 else 0
        return name, val, end

    elif type_name == "BoolProperty":
        # RL 868.x replays ALWAYS write a 1-byte bool value in the data stream,
        # even in the 868.14+ format where value_size=0.
        # Older replays (major version 0 / very old UE format) write a 4-byte
        # int32 bool in the stream instead (also with value_size=0 in the size block).
        # In both cases value_size in the 8-byte block doesn't count these bytes,
        # so we override 'end' and advance manually.
        if value_size >= 1:
            # value_size explicitly covers the bool byte(s) — use end as-is
            val = bool(data[pos]) if pos < len(data) else False
            return name, val, end
        elif _replay_major >= 868:
            # Modern format: 1 byte in stream after the size block
            val = bool(data[pos]) if pos < len(data) else bool(array_index & 1)
            return name, val, pos + 1
        else:
            # Old format (major 0 / pre-868): 4-byte int32 in stream
            val = bool(struct.unpack_from("<I", data, pos)[0]) if pos + 4 <= len(data) else bool(array_index & 1)
            return name, val, pos + 4

    elif type_name == "FloatProperty":
        val = struct.unpack_from("<f", data, pos)[0] if end - pos >= 4 else 0.0
        return name, val, end

    elif type_name == "ByteProperty":
        # Three historical formats:
        #   Raw byte  (value_size == 0 or 1): no prefix, just the byte.
        #   Named enum: [enum_type:string][enum_value:string]
        #     value_size covers ONLY the enum_value string (including its 4-byte
        #     length prefix) — the enum_type name string is written before it and
        #     is NOT counted in value_size.  Same pattern as StructProperty.
        #     Correct end = p + value_size  (p = position after reading enum_type).
        try:
            if value_size <= 1:
                val = data[pos] if pos < len(data) else None
            else:
                enum_type, p = _rl_read_str(data, pos)
                end = p + value_size   # enum_type prefix not counted in value_size
                if enum_type == "None":
                    val = data[p] if p < len(data) else None
                else:
                    val, _ = _rl_read_str(data, p)
        except Exception:
            val = None
        return name, val, end

    elif type_name == "StructProperty":
        # In RL replay format, value_size covers only the struct DATA bytes —
        # the struct type-name string is written first and is NOT counted in
        # value_size.  Read the type name, then parse value_size bytes as a
        # standard property list (same as top-level or array items).
        try:
            struct_type, spos = _rl_read_str(data, pos)
        except Exception:
            struct_type = ""
            spos = pos
        struct_end = spos + value_size
        _dbg = (name == "PlayerID")   # only log the struct we're diagnosing
        if _dbg:
            print(f"[replay]   StructProp '{name}' type='{struct_type}' spos={spos} vsize={value_size} struct_end={struct_end}")
        _none_term = b'\x05\x00\x00\x00None\x00'
        inner = {}
        p = spos
        while p < struct_end:
            # Per-property try/except with resync — same pattern as ArrayProperty.
            # Prevents a single bad inner property (e.g. PSN binary NpId struct)
            # from aborting the entire struct and losing subsequent fields like Team.
            try:
                n2, v2, p = _rl_read_prop(data, p)
            except Exception as exc:
                idx = data.find(_none_term, p, struct_end)
                if idx != -1:
                    p = idx + len(_none_term)
                    if _dbg:
                        print(f"[replay]   StructProp '{name}' inner prop error '{exc}' — resynced to p={p}")
                    # If we're too close to the struct boundary to hold another
                    # valid property (name str header = 4 bytes + type str header
                    # = 4 bytes + size block = 8 bytes = 16 bytes minimum), stop.
                    if p > struct_end - 16:
                        break
                else:
                    if _dbg:
                        print(f"[replay]   StructProp '{name}' inner prop error '{exc}' — no resync, stopping")
                    break
                continue
            if n2 == "None":
                break
            inner[n2] = v2
        if _dbg:
            import binascii as _ba2
            print(f"[replay]   StructProp '{name}' done  struct_end={struct_end}  hex32_at_end={_ba2.hexlify(data[struct_end:struct_end+32]).decode()}")
        return name, inner, struct_end

    elif type_name == "ArrayProperty":
        # value_size covers [count:4] + all item bytes.
        # Parse items for the data we care about, but always finish at 'end'
        # so that unknown/drifted inner properties cannot corrupt the outer stream.
        count = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        import binascii as _ba
        # Byte pattern for the "None" property-list terminator — used to resync
        # after a parse failure so subsequent items can still be read.
        # The pattern is: length=5 (4 bytes LE) + "None\0" (5 bytes) = 9 bytes.
        _none_term = b'\x05\x00\x00\x00None\x00'
        _dbg_arr = (name == "PlayerStats")  # only log the array we're diagnosing
        items = []
        for i in range(count):
            item = {}
            item_start = pos
            while pos < end:
                try:
                    n, v, pos = _rl_read_prop(data, pos)
                except Exception as exc:
                    # A property inside this item failed to parse (binary struct
                    # content, unknown type, version quirk, etc.).
                    # Resync: scan for the next "None" item-terminator so the
                    # remaining items in the array can still be read.
                    #
                    # Two-pass strategy:
                    #   1. Search forward from pos (normal case — error at current spot).
                    #   2. If not found, search from item_start (covers the case where pos
                    #      overshot the terminator due to a wrong value_size read earlier).
                    idx = data.find(_none_term, pos, end)
                    if idx == -1:
                        idx = data.find(_none_term, item_start, end)
                    if idx != -1:
                        pos = idx + len(_none_term)   # land just after the terminator
                        if _dbg_arr:
                            print(f"[replay]  {name}[{i}] prop error '{exc}' — resynced to pos={pos}")
                    else:
                        # Genuinely no terminator anywhere in the array — bail
                        pos = end
                        if _dbg_arr:
                            print(f"[replay]  {name}[{i}] prop error '{exc}' — no resync target, snapping array to {end}")
                    break
                if n == "None":
                    break
                # Flatten StructProperty dicts so callers can do item.get("Name")
                # instead of item["PlayerID"]["Name"].
                if isinstance(v, dict) and v:
                    item.update(v)
                else:
                    item[n] = v
            if _dbg_arr:
                print(f"[replay]  {name}[{i}] item_start={item_start}  parsed keys={list(item.keys())}")
            items.append(item)
        return name, items, end

    else:
        # Unknown or unimplemented type — skip by value_size.
        return name, None, end


def _parse_replay_data(data: bytes) -> dict:
    """
    Parse a Rocket League .replay binary blob and return a result dict.
    Raises ValueError with a descriptive message on failure so callers
    can surface the reason rather than silently returning None.
    """
    if len(data) < 16:
        raise ValueError(f"File too short ({len(data)} bytes) — not a .replay")

    # Bytes 0-3: header_size  4-7: crc  8-11: major  12-15: minor
    major = struct.unpack_from("<I", data, 8)[0]
    minor = struct.unpack_from("<I", data, 12)[0]

    # Expose major to _rl_read_prop so BoolProperty knows how many stream bytes to consume
    global _replay_major
    _replay_major = major

    # Net-version uint32 present when major >= 868 and minor >= 18
    pos = 20 if (major >= 868 and minor >= 18) else 16

    # print(f"[replay] major={major} minor={minor} starting pos={pos} total_bytes={len(data)}")

    # Every replay has a game_type label string here
    # (e.g. "TAGame.Replay_Soccar_TA") — skip it.
    game_type, pos = _rl_read_str(data, pos)
    # print(f"[replay] game_type='{game_type}' properties start at pos={pos}")

    result: dict = {
        "score0": None, "score1": None,
        "date": None, "match_type": None, "team_size": None,
        "match_guid": None, "replay_id": None,
        "players": []
    }
    _goal_players: dict[str, int] = {}   # name → team, for fallback population

    while pos < len(data):
        prop_start = pos
        try:
            name, value, pos = _rl_read_prop(data, prop_start)
        except Exception as exc:
            # A top-level property failed (bad string length, EOF, version quirk, etc.)
            # Try to skip over it using the size block so subsequent properties
            # (including PlayerStats) are still reachable.
            print(f"[replay] top-level prop error at pos={prop_start}: {exc}")
            try:
                _, p1 = _rl_read_str(data, prop_start)   # re-read property name
                _, p2 = _rl_read_str(data, p1)           # re-read type name
                if p2 + 8 <= len(data):
                    skip_size = struct.unpack_from("<I", data, p2)[0]
                    pos = p2 + 8 + skip_size
                    print(f"[replay]   skipped bad property, resuming at pos={pos}")
                    continue
            except Exception:
                pass
            print(f"[replay]   could not determine property size, stopping parse early")
            break
        if name == "None":
            break
        if name == "Team0Score" and value is not None:
            result["score0"] = int(value)
        elif name == "Team1Score" and value is not None:
            result["score1"] = int(value)
        elif name == "Date" and isinstance(value, str):
            result["date"] = value
        elif name == "MatchType" and isinstance(value, str):
            result["match_type"] = value
        elif name == "TeamSize" and value is not None:
            result["team_size"] = int(value)
        elif name == "MatchGUID" and isinstance(value, str):
            result["match_guid"] = value
        elif name == "Id" and isinstance(value, str):
            result["replay_id"] = value
        elif name == "Goals" and isinstance(value, list):
            # Build a name→team map from goal events as a fallback for PlayerStats
            for g in value:
                if not isinstance(g, dict):
                    continue
                gname = g.get("PlayerName", "")
                gteam = g.get("PlayerTeam", -1)
                if gname and gname not in _goal_players:
                    _goal_players[gname] = int(gteam)
        elif name == "PlayerStats" and isinstance(value, list):
            for entry in value:
                if not isinstance(entry, dict):
                    continue
                pname = entry.get("Name", "")
                if not pname:
                    # Item parsed but has no Name — log so we can diagnose why
                    print(f"[replay] PlayerStats item skipped (no Name), keys={list(entry.keys())}")
                    continue
                platform    = str(entry.get("Platform", ""))
                epic_id     = str(entry.get("EpicAccountId", "")) or ""
                # Determine platform from EpicAccountId or Platform field
                is_epic = ("epic" in platform.lower()
                           or bool(epic_id)
                           or int(entry.get("OnlineID", 0)) == 0)
                result["players"].append({
                    "name":      pname,
                    "online_id": int(entry.get("OnlineID", 0)),
                    "epic_id":   epic_id,   # full Epic account UUID, "" for Steam players
                    "team":      int(entry.get("Team", -1)),
                    "platform":  "Epic" if is_epic else platform,
                    "is_epic":   is_epic,
                })

    # Supplement / correct using goal-scorer data.
    # Goals items reliably carry (PlayerName, PlayerTeam) for every goal event.
    # Two passes:
    #   1. Fix Team=-1 for players already in the list (happens when the outer
    #      PlayerStats item properties after PlayerID struct failed to parse).
    #   2. Add any goal-scorer who is completely missing from PlayerStats
    #      (covers players who scored 0 goals themselves but were missed by parse).
    for p in result["players"]:
        if p["team"] == -1 and p["name"] in _goal_players:
            p["team"] = _goal_players[p["name"]]
    known_names = {p["name"] for p in result["players"]}
    for gname, gteam in _goal_players.items():
        if gname not in known_names:
            result["players"].append({
                "name":      gname,
                "online_id": 0,
                "team":      gteam,
                "platform":  "",
                "is_epic":   False,
            })
            known_names.add(gname)

    return result


def parse_replay_header(path: str) -> dict | None:
    """Parse a .replay file by path. Returns None on failure."""
    try:
        with open(path, "rb") as f:
            data = f.read(131072)   # 128 KB covers any RL replay header
        result = _parse_replay_data(data)
        print(f"[replay] parsed ok — players={len(result['players'])} match_type={result['match_type']}")
        return result
    except Exception as exc:
        print(f"[replay] parse error for {path}: {exc}")
        return None


def verify_replay(parsed: dict, m: dict, player_info: dict) -> dict:
    """
    Cross-check a parsed replay against the expected match state.

    player_info: {player_id -> {"steam_id": int|None, "display_name": str}}
      steam_id    — Steam64 ID from the player DB, or None for Epic players.
      display_name — rl_display_name stored at registration, used as Epic fallback.

    Team mapping strategy (in order):
      1. Steam ID anchor — any Steam player establishes the team mapping.
      2. Epic display name fallback — if no Steam players exist, match each
         replay player's Name against registered rl_display_name values.
         A name that matches exactly one registered player is used as an anchor.
         Ambiguous names (matched to >1 player) are skipped.

    Returns:
        {"verdict": "verified",     "winner_ids": [...], "loser_ids": [...],
                                    "score0": int, "score1": int}
        {"verdict": "unverifiable", "reason": str}
        {"verdict": "conflict",     "reason": str}
    """
    score0 = parsed.get("score0")
    score1 = parsed.get("score1")

    # ── 1. Scores must be present and decisive ────────────────────────────────
    if score0 is None or score1 is None:
        return {"verdict": "unverifiable", "reason": "Scores not found in replay"}
    if score0 == score1:
        return {"verdict": "unverifiable",
                "reason": "Scores are level — cannot determine winner from replay"}

    # ── 2. Date must be within 3 hours of lobby_ready_at ─────────────────────
    date_str       = parsed.get("date")
    lobby_ready_at = m.get("lobby_ready_at")
    if date_str and lobby_ready_at:
        try:
            replay_dt = datetime.strptime(date_str, "%Y-%m-%d %H:%M:%S") \
                                 .replace(tzinfo=timezone.utc)
            if abs(replay_dt.timestamp() - lobby_ready_at) > 10800:
                return {
                    "verdict": "conflict",
                    "reason":  f"Replay date ({date_str} UTC) is more than 3 hours "
                               f"from match start — wrong replay submitted",
                }
        except ValueError:
            pass

    # ── 3. Build lookup maps from the replay ──────────────────────────────────
    replay_players  = parsed.get("players", [])
    steam_in_replay = {
        p["online_id"]: p for p in replay_players if p.get("online_id", 0) > 0
    }
    epic_in_replay = {
        p["epic_id"]: p for p in replay_players if p.get("epic_id")
    }

    all_pids = m.get("players", [])
    team_a   = m.get("team_a", [])
    team_b   = m.get("team_b", [])

    # ── 4. Establish team mapping — Steam ID first ────────────────────────────
    # team_map[replay_team_0_or_1] = "team_a" | "team_b"
    team_map: dict[int, str] = {}

    for pid in all_pids:
        steam_id = (player_info.get(pid) or {}).get("steam_id")
        if not steam_id or steam_id not in steam_in_replay:
            continue

        rp           = steam_in_replay[steam_id]
        replay_team  = rp.get("team", -1)
        if replay_team not in (0, 1):
            continue

        server_team = "team_a" if pid in team_a else "team_b"
        other_team  = "team_b" if server_team == "team_a" else "team_a"
        team_map    = {replay_team: server_team, 1 - replay_team: other_team}
        break

    # ── 4b. Epic ID fallback — player_id IS the Epic account ID for Epic players
    if not team_map and epic_in_replay:
        for pid in all_pids:
            if pid not in epic_in_replay:
                continue

            rp          = epic_in_replay[pid]
            replay_team = rp.get("team", -1)
            if replay_team not in (0, 1):
                continue

            server_team = "team_a" if pid in team_a else "team_b"
            other_team  = "team_b" if server_team == "team_a" else "team_a"
            team_map    = {replay_team: server_team, 1 - replay_team: other_team}
            print(f"[replay] Epic ID anchor: {pid[:8]}… → {server_team}")
            break

    if not team_map:
        return {
            "verdict": "unverifiable",
            "reason":  "Could not match any player to the replay "
                       "(no Steam IDs matched and no Epic IDs matched).",
        }

    # ── 5. Cross-check all players against the established mapping ────────────
    for pid in all_pids:
        steam_id = (player_info.get(pid) or {}).get("steam_id")
        server_team = "team_a" if pid in team_a else "team_b"

        if steam_id:
            # Steam player — must appear in replay
            if steam_id not in steam_in_replay:
                return {
                    "verdict": "conflict",
                    "reason":  f"Steam player (…{str(steam_id)[-6:]}) not found in replay",
                }
            rp          = steam_in_replay[steam_id]
            replay_team = rp.get("team", -1)
            if replay_team in (0, 1) and team_map.get(replay_team) != server_team:
                return {
                    "verdict": "conflict",
                    "reason":  "A Steam player was on the wrong team in the replay",
                }
        elif pid in epic_in_replay:
            # Epic player with ID in replay — verify team
            rp          = epic_in_replay[pid]
            replay_team = rp.get("team", -1)
            if replay_team in (0, 1) and team_map.get(replay_team) != server_team:
                return {
                    "verdict": "conflict",
                    "reason":  "An Epic player was on the wrong team in the replay",
                }

    # ── 6. Determine winner using the established mapping ─────────────────────
    winning_replay_team = 0 if score0 > score1 else 1
    winning_server_team = team_map[winning_replay_team]
    winner_ids = team_a if winning_server_team == "team_a" else team_b
    loser_ids  = team_b if winning_server_team == "team_a" else team_a

    return {
        "verdict":    "verified",
        "winner_ids": winner_ids,
        "loser_ids":  loser_ids,
        "score0":     score0,
        "score1":     score1,
    }


# ── models ─────────────────────────────────────────────────────────────────────
class JoinRequest(BaseModel):
    player_id:       str
    system_id:       str = ""
    username:        str = ""
    rl_display_name: str = ""  # updated on every queue join
    region:          str
    mode:            str

class RegisterRequest(BaseModel):
    player_id:         str
    system_id:       str = ""
    username:        str = ""
    rl_display_name: str = ""  # in-game Steam/Epic name, used for replay verification
    platform:        str = ""  # "Steam" or "Epic"

class LeaveRequest(BaseModel):
    player_id: str

class AcceptRequest(BaseModel):
    player_id:  str
    match_id: str

class MatchResultRequest(BaseModel):
    player_id:  str
    match_id: str
    outcome:  str   # "win", "loss", or "draw"

class ForfeitRequest(BaseModel):
    player_id:  str
    match_id: str

# ── startup ────────────────────────────────────────────────────────────────────
@app.on_event("startup")
async def startup():
    load_blocklist()
    init_db()
    # Pre-create replay storage folders so they're ready before the first upload
    for _sub in ("matches", "test-tool"):
        os.makedirs(os.path.join(os.path.dirname(__file__), "replays", _sub),
                    exist_ok=True)
    asyncio.create_task(cleanup_loop())
    asyncio.create_task(run_matchmaker())

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
                if now - p.get("last_heartbeat", p["joined_at"]) < HEARTBEAT_STALE_SECS
            ]
            # (stale entries removed above — player_match is cleaned up in cancel_match)
            removed = before - len(queues[key])
            if removed:
                print(f"[cleanup] removed {removed} stale player(s) from {key}")

        # ── Match cleanup ─────────────────────────────────────────────────────
        for mid in list(matches.keys()):
            m = matches[mid]
            age = now - m["created_at"]

            # 1. Acceptance window expired (60s)
            if age > 60 and len(m["accepted"]) < len(m["players"]):
                # Players who accepted are victims → get queue priority.
                # Players who never responded get a no-response strike.
                # If they hit 3 strikes they are dequeued; nobody gets priority
                # for ghosting — only for actively being wronged by someone else.
                responders    = set(m["accepted"])
                non_responders = [pid for pid in m["players"] if pid not in responders]

                for pid in responders:
                    victim_priority.add(pid)

                for pid in non_responders:
                    _penalize_no_response(pid)   # dequeues if limit reached

                reason = "Not all players responded in time."
                for pid in m["players"]:
                    player_match.pop(pid, None)
                    _push(pid, "match_cancelled", {
                        "reason":    reason,
                        "priority":  pid in responders,
                    })
                matches.pop(mid, None)
                cancelled_matches[mid] = {"reason": reason, "at": now}
                print(f"[cancel] {mid}: {reason} "
                      f"(responders={list(responders)}, "
                      f"no_response={non_responders})")
                continue

            # 2. Host never created the lobby (3 min after all accepted)
            all_acc_at = m.get("all_accepted_at")
            if all_acc_at and not m.get("lobby_ready") \
                    and (now - all_acc_at) > LOBBY_CREATE_TIMEOUT:
                host_id = m["host_id"]
                others  = [pid for pid in m["players"] if pid != host_id]

                # Host gets a no-response strike and is dequeued
                _penalize_no_response(host_id)

                # Everyone else gets priority — they did nothing wrong
                for pid in others:
                    victim_priority.add(pid)

                # Push tailored messages
                _push(host_id, "match_cancelled", {
                    "reason":   "You did not create the lobby in time. "
                                "You have been removed from the queue.",
                    "priority": False,
                })
                for pid in others:
                    _push(pid, "match_cancelled", {
                        "reason":   "Host did not create the lobby in time.",
                        "priority": True,
                    })

                reason = "Host did not create the lobby in time."
                for pid in m["players"]:
                    player_match.pop(pid, None)
                matches.pop(mid, None)
                cancelled_matches[mid] = {"reason": reason, "at": now}
                print(f"[cancel] {mid}: {reason} (host={host_id})")
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


# ── background matchmaker ──────────────────────────────────────────────────────
def _try_form_match(key: str, now: float) -> bool:
    """Try to form one match from the given queue key.

    Iterates every player as a potential anchor (top = highest priority / longest
    wait).  Each anchor's MMR search window expands based on how long they have
    been waiting.  The first anchor that can fill a lobby is used; remaining
    players stay in the queue.

    Returns True if a match was formed (caller should re-run immediately),
    False if no match is possible right now.
    """
    queue  = queues.get(key, [])
    mode   = key.split("_", 1)[1]          # "EU_2s" → "2s"
    needed = PLAYERS_NEEDED.get(mode, 2)

    if len(queue) < needed:
        return False

    for i, anchor in enumerate(queue):
        wait   = now - anchor["joined_at"]
        steps  = int(wait / MMR_RANGE_EXPAND_S)
        window = min(MMR_RANGE_START + steps * MMR_RANGE_STEP, MMR_RANGE_MAX)

        # Candidates: other players whose MMR falls within the anchor's window
        candidates = [
            p for j, p in enumerate(queue)
            if j != i and abs(p["mmr"] - anchor["mmr"]) <= window
        ]

        if len(candidates) < needed - 1:
            continue    # this anchor can't fill a lobby yet — try the next

        # Take anchor + first (needed-1) candidates (queue is priority-sorted)
        match_players = [anchor] + candidates[:needed - 1]

        # Reject if two slots share the same real account (alt-account guard)
        player_ids = [p["player_id"] for p in match_players]
        if len(player_ids) != len(set(player_ids)):
            continue

        # Remove matched players from the queue
        matched_ids = {p["player_id"] for p in match_players}
        queues[key] = [p for p in queue if p["player_id"] not in matched_ids]

        # Build match — anchor is the host (highest priority / longest wait)
        match_id       = f"{key}_{rand_str(6)}"
        lobby_name     = f"RLCQ_{rand_str(4)}"
        lobby_password = rand_str(6)
        team_a, team_b = make_balanced_teams(match_players)

        match_info = {
            "match_id":       match_id,
            "region":         anchor["region"],
            "mode":           mode,
            "players":        [p["player_id"] for p in match_players],
            "team_a":         team_a,
            "team_b":         team_b,
            "host_id":        anchor["player_id"],
            "lobby_name":     lobby_name,
            "lobby_password": lobby_password,
            "accepted":       [],
            "created_at":     now,
            "lobby_ready":    False,
            "lobby_ready_at": None,
        }
        matches[match_id] = match_info
        for p in match_players:
            player_match[p["player_id"]] = match_id

        print(f"[match] formed {match_id} "
              f"({len(match_players)}p, window=±{window}) "
              f"teams A={team_a} B={team_b}")
        return match_id  # caller spawns _push_match_when_ready(match_id)

    return None          # no anchor could fill a lobby


async def _push_match_when_ready(match_id: str, sse_timeout: float = 3.0):
    """Wait until every player in the match has an open SSE connection, then
    push match_found to all of them simultaneously.

    This guarantees the event is delivered to open connections rather than
    racing against clients that are still opening their stream.  The
    replay-on-connect logic in /queue/events acts as a fallback for any
    player whose connection isn't established within sse_timeout seconds.
    """
    m = matches.get(match_id)
    if not m:
        return
    players  = list(m["players"])
    deadline = asyncio.get_event_loop().time() + sse_timeout

    while asyncio.get_event_loop().time() < deadline:
        if all(pid in sse_queues for pid in players):
            break
        await asyncio.sleep(0.05)   # re-check every 50 ms

    connected     = [pid for pid in players if pid in sse_queues]
    not_connected = [pid for pid in players if pid not in sse_queues]

    if not_connected:
        print(f"[match] {match_id}: {len(not_connected)} player(s) not yet on SSE "
              f"after {sse_timeout}s — pushing anyway; replay-on-connect will cover them")

    # Push to all — those not connected yet will get it via replay-on-connect
    # when their SSE stream opens.
    for pid in players:
        if match_id in matches:   # guard: match wasn't cancelled during the wait
            _push(pid, "match_found", _match_found_payload(pid, match_id))

    print(f"[match] {match_id}: match_found pushed "
          f"(live={len(connected)}, pending-replay={len(not_connected)})")


def _next_expansion_in() -> float:
    """Seconds until the earliest queued player's MMR window next expands.

    Used as the matchmaker's sleep timeout when no match was possible —
    there is no point waking up sooner since nothing will have changed.
    Falls back to 30s if the queue is empty.
    """
    now     = time.time()
    soonest = 30.0
    for queue in queues.values():
        for p in queue:
            wait      = now - p["joined_at"]
            next_step = MMR_RANGE_EXPAND_S - (wait % MMR_RANGE_EXPAND_S)
            soonest   = min(soonest, next_step)
    return max(0.5, soonest)


async def run_matchmaker():
    """Event-driven matchmaker — zero CPU when the queue is idle.

    Wakes up for exactly two reasons:
      1. A player joined  → _match_trigger fired by queue_join
      2. A range expansion → timeout calculated by _next_expansion_in()

    After waking, drains every queue completely before sleeping again.
    """
    while True:
        timeout = _next_expansion_in()
        triggered = False
        try:
            await asyncio.wait_for(_match_trigger.wait(), timeout=timeout)
            triggered = True
        except asyncio.TimeoutError:
            pass                    # range expansion — fall through and re-check
        _match_trigger.clear()

        now = time.time()
        for key in list(queues.keys()):
            while True:
                mid = _try_form_match(key, now)
                if not mid:
                    break
                # Spawn a task that waits for all players' SSE connections
                # to be open before pushing match_found, rather than firing
                # blindly into the void and hoping the streams are ready.
                asyncio.create_task(_push_match_when_ready(mid))


# ── replay: parse from raw bytes (used by test page and future auto-link) ────
def parse_replay_bytes(data: bytes) -> dict:
    """
    Same as parse_replay_header() but operates on bytes directly — no temp file.
    Raises ValueError with a descriptive message on failure.
    """
    return _parse_replay_data(data)


# ── routes: health + stats ─────────────────────────────────────────────────────
@app.get("/health")
def health():
    return {"status": "ok"}

@app.get("/debug/state")
def debug_state():
    """Dev-only endpoint — shows live queue and match state."""
    return {
        "queues": {k: [{"pid": p["player_id"], "mmr": p["mmr"]} for p in v]
                   for k, v in queues.items() if v},
        "matches": {mid: {
            "players":        m["players"],
            "accepted":       m["accepted"],
            "team_a":         m.get("team_a", []),
            "team_b":         m.get("team_b", []),
            "lobby_ready":    m.get("lobby_ready", False),
            "awaiting_replay": m.get("awaiting_replay", False),
            "replay_uploads": m.get("replay_uploads", []),
            "replay_results": list(m.get("replay_results", {}).keys()),
        } for mid, m in matches.items()},
    }

@app.get("/queue/stats")
def queue_stats():
    buckets = {key: len(players) for key, players in queues.items() if players}
    buckets["total_searching"] = sum(buckets.values())
    return buckets

@app.get("/queue/status")
def queue_status():
    result = {}
    for key, players in queues.items():
        if players:
            result[key] = len(players)
    return result  # e.g. {"NAE_2s": 3, "EU_1s": 1}


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
  header nav { margin-left: auto; display: flex; gap: 6px; }
  header nav a {
    color: #aaa; text-decoration: none; font-size: 0.88rem;
    padding: 6px 14px; border-radius: 6px; border: 1px solid transparent;
    transition: color .15s, border-color .15s;
  }
  header nav a:hover { color: #5fa8ff; border-color: #2a4a7f; }
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
  th { text-align: left; padding: 8px 12px; color: #5fa8ff; font-weight: 600; cursor: pointer; user-select: none; white-space: nowrap; }
  th:hover { color: #8fc8ff; }
  th.sort-asc::after  { content: ' ▲'; font-size: 0.7rem; }
  th.sort-desc::after { content: ' ▼'; font-size: 0.7rem; }
  td { padding: 9px 12px; border-bottom: 1px solid #1e1e1e; }
  tbody tr { cursor: pointer; transition: background .1s; }
  tbody tr:hover td { background: #1a1a1a; }
  tbody tr.my-row td { border-left: 3px solid #5fa8ff; background: #0f1a2e; }
  tbody tr.my-row:hover td { background: #162236; }
  .rank-1 td:first-child { color: #ffd700; font-weight: bold; }
  .rank-2 td:first-child { color: #c0c0c0; font-weight: bold; }
  .rank-3 td:first-child { color: #cd7f32; font-weight: bold; }
  .mmr-val { color: #5fa8ff; font-weight: 600; }
  .win { color: #4cff91; } .loss { color: #ff5f5f; }
  .empty { color: #555; text-align: center; padding: 30px 0; cursor: default; }
  #no-search { display: none; color: #555; text-align: center; padding: 24px 0; }
  /* queue status pills */
  .q-pill {
    display: inline-flex; align-items: center; padding: 4px 12px; border-radius: 20px;
    background: #1a1a2e; border: 1px solid #2a4a7f; color: #5fa8ff;
    font-size: 0.8rem; white-space: nowrap;
  }
  .q-pill.empty { background: #1a1a1a; border-color: #333; color: #555; }
</style>
</head>
<body>
<header>
  <h1>⚡ RL Custom Queue</h1>
  <nav>
    <a href="/">Leaderboard</a>
    <a href="/replay-test">Replay Tool</a>
    <a href="/admin">Admin</a>
  </nav>
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
        <input id="lb-search" type="text" placeholder="Search player…" oninput="filterAndRender()">
      </div>
    </div>
    <div id="queue-status" style="margin-bottom:14px; display:flex; gap:8px; flex-wrap:wrap;"></div>
    <table>
      <thead>
        <tr>
          <th id="th-rank"    onclick="sortBy('rank')">#</th>
          <th id="th-player"  onclick="sortBy('player')">Player</th>
          <th id="th-rating"  onclick="sortBy('mmr')">2s Rating</th>
          <th id="th-wins"    onclick="sortBy('wins')">W</th>
          <th id="th-losses"  onclick="sortBy('losses')">L</th>
          <th id="th-winpct"  onclick="sortBy('winpct')">Win %</th>
          <th id="th-plat"    onclick="sortBy('platform')">Platform</th>
        </tr>
      </thead>
      <tbody id="lb-body">
        <tr><td colspan="7" class="empty">Loading…</td></tr>
      </tbody>
    </table>
    <div id="no-search">No players match your search.</div>
  </div>
</div>
<script>
let lbData = [], currentMode = '2s', sortCol = 'rank', sortDir = 'asc';
const myPid = new URLSearchParams(window.location.search).get('pid') || '';

// Pre-fill search from ?q= param (used by replay tool name links)
(function() {
  const q = new URLSearchParams(window.location.search).get('q');
  if (q) {
    document.getElementById('lb-search').value = q;
  }
})();

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
      return `<div class="profile-card" style="cursor:pointer" onclick="location.href='/player/${p.player_id}'">
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

function sortBy(col) {
  if (sortCol === col) {
    sortDir = sortDir === 'asc' ? 'desc' : 'asc';
  } else {
    sortCol = col;
    sortDir = col === 'rank' ? 'asc' : 'desc';
  }
  updateSortHeaders();
  filterAndRender();
}

function updateSortHeaders() {
  const colMap = { rank: 'th-rank', player: 'th-player', mmr: 'th-rating',
                   wins: 'th-wins', losses: 'th-losses', winpct: 'th-winpct', platform: 'th-plat' };
  Object.values(colMap).forEach(id => {
    const el = document.getElementById(id);
    if (el) { el.classList.remove('sort-asc', 'sort-desc'); }
  });
  const active = document.getElementById(colMap[sortCol]);
  if (active) active.classList.add(sortDir === 'asc' ? 'sort-asc' : 'sort-desc');
}

function filterAndRender() {
  const query = document.getElementById('lb-search').value.trim().toLowerCase();
  const noSearch = document.getElementById('no-search');
  if (!lbData.length) {
    document.getElementById('lb-body').innerHTML = '<tr><td colspan="7" class="empty">No players yet.</td></tr>';
    noSearch.style.display = 'none'; return;
  }
  let rows = query ? lbData.filter(p => (p.username || p.player_id).toLowerCase().includes(query)) : lbData.slice();
  noSearch.style.display = (rows.length === 0 && query) ? 'block' : 'none';

  // Sort
  const mmrKey = 'mmr_' + currentMode;
  rows.sort((a, b) => {
    let av, bv;
    if (sortCol === 'rank')     { av = a.rank;  bv = b.rank; }
    else if (sortCol === 'player')   { av = (a.username||'').toLowerCase(); bv = (b.username||'').toLowerCase(); }
    else if (sortCol === 'mmr')      { av = a[mmrKey]; bv = b[mmrKey]; }
    else if (sortCol === 'wins')     { av = a.wins;  bv = b.wins; }
    else if (sortCol === 'losses')   { av = a.losses; bv = b.losses; }
    else if (sortCol === 'winpct')   {
      const ta = a.wins + a.losses, tb = b.wins + b.losses;
      av = ta ? a.wins / ta : 0; bv = tb ? b.wins / tb : 0;
    }
    else if (sortCol === 'platform') { av = (a.platform||'').toLowerCase(); bv = (b.platform||'').toLowerCase(); }
    else { av = a.rank; bv = b.rank; }
    if (av < bv) return sortDir === 'asc' ? -1 :  1;
    if (av > bv) return sortDir === 'asc' ?  1 : -1;
    return 0;
  });

  document.getElementById('lb-body').innerHTML = rows.map((p, i) => {
    const total = p.wins + p.losses;
    const winpct = total ? Math.round(p.wins / total * 100) + '%' : '—';
    const name = p.username || '<span style="color:#555">' + p.player_id.slice(0,14) + '…</span>';
    const rowCls = (p.rank <= 3 ? 'rank-' + p.rank : '') + (p.player_id === myPid ? ' my-row' : '');
    const platform = p.platform === 'Steam' ? '<span style="color:#6dcff6">Steam</span>'
                   : p.platform === 'Epic'  ? '<span style="color:#9b7fd4">Epic</span>'
                   : '<span style="color:#555">—</span>';
    const pid = p.player_id;
    return `<tr class="${rowCls.trim()}" onclick="location.href='/player/${pid}'" title="View ${p.username || pid}'s profile">
      <td>${p.rank}</td><td>${name}</td><td class="mmr-val">${p[mmrKey]}</td>
      <td class="win">${p.wins}</td><td class="loss">${p.losses}</td>
      <td>${winpct}</td><td>${platform}</td>
    </tr>`;
  }).join('');
}

// Legacy alias kept for any external callers
function filterLB() { filterAndRender(); }
function renderRows(rows, mode) { filterAndRender(); }

async function loadLB(mode) {
  currentMode = mode;
  document.getElementById('th-rating').textContent = mode + ' Rating';
  // reset sort arrows to show current
  updateSortHeaders();
  document.getElementById('lb-body').innerHTML = '<tr><td colspan="7" class="empty">Loading…</td></tr>';
  try {
    const r = await fetch('/leaderboard/' + mode);
    lbData = await r.json();
    filterAndRender();
  } catch(e) {
    document.getElementById('lb-body').innerHTML = '<tr><td colspan="7" class="empty">Failed to load.</td></tr>';
  }
}

async function updateQueueStatus() {
  const el = document.getElementById('queue-status');
  try {
    const r = await fetch('/queue/status');
    const data = await r.json();
    const keys = Object.keys(data);
    if (!keys.length) {
      el.innerHTML = '<span class="q-pill empty">Queue empty</span>';
    } else {
      el.innerHTML = keys.map(k => {
        const [region, mode] = k.split('_');
        const label = `${region} ${mode}: ${data[k]} in queue`;
        return `<span class="q-pill">${label}</span>`;
      }).join('');
    }
  } catch(e) {
    el.innerHTML = '<span class="q-pill empty">Queue unavailable</span>';
  }
}

updateSortHeaders();
loadLB('2s');
updateQueueStatus();
setInterval(updateQueueStatus, 30000);
</script>
</body>
</html>""")

# ── routes: account ────────────────────────────────────────────────────────────
@app.post("/account/register")
def account_register(req: RegisterRequest):
    if req.username:
        validate_username(req.username)
    ensure_player(req.player_id, req.system_id, req.username)
    update_rl_display_name(req.player_id, req.rl_display_name)
    update_platform(req.player_id, req.platform)
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
@app.get("/player/{player_id}/active_match")
def player_active_match(player_id: str):
    """Returns full match info if the player is currently in a match.
    Used by the plugin to recover state after a missed match_found SSE."""
    mid = player_match.get(player_id)
    if not mid or mid not in matches:
        return {"status": "none"}
    m = matches[mid]
    is_host  = m.get("host_id") == player_id
    team_idx = 0 if player_id in m.get("team_a", []) else 1
    return {
        "status":       "in_match",
        "match_id":     mid,
        "lobby_name":   m.get("lobby_name", ""),
        "lobby_password": m.get("lobby_password", ""),
        "is_host":      is_host,
        "team":         team_idx,
        "mode":         m.get("mode", ""),
        "accepted":     player_id in m.get("accepted", []),
        "all_accepted": len(m.get("accepted", [])) >= len(m.get("players", [])),
        "lobby_ready":  m.get("lobby_ready", False),
    }

@app.post("/queue/join")
def queue_join(req: JoinRequest):
    # Keep in-game display name fresh — used for replay display and as default leaderboard name
    update_rl_display_name(req.player_id, req.rl_display_name)

    existing_mid = player_match.get(req.player_id)
    if existing_mid and existing_mid in matches:
        # Return match info so the plugin can recover instead of just showing an error
        m = matches[existing_mid]
        is_host  = m.get("host_id") == req.player_id
        team_idx = 0 if req.player_id in m.get("team_a", []) else 1
        raise HTTPException(409, detail={
            "error":          "already_in_match",
            "match_id":       existing_mid,
            "lobby_name":     m.get("lobby_name", ""),
            "lobby_password": m.get("lobby_password", ""),
            "is_host":        is_host,
            "team":           team_idx,
            "mode":           m.get("mode", ""),
        })

    ensure_player(req.player_id, req.system_id, req.username)
    if req.system_id:
        system_real_map[req.system_id] = req.player_id

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

    # No-response ban check (failed to accept/decline, or host didn't create lobby)
    recent_nr = [t for t in no_response_log.get(req.player_id, [])
                 if now_check - t < NO_RESPONSE_WINDOW_SECS]
    no_response_log[req.player_id] = recent_nr
    if len(recent_nr) >= NO_RESPONSE_MAX:
        wait = int(NO_RESPONSE_BAN_SECS - (now_check - min(recent_nr)))
        if wait > 0:
            raise HTTPException(429,
                f"Too many missed matches. Wait {wait}s before queuing again.")

    priority = 100 if req.player_id in victim_priority else 50
    victim_priority.discard(req.player_id)

    # Cache MMR at join time so the matchmaker never needs to hit the DB
    mmr_val = get_mmr(req.player_id, req.mode)

    key = f"{req.region}_{req.mode}"
    if key not in queues:
        queues[key] = []

    # Remove duplicates (same player_id)
    queues[key] = [
        p for p in queues[key]
        if p["player_id"] != req.player_id
    ]
    now = time.time()
    queues[key].append({
        "player_id":        req.player_id,
        "system_id":      req.system_id,
        "region":         req.region,
        "mode":           req.mode,
        "mmr":            mmr_val,          # cached — no DB hit during matchmaking
        "joined_at":      now,
        "last_heartbeat": now,
        "priority":       priority,
    })
    # Sort: priority desc, then joined_at asc (FIFO within same priority)
    queues[key].sort(key=lambda p: (-p["priority"], p["joined_at"]))

    # Wake the matchmaker immediately — no need to wait up to 1s
    if _loop:
        _loop.call_soon_threadsafe(_match_trigger.set)

    pos = next((i + 1 for i, p in enumerate(queues[key])
                if p["player_id"] == req.player_id), 0)
    return {"status": "queued", "position": pos}


@app.post("/queue/leave")
def queue_leave(req: LeaveRequest):
    for key in list(queues.keys()):
        queues[key] = [p for p in queues[key] if p["player_id"] != req.player_id]
    player_match.pop(req.player_id, None)
    return {"status": "left"}


HEARTBEAT_STALE_SECS = 180   # 3 × 60s poll cycles before player is removed

def _match_found_payload(player_id: str, mid: str) -> dict:
    m = matches[mid]
    team = 0 if player_id in m["team_a"] else 1
    return {
        "status":         "match_found",
        "match_id":       mid,
        "lobby_name":     m["lobby_name"],
        "lobby_password": m["lobby_password"],
        "is_host":        m["host_id"] == player_id,
        "mode":           m["mode"],
        "region":         m["region"],
        "team":           team,
    }

@app.get("/queue/events/{player_id}")
async def queue_events(player_id: str, request: Request):
    """Persistent SSE stream for a queued/matched player.
    Server pushes: match_found, player_accepted, all_accepted,
                   match_cancelled, lobby_ready.
    Client sends a separate POST /queue/heartbeat every 60s as keep-alive.

    On connect (or reconnect) we immediately replay any match state the player
    may have missed. This eliminates race conditions where the matchmaker fires
    before the SSE connection is open — no artificial delay needed in the
    matchmaker, and dropped connections automatically catch up on reconnect."""
    q: asyncio.Queue = asyncio.Queue()
    sse_queues[player_id] = q

    # ── Replay missed match state ─────────────────────────────────────────────
    # If this player is already in a match (connection opened after match_found
    # was pushed, or after a reconnect), replay the relevant events immediately
    # so they don't miss the accept/deny window.
    mid = player_match.get(player_id)
    if mid and mid in matches:
        m = matches[mid]
        payload = _match_found_payload(player_id, mid)
        payload["event"] = "match_found"
        q.put_nowait({"event": "match_found", "data": _json.dumps(payload)})
        # Replay subsequent events if they already happened
        if m.get("all_accepted"):
            q.put_nowait({"event": "all_accepted", "data": _json.dumps({
                "event":          "all_accepted",
                "accepted_count": len(m["players"]),
                "total":          len(m["players"]),
            })})
        if m.get("lobby_ready"):
            q.put_nowait({"event": "lobby_ready", "data": _json.dumps({
                "event":          "lobby_ready",
                "lobby_name":     m["lobby_name"],
                "lobby_password": m["lobby_password"],
            })})

    async def generator():
        try:
            while True:
                if await request.is_disconnected():
                    break
                try:
                    event = await asyncio.wait_for(q.get(), timeout=25.0)
                    yield event
                except asyncio.TimeoutError:
                    # Send a comment ping every 25s to keep the connection alive
                    yield {"event": "ping", "data": ""}
        finally:
            sse_queues.pop(player_id, None)

    # X-Accel-Buffering: no  → tells nginx/caddy/any reverse proxy not to buffer
    # this streaming response.  Without it, SSE events sit in the proxy's buffer
    # and never reach the client in real-time.
    return EventSourceResponse(generator(), headers={"X-Accel-Buffering": "no"})


@app.post("/queue/heartbeat")
def queue_heartbeat(req: LeaveRequest):
    """Keep-alive only — match notifications are pushed via SSE.
    Stamps last_heartbeat so the janitor knows the player is still online.
    Returns total_searching so the UI can show the live player count.
    Also returns in_match if the player has been moved to a match but their
    plugin hasn't received the SSE yet — lets the plugin recover without polling."""
    in_queue = False
    for key in queues:
        for p in queues[key]:
            if p["player_id"] == req.player_id:
                p["last_heartbeat"] = time.time()
                in_queue = True
                break
        if in_queue:
            break

    if not in_queue:
        # Check if the player is in an active match (missed the SSE match_found event)
        mid = player_match.get(req.player_id)
        if mid and mid in matches:
            m = matches[mid]
            is_host  = m.get("host_id") == req.player_id
            team_idx = 0 if req.player_id in m.get("team_a", []) else 1
            return {
                "status":         "in_match",
                "match_id":       mid,
                "lobby_name":     m.get("lobby_name", ""),
                "lobby_password": m.get("lobby_password", ""),
                "is_host":        is_host,
                "team":           team_idx,
                "mode":           m.get("mode", ""),
            }
        return {"status": "not_in_queue"}

    total_searching = sum(len(q) for q in queues.values())
    return {"status": "queued", "total_searching": total_searching}

# ── routes: match ──────────────────────────────────────────────────────────────
@app.post("/match/accept")
def match_accept(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    if req.player_id not in m["accepted"]:
        m["accepted"].append(req.player_id)

    accepted_count = len(m["accepted"])
    total          = len(m["players"])
    all_accepted   = accepted_count >= total

    # Push updated accept count to all players in the match
    _push_all(m["players"], "player_accepted", {
        "accepted_count": accepted_count,
        "total":          total,
        "player_id":        req.player_id,
    })

    if all_accepted and "all_accepted_at" not in m:
        m["all_accepted_at"] = time.time()
        _push_all(m["players"], "all_accepted", {"match_id": req.match_id})

    return {"status": "accepted", "accepted_count": accepted_count, "total": total}


@app.post("/match/decline")
def match_decline(req: AcceptRequest):
    m = matches.get(req.match_id)
    if not m:
        raise HTTPException(404, "Match not found")
    decline_log.setdefault(req.player_id, []).append(time.time())
    for pid in m["players"]:
        if pid != req.player_id:
            victim_priority.add(pid)
    cancel_match(req.match_id, "A player declined the match.", declining_id=req.player_id)
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
        "status":          "accepting",
        "accepted_count":  len(m["accepted"]),
        "total":           len(m["players"]),
        "all_accepted":    all_accepted,
        "time_remaining":  time_remaining,
        "lobby_ready":     m.get("lobby_ready", False),
        "draw_in":         draw_in,
        "forfeits_a":      len(forfeits.get("team_a", [])),
        "forfeits_b":      len(forfeits.get("team_b", [])),
        "awaiting_replay": m.get("awaiting_replay", False),
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
    # Notify everyone instantly — guests can now join the lobby
    _push_all(m["players"], "lobby_ready", {
        "match_id":      req.match_id,
        "lobby_name":    m["lobby_name"],
        "lobby_password": m["lobby_password"],
    })
    return {"status": "lobby_ready"}


MAX_REPLAY_BYTES = 50 * 1024 * 1024   # 50 MB
REPLAY_MIN_BYTES = 4096               # sanity floor

@app.api_route("/match/no_replay/{match_id}", methods=["GET", "POST"])
async def match_no_replay(match_id: str, request: Request, player_id: str = ""):
    """
    Called by the plugin when the replay watcher expired without finding a file.
    Records the player as 'no_replay' and triggers the resolution check.

    Response statuses:
      recorded         → logged, still waiting for others
      collection_closed → window already expired (idempotent — still OK)
    """
    if not player_id:
        return JSONResponse({"error": "player_id required"}, status_code=400)

    # Check collection record
    conn = sqlite3.connect(DB_PATH)
    coll = conn.execute(
        "SELECT collection_ends_at, replay_status FROM replay_collection WHERE match_id=?",
        (match_id,)
    ).fetchone()
    conn.close()

    if not coll:
        return {"status": "collection_closed", "note": "No collection record found"}

    # Upsert a no_replay submission (ignore if already submitted something)
    conn2 = sqlite3.connect(DB_PATH)
    conn2.execute(
        "INSERT OR IGNORE INTO replay_submissions "
        "(match_id, player_id, status, submitted_at) VALUES (?,?,'no_replay',?)",
        (match_id, player_id, int(time.time()))
    )
    conn2.commit()
    conn2.close()

    print(f"[replay] {match_id}: no_replay from {player_id}")
    _check_replay_resolution(match_id)
    return {"status": "recorded"}


@app.post("/match/upload_replay/{match_id}")
async def upload_replay_for_verification(match_id: str, request: Request,
                                          player_id: str = ""):
    """
    Called automatically by the plugin after a result is submitted.
    Accepts replays for both normal matches (post-award verification)
    and conflict matches (replay determines the winner).

    Response statuses:
      collected        → stored, waiting for more submissions
      auto_resolved    → conflict resolved by replay majority (match awarded now)
      verified         → replay confirms the button result; trust adjusted
      contradicts      → replay contradicts button result; trust penalised
      unverifiable     → replay parsed but teams/players could not be matched
      already_uploaded → duplicate upload from this player (idempotent)
      collection_closed → outside the collection window
    """
    if not player_id:
        return JSONResponse({"error": "player_id required"}, status_code=400)

    # ── check collection window ───────────────────────────────────────────────
    conn_c = sqlite3.connect(DB_PATH)
    coll = conn_c.execute(
        "SELECT collection_ends_at, button_outcome, button_winners, button_losers, "
        "       total_players, replay_status "
        "FROM replay_collection WHERE match_id=?",
        (match_id,)
    ).fetchone()
    conn_c.close()

    now = time.time()

    if not coll:
        # No collection record: might be a conflict match still in-memory
        m = matches.get(match_id)
        if not m:
            return {"status": "collection_closed"}
        # Legacy path: active conflict match, no collection row yet
        # Create one on the fly so the new logic can proceed
        resolved_at = now
        _init_replay_collection(
            match_id, m,
            button_outcome       = m.get("awaiting_replay") and "conflict" or "conflict",
            button_winners       = [],
            button_losers        = [],
            resolved_at          = resolved_at,
            player_ids_to_notify = [],
        )
        # Re-fetch
        conn_c2 = sqlite3.connect(DB_PATH)
        coll = conn_c2.execute(
            "SELECT collection_ends_at, button_outcome, button_winners, button_losers, "
            "       total_players, replay_status "
            "FROM replay_collection WHERE match_id=?",
            (match_id,)
        ).fetchone()
        conn_c2.close()
        if not coll:
            return {"status": "collection_closed"}

    (collection_ends_at, button_outcome, button_winners_str,
     button_losers_str, total_players, rc_status) = coll

    if now > collection_ends_at:
        return {"status": "collection_closed",
                "note":  "Upload window has expired"}

    if rc_status not in ("pending",):
        return {"status": "collection_closed",
                "note":  f"Collection already finalized ({rc_status})"}

    # ── deduplicate ───────────────────────────────────────────────────────────
    conn_d = sqlite3.connect(DB_PATH)
    existing = conn_d.execute(
        "SELECT status FROM replay_submissions WHERE match_id=? AND player_id=?",
        (match_id, player_id)
    ).fetchone()
    conn_d.close()

    if existing and existing[0] not in ("pending", "no_replay"):
        return {"status": "already_uploaded"}

    # ── read & validate raw bytes ─────────────────────────────────────────────
    replay_bytes = await request.body()
    if not replay_bytes or len(replay_bytes) < REPLAY_MIN_BYTES:
        # Count as 'unverifiable' so everyone-submitted check works
        conn_u = sqlite3.connect(DB_PATH)
        conn_u.execute(
            "INSERT OR REPLACE INTO replay_submissions "
            "(match_id, player_id, status, submitted_at) VALUES (?,?,'unverifiable',?)",
            (match_id, player_id, int(now))
        )
        conn_u.commit()
        conn_u.close()
        _check_replay_resolution(match_id)
        return {"status": "unverifiable", "reason": "Replay too small or empty"}

    if len(replay_bytes) > MAX_REPLAY_BYTES:
        conn_u = sqlite3.connect(DB_PATH)
        conn_u.execute(
            "INSERT OR REPLACE INTO replay_submissions "
            "(match_id, player_id, status, submitted_at) VALUES (?,?,'unverifiable',?)",
            (match_id, player_id, int(now))
        )
        conn_u.commit()
        conn_u.close()
        _check_replay_resolution(match_id)
        return {"status": "unverifiable", "reason": "Replay too large"}

    # ── save per-player copy ──────────────────────────────────────────────────
    replay_dir = os.path.join(os.path.dirname(__file__), "replays", "matches")
    os.makedirs(replay_dir, exist_ok=True)
    safe_pid    = safe_filename(player_id)
    replay_path = os.path.join(
        replay_dir, f"{safe_filename(match_id)}_{safe_pid}.replay"
    )
    with open(replay_path, "wb") as f:
        f.write(replay_bytes)

    # ── parse ─────────────────────────────────────────────────────────────────
    parsed = parse_replay_header(replay_path)
    if not parsed:
        conn_u = sqlite3.connect(DB_PATH)
        conn_u.execute(
            "INSERT OR REPLACE INTO replay_submissions "
            "(match_id, player_id, status, replay_path, submitted_at) "
            "VALUES (?,?,'unverifiable',?,?)",
            (match_id, player_id, replay_path, int(now))
        )
        conn_u.commit()
        conn_u.close()
        _check_replay_resolution(match_id)
        return {"status": "unverifiable", "reason": "Replay could not be parsed"}

    match_guid = parsed.get("match_guid") or ""

    # ── MatchGUID dedup: if another player already submitted the same match ───
    if match_guid:
        conn_g = sqlite3.connect(DB_PATH)
        guid_row = conn_g.execute(
            "SELECT player_id, status FROM replay_submissions "
            "WHERE match_id=? AND match_guid=? AND status='verified'",
            (match_id, match_guid)
        ).fetchone()
        conn_g.close()
        if guid_row and guid_row[0] != player_id:
            # A verified submission with the same GUID already exists.
            # Count this player as also verified with the same result, so they
            # don't hold up the collection (fair to them — same match, same file).
            conn_g2 = sqlite3.connect(DB_PATH)
            ref_w = conn_g2.execute(
                "SELECT winner_ids FROM replay_submissions "
                "WHERE match_id=? AND player_id=?",
                (match_id, guid_row[0])
            ).fetchone()
            conn_g2.close()
            winner_ids_str = ref_w[0] if ref_w else ""
            conn_g3 = sqlite3.connect(DB_PATH)
            conn_g3.execute(
                "INSERT OR REPLACE INTO replay_submissions "
                "(match_id, player_id, status, match_guid, replay_path, winner_ids, submitted_at) "
                "VALUES (?,?,'verified',?,?,?,?)",
                (match_id, player_id, match_guid, replay_path, winner_ids_str, int(now))
            )
            conn_g3.commit()
            conn_g3.close()
            _check_replay_resolution(match_id)
            return {"status": "collected",
                    "note":   "Identical match GUID already verified — counted"}

    # ── build player_info for Steam anchor lookup ─────────────────────────────
    # For conflict matches, the active match is still in memory.
    # For already-resolved matches we load from match_history.
    m_live = matches.get(match_id)

    if m_live:
        all_pids = m_live.get("players", [])
        team_a   = m_live.get("team_a", [])
        team_b   = m_live.get("team_b", [])
        m_for_verify = m_live
    else:
        # Resolved match — reconstruct a minimal match dict for verify_replay
        conn_h = sqlite3.connect(DB_PATH)
        hist = conn_h.execute(
            "SELECT winner_ids, loser_ids, mode FROM match_history WHERE match_id=?",
            (match_id,)
        ).fetchone()
        coll_h = conn_h.execute(
            "SELECT lobby_ready_at FROM replay_collection WHERE match_id=?",
            (match_id,)
        ).fetchone()
        conn_h.close()
        if not hist:
            return {"status": "unverifiable", "reason": "Match history not found"}
        team_a = [x for x in (hist[0] or "").split(",") if x]
        team_b = [x for x in (hist[1] or "").split(",") if x]
        all_pids = team_a + team_b
        m_for_verify = {
            "players":       all_pids,
            "team_a":        team_a,
            "team_b":        team_b,
            "mode":          hist[2],
            "lobby_ready_at": coll_h[0] if coll_h else 0,
        }

    conn_pi = sqlite3.connect(DB_PATH)
    player_info: dict = {}
    for pid in all_pids:
        row_db = conn_pi.execute(
            "SELECT rl_display_name FROM players WHERE player_id=?", (pid,)
        ).fetchone()
        display_name = (row_db[0] or "") if row_db else ""
        # pid IS the player_id (Steam64 or Epic ID) — try to parse as Steam64
        steam_id: int | None = None
        try:
            v = int(pid)
            if v > 0:
                steam_id = v
        except (ValueError, TypeError):
            pass
        player_info[pid] = {"steam_id": steam_id, "display_name": display_name}
    conn_pi.close()

    # ── verify replay against match ───────────────────────────────────────────
    result = verify_replay(parsed, m_for_verify, player_info)
    print(f"[replay] {match_id} player={player_id} verdict={result['verdict']} "
          f"detail={result.get('reason', result.get('score0', ''))}")

    if result["verdict"] == "verified":
        winner_ids_str = ",".join(result["winner_ids"])
        sub_status = "verified"
    elif result["verdict"] == "conflict":
        winner_ids_str = ""
        sub_status = "contradicts_match"
        print(f"[replay] contradictory replay from {player_id}: "
              f"{result.get('reason', '')}")
    else:
        winner_ids_str = ""
        sub_status = "unverifiable"

    # ── save canonical replay if this is the first verified one ──────────────
    if sub_status == "verified":
        conn_sv = sqlite3.connect(DB_PATH)
        has_saved = conn_sv.execute(
            "SELECT saved_replay_path FROM replay_collection WHERE match_id=?",
            (match_id,)
        ).fetchone()
        conn_sv.close()
        if has_saved and not has_saved[0]:
            conn_sv2 = sqlite3.connect(DB_PATH)
            conn_sv2.execute(
                "UPDATE replay_collection SET saved_replay_path=?, match_guid=? "
                "WHERE match_id=?",
                (replay_path, match_guid, match_id)
            )
            conn_sv2.commit()
            conn_sv2.close()

    # ── persist submission ────────────────────────────────────────────────────
    conn_s = sqlite3.connect(DB_PATH)
    conn_s.execute(
        "INSERT OR REPLACE INTO replay_submissions "
        "(match_id, player_id, status, match_guid, replay_path, winner_ids, submitted_at) "
        "VALUES (?,?,?,?,?,?,?)",
        (match_id, player_id, sub_status, match_guid, replay_path,
         winner_ids_str, int(now))
    )
    conn_s.commit()
    conn_s.close()

    # ── for CONFLICT matches: check if replays now determine the winner ───────
    if button_outcome == "conflict" and m_live and sub_status == "verified":
        # Reuse the existing in-memory majority logic for active conflict matches
        if "replay_results" not in m_live:
            m_live["replay_results"] = {}
        if "replay_uploads" not in m_live:
            m_live["replay_uploads"] = []

        m_live["replay_results"][player_id] = {
            "winner_ids": result["winner_ids"],
            "loser_ids":  result["loser_ids"],
            "score":      f"{result.get('score0','?')}-{result.get('score1','?')}",
        }
        if player_id not in m_live["replay_uploads"]:
            m_live["replay_uploads"].append(player_id)

        vote_c: Counter = Counter()
        for res in m_live["replay_results"].values():
            w_set = frozenset(res["winner_ids"])
            if w_set == frozenset(team_a):
                vote_c["team_a"] += 1
            elif w_set == frozenset(team_b):
                vote_c["team_b"] += 1

        mode_c     = m_live.get("mode", "1s")
        threshold  = CONSENSUS_THRESHOLD.get(mode_c, 1.0)
        min_votes  = math.ceil(threshold * total_players)

        if vote_c:
            top_v, top_cnt = vote_c.most_common(1)[0]
            if top_cnt >= min_votes and top_v in ("team_a", "team_b"):
                winners_c = team_a if top_v == "team_a" else team_b
                losers_c  = team_b if top_v == "team_a" else team_a
                _award_match(match_id, m_live, winners_c, losers_c,
                             outcome="replay_verified", replay_status="verified")
                _check_replay_resolution(match_id)
                return {"status": "auto_resolved", "winners": winners_c}

        # Check all attempted
        all_attempted = len(m_live["replay_uploads"])
        if all_attempted >= total_players:
            _flag_disputed(match_id, m_live)
            _check_replay_resolution(match_id)
            return {"status": "pending_review",
                    "note":   "All replays collected but no majority — sent to admin"}

    # ── trigger resolution check for normal (post-award) matches ─────────────
    _check_replay_resolution(match_id)

    if sub_status == "unverifiable":
        return {"status": "unverifiable",
                "reason": result.get("reason", "Could not verify")}

    return {"status": "collected",
            "verdict": sub_status}


# Majority thresholds per mode:
#   1s (2 players)  → 2/2 = 100% — any disagreement triggers replay request
#   2s (4 players)  → 3/4 = 75%
#   3s (6 players)  → 5/6 ≈ 84%
CONSENSUS_THRESHOLD = {"1s": 1.0, "2s": 0.75, "3s": 5 / 6}

@app.post("/match/result")
def submit_match_result(req: MatchResultRequest):
    """
    Called when a player presses Win / Loss / Draw in the plugin UI.
    No game hooks — this is the only result detection mechanism.

    Each vote is normalised to "team_a", "team_b", or "draw" so that
    team A saying "win" and team B saying "loss" both count as the same claim.
    Resolution fires as soon as the mode threshold is reached, without waiting
    for every player.  If all players vote but no majority exists, the server
    flags the match as awaiting_replay and the plugin prompts for upload.
    """
    if req.outcome not in ("win", "loss", "draw"):
        raise HTTPException(400, "outcome must be 'win', 'loss', or 'draw'")

    m = matches.get(req.match_id)
    if not m:
        return {"status": "recorded", "note": "match not active"}

    if req.player_id not in m["players"]:
        raise HTTPException(400, "Player not in this match")

    # Persist this player's vote
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

    submitted = {r[0]: r[1] for r in rows}
    total  = len(m["players"])
    team_a = m["team_a"]
    team_b = m["team_b"]
    mode   = m.get("mode", "1s")

    # Normalise every vote to a shared reference frame
    #   team_a player "win"  → "team_a"   team_b player "loss" → "team_a"
    #   team_a player "loss" → "team_b"   team_b player "win"  → "team_b"
    #   either player "draw" → "draw"
    normalized: dict[str, str] = {}
    for pid, outcome in submitted.items():
        if outcome == "draw":
            normalized[pid] = "draw"
        elif pid in team_a:
            normalized[pid] = "team_a" if outcome == "win" else "team_b"
        else:
            normalized[pid] = "team_b" if outcome == "win" else "team_a"

    threshold  = CONSENSUS_THRESHOLD.get(mode, 1.0)
    min_votes  = math.ceil(threshold * total)
    vote_counts = Counter(normalized.values())

    # Resolve as soon as threshold is hit (don't wait for every player)
    if vote_counts:
        top_verdict, top_count = vote_counts.most_common(1)[0]
        if top_count >= min_votes:
            resolved_at     = time.time()
            player_ids_snap = list(m["players"])   # capture before match removed

            if top_verdict == "draw":
                _record_draw(req.match_id, m, outcome="draw")
                _init_replay_collection(
                    req.match_id, {"players": player_ids_snap,
                                   "lobby_ready_at": m.get("lobby_ready_at", 0)},
                    button_outcome  = "draw",
                    button_winners  = [],
                    button_losers   = [],
                    resolved_at     = resolved_at,
                    player_ids_to_notify = player_ids_snap,
                )
                return {"status": "draw_recorded",
                        "collection_ends_at": resolved_at + REPLAY_COLLECTION_WINDOW}
            elif top_verdict == "team_a":
                _award_match(req.match_id, m, team_a, team_b, outcome="normal")
                _init_replay_collection(
                    req.match_id, {"players": player_ids_snap,
                                   "lobby_ready_at": m.get("lobby_ready_at", 0)},
                    button_outcome  = "team_a",
                    button_winners  = team_a,
                    button_losers   = team_b,
                    resolved_at     = resolved_at,
                    player_ids_to_notify = player_ids_snap,
                )
                return {"status": "awarded", "winners": team_a,
                        "collection_ends_at": resolved_at + REPLAY_COLLECTION_WINDOW}
            else:
                _award_match(req.match_id, m, team_b, team_a, outcome="normal")
                _init_replay_collection(
                    req.match_id, {"players": player_ids_snap,
                                   "lobby_ready_at": m.get("lobby_ready_at", 0)},
                    button_outcome  = "team_b",
                    button_winners  = team_b,
                    button_losers   = team_a,
                    resolved_at     = resolved_at,
                    player_ids_to_notify = player_ids_snap,
                )
                return {"status": "awarded", "winners": team_b,
                        "collection_ends_at": resolved_at + REPLAY_COLLECTION_WINDOW}

    # Not enough votes yet
    if len(submitted) < total:
        return {"status": "recorded", "waiting": total - len(submitted)}

    # All players voted but no majority — collect replays to determine winner
    # Set up replay collection with no predetermined winner (conflict case)
    m["awaiting_replay"]  = True
    m["replay_results"]   = {}   # player_id -> {winner_ids, loser_ids, score}
    m["replay_uploads"]   = []   # player_ids who attempted upload (any verdict)
    resolved_at     = time.time()
    player_ids_snap = list(m["players"])
    _init_replay_collection(
        req.match_id, m,
        button_outcome  = "conflict",
        button_winners  = [],
        button_losers   = [],
        resolved_at     = resolved_at,
        player_ids_to_notify = player_ids_snap,
    )
    _push_all(m["players"], "conflict", {
        "match_id":           req.match_id,
        "collection_ends_at": resolved_at + REPLAY_COLLECTION_WINDOW,
    })
    print(f"[match] {req.match_id} → no majority, awaiting replays. votes={dict(vote_counts)}")
    return {"status": "conflict",
            "note":   "No majority — upload your replay for verification",
            "collection_ends_at": resolved_at + REPLAY_COLLECTION_WINDOW}


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
  header nav { margin-left: auto; display: flex; gap: 6px; }
  header nav a {
    color: #aaa; text-decoration: none; font-size: 0.88rem;
    padding: 6px 14px; border-radius: 6px; border: 1px solid transparent;
    transition: color .15s, border-color .15s;
  }
  header nav a:hover { color: #5fa8ff; border-color: #2a4a7f; }
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
  <nav>
    <a href="/">Leaderboard</a>
    <a href="/replay-test">Replay Tool</a>
    <a href="/admin">Admin</a>
  </nav>
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
        "WHERE r.status = 'pending' "
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
                  COALESCE(outcome, 'normal') as outcome,
                  COALESCE(replay_status, 'unverified') as replay_status
           FROM match_history
           WHERE winner_ids LIKE ? OR loser_ids LIKE ?
           ORDER BY timestamp DESC LIMIT 10""",
        (f"%{player_id}%", f"%{player_id}%")
    ).fetchall()
    conn.close()
    history = []
    for mid, mode, region, winner_ids, loser_ids, ts, dw, dl, outcome, replay_status in rows:
        won = player_id in (winner_ids or "").split(",")
        history.append({
            "match_id":     mid,
            "mode":         mode,
            "region":       region,
            "won":          won,
            "outcome":      outcome,
            "replay_status": replay_status,
            "mmr_change":   round(dw if won else dl, 1),
            "timestamp":    ts,
        })
    return history

@app.get("/player/{player_id}/profile")
def player_profile_api(player_id: str):
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT username, mmr_1s, mmr_2s, mmr_3s, wins, losses, platform, rl_display_name, trust_score "
        "FROM players WHERE player_id=?", (player_id,)
    ).fetchone()
    if not row:
        conn.close()
        raise HTTPException(404, "Player not found")

    username, m1, m2, m3, wins, losses, platform, rldn, trust = row

    # last 20 matches
    rows = conn.execute(
        """SELECT match_id, mode, region, winner_ids, loser_ids,
                  timestamp, mmr_delta_win, mmr_delta_loss,
                  COALESCE(outcome,'normal'), COALESCE(replay_status,'unverified')
           FROM match_history
           WHERE winner_ids LIKE ? OR loser_ids LIKE ?
           ORDER BY timestamp DESC LIMIT 20""",
        (f"%{player_id}%", f"%{player_id}%")
    ).fetchall()
    conn.close()

    history = []
    streak = 0
    streak_type = None
    for mid, mode, region, wids, lids, ts, dw, dl, outcome, rstat in rows:
        won = player_id in (wids or "").split(",")
        history.append({
            "match_id": mid, "mode": mode, "region": region,
            "won": won, "outcome": outcome, "replay_status": rstat,
            "mmr_change": round(dw if won else dl, 1), "timestamp": ts,
        })
        if streak_type is None:
            streak_type = won
        if won == streak_type:
            streak += 1
        else:
            break  # streak broken

    total = wins + losses
    return {
        "player_id": player_id,
        "username": username or "",
        "rl_display_name": rldn or "",
        "platform": platform or "",
        "mmr_1s": round(m1, 1), "mmr_2s": round(m2, 1), "mmr_3s": round(m3, 1),
        "wins": wins, "losses": losses,
        "win_pct": round(wins / total * 100, 1) if total else 0,
        "trust_score": round(trust, 1) if trust else 100.0,
        "streak": streak,
        "streak_type": "win" if streak_type else ("loss" if streak_type is False else None),
        "history": history,
    }


PLAYER_PROFILE_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RL Custom Queue — Player Profile</title>
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
  header nav { margin-left: auto; display: flex; gap: 6px; }
  header nav a {
    color: #aaa; text-decoration: none; font-size: 0.88rem;
    padding: 6px 14px; border-radius: 6px; border: 1px solid transparent;
    transition: color .15s, border-color .15s;
  }
  header nav a:hover { color: #5fa8ff; border-color: #2a4a7f; }
  .container { max-width: 860px; margin: 36px auto; padding: 0 20px; }
  .card {
    background: #161616; border: 1px solid #2a2a2a; border-radius: 10px;
    padding: 26px 30px; margin-bottom: 24px;
  }
  .card h2 { font-size: 1.05rem; color: #5fa8ff; margin-bottom: 16px; }
  /* player header */
  .player-header { display: flex; align-items: flex-start; gap: 20px; flex-wrap: wrap; margin-bottom: 24px; }
  .player-name { font-size: 1.8rem; font-weight: 700; color: #5fa8ff; }
  .platform-badge {
    display: inline-block; padding: 3px 12px; border-radius: 20px;
    font-size: 0.8rem; font-weight: 600; vertical-align: middle; margin-left: 10px;
  }
  .platform-badge.steam { background: #0e2a3a; color: #6dcff6; border: 1px solid #1a4a6a; }
  .platform-badge.epic  { background: #1a0e3a; color: #9b7fd4; border: 1px solid #3a2a6a; }
  .rldn { color: #888; font-size: 0.92rem; margin-top: 4px; }
  /* mmr cards */
  .mmr-row { display: flex; gap: 14px; flex-wrap: wrap; margin-bottom: 20px; }
  .mmr-card {
    background: #1a1a2e; border: 1px solid #2a4a7f; border-radius: 8px;
    padding: 14px 22px; text-align: center; min-width: 110px;
  }
  .mmr-card .mode-lbl { font-size: 0.8rem; color: #888; margin-bottom: 4px; }
  .mmr-card .mmr-val  { font-size: 1.5rem; font-weight: 700; color: #5fa8ff; }
  /* stats row */
  .stats-row { display: flex; gap: 20px; flex-wrap: wrap; margin-bottom: 20px; }
  .stat-item { font-size: 0.95rem; color: #aaa; }
  .stat-item .val { font-weight: 700; font-size: 1.1rem; }
  .stat-item .val.green { color: #4cff91; }
  .stat-item .val.red   { color: #ff5f5f; }
  .stat-item .val.blue  { color: #5fa8ff; }
  /* streak badge */
  .streak-badge {
    display: inline-block; padding: 4px 14px; border-radius: 20px;
    font-size: 0.88rem; font-weight: 600; margin-bottom: 8px;
  }
  .streak-badge.win  { background: #0e2a1a; color: #4cff91; border: 1px solid #1a5c38; }
  .streak-badge.loss { background: #2a0a0a; color: #ff5f5f; border: 1px solid #6a1a1a; }
  /* history table */
  table { width: 100%; border-collapse: collapse; font-size: 0.88rem; }
  thead tr { border-bottom: 1px solid #2a2a2a; }
  th { text-align: left; padding: 8px 12px; color: #5fa8ff; font-weight: 600; }
  td { padding: 8px 12px; border-bottom: 1px solid #1e1e1e; }
  .result-win  { color: #4cff91; font-weight: 600; }
  .result-loss { color: #ff5f5f; font-weight: 600; }
  .mmr-pos { color: #4cff91; }
  .mmr-neg { color: #ff5f5f; }
  .mmr-neu { color: #888; }
  .replay-ok  { color: #4cff91; }
  .replay-unk { color: #ff9933; }
  .replay-neu { color: #555; }
  .empty { color: #555; text-align: center; padding: 30px 0; }
  .back-link { color: #5fa8ff; text-decoration: none; font-size: 0.9rem; }
  .back-link:hover { text-decoration: underline; }
  #loading-msg { color: #888; padding: 40px 0; text-align: center; }
  #error-msg { color: #ff5f5f; padding: 40px 0; text-align: center; font-size: 1.05rem; }
</style>
</head>
<body>
<header>
  <h1>⚡ RL Custom Queue</h1>
  <nav>
    <a href="/">Leaderboard</a>
    <a href="/replay-test">Replay Tool</a>
    <a href="/admin">Admin</a>
  </nav>
</header>
<div class="container">
  <div id="loading-msg">Loading player profile...</div>
  <div id="error-msg" style="display:none"></div>
  <div id="profile-content" style="display:none">
    <div style="margin-bottom:18px;">
      <a href="/" class="back-link">← Back to Leaderboard</a>
    </div>
    <div class="card">
      <div class="player-header">
        <div>
          <div>
            <span class="player-name" id="pname"></span>
            <span class="platform-badge" id="plat-badge"></span>
          </div>
          <div class="rldn" id="rldn"></div>
        </div>
      </div>
      <div class="mmr-row">
        <div class="mmr-card"><div class="mode-lbl">1s Rating</div><div class="mmr-val" id="mmr1s">—</div></div>
        <div class="mmr-card"><div class="mode-lbl">2s Rating</div><div class="mmr-val" id="mmr2s">—</div></div>
        <div class="mmr-card"><div class="mode-lbl">3s Rating</div><div class="mmr-val" id="mmr3s">—</div></div>
      </div>
      <div class="stats-row">
        <div class="stat-item"><div class="val green" id="stat-wins">0</div><div>Wins</div></div>
        <div class="stat-item"><div class="val red"   id="stat-losses">0</div><div>Losses</div></div>
        <div class="stat-item"><div class="val blue"  id="stat-winpct">0%</div><div>Win %</div></div>
      </div>
      <div id="streak-wrap" style="display:none">
        <span class="streak-badge" id="streak-badge"></span>
      </div>
    </div>
    <div class="card">
      <h2>Match History (last 20)</h2>
      <table>
        <thead>
          <tr>
            <th>Date</th><th>Mode</th><th>Region</th>
            <th>Result</th><th>MMR Change</th><th>Replay</th>
          </tr>
        </thead>
        <tbody id="history-rows">
          <tr><td colspan="6" class="empty">No matches on record.</td></tr>
        </tbody>
      </table>
    </div>
    <div style="margin-top:10px;">
      <a href="/" class="back-link">← Back to Leaderboard</a>
    </div>
  </div>
</div>
<script>
const playerId = '__PLAYER_ID__';

function fmtDate(ts) {
  if (!ts) return '—';
  const d = new Date(ts * 1000);
  return d.toLocaleDateString() + ' ' + d.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'});
}

async function loadProfile() {
  try {
    const r = await fetch('/player/' + playerId + '/profile');
    if (r.status === 404) {
      document.getElementById('loading-msg').style.display = 'none';
      const em = document.getElementById('error-msg');
      em.textContent = 'Player not found.';
      em.style.display = 'block';
      return;
    }
    const p = await r.json();
    document.getElementById('loading-msg').style.display = 'none';

    document.getElementById('pname').textContent = p.username || p.player_id;

    const platBadge = document.getElementById('plat-badge');
    if (p.platform === 'Steam') {
      platBadge.textContent = 'Steam'; platBadge.className = 'platform-badge steam';
    } else if (p.platform === 'Epic') {
      platBadge.textContent = 'Epic';  platBadge.className = 'platform-badge epic';
    } else {
      platBadge.style.display = 'none';
    }

    const rlEl = document.getElementById('rldn');
    if (p.rl_display_name && p.rl_display_name !== p.username) {
      rlEl.textContent = 'In-game: ' + p.rl_display_name;
    }

    document.getElementById('mmr1s').textContent = p.mmr_1s;
    document.getElementById('mmr2s').textContent = p.mmr_2s;
    document.getElementById('mmr3s').textContent = p.mmr_3s;
    document.getElementById('stat-wins').textContent   = p.wins;
    document.getElementById('stat-losses').textContent = p.losses;
    document.getElementById('stat-winpct').textContent = p.win_pct + '%';

    if (p.streak >= 2 && p.streak_type) {
      const sw = document.getElementById('streak-wrap');
      const sb = document.getElementById('streak-badge');
      sw.style.display = 'block';
      if (p.streak_type === 'win') {
        sb.textContent = '🔥 ' + p.streak + 'W Streak';
        sb.className   = 'streak-badge win';
      } else {
        sb.textContent = '❄ ' + p.streak + 'L Streak';
        sb.className   = 'streak-badge loss';
      }
    }

    const tbody = document.getElementById('history-rows');
    if (!p.history || !p.history.length) {
      tbody.innerHTML = '<tr><td colspan="6" class="empty">No matches on record.</td></tr>';
    } else {
      tbody.innerHTML = p.history.map(h => {
        const resultHtml = h.won
          ? '<span class="result-win">Win</span>'
          : '<span class="result-loss">Loss</span>';
        const mmr = h.mmr_change;
        const mmrHtml = mmr > 0
          ? '<span class="mmr-pos">+' + mmr + '</span>'
          : mmr < 0
          ? '<span class="mmr-neg">' + mmr + '</span>'
          : '<span class="mmr-neu">—</span>';
        let replayHtml;
        if (h.replay_status === 'verified') {
          replayHtml = '<span class="replay-ok" title="Verified">✓</span>';
        } else if (h.replay_status === 'no_majority' || h.replay_status === 'admin_review') {
          replayHtml = '<span class="replay-unk" title="' + h.replay_status + '">?</span>';
        } else {
          replayHtml = '<span class="replay-neu">—</span>';
        }
        return `<tr>
          <td>${fmtDate(h.timestamp)}</td>
          <td>${h.mode || '—'}</td>
          <td>${h.region || '—'}</td>
          <td>${resultHtml}</td>
          <td>${mmrHtml}</td>
          <td>${replayHtml}</td>
        </tr>`;
      }).join('');
    }

    document.getElementById('profile-content').style.display = 'block';
  } catch(e) {
    document.getElementById('loading-msg').style.display = 'none';
    const em = document.getElementById('error-msg');
    em.textContent = 'Could not load profile: ' + e.message;
    em.style.display = 'block';
  }
}

loadProfile();
</script>
</body>
</html>"""


@app.get("/player/{player_id}", response_class=HTMLResponse)
def player_profile_page(player_id: str):
    return HTMLResponse(content=PLAYER_PROFILE_HTML.replace("__PLAYER_ID__", player_id))


@app.get("/account/link/status/{player_id}")
def account_link_status_legacy(player_id: str):
    """Stub for old plugin versions — keeps them from spamming 404s."""
    return {"linked": False, "platform": "", "rl_display_name": ""}

@app.get("/account/lookup")
def account_lookup(player_id: str):
    """Look up a player by their player_id (Steam64 or Epic account ID).
    Used by the plugin on any computer to restore a saved username without
    the user having to type it again."""
    if not player_id:
        return {"found": False}
    conn = sqlite3.connect(DB_PATH)
    row = conn.execute(
        "SELECT player_id, username, mmr_1s, mmr_2s, mmr_3s, rl_display_name, platform "
        "FROM players WHERE player_id=?",
        (player_id,)
    ).fetchone()
    conn.close()
    if row and row[1]:  # must have a username saved
        return {
            "found":           True,
            "player_id":         row[0],
            "username":        row[1],
            "mmr_1s":          round(row[2], 1),
            "mmr_2s":          round(row[3], 1),
            "mmr_3s":          round(row[4], 1),
            "rl_display_name": row[5] or "",
            "platform":        row[6] or "",
        }
    return {"found": False}

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
        f"SELECT player_id, username, mmr_1s, mmr_2s, mmr_3s, wins, losses, platform "
        f"FROM players WHERE username != '' ORDER BY {col} DESC LIMIT 50"
    ).fetchall()
    conn.close()
    return [{"rank": i + 1, "player_id": r[0], "username": r[1] or "",
             "mmr_1s": round(r[2], 1), "mmr_2s": round(r[3], 1), "mmr_3s": round(r[4], 1),
             "wins": r[5], "losses": r[6], "platform": r[7] or ""}
            for i, r in enumerate(rows)]


# ── replay metadata cache (path → (mtime, dict)) ─────────────────────────────
_replay_meta_cache: dict = {}

def _sidecar_path(replay_path: str) -> str:
    """Return the sidecar JSON path for a given .replay path.
    Stored next to the replay with the same name, e.g. ABC123.replay → ABC123.json.
    """
    return os.path.splitext(replay_path)[0] + ".json"


def _replay_meta(path: str) -> dict:
    """Return display metadata for a replay. Priority order:
       1. In-memory cache  (fastest — avoids all disk I/O on repeat calls)
       2. Sidecar .json    (fast — survives server restarts, no replay parsing needed)
       3. Parse .replay    (slow — used only once per file, then sidecar is written)
    """
    try:
        replay_mtime = os.path.getmtime(path)
    except OSError:
        return {}

    # 1. Memory cache
    if path in _replay_meta_cache:
        cached_mtime, cached_data = _replay_meta_cache[path]
        if cached_mtime == replay_mtime:
            return cached_data

    # 2. Sidecar JSON — valid if it exists and is at least as new as the replay
    sidecar = _sidecar_path(path)
    try:
        sidecar_mtime = os.path.getmtime(sidecar)
        if sidecar_mtime >= replay_mtime:
            with open(sidecar, "r", encoding="utf-8") as f:
                meta = _json.load(f)
            _replay_meta_cache[path] = (replay_mtime, meta)
            return meta
    except (OSError, _json.JSONDecodeError):
        pass  # sidecar missing or corrupt — fall through to parse

    # 3. Parse the replay, build meta, write sidecar for next time
    parsed = parse_replay_header(path)
    if not parsed:
        meta = {}
    else:
        players = parsed.get("players", [])
        tc: dict[int, int] = {}
        for _p in players:
            _t = _p.get("team", -1)
            if _t in (0, 1):
                tc[_t] = tc.get(_t, 0) + 1
        if len(tc) == 2:
            _a, _b = sorted(tc.values(), reverse=True)
            mode = f"{_a}v{_b}"
        elif len(tc) == 1:
            mode = f"{list(tc.values())[0]}v0"
        else:
            _vs = {1: "1v1", 2: "2v2", 3: "3v3"}
            ts = parsed.get("team_size")
            mode = _vs.get(ts, f"{ts}v{ts}" if ts else None)

        s0 = parsed.get("score0")
        s1 = parsed.get("score1")
        if s0 is None and s1 is not None: s0 = 0
        if s1 is None and s0 is not None: s1 = 0
        if s0 is None and s1 is None:     s0 = s1 = 0

        mt = (parsed.get("match_type") or "").lower()
        def _player_entry(p: dict) -> dict:
            """Compact player record stored in the sidecar."""
            entry: dict = {"name": p["name"]}
            if p.get("epic_id"):
                entry["epic_id"] = p["epic_id"]
            if p.get("online_id", 0) > 0:
                entry["steam_id"] = p["online_id"]
            return entry

        meta = {
            "mode":           mode,
            "is_private":     "private" in mt,
            "match_type":     parsed.get("match_type"),
            "score_blue":     s0,
            "score_orange":   s1,
            "date":           parsed.get("date"),
            "match_guid":     parsed.get("match_guid") or "",
            "players_blue":   [_player_entry(p) for p in players
                               if p.get("name") and p.get("team") == 0],
            "players_orange": [_player_entry(p) for p in players
                               if p.get("name") and p.get("team") == 1],
        }

    # Write sidecar so the next restart doesn't need to re-parse
    try:
        with open(sidecar, "w", encoding="utf-8") as f:
            _json.dump(meta, f, ensure_ascii=False)
    except OSError:
        pass  # non-fatal — we'll just re-parse next time

    _replay_meta_cache[path] = (replay_mtime, meta)
    return meta


@app.get("/replay/list")
def replay_list():
    """Return metadata for every stored replay, newest first."""
    base = os.path.join(os.path.dirname(__file__), "replays")
    entries = []
    for source in ("matches", "test-tool"):
        folder = os.path.join(base, source)
        if not os.path.isdir(folder):
            continue
        for fname in os.listdir(folder):
            if not fname.endswith(".replay"):
                continue
            fpath = os.path.join(folder, fname)
            try:
                mtime  = os.path.getmtime(fpath)
                fsize  = os.path.getsize(fpath)
            except OSError:
                continue
            meta = _replay_meta(fpath)
            entries.append({
                "filename":    fname,
                "source":      source,
                "size_kb":     round(fsize / 1024, 1),
                "saved_at":    datetime.utcfromtimestamp(mtime).strftime("%Y-%m-%d %H:%M"),
                "mtime":       mtime,
                **meta,
            })

    # Sort by match date from the replay header (newest first).
    # Falls back to file mtime if the date field is missing.
    def _sort_key(e):
        d = e.get("date") or ""
        return d if d else datetime.utcfromtimestamp(e["mtime"]).strftime("%Y-%m-%d %H-%M-%S")
    entries.sort(key=_sort_key, reverse=True)
    for e in entries:
        del e["mtime"]   # don't expose raw epoch to the client
    return {"replays": entries}


@app.post("/replay/save")
async def replay_save(request: Request):
    """Save a raw .replay upload to the test-tool folder without parsing it."""
    body = await request.body()
    if len(body) < 16:
        return JSONResponse({"error": "File too small — not a valid .replay"}, status_code=400)
    _test_dir = os.path.join(os.path.dirname(__file__), "replays", "test-tool")
    os.makedirs(_test_dir, exist_ok=True)
    _ts  = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    _uid = os.urandom(4).hex()
    _fname = f"{_ts}_{_uid}.replay"
    _path  = os.path.join(_test_dir, _fname)
    with open(_path, "wb") as f:
        f.write(body)
    print(f"[save] stored {_fname} ({len(body)//1024} KB)")
    return {"status": "saved", "filename": _fname, "size_kb": round(len(body)/1024, 1)}


@app.get("/replay/download/{source}/{filename}")
def replay_download(source: str, filename: str):
    """Serve a stored replay file as a download."""
    # Validate source and sanitise filename to prevent path traversal
    if source not in ("matches", "test-tool"):
        raise HTTPException(status_code=400, detail="Invalid source")
    safe = os.path.basename(filename)
    if not safe.endswith(".replay"):
        raise HTTPException(status_code=400, detail="Invalid filename")
    path = os.path.join(os.path.dirname(__file__), "replays", source, safe)
    if not os.path.isfile(path):
        raise HTTPException(status_code=404, detail="File not found")
    return FileResponse(path, media_type="application/octet-stream",
                        filename=safe)


# ── routes: replay test tool ───────────────────────────────────────────────────
@app.post("/replay/parse")
async def replay_parse(request: Request):
    """
    Accept a raw .replay file upload and return the parsed header as JSON.
    Also runs verify_replay if match_id is provided as a query param.
    """
    from fastapi import Query
    match_id = request.query_params.get("match_id", "")
    body = await request.body()
    if len(body) < 16:
        return JSONResponse({"error": "File too small — not a valid .replay"}, status_code=400)

    # ── save to disk so every test-tool upload is kept ────────────────────────
    _test_dir = os.path.join(os.path.dirname(__file__), "replays", "test-tool")
    os.makedirs(_test_dir, exist_ok=True)
    _ts  = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    _uid = os.urandom(4).hex()   # short random suffix to avoid collisions
    _test_path = os.path.join(_test_dir, f"{_ts}_{_uid}.replay")
    with open(_test_path, "wb") as _f:
        _f.write(body)

    import io, sys
    _buf = io.StringIO()
    _old = sys.stdout
    sys.stdout = _buf
    try:
        parsed = parse_replay_bytes(body)
    except Exception as exc:
        sys.stdout = _old
        trace = _buf.getvalue()
        _old.write(f"[replay/parse endpoint] {exc}\n{trace}\n")
        _old.flush()
        return JSONResponse({"error": f"Parse failed: {exc}", "trace": trace}, status_code=422)
    finally:
        sys.stdout = _old

    trace = _buf.getvalue()
    # Always echo the parse trace to the real console so it's visible even on 200 OK
    _old.write(trace)
    _old.flush()

    # Determine if this looks like a private match
    mt         = (parsed.get("match_type") or "").lower()
    is_private = "private" in mt
    team_size  = parsed.get("team_size")
    players    = parsed.get("players", [])

    # Build mode label from ACTUAL player counts per team rather than TeamSize.
    # TeamSize reflects the lobby slot setting and can be misleading (e.g. a
    # 4-slot lobby with only 4 players playing gives TeamSize=4 → "4v4" is wrong).
    _team_counts: dict[int, int] = {}
    for _p in players:
        _t = _p.get("team", -1)
        if _t in (0, 1):
            _team_counts[_t] = _team_counts.get(_t, 0) + 1
    if len(_team_counts) == 2:
        _a, _b = sorted(_team_counts.values(), reverse=True)
        mode_label = f"{_a}v{_b}"
    elif len(_team_counts) == 1:
        _a = list(_team_counts.values())[0]
        mode_label = f"{_a}v0"
    else:
        # No team data — fall back to TeamSize header
        _vs = {1: "1v1", 2: "2v2", 3: "3v3"}
        mode_label = _vs.get(team_size, f"{team_size}v{team_size}" if team_size else None)

    # RL omits Team0Score / Team1Score from the header when the value is 0.
    # Default the missing side to 0 so we display "0–3" rather than "?–3".
    # If BOTH are absent (e.g. no-contest / abandoned match) default both to 0.
    score0 = parsed.get("score0")
    score1 = parsed.get("score1")
    if score0 is None and score1 is not None: score0 = 0
    if score1 is None and score0 is not None: score1 = 0
    if score0 is None and score1 is None:     score0 = score1 = 0

    result = {
        "match_type":  parsed.get("match_type"),
        "team_size":   team_size,
        "mode":        mode_label,           # e.g. "1v1", "2v2", "3v3"
        "is_private":  is_private,
        "date":        parsed.get("date"),
        "score_blue":  score0,
        "score_orange":score1,
        "players":     players,
        "trace":       trace,   # always included so the test page can show it
    }

    # If a match_id was given, also run verify_replay against it
    if match_id:
        m = matches.get(match_id)
        if not m:
            result["verify"] = {"error": f"match_id '{match_id}' not found in active matches"}
        else:
            conn = sqlite3.connect(DB_PATH)
            pids = list(m.get("players", []))
            player_info = {}
            for pid in pids:
                row = conn.execute(
                    "SELECT rl_display_name FROM players WHERE player_id=?",
                    (pid,)
                ).fetchone()
                display_name = (row[0] or "") if row else ""
                # pid IS the player_id — try to parse as Steam64
                steam_id = None
                try:
                    v = int(pid)
                    if v > 0:
                        steam_id = v
                except (ValueError, TypeError):
                    pass
                player_info[pid] = {"steam_id": steam_id, "display_name": display_name}
            conn.close()
            verdict = verify_replay(parsed, m, player_info)
            result["verify"] = verdict

    return JSONResponse(result)


@app.get("/replay-test", response_class=HTMLResponse)
def replay_test_page():
    return HTMLResponse(content="""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RL Queue — Replay Test</title>
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
  header nav { margin-left: auto; display: flex; gap: 6px; }
  header nav a {
    color: #aaa; text-decoration: none; font-size: 0.88rem;
    padding: 6px 14px; border-radius: 6px; border: 1px solid transparent;
    transition: color .15s, border-color .15s;
  }
  header nav a:hover { color: #5fa8ff; border-color: #2a4a7f; }
  .container { max-width: 860px; margin: 36px auto; padding: 0 20px; transition: max-width .2s; }
  .container.wide { max-width: 1400px; }
  .card {
    background: #161616; border: 1px solid #2a2a2a; border-radius: 10px;
    padding: 26px 30px; margin-bottom: 28px;
  }
  .card h2 { font-size: 1.05rem; color: #5fa8ff; margin-bottom: 6px; }
  .card > p { color: #888; font-size: 0.88rem; margin-bottom: 18px; }

  /* drop zone */
  #drop-zone {
    border: 2px dashed #333; border-radius: 8px;
    padding: 40px 20px; text-align: center; cursor: pointer;
    transition: border-color .2s, background .2s;
  }
  #drop-zone:hover, #drop-zone.drag-over {
    border-color: #5fa8ff; background: #0f1a2e;
  }
  #drop-zone p { color: #555; font-size: 0.95rem; }
  #drop-zone p span { color: #5fa8ff; text-decoration: underline; cursor: pointer; }
  #file-input { display: none; }
  #file-name { margin-top: 10px; font-size: 0.88rem; color: #888; }

  .opt-row { display: flex; gap: 10px; margin-top: 14px; flex-wrap: wrap; align-items: center; }
  .opt-row input {
    flex: 1; min-width: 200px; background: #222; border: 1px solid #333;
    border-radius: 6px; color: #e0e0e0; padding: 9px 14px; font-size: 0.9rem; outline: none;
    transition: border-color .2s;
  }
  .opt-row input:focus { border-color: #5fa8ff; }
  .opt-row input::placeholder { color: #555; }

  button#parse-btn {
    background: #2a5298; color: #fff; border: none; border-radius: 6px;
    padding: 10px 26px; font-size: 0.95rem; cursor: pointer; transition: background .2s;
    margin-top: 14px;
  }
  button#parse-btn:hover { background: #3a6bc4; }
  button#parse-btn:disabled { background: #333; color: #666; cursor: default; }

  /* result */
  #result { display: none; }
  .section-title { font-size: 0.78rem; text-transform: uppercase; letter-spacing: 1px;
                   color: #555; margin: 20px 0 8px; }
  .kv-grid { display: grid; grid-template-columns: 160px 1fr; gap: 4px 12px; font-size: 0.9rem; }
  .kv-grid .k { color: #888; }
  .kv-grid .v { color: #e0e0e0; }
  .badge {
    display: inline-block; padding: 2px 10px; border-radius: 12px;
    font-size: 0.8rem; font-weight: 600;
  }
  .badge.ok  { background: #0e3320; color: #4cff91; border: 1px solid #1a5c38; }
  .badge.warn{ background: #3a2800; color: #ffc94d; border: 1px solid #6a4800; }
  .badge.err { background: #3a0a0a; color: #ff5f5f; border: 1px solid #6a1a1a; }

  table.ptable { width: 100%; border-collapse: collapse; font-size: 0.88rem; margin-top: 6px; }
  table.ptable thead tr { border-bottom: 1px solid #2a2a2a; }
  table.ptable th { text-align: left; padding: 6px 10px; color: #5fa8ff; font-weight: 600; }
  table.ptable td { padding: 7px 10px; border-bottom: 1px solid #1e1e1e; }

  .verdict-box {
    border-radius: 8px; padding: 14px 18px; margin-top: 10px; font-size: 0.9rem;
  }
  .verdict-box.verified  { background: #0e2a1a; border: 1px solid #1a5c38; }
  .verdict-box.unverif   { background: #2a1e00; border: 1px solid #6a4800; }
  .verdict-box.error-box { background: #2a0a0a; border: 1px solid #6a1a1a; }
  .verdict-box strong    { display: block; margin-bottom: 6px; font-size: 1rem; }

  .err-msg { color: #ff5f5f; margin-top: 12px; font-size: 0.9rem; }
  #spinner { display: none; color: #888; margin-top: 12px; font-size: 0.9rem; }

  /* tabs */
  .tabs { display: flex; gap: 0; margin-bottom: 28px; border-bottom: 2px solid #2a2a2a; }
  .tab-btn {
    background: none; border: none; color: #888; font-size: 0.95rem;
    padding: 10px 26px; cursor: pointer; border-bottom: 2px solid transparent;
    margin-bottom: -2px; transition: color .15s, border-color .15s;
  }
  .tab-btn:hover { color: #c0c0c0; }
  .tab-btn.active { color: #5fa8ff; border-bottom-color: #5fa8ff; font-weight: 600; }
  .tab-pane { display: none; }
  .tab-pane.active { display: block; }

  /* stored replays table */
  .rep-table { width: 100%; border-collapse: collapse; font-size: 0.88rem; }
  .rep-table thead tr { border-bottom: 1px solid #2a2a2a; }
  .rep-table th { text-align: left; padding: 8px 12px; color: #5fa8ff; font-weight: 600; }
  .rep-table td { padding: 8px 12px; border-bottom: 1px solid #1e1e1e; vertical-align: middle; }
  .rep-table tr:last-child td { border-bottom: none; }
  #rep-spinner { color: #888; font-size: 0.9rem; margin: 20px 0; display: none; }
  #rep-empty   { color: #555; font-size: 0.9rem; margin: 20px 0; display: none; }
  .players-cell { font-size: 0.82rem; vertical-align: top; }
  .pname { padding: 2px 0; white-space: nowrap; }
  .blue-name   { color: #5fa8ff; }
  .orange-name { color: #ff7a33; }
  .dl-btn {
    display: inline-block; padding: 2px 8px; border-radius: 4px;
    background: #1a2e4a; color: #5fa8ff; border: 1px solid #2a4a7f;
    font-size: 0.75rem; text-decoration: none; white-space: nowrap;
    transition: background .15s;
  }
  .dl-btn:hover { background: #2a4a7f; }
  /* filter bar */
  .rep-filters { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 16px; align-items: center; }
  .rep-filters input, .rep-filters select {
    background: #1e1e1e; border: 1px solid #333; border-radius: 6px;
    color: #e0e0e0; padding: 6px 12px; font-size: 0.88rem; outline: none;
    transition: border-color .2s;
  }
  .rep-filters input:focus, .rep-filters select:focus { border-color: #5fa8ff; }
  .rep-filters input::placeholder { color: #555; }
  .date-btn-wrap {
    display: flex; align-items: center; gap: 6px;
    background: #1e1e1e; border: 1px solid #333; border-radius: 6px;
    color: #aaa; padding: 6px 10px; font-size: 0.88rem; cursor: pointer;
    transition: border-color .2s;
  }
  .date-btn-wrap:hover { border-color: #5fa8ff; }
  .date-btn-wrap input[type="date"] {
    background: none; border: none; outline: none; padding: 0; cursor: pointer;
    width: 20px; opacity: 0.6;
  }
  .date-btn-wrap input[type="date"]::-webkit-datetime-edit { display: none; }
  .date-btn-wrap input[type="date"]::-webkit-calendar-picker-indicator { cursor: pointer; }
  .date-val { color: #e0e0e0; font-size: 0.88rem; min-width: 0; }
  .rep-filters .btn-clear {
    background: #2a2a2a; border: 1px solid #444; border-radius: 6px;
    color: #aaa; padding: 6px 14px; font-size: 0.88rem; cursor: pointer;
    transition: background .15s;
  }
  .rep-filters .btn-clear:hover { background: #3a3a3a; }
  .pname a { color: inherit; text-decoration: none; }
  .pname a:hover { text-decoration: underline; }
</style>
</head>
<body>
<header>
  <h1>⚡ RL Custom Queue</h1>
  <nav>
    <a href="/">Leaderboard</a>
    <a href="/replay-test">Replay Tool</a>
    <a href="/admin">Admin</a>
  </nav>
</header>
<div class="container">

  <div class="tabs">
    <button class="tab-btn active" onclick="switchTab('parse',this)">Parse Replay</button>
    <button class="tab-btn"        onclick="switchTab('stored',this)">Stored Replays</button>
  </div>

  <div id="tab-parse" class="tab-pane active">
  <div class="card">
    <h2>Replay Parser — Test Tool</h2>
    <p>Upload a <code>.replay</code> file to inspect what the server sees. Optionally paste a
       match ID to run the full verification check against a live match.</p>

    <div id="drop-zone" onclick="document.getElementById('file-input').click()"
         ondragover="onDragOver(event)" ondragleave="onDragLeave(event)" ondrop="onDrop(event)">
      <p>Drop a <strong>.replay</strong> file here, or <span>click to browse</span></p>
      <div id="file-name"></div>
    </div>
    <input type="file" id="file-input" accept=".replay" onchange="onFileChosen(event)">

    <div class="opt-row">
      <input id="match-id-input" type="text" placeholder="Match ID (optional) — runs verify_replay against a live match">
    </div>

    <button id="parse-btn" disabled onclick="parseReplay()">Parse Replay</button>
    <button id="save-btn"  disabled onclick="saveReplay()"  style="background:#333;color:#666;border:1px solid transparent;border-radius:6px;padding:10px 18px;font-size:0.88rem;cursor:default;margin-top:14px;margin-left:8px;">⬆ Upload to Server</button>
    <div id="save-msg" style="display:none;margin-top:10px;font-size:0.88rem;"></div>
    <div id="spinner">⏳ Parsing…</div>
    <div class="err-msg" id="err-msg"></div>
  </div>

  <div id="result">
    <div class="card" id="card-header">
      <h2>Header</h2>
      <div class="kv-grid" id="kv-header"></div>
    </div>

    <div class="card" id="card-players">
      <h2>Players</h2>
      <table class="ptable">
        <thead><tr><th>Name</th><th>Team</th><th>Platform</th><th>Online ID</th></tr></thead>
        <tbody id="player-rows"></tbody>
      </table>
    </div>

    <div class="card" id="card-verify" style="display:none">
      <h2>Verification against match</h2>
      <div id="verify-content"></div>
    </div>
  </div>

  </div><!-- /tab-parse -->

  <div id="tab-stored" class="tab-pane">
    <div class="card">
      <h2>Stored Replays</h2>
      <p>All replays received by the server — from the match verifier and this test tool.</p>
      <div class="rep-filters">
        <input id="rep-filter-name" type="text" placeholder="Filter by player name" oninput="applyRepFilters()">
        <select id="rep-filter-mode" onchange="applyRepFilters()">
          <option value="">All modes</option>
          <option value="1v1">1s</option>
          <option value="2v2">2s</option>
          <option value="3v3">3s</option>
        </select>
        <select id="rep-filter-type" onchange="applyRepFilters()">
          <option value="">All types</option>
          <option value="private">Private</option>
          <option value="online">Online (Ranked / Casual)</option>
          <option value="tournament">Tournament</option>
        </select>
        <label class="date-btn-wrap">From <span id="rep-from-val" class="date-val"></span><input id="rep-filter-from" type="date" onchange="document.getElementById('rep-from-val').textContent=this.value?formatDate(this.value):''; applyRepFilters()"></label>
        <label class="date-btn-wrap">To <span id="rep-to-val" class="date-val"></span><input id="rep-filter-to"   type="date" onchange="document.getElementById('rep-to-val').textContent=this.value?formatDate(this.value):''; applyRepFilters()"></label>
        <button class="btn-clear" onclick="clearRepFilters()">Clear</button>
      </div>
      <div id="rep-spinner" style="display:none">⏳ Loading…</div>
      <div id="rep-empty"   style="display:none">No replays stored yet.</div>
      <div class="err-msg" id="rep-error" style="display:none"></div>
      <table class="rep-table">
        <thead>
          <tr>
            <th>Mode</th>
            <th><span style="color:#5fa8ff">B</span> – <span style="color:#ff7a33">O</span></th>
            <th>Date</th>
            <th style="color:#5fa8ff">🔵 Blue</th>
            <th style="color:#ff7a33">🟠 Orange</th>
            <th></th>
          </tr>
        </thead>
        <tbody id="rep-rows"></tbody>
      </table>
    </div>
  </div><!-- /tab-stored -->

</div>
<script>
let selectedFile = null;

function onDragOver(e) {
  e.preventDefault();
  document.getElementById('drop-zone').classList.add('drag-over');
}
function onDragLeave(e) {
  document.getElementById('drop-zone').classList.remove('drag-over');
}
function onDrop(e) {
  e.preventDefault();
  document.getElementById('drop-zone').classList.remove('drag-over');
  const f = e.dataTransfer.files[0];
  if (f) setFile(f);
}
function onFileChosen(e) {
  const f = e.target.files[0];
  if (f) setFile(f);
}
function setFile(f) {
  selectedFile = f;
  document.getElementById('file-name').textContent = f.name + ' (' + (f.size / 1024).toFixed(1) + ' KB)';
  document.getElementById('parse-btn').disabled = false;
  const sb = document.getElementById('save-btn');
  sb.disabled = false;
  sb.style.cssText = 'background:#0e3320;color:#4cff91;border:1px solid #1a5c38;border-radius:6px;padding:10px 18px;font-size:0.88rem;cursor:pointer;margin-top:14px;margin-left:8px;';
  document.getElementById('err-msg').textContent = '';
  document.getElementById('result').style.display = 'none';
}

function badge(text, cls) {
  return `<span class="badge ${cls}">${text}</span>`;
}
function teamLabel(t) {
  if (t === 0) return '<span style="color:#5fa8ff">Blue</span>';
  if (t === 1) return '<span style="color:#ff7a33">Orange</span>';
  return '<span style="color:#555">?</span>';
}

async function saveReplay() {
  if (!selectedFile) return;
  const btn = document.getElementById('save-btn');
  const msg = document.getElementById('save-msg');
  btn.disabled = true;
  btn.textContent = '⏳ Uploading…';
  msg.style.display = 'none';
  try {
    const ab = await selectedFile.arrayBuffer();
    const resp = await fetch('/replay/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: ab
    });
    const data = await resp.json();
    if (data.error) {
      msg.style.cssText = 'display:block;color:#ff5f5f;margin-top:10px;font-size:0.88rem;';
      msg.textContent = '✖ ' + data.error;
    } else {
      msg.style.cssText = 'display:block;color:#4cff91;margin-top:10px;font-size:0.88rem;';
      msg.textContent = `✔ Saved — ${data.filename} (${data.size_kb} KB)`;
    }
  } catch(e) {
    msg.style.cssText = 'display:block;color:#ff5f5f;margin-top:10px;font-size:0.88rem;';
    msg.textContent = '✖ Upload failed: ' + e.message;
  } finally {
    btn.disabled = false;
    btn.textContent = '⬆ Upload to Server';
  }
}

async function parseReplay() {
  if (!selectedFile) return;
  const btn = document.getElementById('parse-btn');
  btn.disabled = true;
  document.getElementById('spinner').style.display = 'block';
  document.getElementById('err-msg').textContent = '';
  document.getElementById('result').style.display = 'none';

  const matchId = document.getElementById('match-id-input').value.trim();
  let url = '/replay/parse';
  if (matchId) url += '?match_id=' + encodeURIComponent(matchId);

  try {
    const ab = await selectedFile.arrayBuffer();
    const resp = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: ab
    });
    const data = await resp.json();

    if (data.error) {
      document.getElementById('err-msg').textContent = '✖ ' + data.error;
      return;
    }

    // ── Header card ──
    const score = (data.score_blue ?? '?') + ' – ' + (data.score_orange ?? '?');
    const modeStr = data.mode || '';                          // "1v1" / "2v2" / "3v3"
    const typeStr = data.is_private ? 'Private' : (data.match_type || 'Online');
    const fullLabel = modeStr ? `${typeStr} ${modeStr}` : typeStr || 'Unknown';
    const modeBadge = data.is_private
      ? badge(fullLabel, 'ok')
      : (data.match_type ? badge(fullLabel, 'warn') : badge('Unknown', 'err'));
    document.getElementById('kv-header').innerHTML = `
      <div class="k">Mode</div>                              <div class="v">${modeBadge}</div>
      <div class="k">Score (Blue–Orange)</div>               <div class="v">${score}</div>
      <div class="k">Date (UTC)</div>                        <div class="v">${data.date || '—'}</div>
    `;

    // ── Players card ──
    const rows = (data.players || []).map(p => {
      const plat = p.is_epic
        ? '<span style="color:#9b7fd4">Epic</span>'
        : '<span style="color:#6dcff6">Steam</span>';
      const oid = p.is_epic ? '<span style="color:#555">—</span>' : p.online_id;
      return `<tr>
        <td>${p.name || '<span style="color:#555">?</span>'}</td>
        <td>${teamLabel(p.team)}</td>
        <td>${plat}</td>
        <td style="font-size:0.8rem;color:#888">${oid}</td>
      </tr>`;
    }).join('');
    document.getElementById('player-rows').innerHTML = rows ||
      '<tr><td colspan="4" style="color:#555;text-align:center">No players found</td></tr>';

    // ── Verify card ──
    const vc = document.getElementById('card-verify');
    const vcontent = document.getElementById('verify-content');
    if (data.verify) {
      vc.style.display = 'block';
      const v = data.verify;
      if (v.error) {
        vcontent.innerHTML = `<div class="verdict-box error-box"><strong>⚠ ${v.error}</strong></div>`;
      } else if (v.verdict === 'verified') {
        vcontent.innerHTML = `
          <div class="verdict-box verified">
            <strong>✅ Verified</strong>
            <div>Winners: ${(v.winner_ids||[]).join(', ') || '—'}</div>
            <div>Losers:  ${(v.loser_ids||[]).join(', ')  || '—'}</div>
            <div style="margin-top:6px;color:#aaa">Score: Blue ${v.score0} – Orange ${v.score1}</div>
          </div>`;
      } else {
        vcontent.innerHTML = `
          <div class="verdict-box unverif">
            <strong>⚠ Unverifiable</strong>
            <div>${v.reason || ''}</div>
          </div>`;
      }
    } else {
      vc.style.display = 'none';
    }

    document.getElementById('result').style.display = 'block';
  } catch(e) {
    document.getElementById('err-msg').textContent = '✖ Request failed: ' + e.message;
  } finally {
    btn.disabled = false;
    document.getElementById('spinner').style.display = 'none';
  }
}

// ── Tab switching ────────────────────────────────────────────────────────────
function switchTab(name, btn) {
  document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  btn.classList.add('active');
  document.querySelector('.container').classList.toggle('wide', name === 'stored');
  if (name === 'stored') loadStoredReplays();
}

// ── Stored Replays tab ───────────────────────────────────────────────────────
let allReplays = [];

function fmtRepDate(d) {
  if (!d) return '—';
  const [datePart, timePart] = d.split(' ');
  if (!datePart || !timePart) return d;
  const [y, mo, day] = datePart.split('-');
  const parts = timePart.split('-');
  return `${parts[0]}:${parts[1]} - ${day}-${mo}-${y}`;
}

function pname(e) { return typeof e === 'object' ? (e.name || '?') : e; }

function renderRepRow(r) {
  const modeStr  = r.mode || '';
  const typeStr  = r.is_private ? 'Private' : (r.match_type || 'Online');
  const fullMode = modeStr ? `${typeStr} ${modeStr}` : (typeStr || '?');
  const modeCls  = r.is_private ? 'ok' : (r.match_type ? 'warn' : 'err');
  const modeBadge = `<span class="badge ${modeCls}">${fullMode}</span>`;

  const sb = r.score_blue  != null ? r.score_blue  : '?';
  const so = r.score_orange != null ? r.score_orange : '?';
  const score = `<span style="color:#5fa8ff">${sb}</span> – <span style="color:#ff7a33">${so}</span>`;


  const blue   = (r.players_blue   || []);
  const orange = (r.players_orange || []);

  const blueCell   = blue.length   ? blue.map(e   => {
    const n = pname(e);
    return `<div class="pname blue-name"><a href="/?q=${encodeURIComponent(n)}" title="Search ${n} on leaderboard">${n}</a></div>`;
  }).join('') : '<span style="color:#555">—</span>';
  const orangeCell = orange.length ? orange.map(e => {
    const n = pname(e);
    return `<div class="pname orange-name"><a href="/?q=${encodeURIComponent(n)}" title="Search ${n} on leaderboard">${n}</a></div>`;
  }).join('') : '<span style="color:#555">—</span>';

  const dlUrl = `/replay/download/${encodeURIComponent(r.source)}/${encodeURIComponent(r.filename)}`;

  return `<tr>
    <td>${modeBadge}</td>
    <td>${score}</td>
    <td style="white-space:nowrap">${fmtRepDate(r.date)}</td>
    <td class="players-cell">${blueCell}</td>
    <td class="players-cell">${orangeCell}</td>
    <td><a href="${dlUrl}" download="${r.filename}" class="dl-btn" title="${r.filename}">⬇</a></td>
  </tr>`;
}

function applyRepFilters() {
  const nameQ   = (document.getElementById('rep-filter-name').value || '').toLowerCase().trim();
  const modeQ   = (document.getElementById('rep-filter-mode').value || '').toLowerCase();
  const typeQ   = (document.getElementById('rep-filter-type').value || '').toLowerCase();
  const fromQ   = document.getElementById('rep-filter-from').value;  // "YYYY-MM-DD"
  const toQ     = document.getElementById('rep-filter-to').value;
  const empty   = document.getElementById('rep-empty');
  const tbody   = document.getElementById('rep-rows');

  let filtered = allReplays.filter(r => {
    // player name filter
    if (nameQ) {
      const allPlayers = [...(r.players_blue||[]), ...(r.players_orange||[])];
      if (!allPlayers.some(e => pname(e).toLowerCase().includes(nameQ))) return false;
    }
    // mode filter (1s/2s/3s)
    if (modeQ) {
      if (!(r.mode || '').toLowerCase().includes(modeQ)) return false;
    }
    // type filter (private / online / tournament)
    if (typeQ) {
      const mt = (r.match_type || '').toLowerCase();
      if (typeQ === 'private'    && !mt.includes('private'))    return false;
      if (typeQ === 'online'     && !mt.includes('online'))     return false;
      if (typeQ === 'tournament' && !mt.includes('tournament')) return false;
    }
    // date filter — type="date" gives YYYY-MM-DD, r.date is "YYYY-MM-DD HH-MM-SS"
    if (fromQ || toQ) {
      const dateOnly = r.date ? r.date.split(' ')[0] : null;
      if (!dateOnly) return false;
      if (fromQ && dateOnly < fromQ) return false;
      if (toQ   && dateOnly > toQ)   return false;
    }
    return true;
  });

  if (filtered.length === 0) {
    tbody.innerHTML = '';
    empty.textContent = nameQ || modeQ || fromQ || toQ ? 'No replays match the current filters.' : 'No replays stored yet.';
    empty.style.display = 'block';
  } else {
    empty.style.display = 'none';
    tbody.innerHTML = filtered.map(renderRepRow).join('');
  }
}

function formatDate(iso) {
  // YYYY-MM-DD → DD/MM/YYYY
  const [y, m, d] = iso.split('-');
  return `${d}/${m}/${y}`;
}
function clearRepFilters() {
  document.getElementById('rep-filter-name').value = '';
  document.getElementById('rep-filter-mode').value = '';
  document.getElementById('rep-filter-type').value = '';
  document.getElementById('rep-filter-from').value = '';
  document.getElementById('rep-filter-to').value   = '';
  document.getElementById('rep-from-val').textContent = '';
  document.getElementById('rep-to-val').textContent   = '';
  applyRepFilters();
}

async function loadStoredReplays() {
  const spinner = document.getElementById('rep-spinner');
  const empty   = document.getElementById('rep-empty');
  const tbody   = document.getElementById('rep-rows');
  spinner.style.display = 'block';
  empty.style.display   = 'none';
  tbody.innerHTML       = '';

  try {
    const resp = await fetch('/replay/list');
    const data = await resp.json();
    allReplays = data.replays || [];
    spinner.style.display = 'none';
    applyRepFilters();
  } catch(e) {
    spinner.style.display = 'none';
    document.getElementById('rep-error').textContent = '✖ Failed to load: ' + e.message;
    document.getElementById('rep-error').style.display = 'block';
  }
}
</script>
</body>
</html>""")
