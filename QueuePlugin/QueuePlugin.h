#pragma once
#include <windows.h>   // must come first — defines DWORD and other Win32 types
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/pluginsettingswindow.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>

// ── server config (hard-coded) ─────────────────────────────────────────────────
static const std::string SERVER_HOST             = "rlcustomranked.com";
static const int         SERVER_PORT             = 443;
static const std::string SERVER_WEBSITE          = "https://rlcustomranked.com";
static const int         REPLAY_COLLECTION_WINDOW = 180;  // seconds after result to upload

struct ReportEntry {
    int         id;
    std::string matchId;
    std::string reporterUsername;
    std::string submittedAt;
    std::string status;
};

class QueuePlugin : public BakkesMod::Plugin::BakkesModPlugin,
                    public BakkesMod::Plugin::PluginSettingsWindow,
                    public BakkesMod::Plugin::PluginWindow
{
public:
    void onLoad() override;
    void onUnload() override;

    // PluginSettingsWindow
    void RenderSettings() override;
    std::string GetPluginName() override;
    void SetImGuiContext(uintptr_t ctx) override;

    // PluginWindow
    void Render() override;
    std::string GetMenuName() override;
    std::string GetMenuTitle() override;
    bool ShouldBlockInput() override;
    bool IsActiveOverlay() override;
    void OnOpen() override;
    void OnClose() override;

private:
    // ── queue state ───────────────────────────────────────────────────────────
    int  selectedRegion  = 0;
    int  selectedMode    = 0;
    bool inQueue         = false;
    bool hasPriority     = false;    // victim of a decline — gets front-of-queue

    // ── match state ───────────────────────────────────────────────────────────
    bool        matchFound         = false;
    bool        isHost             = false;
    bool        myAccepted         = false;
    bool        allAccepted        = false;
    bool        lobbyReady         = false;  // host has created the lobby
    int         acceptedCount      = 0;
    int         totalPlayers       = 0;
    int         matchTimeRemaining = 30;
    int         myTeamIndex        = -1;     // 0 = Blue, 1 = Orange (server-assigned)
    std::string matchID            = "";
    std::string lobbyName          = "";
    std::string lobbyPassword      = "";

    // ── result reporting (manual buttons) ────────────────────────────────────
    // Players press Win / Loss / Draw after finishing the private match.
    // No game hooks are used — this is the entire result detection system.
    bool        outcomeSent        = false;  // player has pressed a button
    bool        outcomeConfirm     = false;  // confirmation dialog open
    std::string pendingOutcome     = "";     // "win" / "loss" / "draw" before confirm
    std::string outcomeStatus      = "";     // feedback from server
    int         drawCountdown      = -1;     // seconds until auto-draw (-1 = not started)
    bool        awaitingReplay     = false;  // server flagged no-majority → need replay
    int         pollEpoch         = 0;      // incremented each time a new poll chain starts; old chains bail on mismatch
    time_t      matchFoundTime       = 0;      // when match was found — used to filter replays
    time_t      lobbyReadyTime       = 0;      // when lobbyReady first became true — elapsed timer
    time_t      collectionEndsAt     = 0;      // server deadline for replay uploads (unix epoch)

    // ── forfeit ───────────────────────────────────────────────────────────────
    bool myForfeited          = false;
    bool forfeitConfirmPending = false;   // shared by both normal and conflict UI

    // ── player ────────────────────────────────────────────────────────────────
    // Per-account data stored in config, keyed by Steam64 ID.
    // Lets players switch accounts without losing their registration or cached MMR.
    struct AccountData {
        std::string displayName;         // chosen queue username
        std::string platformDisplayName; // in-game name from Steam/Epic (for replay verification)
        std::string mmr1s, mmr2s, mmr3s;
    };
    std::map<std::string, AccountData> accounts;

    // systemID      = permanent BakkesMod install ID (file-based, survives account switches)
    // playerID        = Steam64 or Epic account ID of the account currently logged into RL
    // activeAccountID = playerID that was active when config was last written —
    //                   used as the map key to detect and handle account switches
    // displayName   = chosen queue username for the active account
    std::string systemID             = "";
    std::string playerID               = "";
    std::string activeAccountID      = "";
    std::string platform             = "";  // "Steam" | "Epic" | "" (unknown/fallback)
    std::string displayName          = "";
    std::string platformDisplayName  = "";  // in-game name from Steam/Epic (for replay verification)
    std::string queueStatus   = "Not in queue";
    bool        registering      = false;
    bool        changingUsername = false;  // Epic user clicked "Change" on their auto-filled name
    bool        pluginEnabled    = false;  // default OFF — user must enable explicitly
    time_t      queueStartTime = 0;
    char        usernameInputBuf[32] = {};

    // ── ratings ───────────────────────────────────────────────────────────────
    std::string mmr1s = "";
    std::string mmr2s = "";
    std::string mmr3s = "";

    // ── replay watcher ────────────────────────────────────────────────────────
    std::atomic<bool> replayWatchActive{false};

    // ── dispute reporting ────────────────────────────────────────────────────
    std::string lastMatchID   = "";
    bool        reportPending = false;
    bool        reportSent    = false;
    std::string reportStatus  = "";
    time_t      lastMatchTimestamp = 0;

    // ── report replay picker ──────────────────────────────────────────────────
    bool reportPanelOpen   = false;
    bool replayPickerBusy  = false;
    char reportReplayBuf[512] = {};

    // ── replay path (user-configurable) ──────────────────────────────────────
    std::string replayPath         = "";
    char        replayPathBuf[512] = {};

    // ── match history (last 10) ───────────────────────────────────────────────
    struct MatchHistoryEntry {
        std::string matchId, mode, region, outcome, replayStatus;
        bool        won;
        float       mmrChange;
        time_t      timestamp;
    };
    std::vector<MatchHistoryEntry> matchHistory;
    bool historyFetching = false;

    // ── server status ─────────────────────────────────────────────────────────
    bool serverOnline        = false;
    bool serverChecked       = false;
    bool serverCheckStarted  = false;  // set at entry of CheckServerStatus — one-shot guard
    bool pollServerStarted   = false;  // ensures exactly one PollServerStatus loop runs
    int  totalOnline         = 0;

    // ── RL matchmaking state (used to warn player, no hooks needed) ───────────
    bool inRankedQueue = false;

    // Shared across all async threads — outlives `this` so threads never
    // touch freed memory.  Set to false in onUnload; every callback checks it.
    std::shared_ptr<std::atomic<bool>> pluginAlive =
        std::make_shared<std::atomic<bool>>(true);

    // ── mini window ───────────────────────────────────────────────────────────
    bool showMiniWindow      = false;
    bool overlayRegistered   = false;  // togglemenu called exactly once

    // ── admin ─────────────────────────────────────────────────────────────────
    bool                     adminUnlocked      = false;
    bool                     showAdminWindow    = false;
    char                     adminPassBuf[64]   = {};
    int                      adminAttempts      = 0;    // failed console login attempts
    time_t                   adminCooldownUntil = 0;    // epoch when lockout expires
    std::vector<ReportEntry> adminReports;
    bool                     adminFetching      = false;
    std::string              adminStatus        = "";

    // ── UI ────────────────────────────────────────────────────────────────────
    void RenderQueueUI(bool compact = false);  // compact=true omits dispute section
    void RenderMatchFoundUI();
    void RenderLinkUI();
    void RenderAdminUI();

    // ── queue actions ─────────────────────────────────────────────────────────
    void JoinQueue();
    void LeaveQueue();

    // ── match ─────────────────────────────────────────────────────────────────
    void SendHeartbeat();
    void OnMatchFound(const std::string& response);
    void AcceptMatch();
    void DeclineMatch();
    void PollMatchStatus(int epoch = -1);
    void CancelMatchLocally(const std::string& reason);
    void NotifyLobbyReady();
    void ForfeitMatch();

    // ── result (manual) ───────────────────────────────────────────────────────
    void SubmitOutcome(const std::string& outcome);  // "win" / "loss" / "draw"
    void UploadReplayForVerification();              // called by replay watcher or manually
    void StartReplayWatcher();                       // background thread: auto-upload when conflict

    // ── history ───────────────────────────────────────────────────────────────
    void FetchHistory();

    // ── account ───────────────────────────────────────────────────────────────
    void ApplyAccountSetup(std::shared_ptr<std::atomic<bool>> alive, int attempt);
    void LookupAccountByRealID();
    void RegisterWithServer();
    void FetchMMR();

    // ── server ────────────────────────────────────────────────────────────────
    void CheckServerStatus();
    void PollServerStatus();

    // ── config ────────────────────────────────────────────────────────────────
    void LoadConfig();
    void SaveConfig();

    // ── dispute replay ────────────────────────────────────────────────────────
    void ReportMatch();
    void BrowseReplayAsync();
    std::string FindNewestReplay(time_t minTime = 0);

    // ── admin ─────────────────────────────────────────────────────────────────
    void TryAdminLogin(const std::string& password);
    void FetchAdminReports();
    void AdminAcceptMatch(const std::string& matchId);
    void AdminCancelMatch(const std::string& matchId);

    // ── helpers ───────────────────────────────────────────────────────────────
    // Returns the Steam64 or Epic account ID to use as player_id in server
    // API calls.  Falls back to the BakkesMod install ID (systemID) only during
    // startup before the account ID has been resolved.
    std::string ServerID() const {
        return playerID.empty() ? systemID : playerID;
    }

    // ── SSE ───────────────────────────────────────────────────────────────────
    std::atomic<bool> sseActive{false};   // set true while SSE thread should run
    void StartSSE();
    void StopSSE();
    void SSELoop(std::shared_ptr<std::atomic<bool>> alive);
    void HandleSSEEvent(const std::string& json);

    // ── HTTP ──────────────────────────────────────────────────────────────────
    std::string HttpPost(const std::string& path, const std::string& body,
                         DWORD timeoutMs = 8000);
    std::string HttpGet(const std::string& path, DWORD timeoutMs = 8000);
    void        HttpPostAsync(const std::string& path, const std::string& body,
                              std::function<void(std::string)> callback,
                              DWORD timeoutMs = 8000);
    void        HttpGetAsync(const std::string& path,
                             std::function<void(std::string)> callback,
                             DWORD timeoutMs = 8000);

    // ── JSON helpers ──────────────────────────────────────────────────────────
    std::string JsonStr(const std::string& json, const std::string& key);
    bool        JsonBool(const std::string& json, const std::string& key);
    std::string JsonNum(const std::string& json, const std::string& key);

    // ── misc ──────────────────────────────────────────────────────────────────
    void FetchRealID();
};
