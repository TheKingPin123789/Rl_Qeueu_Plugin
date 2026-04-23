#include "QueuePlugin.h"   // pulls in <windows.h> first
#include "imgui/imgui.h"
#include <shellapi.h>
#include <winhttp.h>
#include <thread>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <random>
#include <atomic>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#include <commdlg.h>

BAKKESMOD_PLUGIN(QueuePlugin, "RL Custom Queue", "0.1", PLUGINTYPE_FREEPLAY)

static std::string rand_str(int n)
{
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(0, (int)(sizeof(chars) - 2));
    std::string result;
    result.reserve(n);
    for (int i = 0; i < n; i++)
        result += chars[dist(rng)];
    return result;
}

static int SafeStoi(const std::string& s, int def = 0)
{
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

static const char* REGIONS[] = { "NAE", "NAW", "EU", "OCE", "SAM", "ME", "ASIA" };
static const char* MODES[]   = { "1v1 (1s)", "2v2 (2s)", "3v3 (3s)" };
static const char* MODE_IDS[]= { "1s", "2s", "3s" };

// ── lifecycle ──────────────────────────────────────────────────────────────────
void QueuePlugin::onLoad()
{
    // Permanent BakkesMod install ID
    std::string idFile = gameWrapper->GetBakkesModPath().string() + "\\plugins\\rlcq_id.txt";
    std::ifstream in(idFile);
    if (in.is_open()) { std::getline(in, playerID); in.close(); }
    if (playerID.empty()) {
        playerID = "bm_" + rand_str(12);
        std::ofstream out(idFile);
        out << playerID;
    }

    LoadConfig();

    // If already registered, silently re-sync with server
    if (!displayName.empty()) {
        gameWrapper->SetTimeout([this](GameWrapper* gw) {
            FetchRealID();
            RegisterWithServer();
        }, 2.0f);
    }

    // Fetch real ID and clear any stale in-game flags when the main menu loads
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_MainMenu_TA.MainMenuAdded",
        [this](std::string) {
            gameWrapper->Execute([this](GameWrapper* gw) {
                FetchRealID();
                inRankedQueue = false;
            });
        });

    // Track whether RL ranked/casual matchmaking search is active
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_Matchmaking_TA.EventSearchStarted",
        [this](std::string) {
            gameWrapper->Execute([this](GameWrapper* gw) { inRankedQueue = true; });
        });
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_Matchmaking_TA.EventSearchCanceled",
        [this](std::string) {
            gameWrapper->Execute([this](GameWrapper* gw) { inRankedQueue = false; });
        });

    CheckServerStatus();
    PollServerStatus();

    // Activate the overlay so Render() is called every frame.
    // Render() returns immediately when showMiniWindow is false.
    gameWrapper->SetTimeout([this](GameWrapper*) {
        cvarManager->executeCommand("togglemenu rlcustomqueue", false);
    }, 0.5f);
}

void QueuePlugin::onUnload()
{
    pluginAlive = false;
    if (inQueue) {
        std::string body = "{\"player_id\":\"" + playerID + "\"}";
        HttpPost("/queue/leave", body);
    }
}

// ── PluginSettingsWindow ───────────────────────────────────────────────────────
std::string QueuePlugin::GetPluginName() { return "RL Custom Queue"; }
void QueuePlugin::RenderSettings()
{
    // Mini window toggle
    if (ImGui::Button(showMiniWindow ? "Close Mini Window" : "Open Mini Window"))
        showMiniWindow = !showMiniWindow;
    ImGui::SameLine();
    ImGui::TextDisabled("(drag it anywhere on screen)");
    ImGui::Separator();
    ImGui::Spacing();

    // Replay folder (for dispute reports)
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Replay Folder (for dispute reports)");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Default: %%USERPROFILE%%\\Documents\\My Games\\Rocket League\\TAGame\\Demos\\");
    ImGui::TextWrapped(
        "Leave blank to use the default. Change this only if you store replays elsewhere.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(460);
    if (ImGui::InputText("##replaypath", replayPathBuf, sizeof(replayPathBuf)))
        replayPath = replayPathBuf;

    ImGui::SameLine();
    if (ImGui::Button("Save##rp")) {
        replayPath = replayPathBuf;
        SaveConfig();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    RenderQueueUI();

    // Match history
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Match History (last 10)");
    ImGui::Spacing();

    if (historyFetching) {
        ImGui::TextDisabled("Loading...");
    } else {
        if (ImGui::Button("Refresh##history")) FetchHistory();

        if (matchHistory.empty()) {
            ImGui::SameLine(0, 10);
            ImGui::TextDisabled("No matches on record.");
        } else {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            ImGui::Text("%-8s  %-4s  %-8s  %-8s  %s", "Mode", "Reg", "Result", "MMR\xe2\x80\x8b\xce\x94", "Date");
            ImGui::PopStyleColor();
            ImGui::Separator();

            for (auto& e : matchHistory) {
                ImVec4 col;
                std::string label;
                if (e.outcome == "draw" || e.outcome == "draw_timeout") {
                    col   = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    label = "Draw";
                } else if (e.outcome == "forfeit") {
                    col   = e.won ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                                  : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                    label = e.won ? "Win (FF)" : "Loss (FF)";
                } else if (e.won) {
                    col   = ImVec4(0.2f, 1.0f, 0.4f, 1.0f);
                    label = "Win";
                } else {
                    col   = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                    label = "Loss";
                }

                char mmrBuf[16];
                if (e.mmrChange > 0.0f)       snprintf(mmrBuf, sizeof(mmrBuf), "+%.0f", e.mmrChange);
                else if (e.mmrChange < 0.0f)  snprintf(mmrBuf, sizeof(mmrBuf), "%.0f",  e.mmrChange);
                else                           snprintf(mmrBuf, sizeof(mmrBuf), "--");

                char dateBuf[20] = {};
                if (e.timestamp > 0) {
                    struct tm lt{};
                    localtime_s(&lt, &e.timestamp);
                    strftime(dateBuf, sizeof(dateBuf), "%d %b %H:%M", &lt);
                }

                ImGui::TextColored(col, "%-8s  %-4s  %-8s  %-8s  %s",
                    e.mode.c_str(), e.region.c_str(), label.c_str(), mmrBuf, dateBuf);
            }
        }
    }

    // Admin panel
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    RenderAdminUI();
}

// ── PluginWindow ───────────────────────────────────────────────────────────────
std::string QueuePlugin::GetMenuName()  { return "rlcustomqueue"; }
std::string QueuePlugin::GetMenuTitle() { return "Custom Queue"; }
void QueuePlugin::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }
bool QueuePlugin::ShouldBlockInput() { return false; }
bool QueuePlugin::IsActiveOverlay()  { return true; }
void QueuePlugin::OnOpen()  {}
void QueuePlugin::OnClose() {}

