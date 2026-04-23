#pragma once
#include <windows.h>   // must come first — defines DWORD and other Win32 types
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/pluginsettingswindow.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>

// ── server config (hard-coded) ─────────────────────────────────────────────────
static const std::string SERVER_HOST    = "46.101.184.78";
static const int         SERVER_PORT    = 8000;
static const std::string SERVER_WEBSITE = "http://46.101.184.78:8000";

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
    int  queueCount      = 0;        // players in same region+mode queue
    int  queuePosition   = 0;        // our position (1 = next)
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

    // ── forfeit ───────────────────────────────────────────────────────────────
    bool myForfeited = false;

    // ── player ────────────────────────────────────────────────────────────────
    // playerID    = permanent BakkesMod install ID (file-based, survives account switches)
    // realID      = current RL account Epic/Steam ID (session only, stored privately on server)
    // displayName = chosen username
    std::string playerID    = "";
    std::string realID      = "";
    std::string displayName = "";
    std::string queueStatus = "Not in queue";
    bool        registering    = false;
    time_t      queueStartTime = 0;
    char        usernameInputBuf[32] = {};

    // ── ratings ───────────────────────────────────────────────────────────────
    std::string mmr1s = "";
    std::string mmr2s = "";
    std::string mmr3s = "";

    // ── match players (for party invites) ────────────────────────────────────
    std::vector<std::string> matchRealIDs;

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
        std::string matchId, mode, region, outcome;
        bool        won;
        float       mmrChange;
        time_t      timestamp;
    };
    std::vector<MatchHistoryEntry> matchHistory;
    bool historyFetching = false;

    // ── server status ─────────────────────────────────────────────────────────
    bool serverOnline    = false;
    bool serverChecked   = false;
    int  totalOnline     = 0;

    // ── RL matchmaking state (used to warn player, no hooks needed) ───────────
    bool inRankedQueue = false;

    // set to false in onUnload — all async callbacks bail out immediately if false
    std::atomic<bool> pluginAlive { true };

    // ── mini window ───────────────────────────────────────────────────────────
    bool showMiniWindow = false;

    // ── admin ─────────────────────────────────────────────────────────────────
    bool                     adminUnlocked    = false;
    char                     adminPassBuf[64] = {};
    std::vector<ReportEntry> adminReports;
    bool                     adminFetching    = false;
    std::string              adminStatus      = "";

    // ── UI ────────────────────────────────────────────────────────────────────
    void RenderQueueUI();
    void RenderMatchFoundUI();
    void RenderLinkUI();
    void RenderAdminUI();

    // ── queue actions ─────────────────────────────────────────────────────────
    void JoinQueue();
    void LeaveQueue();

    // ── match ─────────────────────────────────────────────────────────────────
    void StartPolling();
    void SendHeartbeat();
    void OnMatchFound(const std::string& response);
    void AcceptMatch();
    void DeclineMatch();
    void PollMatchStatus();
    void CancelMatchLocally(const std::string& reason);
    void NotifyLobbyReady();
    void ForfeitMatch();

    // ── result (manual) ───────────────────────────────────────────────────────
    void SubmitOutcome(const std::string& outcome);  // "win" / "loss" / "draw"

    // ── history ───────────────────────────────────────────────────────────────
    void FetchHistory();

    // ── account ───────────────────────────────────────────────────────────────
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
    std::string FindNewestReplay();

    // ── admin ─────────────────────────────────────────────────────────────────
    void FetchAdminReports();
    void AdminAcceptMatch(const std::string& matchId);
    void AdminCancelMatch(const std::string& matchId);

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
    void SendPartyInvites();
    void CopyToClipboard(const std::string& text);
};
