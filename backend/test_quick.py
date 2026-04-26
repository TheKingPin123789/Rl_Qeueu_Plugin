"""
Quick automated test — runs through the full match flow for two fake players.
Just run:  python test_quick.py
"""
import requests, time, sys

BASE = "http://localhost:9000"
PASS = "\033[92m PASS\033[0m"
FAIL = "\033[91m FAIL\033[0m"
results = []

def check(label, got, expected):
    ok = got == expected
    tag = PASS if ok else FAIL
    print(f"{tag}  {label}  (got: {got!r})")
    results.append(ok)
    return ok

def post(path, body):
    try:
        return requests.post(BASE + path, json=body, timeout=5).json()
    except Exception as e:
        return {"error": str(e)}

def get(path):
    try:
        return requests.get(BASE + path, timeout=5).json()
    except Exception as e:
        return {"error": str(e)}

def post_bytes(path, data):
    try:
        return requests.post(BASE + path, data=data,
                             headers={"Content-Type": "application/octet-stream"},
                             timeout=10).json()
    except Exception as e:
        return {"error": str(e)}

print("\n─── Server health ───────────────────────────────")
h = get("/health")
if not check("Server online", h.get("status"), "ok"):
    print("  → Start the server first:  python main.py")
    sys.exit(1)

print("\n─── Register players ────────────────────────────")
post("/player/register", {"player_id": "p1", "real_id": "76561198000000001",
                          "username": "TestP1", "mmr": 1000, "mode": "1s"})
post("/player/register", {"player_id": "p2", "real_id": "76561198000000002",
                          "username": "TestP2", "mmr": 1010, "mode": "1s"})
print("  (registration errors are fine if players already exist)")

print("\n─── TEST A: Clean win/loss consensus ────────────")
# Both join queue
r = post("/queue/join", {"player_id": "p1", "real_id": "76561198000000001",
                         "username": "TestP1", "region": "EU", "mode": "1s"})
check("P1 joins queue", r.get("status"), "queued")

time.sleep(1)  # let matchmaker wake

r = post("/queue/join", {"player_id": "p2", "real_id": "76561198000000002",
                         "username": "TestP2", "region": "EU", "mode": "1s"})
check("P2 joins queue", r.get("status"), "queued")

# Wait for matchmaker
time.sleep(3)

# Find the match via debug endpoint
state = get("/debug/state")
match_ids = list(state.get("matches", {}).keys())
if not match_ids:
    print(f"{FAIL}  No match formed after 3s — check server logs")
    results.append(False)
    sys.exit(1)

mid = match_ids[0]
print(f"  Match formed: {mid}")

# Accept
r = post("/match/accept", {"player_id": "p1", "match_id": mid})
check("P1 accepts", r.get("status"), "accepted")
r = post("/match/accept", {"player_id": "p2", "match_id": mid})
check("P2 accepts", r.get("status"), "accepted")

# Lobby ready (p1 is host)
r = post("/match/lobby_ready", {"player_id": "p1", "match_id": mid})
check("Lobby ready", r.get("status"), "lobby_ready")

# Both submit matching results
r = post("/match/result", {"player_id": "p1", "match_id": mid, "outcome": "win"})
check("P1 submits win — waiting", r.get("status"), "recorded")
r = post("/match/result", {"player_id": "p2", "match_id": mid, "outcome": "loss"})
check("P2 submits loss — awarded", r.get("status"), "awarded")

# MMR should have changed
time.sleep(1)
h1 = get("/player/p1/mmr")
h2 = get("/player/p2/mmr")
check("P1 MMR increased", float(h1.get("mmr_1s", 0)) > 1000, True)
check("P2 MMR decreased", float(h2.get("mmr_1s", 9999)) < 1010, True)

print("\n─── TEST B: Conflict → replay collection ─────────")
# Join again
r1 = post("/queue/join", {"player_id": "p1", "real_id": "76561198000000001",
                          "username": "TestP1", "region": "EU", "mode": "1s"})
time.sleep(1)
r2 = post("/queue/join", {"player_id": "p2", "real_id": "76561198000000002",
                          "username": "TestP2", "region": "EU", "mode": "1s"})
time.sleep(3)

state = get("/debug/state")
match_ids = list(state.get("matches", {}).keys())
if not match_ids:
    print(f"{FAIL}  No match formed for test B")
    results.append(False)
else:
    mid2 = match_ids[0]
    print(f"  Match formed: {mid2}")

    post("/match/accept",    {"player_id": "p1", "match_id": mid2})
    post("/match/accept",    {"player_id": "p2", "match_id": mid2})
    post("/match/lobby_ready", {"player_id": "p1", "match_id": mid2})

    # Both say they won → conflict for 1s (100% threshold)
    post("/match/result", {"player_id": "p1", "match_id": mid2, "outcome": "win"})
    r = post("/match/result", {"player_id": "p2", "match_id": mid2, "outcome": "win"})
    check("Conflicting votes → conflict status", r.get("status"), "conflict")

    # Simulate replay upload (tiny fake bytes — will be "unverifiable" but tests the endpoint)
    fake_replay = b"\x00" * 5000   # too small/invalid → unverifiable
    r1 = post_bytes(f"/match/upload_replay/{mid2}?player_id=p1", fake_replay)
    check("P1 upload → unverifiable (invalid file)", r1.get("status"), "unverifiable")

    r2 = post_bytes(f"/match/upload_replay/{mid2}?player_id=p2", fake_replay)
    # Both tried with unverifiable files → pending_review
    check("P2 upload → pending_review (all tried, no majority)", r2.get("status"), "pending_review")

print("\n─── TEST C: Duplicate upload blocked ────────────")
# Try uploading again for same player on resolved match
r = post_bytes(f"/match/upload_replay/{mid2}?player_id=p1", fake_replay)
check("Re-upload returns already_resolved", r.get("status"), "already_resolved")

print("\n─────────────────────────────────────────────────")
passed = sum(results)
total  = len(results)
color  = "\033[92m" if passed == total else "\033[91m"
print(f"{color}{passed}/{total} tests passed\033[0m\n")