void QueuePlugin::Render()
{
    if (!showMiniWindow) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 300, 40),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Custom Queue##mini", &open,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    if (!open) showMiniWindow = false;
    RenderQueueUI();
    ImGui::End();
}

// ── UI ─────────────────────────────────────────────────────────────────────────
void QueuePlugin::RenderQueueUI()
{
    if (matchFound) { RenderMatchFoundUI(); return; }

    // Server status pill
    if (serverChecked) {
        if (serverOnline) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "● Server online");
            ImGui::SameLine(0, 10);
            if (totalOnline > 0)
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                    "(%d player%s searching)", totalOnline, totalOnline == 1 ? "" : "s");
            else
                ImGui::TextDisabled("(0 players searching)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "● Server offline  (queue unavailable)");
        }
    } else {
        ImGui::TextDisabled("● Checking server...");
    }

    RenderLinkUI();
    ImGui::Separator();
    ImGui::Spacing();

    // Ratings row
    if (!mmr1s.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("1s"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", mmr1s.c_str());
        ImGui::SameLine(0, 16);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("2s"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", mmr2s.c_str());
        ImGui::SameLine(0, 16);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("3s"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", mmr3s.c_str());
        ImGui::Spacing();
    }

    // Region / mode selectors
    bool locked = inQueue || realID.empty();
    if (locked) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);

    ImGui::Text("Region");
    ImGui::SetNextItemWidth(200);
    if (!inQueue)
        ImGui::Combo("##region", &selectedRegion, REGIONS, IM_ARRAYSIZE(REGIONS));
    else
        ImGui::TextDisabled("%s", REGIONS[selectedRegion]);

    ImGui::Spacing();
    ImGui::Text("Game Mode");
    for (int i = 0; i < IM_ARRAYSIZE(MODES); i++) {
        if (i > 0) ImGui::SameLine();
        if (!inQueue) {
            if (ImGui::RadioButton(MODES[i], selectedMode == i)) selectedMode = i;
        } else {
            ImGui::RadioButton(MODES[i], selectedMode == i);
        }
    }
    if (locked) ImGui::PopStyleVar();

    ImGui::Separator();

    // Status line
    if (realID.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Go to main menu to connect");
    } else if (inRankedQueue) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.1f, 1.0f),
            "⛔ Cancel your ranked search first");
    } else {
        ImGui::TextColored(
            inQueue ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "%s", queueStatus.c_str());
        if (inQueue && queueStartTime > 0) {
            int elapsed = (int)(time(nullptr) - queueStartTime);
            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", elapsed/60, elapsed%60);
            ImGui::SameLine(0, 10);
            ImGui::TextDisabled("(%s)", timeBuf);
        }
        if (inQueue && queueCount > 0)
            ImGui::TextDisabled("Position #%d of %d in queue", queuePosition, queueCount);
    }
    ImGui::Spacing();

    // Join / Leave buttons
    if (!inQueue) {
        bool joinBlocked = realID.empty() || inRankedQueue;

        if (hasPriority) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.1f, 1.0f),
                "⚡ You have queue priority (someone declined)");
            ImGui::Spacing();
        }

        if (joinBlocked) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        ImGui::PushStyleColor(ImGuiCol_Button,
            hasPriority ? ImVec4(0.6f, 0.5f, 0.0f, 1.0f) : ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            hasPriority ? ImVec4(0.8f, 0.7f, 0.0f, 1.0f) : ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
        const char* joinLabel = hasPriority ? "⚡ Rejoin (Priority)" : "Join Queue";
        if (ImGui::Button(joinLabel, ImVec2(160, 32)) && !joinBlocked)
            JoinQueue();
        ImGui::PopStyleColor(2);
        if (joinBlocked) ImGui::PopStyleVar();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("Leave Queue", ImVec2(140, 32))) LeaveQueue();
        ImGui::PopStyleColor(2);
    }

    // ── Dispute report (within 1 hour of last match) ──────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canReport = !lastMatchID.empty()
        && lastMatchTimestamp > 0
        && (time(nullptr) - lastMatchTimestamp) < 3600;

    if (reportSent) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
            "✅ Dispute submitted — under review.");
    } else if (reportPending) {
        ImGui::TextDisabled("Uploading replay...");
    } else if (reportPanelOpen) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Dispute last match");
        ImGui::Spacing();

        ImGui::Text("Replay:");
        ImGui::SameLine();
        std::string displayPath = reportReplayBuf[0]
            ? std::string(reportReplayBuf) : "(none selected)";
        auto slash = displayPath.find_last_of("\\/");
        if (slash != std::string::npos) displayPath = displayPath.substr(slash + 1);
        ImGui::TextColored(
            reportReplayBuf[0] ? ImVec4(0.8f, 0.8f, 0.8f, 1.0f)
                               : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "%s", displayPath.c_str());

        ImGui::Spacing();
        if (replayPickerBusy) {
            ImGui::TextDisabled("Picking file...");
        } else {
            if (ImGui::Button("Browse...", ImVec2(90, 26)))
                BrowseReplayAsync();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Newest", ImVec2(90, 26)) && !replayPickerBusy) {
            replayPickerBusy = true;
            std::thread([this]() {
                std::string newest = FindNewestReplay();
                gameWrapper->Execute([this, newest](GameWrapper* gw) {
                    if (!pluginAlive) return;
                    replayPickerBusy = false;
                    if (!newest.empty())
                        strncpy_s(reportReplayBuf, sizeof(reportReplayBuf),
                                  newest.c_str(), _TRUNCATE);
                    else
                        reportStatus = "No replay file found.";
                });
            }).detach();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.15f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.0f, 1.0f));
        if (ImGui::Button("Submit Dispute", ImVec2(130, 28))) ReportMatch();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##rptcancel", ImVec2(70, 28))) {
            reportPanelOpen = false;
            reportStatus    = "";
        }
        if (!reportStatus.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", reportStatus.c_str());
    } else {
        bool buttonEnabled = canReport;
        if (!buttonEnabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.15f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.0f, 1.0f));
        if (ImGui::Button("Dispute last match", ImVec2(160, 28)) && buttonEnabled)
            reportPanelOpen = true;
        ImGui::PopStyleColor(2);
        if (!buttonEnabled) ImGui::PopStyleVar();

        if (lastMatchID.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no match to dispute)");
        } else if (!canReport) {
            ImGui::SameLine();
            ImGui::TextDisabled("(only within 1 hour of a match)");
        }
    }
}

