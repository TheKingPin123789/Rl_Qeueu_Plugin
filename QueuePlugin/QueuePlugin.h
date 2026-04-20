#pragma once
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/pluginsettingswindow.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>

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
    int  selectedRegion = 0;
    int  selectedMode   = 0;
    bool inQueue        = false;

    // match state
    bool        matchFound     = false;
    bool        isHost         = false;
    std::string matchID        = "";
    std::string lobbyName      = "";
    std::string lobbyPassword  = "";
    float       acceptTimer    = 0.0f;

    // player
    // playerID  = permanent BakkesMod install ID (file-based, survives account switches)
    // realID    = current RL account's Epic/Steam ID (session only, updated on each login)
    std::string playerID    = "";
    std::string realID      = "";
    std::string queueStatus = "Not in queue";

    // replay
    std::string replayWatchPath = "";
    bool        watchingReplay  = false;

    // server
    std::string serverHost = "127.0.0.1";
    int         serverPort = 8000;

    // UI
    void RenderQueueUI();
    void RenderMatchFoundUI();

    // queue actions
    void JoinQueue();
    void LeaveQueue();

    // match
    void StartPolling();
    void PollOnce();
    void OnMatchFound(const std::string& response);
    void AcceptMatch();
    void DeclineMatch();

    // replay
    void HookMatchEnd();
    void UploadNewestReplay();
    std::string FindNewestReplay();

    // HTTP
    std::string HttpPost(const std::string& path, const std::string& body);
    std::string HttpGet(const std::string& path);
    void        HttpPostAsync(const std::string& path, const std::string& body,
                              std::function<void(std::string)> callback);
    void        HttpGetAsync(const std::string& path,
                             std::function<void(std::string)> callback);

    // JSON helpers
    std::string JsonStr(const std::string& json, const std::string& key);
    bool        JsonBool(const std::string& json, const std::string& key);

    // match players (real IDs returned by server for party invites)
    std::vector<std::string> matchRealIDs;

    // misc
    void FetchRealID();
    void SendPartyInvites();
    void CopyToClipboard(const std::string& text);
};
