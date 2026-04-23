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
    // queue state
    int  selectedRegion  = 0;
    int  selectedMode    = 0;
    bool inQueue         = false;
    int  queueCount      = 0;   // players in same region+mode queue (from heartbeat)
    int  queuePosition   = 0;   // our position in the queue (1 = next to match)
    bool hasPriority     = false; // we were a victim of a decline — gets front-of-queue spot

    // match state
    bool        matchFound         = false;
    bool        isHost             = false;
    bool        myAccepted         = false;
    bool        allAccepted        = false;
    bool        lobbyReady         = false;
    int         acceptedCount      = 0;
    int         totalPlayers       = 0;
    int         matchTimeRemaining = 30;
    int         myTeamIndex        = -1;  // 0 = Blue, 1 = Orange (assigned by server)
    std::string matchID            = "";
    std::string lobbyName          = "";
    std::string lobbyPassword      = "";

    // forfeit state
    bool myForfeited      = false;   // we have pressed Forfeit this match
    int  drawCountdown    = -1;      // seconds until auto-draw (-1 = not started)

    // player
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

    // ratings
    std::string mmr1s = "";
    std::string mmr2s = "";
    std::string mmr3s = "";

    // match players
    std::vector<std::string> matchRealIDs;

    // reporting
    std::string lastMatchID   = "";
    bool        reportPending = false;
    bool        reportSent    = false;
    std::string reportStatus  = "";

    // match history (last 10 fetched from server)
    struct MatchHistoryEntry {
        std::string matchId, mode, region, outcome;
        bool        won;
        float       mmrChange;
        time_t      timestamp;
    };
    std::vector<MatchHistoryEntry> matchHistory;
    bool historyFetching = false;

    // server status
    bool serverOnline    = false;
    bool serverChecked   = false;
    int  totalOnline     = 0;   // players currently searching across all modes/regions

    // set to false in onUnload — all async callbacks bail out immediately if false,
    // preventing use-after-free when detached HTTP threads outlive the plugin
    std::atomic<bool> pluginAlive { true };

    // mini window
    bool showMiniWindow = false;

    // game state
    bool   inGame             = false;
    bool   inNormalGame       = false;   // in a ranked/casual/casual game (not our queue)
    bool   inRankedQueue      = false;   // RL matchmaking search is active
    time_t lastMatchTimestamp = 0;
    bool   resultSubmitted    = false;   // auto result already sent for this match
    int    myTeamNum          = -1;      // cached at StartRound — car/PRI objects are alive then

    // goal tracking (tamper detection)
    int  trackedScore0  = 0;
    int  trackedScore1  = 0;
    bool scoreTampered  = false;

    // report replay picker
    bool reportPanelOpen   = false;      // user has clicked "Report" — show sub-panel
    bool replayPickerBusy  = false;      // file dialog is open in background thread
    char reportReplayBuf[512] = {};      // path of replay chosen for the current report

    // replay path (user-configurable)
    std::string replayPath         = "";
    char        replayPathBuf[512] = {};

    // admin
    bool                     adminUnlocked    = false;
    char                     adminPassBuf[64] = {};
    std::vector<ReportEntry> adminReports;
    bool                     adminFetching    = false;
    std::string              adminStatus      = "";

    // UI
    void RenderQueueUI();
    void RenderMatchFoundUI();
    void RenderLinkUI();
    void RenderAdminUI();

    // queue actions
    void JoinQueue();
    void LeaveQueue();

    // match
    void StartPolling();
    void SendHeartbeat();
    void OnMatchFound(const std::string& response);
    void AcceptMatch();
    void DeclineMatch();
    void PollMatchStatus();
    void CancelMatchLocally(const std::string& reason);
    void ConfirmLobbyJoined();
    void NotifyLobbyReady();
    void ForfeitMatch();
    void FetchHistory();

    // account
    void RegisterWithServer();
    void FetchMMR();

    // server
    void CheckServerStatus();
    void PollServerStatus();

    // config
    void LoadConfig();
    void SaveConfig();

    // replay / result
    void HookMatchEnd();
    void ReportMatch();
    void SubmitMatchResult(bool won, int score0, int score1, bool tampered);
    void BrowseReplayAsync();
    std::string FindNewestReplay();

    // admin
    void FetchAdminReports();
    void AdminAcceptMatch(const std::string& matchId);
    void AdminCancelMatch(const std::string& matchId);

    // HTTP
    // timeoutMs controls each WinHTTP phase; use a larger value for long-poll calls
    std::string HttpPost(const std::string& path, const std::string& body,
                         DWORD timeoutMs = 8000);
    std::string HttpGet(const std::string& path, DWORD timeoutMs = 8000);
    void        HttpPostAsync(const std::string& path, const std::string& body,
                              std::function<void(std::string)> callback,
                              DWORD timeoutMs = 8000);
    void        HttpGetAsync(const std::string& path,
                             std::function<void(std::string)> callback,
                             DWORD timeoutMs = 8000);

    // JSON helpers
    std::string JsonStr(const std::string& json, const std::string& key);
    bool        JsonBool(const std::string& json, const std::string& key);
    std::string JsonNum(const std::string& json, const std::string& key);

    // misc
    void FetchRealID();
    void SendPartyInvites();
    void CopyToClipboard(const std::string& text);
};