void QueuePlugin::RenderMatchFoundUI()
{
    if (allAccepted)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "All players ready!");
        ImGui::Separator();
        ImGui::Spacing();

        // Lobby details
        if (isHost)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                "You are the HOST — create this lobby:");
        else
            ImGui::TextDisabled("Use these details to join:");
        ImGui::Spacing();

        ImGui::Text("Lobby Name:"); ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", lobbyName.c_str());
        ImGui::Text("Password:  "); ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", lobbyPassword.c_str());

        // Team assignment
        if (myTeamIndex == 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "⬛ Your team: BLUE (left side)");
        } else if (myTeamIndex == 1) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "🟧 Your team: ORANGE (right side)");
        }

        ImGui::Spacing();

        // Lobby ready flag (host only)
        if (!lobbyReady) {
            if (isHost) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                    "Create the private match in RL, then click below.");
                ImGui::TextDisabled("(you have 3 minutes before the match is cancelled)");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
                if (ImGui::Button("Lobby is ready — notify players", ImVec2(270, 30)))
                    NotifyLobbyReady();
                ImGui::PopStyleColor(2);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                    "⏳ Host is setting up the lobby...");
            }
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "✅ Lobby is ready — join the game!");
        }

        // Draw countdown (server told us it's coming)
        if (drawCountdown >= 0) {
            ImGui::Spacing();
            int mins = drawCountdown / 60;
            int secs = drawCountdown % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                "⏳ Auto-draw in %dm %02ds", mins, secs);
        }

        // ── Result buttons ────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "After the match:");
        ImGui::Spacing();

        if (!outcomeStatus.empty()) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", outcomeStatus.c_str());
            ImGui::Spacing();
        }

        if (outcomeSent) {
            ImGui::TextDisabled("Result submitted — waiting for other players...");
        } else if (outcomeConfirm) {
            // Confirmation dialog
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                "Submit: %s?", pendingOutcome.c_str());
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.55f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.75f, 0.1f, 1.0f));
            if (ImGui::Button("Yes, confirm", ImVec2(110, 28))) {
                outcomeConfirm = false;
                SubmitOutcome(pendingOutcome);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel##outcancel", ImVec2(70, 28))) {
                outcomeConfirm  = false;
                pendingOutcome  = "";
            }
        } else {
            // Win / Loss / Draw buttons
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.5f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.7f, 0.15f, 1.0f));
            if (ImGui::Button("Win", ImVec2(76, 30))) {
                pendingOutcome = "win";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.05f, 0.05f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.05f, 0.05f, 1.0f));
            if (ImGui::Button("Loss", ImVec2(76, 30))) {
                pendingOutcome = "loss";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.3f, 0.3f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.15f, 1.0f));
            if (ImGui::Button("Draw", ImVec2(76, 30))) {
                pendingOutcome = "draw";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);

            ImGui::Spacing();
            ImGui::TextDisabled("Press the result once the game is over.");
        }

        // Forfeit button
        ImGui::Separator();
        ImGui::Spacing();
        static bool forfeitConfirm = false;
        if (myForfeited) {
            forfeitConfirm = false;
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                "⚑ Forfeit submitted — waiting for teammates...");
        } else if (forfeitConfirm) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Confirm forfeit? All on your team must press.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Yes, Forfeit", ImVec2(110, 28))) {
                forfeitConfirm = false;
                ForfeitMatch();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel##fcancel", ImVec2(70, 28)))
                forfeitConfirm = false;
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("⚑ Forfeit", ImVec2(100, 28)))
                forfeitConfirm = true;
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextDisabled("(all on your team must press)");
        }
        return;
    }

    // ── Acceptance phase ──────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "MATCH FOUND!");
    ImGui::SameLine();
    ImVec4 timerCol = matchTimeRemaining > 10
        ? ImVec4(0.8f, 0.8f, 0.8f, 1.0f)
        : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::TextColored(timerCol, "(%ds)", matchTimeRemaining);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Players accepted: %d / %d", acceptedCount, totalPlayers);
    ImGui::Spacing();

    if (myAccepted) {
        ImGui::TextDisabled("Waiting for other players...");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
        if (ImGui::Button("Accept", ImVec2(100, 30))) AcceptMatch();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("Decline", ImVec2(100, 30))) DeclineMatch();
    ImGui::PopStyleColor(2);
}

// ── account status UI ──────────────────────────────────────────────────────────
void QueuePlugin::RenderLinkUI()
{
    if (!displayName.empty()) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", displayName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        if (ImGui::SmallButton("Website"))
            ShellExecuteA(nullptr, "open", SERVER_WEBSITE.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        if (ImGui::SmallButton("Website"))
            ShellExecuteA(nullptr, "open", SERVER_WEBSITE.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
        ImGui::Text("Username");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##username", usernameInputBuf, sizeof(usernameInputBuf));
        ImGui::Spacing();
        if (registering) {
            ImGui::TextDisabled("Connecting...");
        } else {
            bool hasName = usernameInputBuf[0] != '\0';
            if (!hasName) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.45f, 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.6f, 0.95f, 1.0f));
            if (ImGui::Button("Connect Account", ImVec2(150, 30)) && hasName) {
                registering = true;
                displayName = usernameInputBuf;
                gameWrapper->Execute([this](GameWrapper* gw) {
                    FetchRealID();
                    RegisterWithServer();
                });
            }
            ImGui::PopStyleColor(2);
            if (!hasName) ImGui::PopStyleVar();
        }
    }
}

// ── queue actions ─────────────────────────────────────────────────────────────
void QueuePlugin::JoinQueue()
{
    if (inRankedQueue) { queueStatus = "Cancel your ranked search first."; return; }

    inQueue        = true;
    matchFound     = false;
    queueCount     = 0;
    queuePosition  = 0;
    queueStartTime = time(nullptr);
    queueStatus    = "Searching... (" + std::string(MODE_IDS[selectedMode])
                   + " | " + REGIONS[selectedRegion] + ")";

    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"real_id\":\"" + realID + "\","
                       "\"username\":\"" + JsonEscape(displayName) + "\","
                       "\"region\":\"" + REGIONS[selectedRegion] + "\","
                       "\"mode\":\"" + MODE_IDS[selectedMode] + "\"}";

    hasPriority = false;

    HttpPostAsync("/queue/join", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (resp.empty()) {
                inQueue     = false;
                queueStatus = "Error: server unreachable";
                return;
            }
            // A non-queued response means the server rejected the join (e.g. 409
            // "already in an active match").  Show the detail message and bail out.
            std::string status = JsonStr(resp, "status");
            if (status != "queued") {
                inQueue     = false;
                std::string detail = JsonStr(resp, "detail");
                queueStatus = detail.empty() ? "Could not join queue." : detail;
                return;
            }
            int pos = SafeStoi(JsonNum(resp, "position"), 0);
            if (pos > 0) queuePosition = pos;
            StartPolling();
        });
    });
}

void QueuePlugin::LeaveQueue()
{
    inQueue        = false;
    matchFound     = false;
    matchID        = "";
    queueStartTime = 0;
    queueStatus    = "Not in queue";

    std::string body = "{\"player_id\":\"" + playerID + "\"}";
    HttpPostAsync("/queue/leave", body, [](std::string) {});
}

// ── heartbeat ──────────────────────────────────────────────────────────────────
void QueuePlugin::StartPolling()
{
    gameWrapper->SetTimeout([this](GameWrapper* gw) { SendHeartbeat(); }, 2.0f);
}

void QueuePlugin::SendHeartbeat()
{
    if (!inQueue || matchFound) return;

    std::string body = "{\"player_id\":\"" + playerID + "\"}";
    HttpPostAsync("/queue/heartbeat", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (!inQueue || matchFound) return;
            if (resp.empty()) {
                gameWrapper->SetTimeout([this](GameWrapper* gw) { SendHeartbeat(); }, 5.0f);
                return;
            }
            std::string status = JsonStr(resp, "status");
            if (status == "match_found") {
                OnMatchFound(resp);
            } else if (status == "not_in_queue") {
                inQueue     = false;
                queueStatus = "Removed from queue (timeout). Rejoin to continue.";
            } else {
                int cnt = SafeStoi(JsonNum(resp, "queue_count"),    queueCount);
                int pos = SafeStoi(JsonNum(resp, "queue_position"), queuePosition);
                if (cnt > 0) queueCount    = cnt;
                if (pos > 0) queuePosition = pos;
                gameWrapper->SetTimeout([this](GameWrapper* gw) { SendHeartbeat(); }, 0.1f);
            }
        });
    }, 25000);
}

void QueuePlugin::OnMatchFound(const std::string& resp)
{
    matchFound         = true;
    inQueue            = false;
    queueStartTime     = 0;
    myAccepted         = false;
    allAccepted        = false;
    lobbyReady         = false;
    acceptedCount      = 0;
    matchTimeRemaining = 30;
    outcomeSent        = false;
    outcomeConfirm     = false;
    pendingOutcome     = "";
    outcomeStatus      = "";
    myForfeited        = false;
    drawCountdown      = -1;
    reportSent         = false;
    reportStatus       = "";
    matchID            = JsonStr(resp, "match_id");
    lobbyName          = JsonStr(resp, "lobby_name");
    lobbyPassword      = JsonStr(resp, "lobby_password");
    isHost             = JsonBool(resp, "is_host");
    myTeamIndex        = SafeStoi(JsonNum(resp, "team"), -1);
    queueStatus        = "Match found!";

    std::string mode = JsonStr(resp, "mode");
    if      (mode == "1s") totalPlayers = 2;
    else if (mode == "2s") totalPlayers = 4;
    else                   totalPlayers = 6;

    // Parse real IDs for party invites
    matchRealIDs.clear();
    auto pos = resp.find("\"real_ids\"");
    if (pos != std::string::npos) {
        auto start = resp.find('[', pos);
        auto end   = resp.find(']', start);
        if (start != std::string::npos && end != std::string::npos) {
            std::string arr = resp.substr(start + 1, end - start - 1);
            size_t i = 0;
            while (i < arr.size()) {
                auto q1 = arr.find('"', i);
                if (q1 == std::string::npos) break;
                auto q2 = arr.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string id = arr.substr(q1 + 1, q2 - q1 - 1);
                if (!id.empty() && id != realID) matchRealIDs.push_back(id);
                i = q2 + 1;
            }
        }
    }
}

void QueuePlugin::AcceptMatch()
{
    myAccepted = true;
    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    HttpPostAsync("/match/accept", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (resp.empty()) {
                myAccepted  = false;
                queueStatus = "Accept failed — server unreachable, try again.";
                return;
            }
            PollMatchStatus();
        });
    });
}

void QueuePlugin::DeclineMatch()
{
    std::string mid    = matchID;
    matchFound         = false;
    myAccepted         = false;
    allAccepted        = false;
    matchID            = "";
    lobbyName          = "";
    lobbyPassword      = "";
    queueStatus        = "Not in queue";

    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + mid + "\"}";
    HttpPostAsync("/match/decline", body, [](std::string) {});
}

void QueuePlugin::CancelMatchLocally(const std::string& reason)
{
    if (reason.find("declined") != std::string::npos)
        hasPriority = true;

    matchFound         = false;
    myAccepted         = false;
    allAccepted        = false;
    lobbyReady         = false;
    isHost             = false;
    myTeamIndex        = -1;
    myForfeited        = false;
    drawCountdown      = -1;
    outcomeSent        = false;
    outcomeConfirm     = false;
    pendingOutcome     = "";
    outcomeStatus      = "";
    matchID            = "";
    lobbyName          = "";
    lobbyPassword      = "";
    queueStatus        = reason;
}

void QueuePlugin::PollMatchStatus()
{
    if (!matchFound) return;

    HttpGetAsync("/match/status/" + matchID, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (!matchFound) return;

            std::string status = JsonStr(resp, "status");

            if (status == "cancelled") {
                std::string reason = JsonStr(resp, "reason");
                CancelMatchLocally(reason.empty() ? "Match cancelled." : reason);
                return;
            }

            if (status == "resolved") {
                // Match was fully resolved — clear UI, update MMR display
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                std::string outcome = JsonStr(resp, "outcome");
                if (!outcome.empty()) {
                    outcomeStatus = "Match recorded: " + outcome;
                }
                CancelMatchLocally("Not in queue");
                FetchMMR();
                FetchHistory();
                return;
            }

            if (status == "not_found" || status == "expired" || resp.empty()) {
                // Match removed — treat as done
                if (!matchID.empty()) lastMatchID = matchID;
                lastMatchTimestamp = time(nullptr);
                CancelMatchLocally("Not in queue");
                FetchMMR();
                return;
            }

            // Match still active — update fields
            matchTimeRemaining = SafeStoi(JsonNum(resp, "time_remaining"), matchTimeRemaining);
            acceptedCount      = SafeStoi(JsonNum(resp, "accepted_count"), acceptedCount);
            totalPlayers       = SafeStoi(JsonNum(resp, "total"),          totalPlayers);

            std::string drawInStr = JsonNum(resp, "draw_in");
            if (!drawInStr.empty()) drawCountdown = SafeStoi(drawInStr, -1);

            if (!allAccepted && JsonBool(resp, "all_accepted")) {
                allAccepted = true;
                if (isHost) SendPartyInvites();
            }

            if (allAccepted && JsonBool(resp, "lobby_ready"))
                lobbyReady = true;

            // Keep polling every 3 seconds throughout the whole match
            gameWrapper->SetTimeout([this](GameWrapper* gw) {
                PollMatchStatus();
            }, 3.0f);
        });
    });
}

// ── result submission ──────────────────────────────────────────────────────────
void QueuePlugin::SubmitOutcome(const std::string& outcome)
{
    if (outcomeSent || matchID.empty()) return;
    outcomeSent   = true;
    outcomeStatus = "Submitting...";

    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\","
                       "\"outcome\":\"" + outcome + "\"}";

    HttpPostAsync("/match/result", body, [this, outcome](std::string resp) {
        gameWrapper->Execute([this, resp, outcome](GameWrapper* gw) {
            if (resp.empty()) {
                outcomeSent   = false;  // allow retry
                outcomeStatus = "Server unreachable — try again.";
                return;
            }
            std::string s = JsonStr(resp, "status");
            if (s == "awarded") {
                outcomeStatus = "✅ Result accepted — MMR updated!";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                gameWrapper->SetTimeout([this](GameWrapper* gw) {
                    if (!matchFound) return;
                    CancelMatchLocally("Not in queue");
                    FetchMMR();
                    FetchHistory();
                }, 3.0f);
            } else if (s == "draw_recorded") {
                outcomeStatus = "Draw recorded — no MMR change.";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                gameWrapper->SetTimeout([this](GameWrapper* gw) {
                    if (!matchFound) return;
                    CancelMatchLocally("Not in queue");
                    FetchHistory();
                }, 3.0f);
            } else if (s == "disputed") {
                outcomeStatus = "⚠ Conflicting reports — flagged for admin review.";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                gameWrapper->SetTimeout([this](GameWrapper* gw) {
                    if (!matchFound) return;
                    CancelMatchLocally("Not in queue");
                }, 4.0f);
            } else if (s == "recorded" || s == "waiting") {
                outcomeStatus = "Recorded — waiting for other players...";
            } else {
                outcomeStatus = "Unexpected response: " + s;
            }

            // Always try to auto-upload the replay for server-side verification.
            // This runs on a background thread regardless of the result status above.
            // If the replay verifies cleanly, the server resolves the match automatically
            // and PollMatchStatus() will notice via status=="resolved".
            UploadReplayForVerification();
        });
    });
}

void QueuePlugin::UploadReplayForVerification()
{
    if (matchID.empty()) return;

    std::string mid = matchID;
    std::string pid = playerID;

    // Find the newest replay on a background thread (file I/O off the game thread)
    std::thread([this, mid, pid]() {
        std::string path = FindNewestReplay();
        if (path.empty()) return;   // no replay found — skip silently

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return;
        std::vector<char> data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        file.close();

        if (data.size() < 4096) return;   // too small to be a real replay

        if (!pluginAlive) return;

        // Upload to the verification endpoint
        HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        DWORD timeoutMs = 30000;   // 30 s — replay files are up to a few MB
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return; }

        std::wstring wPath = L"/match/upload_replay/"
            + std::wstring(mid.begin(), mid.end())
            + L"?player_id=" + std::wstring(pid.begin(), pid.end());

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return;
        }

        WinHttpSendRequest(hRequest,
            L"Content-Type: application/octet-stream\r\n", -1,
            (LPVOID)data.data(), (DWORD)data.size(), (DWORD)data.size(), 0);
        WinHttpReceiveResponse(hRequest, nullptr);

        // Read response
        std::string response;
        DWORD dwSize = 0;
        do {
            DWORD downloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            std::vector<char> buf(dwSize + 1, 0);
            WinHttpReadData(hRequest, buf.data(), dwSize, &downloaded);
            response.append(buf.data(), downloaded);
        } while (dwSize > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (!pluginAlive) return;

        gameWrapper->Execute([this, response, mid](GameWrapper* gw) {
            if (!pluginAlive) return;
            // Only update UI if we're still in this match
            if (matchID != mid) return;

            std::string status = JsonStr(response, "status");
            if (status == "auto_resolved") {
                // Replay was clean — server already awarded MMR.
                // PollMatchStatus will detect this and clear state, but give instant feedback.
                std::string score = JsonStr(response, "score");
                outcomeStatus = "✅ Replay verified (" + score + ") — MMR updated!";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                gameWrapper->SetTimeout([this](GameWrapper* gw) {
                    if (!matchFound) return;
                    CancelMatchLocally("Not in queue");
                    FetchMMR();
                    FetchHistory();
                }, 3.0f);
            } else if (status == "flagged") {
                std::string reason = JsonStr(response, "reason");
                outcomeStatus = "⚠ Replay flagged: " + reason;
            }
            // "unverifiable" and "already_resolved" — no UI change needed
        });
    }).detach();
}

// ── lobby ─────────────────────────────────────────────────────────────────────
void QueuePlugin::NotifyLobbyReady()
{
    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    HttpPostAsync("/match/lobby_ready", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (!resp.empty()) lobbyReady = true;
        });
    });
}

void QueuePlugin::ForfeitMatch()
{
    if (myForfeited || matchID.empty()) return;
    myForfeited = true;
    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    HttpPostAsync("/match/forfeit", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (resp.empty()) {
                myForfeited = false;  // allow retry
            }
        });
    });
}

// ── match history ─────────────────────────────────────────────────────────────
void QueuePlugin::FetchHistory()
{
    if (historyFetching || playerID.empty()) return;
    historyFetching = true;
    HttpGetAsync("/player/" + playerID + "/history", [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            historyFetching = false;
            if (resp.empty()) return;
            matchHistory.clear();
            size_t pos = 0;
            while ((pos = resp.find('{', pos)) != std::string::npos) {
                MatchHistoryEntry e;
                e.matchId  = JsonStr(resp.substr(pos), "match_id");
                e.mode     = JsonStr(resp.substr(pos), "mode");
                e.region   = JsonStr(resp.substr(pos), "region");
                e.outcome  = JsonStr(resp.substr(pos), "outcome");
                e.won      = JsonBool(resp.substr(pos), "won");
                {
                    std::string mmrStr = JsonNum(resp.substr(pos), "mmr_change");
                    try { e.mmrChange = mmrStr.empty() ? 0.0f : (float)std::stod(mmrStr); }
                    catch (...) { e.mmrChange = 0.0f; }
                }
                e.timestamp = (time_t)SafeStoi(JsonNum(resp.substr(pos), "timestamp"), 0);
                if (!e.matchId.empty()) matchHistory.push_back(e);
                auto end = resp.find('}', pos);
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        });
    });
}

// ── admin UI ───────────────────────────────────────────────────────────────────
void QueuePlugin::RenderAdminUI()
{
    if (ImGui::CollapsingHeader("Admin Panel##hdr")) {
        ImGui::Spacing();

        if (!adminUnlocked) {
            ImGui::Text("Password:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            ImGui::InputText("##adminpass", adminPassBuf, sizeof(adminPassBuf),
                             ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            if (adminFetching) {
                ImGui::TextDisabled("Checking...");
            } else {
                if (ImGui::Button("Unlock##admin")) FetchAdminReports();
            }
            if (!adminStatus.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", adminStatus.c_str());
            return;
        }

        if (ImGui::Button("Refresh##admin")) FetchAdminReports();
        ImGui::SameLine();
        if (ImGui::Button("Lock##admin")) { adminUnlocked = false; adminReports.clear(); }

        if (!adminStatus.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", adminStatus.c_str());
        }

        ImGui::Spacing();

        if (adminReports.empty()) {
            ImGui::TextDisabled("No pending reports.");
        } else {
            for (auto& r : adminReports) {
                ImGui::PushID(r.id);
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                    "Match: %s", r.matchId.c_str());
                ImGui::Text("Reporter: %s", r.reporterUsername.c_str());
                ImGui::Text("Status:   %s", r.status.c_str());
                ImGui::Spacing();
                if (ImGui::Button("Download Replay##dl")) {
                    std::string url = SERVER_WEBSITE + "/admin/replay/"
                        + std::to_string(r.id) + "?password=" + std::string(adminPassBuf);
                    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.55f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.75f, 0.1f, 1.0f));
                if (ImGui::Button("Accept Result##acc")) AdminAcceptMatch(r.matchId);
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("Cancel Match##can")) AdminCancelMatch(r.matchId);
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
    }
}

// ── server status ─────────────────────────────────────────────────────────────
void QueuePlugin::CheckServerStatus()
{
    HttpGetAsync("/health", [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            serverChecked = true;
            serverOnline  = !resp.empty() && resp.find("ok") != std::string::npos;
        });
    });
    HttpGetAsync("/queue/stats", [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            int n = SafeStoi(JsonNum(resp, "total_searching"), 0);
            if (n >= 0) totalOnline = n;
        });
    });
}

void QueuePlugin::PollServerStatus()
{
    gameWrapper->SetTimeout([this](GameWrapper* gw) {
        CheckServerStatus();
        PollServerStatus();
    }, 30.0f);
}

// ── config ─────────────────────────────────────────────────────────────────────
void QueuePlugin::LoadConfig()
{
    std::string cfgFile = gameWrapper->GetBakkesModPath().string()
                        + "\\plugins\\rlcq_config.txt";
    std::ifstream in(cfgFile);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        auto sep = line.find('=');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        if (key == "replay_path") {
            replayPath = val;
            strncpy_s(replayPathBuf, sizeof(replayPathBuf), val.c_str(), _TRUNCATE);
        } else if (key == "username") {
            displayName = val;
            strncpy_s(usernameInputBuf, sizeof(usernameInputBuf), val.c_str(), _TRUNCATE);
        }
    }
}

void QueuePlugin::SaveConfig()
{
    std::string cfgFile = gameWrapper->GetBakkesModPath().string()
                        + "\\plugins\\rlcq_config.txt";
    std::ofstream out(cfgFile);
    out << "replay_path=" << replayPath << "\n";
    out << "username=" << displayName << "\n";
}

// ── dispute / replay ───────────────────────────────────────────────────────────
void QueuePlugin::BrowseReplayAsync()
{
    if (replayPickerBusy) return;
    replayPickerBusy = true;
    std::thread([this]() {
        char filePath[MAX_PATH] = {};
        strncpy_s(filePath, sizeof(filePath), reportReplayBuf, _TRUNCATE);
        OPENFILENAMEA ofn   = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = GetForegroundWindow();
        ofn.lpstrFilter     = "Rocket League Replay\0*.replay\0All Files\0*.*\0";
        ofn.lpstrFile       = filePath;
        ofn.nMaxFile        = MAX_PATH;
        ofn.lpstrTitle      = "Select Replay File";
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) {
            gameWrapper->Execute([this, path = std::string(filePath)](GameWrapper* gw) {
                if (!pluginAlive) return;
                strncpy_s(reportReplayBuf, sizeof(reportReplayBuf),
                          path.c_str(), _TRUNCATE);
                replayPickerBusy = false;
            });
        } else {
            gameWrapper->Execute([this](GameWrapper* gw) {
                if (!pluginAlive) return;
                replayPickerBusy = false;
            });
        }
    }).detach();
}

void QueuePlugin::ReportMatch()
{
    if (!reportReplayBuf[0]) { reportStatus = "Select a replay file first."; return; }

    std::string path = std::string(reportReplayBuf);
    reportPending = true;
    reportStatus  = "";
    std::string mid = lastMatchID;
    std::string pid = playerID;

    std::thread([this, path, mid, pid]() {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            gameWrapper->Execute([this](GameWrapper* gw) {
                reportPending = false;
                reportStatus  = "Could not read replay.";
            });
            return;
        }
        std::vector<char> data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        file.close();

        HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            gameWrapper->Execute([this](GameWrapper* gw) {
                reportPending = false; reportStatus = "Server unreachable.";
            });
            return;
        }
        std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            gameWrapper->Execute([this](GameWrapper* gw) {
                reportPending = false; reportStatus = "Server unreachable.";
            });
            return;
        }
        std::wstring wPath = L"/match/report/"
            + std::wstring(mid.begin(), mid.end())
            + L"?reporter=" + std::wstring(pid.begin(), pid.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            gameWrapper->Execute([this](GameWrapper* gw) {
                reportPending = false; reportStatus = "Server unreachable.";
            });
            return;
        }
        WinHttpSendRequest(hRequest,
            L"Content-Type: application/octet-stream\r\n", -1,
            (LPVOID)data.data(), (DWORD)data.size(), (DWORD)data.size(), 0);
        WinHttpReceiveResponse(hRequest, nullptr);
        std::string response;
        DWORD dwSize = 0;
        do {
            DWORD downloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            std::vector<char> buf(dwSize + 1, 0);
            WinHttpReadData(hRequest, buf.data(), dwSize, &downloaded);
            response.append(buf.data(), downloaded);
        } while (dwSize > 0);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        gameWrapper->Execute([this, response](GameWrapper* gw) {
            if (!pluginAlive) return;
            reportPending = false;
            if (response.find("reported") != std::string::npos) {
                reportSent   = true;
                reportStatus = "";
            } else {
                reportStatus = "Dispute failed. Try again.";
            }
        });
    }).detach();
}

std::string QueuePlugin::FindNewestReplay()
{
    std::string folder;
    if (!replayPath.empty()) {
        folder = replayPath;
        if (folder.back() != '\\' && folder.back() != '/') folder += '\\';
    } else {
        char userprofile[MAX_PATH];
        GetEnvironmentVariableA("USERPROFILE", userprofile, MAX_PATH);
        folder = std::string(userprofile) +
            "\\Documents\\My Games\\Rocket League\\TAGame\\Demos\\";
    }
    std::string newest;
    FILETIME latestTime = {};
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((folder + "*.replay").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return "";
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &latestTime) > 0) {
            latestTime = fd.ftLastWriteTime;
            newest = folder + fd.cFileName;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return newest;
}

// ── admin ──────────────────────────────────────────────────────────────────────
void QueuePlugin::FetchAdminReports()
{
    adminFetching = true;
    adminStatus   = "";
    std::string pass(adminPassBuf);
    HttpGetAsync("/admin/reports?password=" + pass, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            adminFetching = false;
            if (resp.empty()) { adminStatus = "Server unreachable."; return; }
            if (resp.find("Unauthorized") != std::string::npos) {
                adminStatus = "Wrong password."; return;
            }
            adminReports.clear();
            adminUnlocked = true;
            size_t pos = 0;
            while ((pos = resp.find("\"id\"", pos)) != std::string::npos) {
                size_t safePos = pos > 0 ? pos - 1 : 0;
                ReportEntry e;
                e.id               = SafeStoi(JsonNum(resp.substr(safePos), "id"));
                e.matchId          = JsonStr(resp.substr(safePos), "match_id");
                e.reporterUsername = JsonStr(resp.substr(safePos), "reporter_username");
                e.submittedAt      = JsonNum(resp.substr(safePos), "submitted_at");
                e.status           = JsonStr(resp.substr(safePos), "status");
                adminReports.push_back(e);
                pos += 4;
            }
            if (adminReports.empty()) adminStatus = "No reports found.";
        });
    });
}

void QueuePlugin::AdminAcceptMatch(const std::string& matchId)
{
    std::string pass(adminPassBuf);
    HttpPostAsync("/admin/match/accept/" + matchId + "?password=" + pass, "{}",
        [this](std::string resp) {
            gameWrapper->Execute([this, resp](GameWrapper* gw) {
                adminStatus = resp.find("accepted") != std::string::npos
                    ? "Result accepted." : "Failed.";
                FetchAdminReports();
            });
        });
}

void QueuePlugin::AdminCancelMatch(const std::string& matchId)
{
    std::string pass(adminPassBuf);
    HttpPostAsync("/admin/match/cancel/" + matchId + "?password=" + pass, "{}",
        [this](std::string resp) {
            gameWrapper->Execute([this, resp](GameWrapper* gw) {
                adminStatus = resp.find("cancelled") != std::string::npos
                    ? "Match cancelled, MMR reversed." : "Failed.";
                FetchAdminReports();
            });
        });
}

// ── account ────────────────────────────────────────────────────────────────────
void QueuePlugin::RegisterWithServer()
{
    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"real_id\":\"" + realID + "\","
                       "\"username\":\"" + JsonEscape(displayName) + "\"}";
    HttpPostAsync("/account/register", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            registering = false;
            if (!resp.empty()) {
                SaveConfig();
                FetchMMR();
            }
        });
    });
}

void QueuePlugin::FetchMMR()
{
    HttpGetAsync("/player/" + playerID + "/mmr", [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            std::string v1 = JsonNum(resp, "mmr_1s");
            std::string v2 = JsonNum(resp, "mmr_2s");
            std::string v3 = JsonNum(resp, "mmr_3s");
            if (!v1.empty()) mmr1s = v1;
            if (!v2.empty()) mmr2s = v2;
            if (!v3.empty()) mmr3s = v3;
        });
    });
}

void QueuePlugin::FetchRealID()
{
    unsigned long long uid = gameWrapper->GetSteamID();
    if (uid != 0) realID = std::to_string(uid);
    if (realID.empty() || realID == "0") realID = playerID;
}

void QueuePlugin::SendPartyInvites()
{
    for (const auto& rid : matchRealIDs) {
        if (rid.empty()) continue;
        gameWrapper->ExecuteUnrealCommand("InviteToGame " + rid);
    }
}

void QueuePlugin::CopyToClipboard(const std::string& text)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hMem) {
        char* dst = static_cast<char*>(GlobalLock(hMem));
        if (dst) {
            memcpy(dst, text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            if (!SetClipboardData(CF_TEXT, hMem)) GlobalFree(hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

// ── HTTP ───────────────────────────────────────────────────────────────────────
std::string QueuePlugin::HttpPost(const std::string& path, const std::string& body,
                                   DWORD timeoutMs)
{
    HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    std::wstring wPath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }
    WinHttpSendRequest(hRequest,
        L"Content-Type: application/json\r\n", -1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hRequest, nullptr);
    std::string response;
    DWORD dwSize = 0;
    do {
        DWORD downloaded = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (!dwSize) break;
        std::vector<char> buf(dwSize + 1, 0);
        WinHttpReadData(hRequest, buf.data(), dwSize, &downloaded);
        response.append(buf.data(), downloaded);
    } while (dwSize > 0);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

std::string QueuePlugin::HttpGet(const std::string& path, DWORD timeoutMs)
{
    HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    std::wstring wPath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, nullptr);
    std::string response;
    DWORD dwSize = 0;
    do {
        DWORD downloaded = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (!dwSize) break;
        std::vector<char> buf(dwSize + 1, 0);
        WinHttpReadData(hRequest, buf.data(), dwSize, &downloaded);
        response.append(buf.data(), downloaded);
    } while (dwSize > 0);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

void QueuePlugin::HttpPostAsync(const std::string& path, const std::string& body,
                                std::function<void(std::string)> callback,
                                DWORD timeoutMs)
{
    std::thread([this, path, body, callback, timeoutMs]() {
        std::string resp = HttpPost(path, body, timeoutMs);
        if (!pluginAlive) return;
        callback(resp);
    }).detach();
}

void QueuePlugin::HttpGetAsync(const std::string& path,
                               std::function<void(std::string)> callback,
                               DWORD timeoutMs)
{
    std::thread([this, path, callback, timeoutMs]() {
        std::string resp = HttpGet(path, timeoutMs);
        if (!pluginAlive) return;
        callback(resp);
    }).detach();
}

// ── JSON helpers ───────────────────────────────────────────────────────────────
std::string QueuePlugin::JsonStr(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

std::string QueuePlugin::JsonNum(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

bool QueuePlugin::JsonBool(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return false;
    return json.substr(pos, 4) == "true";
}
