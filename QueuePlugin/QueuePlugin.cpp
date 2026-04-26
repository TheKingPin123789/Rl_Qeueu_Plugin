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
    if (in.is_open()) { std::getline(in, systemID); in.close(); }
    if (systemID.empty()) {
        systemID = "bm_" + rand_str(12);
        std::ofstream out(idFile);
        out << systemID;
    }

    LoadConfig();

    // Always start disabled — player must enable explicitly each session.
    // This preserves username/replay_path from config but resets the active flag.
    pluginEnabled = false;
    SaveConfig();

    cvarManager->log("[RLCQ] onLoad: config loaded, plugin forced to disabled");

    // Fetch real ID and clear any stale in-game flags when the main menu loads.
    // Also call togglemenu once (first call only) to register the overlay window
    // with BakkesMod's renderer — required for IsActiveOverlay()/Render() to work.
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_MainMenu_TA.MainMenuAdded",
        [this, alive = pluginAlive](std::string) {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                FetchRealID();
                inRankedQueue = false;
                ApplyAccountSetup(alive, 0);

                if (!overlayRegistered) {
                    overlayRegistered = true;
                    cvarManager->log("[RLCQ] MainMenuAdded: registering overlay");
                    cvarManager->executeCommand("togglemenu rlcustomqueue");
                }
            });
        });

    // Track whether RL ranked/casual matchmaking search is active
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_Matchmaking_TA.EventSearchStarted",
        [this, alive = pluginAlive](std::string) {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                inRankedQueue = true;
            });
        });
    gameWrapper->HookEvent(
        "Function TAGame.GFxData_Matchmaking_TA.EventSearchCanceled",
        [this, alive = pluginAlive](std::string) {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                inRankedQueue = false;
            });
        });

    // Hook into player spawn / PRI received — fires once the player session is
    // fully loaded and GetPlayerName() reliably returns a value.  Used to capture
    // platformDisplayName for Epic users when GetPlayerName() was empty on the
    // main menu (it becomes available once RL's online session initialises).
    gameWrapper->HookEvent(
        "Function TAGame.PlayerController_TA.eventReceivedPlayer",
        [this, alive = pluginAlive](std::string) {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                std::string nameStr = gameWrapper->GetPlayerName().ToString();
                if (!nameStr.empty() && platformDisplayName != nameStr) {
                    platformDisplayName = nameStr;
                    cvarManager->log("[RLCQ] ReceivedPlayer: platformDisplayName='" + nameStr + "'");
                    // If Epic user still has no display name, auto-fill now
                    if (platform == "Epic" && displayName.empty()) {
                        displayName = platformDisplayName;
                        strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                                  displayName.c_str(), _TRUNCATE);
                        if (!activeAccountID.empty() && pluginEnabled)
                            RegisterWithServer();
                        SaveConfig();
                    } else if (!activeAccountID.empty()) {
                        SaveConfig();  // persist updated platformDisplayName
                    }
                }
            });
        });

    // No HTTP calls during onLoad — deferred to first RenderSettings() call.
    // togglemenu registers the overlay window with BakkesMod's renderer so that
    // IsActiveOverlay()/Render() starts being called.  The MainMenuAdded hook
    // above handles the normal case.  This 5-second fallback covers the edge case
    // where the plugin is loaded after the main menu is already showing (e.g. hot
    // reload in developer mode) and the hook never fires.
    {
        auto alive = pluginAlive;
        gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
            if (!*alive) return;
            // Always run account detection on the fallback path — covers the case
            // where the plugin is loaded while the main menu is already showing and
            // MainMenuAdded never fires.
            FetchRealID();
            ApplyAccountSetup(alive, 0);
            if (!overlayRegistered) {
                overlayRegistered = true;
                cvarManager->log("[RLCQ] fallback 5s: registering overlay");
                cvarManager->executeCommand("togglemenu rlcustomqueue");
            }
        }, 5.0f);
    }
    cvarManager->log("[RLCQ] onLoad: hooks registered (server check deferred)");

    // ── Admin CVar: type  rlcq_admin_key <password>  in the F6 console ──────────
    // registerCvar is the stable BakkesMod API (registerNotifier is crash-prone).
    // The value is cleared immediately after each attempt for security.
    cvarManager->registerCvar("rlcq_admin_key", "", "Admin unlock key", false)
        .addOnValueChanged([this](std::string, CVarWrapper cvar) {
            std::string val = cvar.getStringValue();
            if (val.empty()) return;
            // Schedule the clear AFTER the callback returns — calling setValue
            // inside addOnValueChanged corrupts BakkesMod's iterator and crashes.
            auto alive = pluginAlive;
            gameWrapper->SetTimeout([this, alive](GameWrapper*) {
                if (!*alive) return;
                cvarManager->getCvar("rlcq_admin_key").setValue("");
            }, 0.05f);
            TryAdminLogin(val);
        });

    cvarManager->log("[RLCQ] onLoad: complete");
}

void QueuePlugin::onUnload()
{
    StopSSE();          // signal SSE thread to exit before invalidating pluginAlive
    *pluginAlive = false;
    if (inQueue) {
        std::string body = "{\"player_id\":\"" + ServerID() + "\"}";
        HttpPost("/queue/leave", body);
    }
}

// ── PluginSettingsWindow ───────────────────────────────────────────────────────
std::string QueuePlugin::GetPluginName() { return "RL Custom Queue"; }
void QueuePlugin::RenderSettings()
{
    // First time the settings panel opens BakkesMod is fully initialised —
    // safe to start background HTTP calls now.  CheckServerStatus() is itself
    // guarded by serverCheckStarted so this is safe to call unconditionally.
    CheckServerStatus();

    // ── Plugin enable / disable ────────────────────────────────────────────────
    ImGui::Spacing();
    if (pluginEnabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.08f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.1f,  0.1f,  1.0f));
        if (ImGui::Button("Disable Plugin", ImVec2(180, 34))) {
            pluginEnabled = false;
            if (inQueue) LeaveQueue();
            SaveConfig();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Plugin is ON");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.45f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f,  0.65f, 0.1f,  1.0f));
        if (ImGui::Button("Enable Plugin", ImVec2(180, 34))) {
            pluginEnabled = true;
            SaveConfig();
            CheckServerStatus();
            auto alive = pluginAlive;
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                FetchRealID();
                ApplyAccountSetup(alive, 0);  // corrects stale config data + registers
            });
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Plugin is OFF");
        ImGui::Spacing();
        ImGui::TextDisabled("Enable the plugin to join the queue.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Registration is available even while disabled so new players can set
        // their username before enabling.  RenderLinkUI() handles the full flow.
        RenderLinkUI();

        ImGui::Separator();
        ImGui::Spacing();
        // Mini-window toggle and admin panel remain accessible when disabled
        if (ImGui::Button(showMiniWindow ? "Close Mini Window" : "Open Mini Window"))
            showMiniWindow = !showMiniWindow;
        ImGui::SameLine();
        ImGui::TextDisabled("(drag it anywhere on screen)");
        return;
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Mini window toggle
    if (ImGui::Button(showMiniWindow ? "Close Mini Window" : "Open Mini Window"))
        showMiniWindow = !showMiniWindow;
    ImGui::SameLine();
    ImGui::TextDisabled("(drag it anywhere on screen)");
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

            // Map server mode strings to display labels
            auto modeLabel = [](const std::string& m) -> const char* {
                if (m == "1s") return "1v1";
                if (m == "2s") return "2v2";
                if (m == "3s") return "3v3";
                return m.c_str();
            };

            // Fixed pixel offsets — keeps columns aligned regardless of font proportionality.
            // The BakkesMod SDK ships pre-1.80 ImGui which lacks the Table API,
            // so we use SetCursorPosX relative to the current window's left edge.
            // Fixed pixel offsets — keeps columns aligned regardless of font proportionality.
            // The BakkesMod SDK ships pre-1.80 ImGui which lacks the Table API,
            // so we use SetCursorPosX relative to the current window's left edge.
            const float colMode   = 0.0f;
            const float colRegion = 38.0f;
            const float colRes    = 84.0f;
            const float colVerif  = 100.0f;   // replay verification badge
            const float colMMR    = 116.0f;
            const float colDate   = 174.0f;

            // Header row
            float baseX = ImGui::GetCursorPosX();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
            ImGui::SetCursorPosX(baseX + colMode);   ImGui::Text("Mode");
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(baseX + colRegion); ImGui::Text("Region");
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(baseX + colRes);    ImGui::Text("Res");
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(baseX + colVerif);  ImGui::Text("Rpl");
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(baseX + colMMR);    ImGui::Text("+/-MMR");
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosX(baseX + colDate);   ImGui::Text("Date");
            ImGui::PopStyleColor();
            ImGui::Separator();

            for (auto& e : matchHistory) {
                // Single-letter result + colour
                // W = win  |  L = loss  |  D = draw  |  U = undecided (no majority)
                const char* result;
                ImVec4      resultCol;
                bool isDraw = (e.outcome == "draw" || e.outcome == "draw_timeout");

                if (isDraw) {
                    result    = "D";
                    resultCol = ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
                } else if (e.outcome == "undecided" || e.outcome == "no_majority") {
                    result    = "U";
                    resultCol = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);
                } else if (e.won) {
                    result    = "W";
                    resultCol = ImVec4(0.2f, 0.95f, 0.45f, 1.0f);
                } else {
                    result    = "L";
                    resultCol = ImVec4(0.95f, 0.3f, 0.3f, 1.0f);
                }

                // Replay verification badge
                const char* verifBadge;
                ImVec4      verifCol;
                if (e.replayStatus == "verified") {
                    verifBadge = "V";
                    verifCol   = ImVec4(0.2f, 0.95f, 0.5f, 1.0f);   // green
                } else if (e.replayStatus == "no_majority" || e.replayStatus == "admin_review") {
                    verifBadge = "?";
                    verifCol   = ImVec4(1.0f, 0.6f, 0.1f, 1.0f);    // orange
                } else {
                    verifBadge = "-";
                    verifCol   = ImVec4(0.35f, 0.35f, 0.35f, 1.0f); // dim
                }

                // MMR delta with colour
                char mmrBuf[16];
                ImVec4 mmrCol;
                if (e.mmrChange > 0.0f) {
                    snprintf(mmrBuf, sizeof(mmrBuf), "+%.0f", e.mmrChange);
                    mmrCol = ImVec4(0.2f, 0.9f, 0.4f, 1.0f);
                } else if (e.mmrChange < 0.0f) {
                    snprintf(mmrBuf, sizeof(mmrBuf), "%.0f", e.mmrChange);
                    mmrCol = ImVec4(0.95f, 0.3f, 0.3f, 1.0f);
                } else {
                    snprintf(mmrBuf, sizeof(mmrBuf), "--");
                    mmrCol = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                }

                char dateBuf[20] = {};
                if (e.timestamp > 0) {
                    struct tm lt{};
                    localtime_s(&lt, &e.timestamp);
                    strftime(dateBuf, sizeof(dateBuf), "%d %b %H:%M", &lt);
                }

                // Each item on the same line via SameLine + SetCursorPosX
                float rowX = ImGui::GetCursorPosX();
                ImGui::SetCursorPosX(rowX + colMode);
                ImGui::TextDisabled("%s", modeLabel(e.mode));
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(rowX + colRegion);
                ImGui::TextDisabled("%s", e.region.c_str());
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(rowX + colRes);
                ImGui::TextColored(resultCol, "%s", result);
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(rowX + colVerif);
                ImGui::TextColored(verifCol, "%s", verifBadge);
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(rowX + colMMR);
                ImGui::TextColored(mmrCol, "%s", mmrBuf);
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(rowX + colDate);
                ImGui::TextDisabled("%s", dateBuf);
            }
        }
    }

    // ── Advanced: replay folder (collapsed by default) ───────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced settings")) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Replay Folder");
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
    }

    // ── Admin panel — visible only after successful F6 console login ──────────
    if (adminUnlocked) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        RenderAdminUI();
    }

}

// ── PluginWindow ───────────────────────────────────────────────────────────────
std::string QueuePlugin::GetMenuName()  { return "rlcustomqueue"; }
std::string QueuePlugin::GetMenuTitle() { return "Custom Queue"; }
void QueuePlugin::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }
bool QueuePlugin::ShouldBlockInput() { return false; }
bool QueuePlugin::IsActiveOverlay()  { return showMiniWindow; }
void QueuePlugin::OnOpen()  {}
void QueuePlugin::OnClose() {}

void QueuePlugin::Render()
{
    if (!showMiniWindow) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 300, 40),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_Appearing);
    bool open = true;
    ImGui::Begin("Custom Queue##mini", &open,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    if (!open) showMiniWindow = false;

    if (!pluginEnabled) {
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), "Custom Queue  [OFF]");
        ImGui::SameLine(0, 10);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.45f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.65f, 0.1f, 1.0f));
        if (ImGui::SmallButton("Enable")) {
            pluginEnabled = true;
            SaveConfig();
            CheckServerStatus();
            auto alive = pluginAlive;
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                FetchRealID();
                ApplyAccountSetup(alive, 0);
            });
        }
        ImGui::PopStyleColor(2);

        if (displayName.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "New here?");
            ImGui::TextWrapped("Open Settings > RL Custom Queue, set a username and enable the plugin to start playing.");
        }
    } else {
        RenderQueueUI(true);  // compact — no dispute section in mini window
    }

    ImGui::End();
}

// ── UI ─────────────────────────────────────────────────────────────────────────
void QueuePlugin::RenderQueueUI(bool compact)
{
    if (matchFound) { RenderMatchFoundUI(); return; }

    // Server status pill (player count moved to above the queue button)
    if (serverChecked) {
        if (serverOnline)
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Server online");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Server offline  (queue unavailable)");
    } else {
        ImGui::TextDisabled("Checking server...");
    }

    RenderLinkUI();
    ImGui::Separator();
    ImGui::Spacing();

    // Ratings row  (stored as float strings — display as integers)
    auto fmtMMR = [](const std::string& s) -> std::string {
        if (s.empty()) return "";
        try { return std::to_string(static_cast<int>(std::stof(s))); }
        catch (...) { return s; }
    };
    if (!mmr1s.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("1v1"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", fmtMMR(mmr1s).c_str());
        ImGui::SameLine(0, 16);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("2v2"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", fmtMMR(mmr2s).c_str());
        ImGui::SameLine(0, 16);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::Text("3v3"); ImGui::SameLine();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "%s", fmtMMR(mmr3s).c_str());
        ImGui::Spacing();
    }

    // Region / mode selectors
    bool locked = inQueue || playerID.empty();
    if (locked) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);

    ImGui::Text("Region");
    ImGui::SetNextItemWidth(200);
    if (!inQueue) {
        int prevRegion = selectedRegion;
        ImGui::Combo("##region", &selectedRegion, REGIONS, IM_ARRAYSIZE(REGIONS));
        if (selectedRegion != prevRegion) SaveConfig();
    } else {
        ImGui::TextDisabled("%s", REGIONS[selectedRegion]);
    }

    ImGui::Spacing();
    ImGui::Text("Game Mode");
    for (int i = 0; i < IM_ARRAYSIZE(MODES); i++) {
        if (i > 0) ImGui::SameLine();
        if (!inQueue) {
            if (ImGui::RadioButton(MODES[i], selectedMode == i)) {
                selectedMode = i;
                SaveConfig();
            }
        } else {
            ImGui::RadioButton(MODES[i], selectedMode == i);
        }
    }
    if (locked) ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::Spacing();

    // Players searching + search timer — shown above the queue button
    if (serverOnline && serverChecked) {
        if (totalOnline > 0)
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                "%d player%s searching", totalOnline, totalOnline == 1 ? "" : "s");
        else
            ImGui::TextDisabled("0 players searching");
    }

    if (inQueue && queueStartTime > 0) {
        int elapsed = (int)(time(nullptr) - queueStartTime);
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", elapsed / 60, elapsed % 60);
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Searching...  %s", timeBuf);
    } else if (playerID.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Go to main menu to connect");
    } else if (inRankedQueue) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), "Cancel your ranked search first");
    } else if (!inQueue) {
        ImGui::TextDisabled("Not in queue");
    }
    ImGui::Spacing();

    // Join / Leave buttons
    if (!inQueue) {
        bool joinBlocked = playerID.empty() || inRankedQueue;

        if (hasPriority) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.1f, 1.0f),
                "You have queue priority (someone declined)");
            ImGui::Spacing();
        }

        if (joinBlocked) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        ImGui::PushStyleColor(ImGuiCol_Button,
            hasPriority ? ImVec4(0.6f, 0.5f, 0.0f, 1.0f) : ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            hasPriority ? ImVec4(0.8f, 0.7f, 0.0f, 1.0f) : ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
        const char* joinLabel = hasPriority ? "Rejoin (Priority)" : "Join Queue";
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

    // ── Dispute report (full panel only — hidden in compact / mini-window mode) ─
    if (compact) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canReport = !lastMatchID.empty()
        && lastMatchTimestamp > 0
        && (time(nullptr) - lastMatchTimestamp) < 3600;

    if (reportSent) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
            "Dispute submitted — under review.");
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
            auto alive = pluginAlive;
            std::thread([this, alive]() {
                std::string newest = FindNewestReplay();
                gameWrapper->Execute([this, newest, alive](GameWrapper* gw) {
                    if (!*alive) return;
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

        ImGui::Text("Lobby Name:");
        ImGui::SetNextItemWidth(220);
        {
            char lnBuf[64]; strncpy_s(lnBuf, sizeof(lnBuf), lobbyName.c_str(), _TRUNCATE);
            ImGui::InputText("##lobbyname", lnBuf, sizeof(lnBuf),
                             ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::Text("Password:");
        ImGui::SetNextItemWidth(220);
        {
            char lpBuf[64]; strncpy_s(lpBuf, sizeof(lpBuf), lobbyPassword.c_str(), _TRUNCATE);
            ImGui::InputText("##lobbypass", lpBuf, sizeof(lpBuf),
                             ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::TextDisabled("(click a field and press Ctrl+A, Ctrl+C to copy)");

        // Team assignment — prominent coloured banner
        if (myTeamIndex == 0 || myTeamIndex == 1) {
            ImGui::Spacing();
            bool  isBlue   = (myTeamIndex == 0);
            ImVec4 bgCol   = isBlue ? ImVec4(0.08f, 0.22f, 0.55f, 1.0f)
                                    : ImVec4(0.55f, 0.22f, 0.02f, 1.0f);
            ImVec4 txtCol  = isBlue ? ImVec4(0.55f, 0.8f, 1.0f, 1.0f)
                                    : ImVec4(1.0f, 0.65f, 0.2f, 1.0f);
            const char* teamLabel = isBlue ? "  BLUE TEAM  — join the LEFT side  "
                                           : "  ORANGE TEAM  — join the RIGHT side  ";
            ImVec2 textSz = ImGui::CalcTextSize(teamLabel);
            ImVec2 pad    = ImVec2(10, 6);
            ImVec2 rectMin = ImGui::GetCursorScreenPos();
            ImVec2 rectMax = ImVec2(rectMin.x + textSz.x + pad.x * 2,
                                    rectMin.y + textSz.y + pad.y * 2);
            ImGui::GetWindowDrawList()->AddRectFilled(rectMin, rectMax, ImGui::ColorConvertFloat4ToU32(bgCol), 4.0f);
            ImGui::SetCursorScreenPos(ImVec2(rectMin.x + pad.x, rectMin.y + pad.y));
            ImGui::TextColored(txtCol, "%s", teamLabel);
            ImGui::Dummy(ImVec2(0, 4));   // gap after banner
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
            if (lobbyReadyTime > 0) {
                int elapsed = (int)(time(nullptr) - lobbyReadyTime);
                char elBuf[16];
                snprintf(elBuf, sizeof(elBuf), "%02d:%02d", elapsed / 60, elapsed % 60);
                ImGui::SameLine(0, 10);
                ImGui::TextDisabled("(%s)", elBuf);
            }
        }

        // Draw countdown (server told us it's coming)
        if (drawCountdown >= 0) {
            ImGui::Spacing();
            int mins = drawCountdown / 60;
            int secs = drawCountdown % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                "⏳ Auto-draw in %dm %02ds", mins, secs);
        }

        // ── Conflict: replay required ─────────────────────────────────────────
        if (awaitingReplay) {
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                "⚠ Result conflict — replay required");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Players disagree on the outcome. Upload your replay so the "
                "server can verify the result automatically.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            ImGui::TextWrapped(
                "If the replay confirms a clear result, MMR is updated instantly. "
                "If it cannot be verified (e.g. Epic ID mismatch or incomplete file), "
                "the match is escalated to an admin for manual review.");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            if (!outcomeStatus.empty()) {
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f),
                    "%s", outcomeStatus.c_str());
                ImGui::Spacing();
            }

            if (replayPickerBusy) {
                ImGui::TextDisabled("Processing replay...");
            } else if (replayWatchActive.load()) {
                // Auto-watcher is running — show live search status and a Browse escape hatch
                ImGui::Spacing();
                if (ImGui::Button("Browse to override##conflict_browse", ImVec2(175, 28)))
                    BrowseReplayAsync();
                ImGui::SameLine(0, 8);
                ImGui::TextDisabled("(pick a file manually instead)");
            } else {
                // Watcher finished (timed out or not yet started) — manual controls
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.4f, 0.75f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f,  0.55f, 0.95f, 1.0f));
                if (ImGui::Button("Upload Newest Replay", ImVec2(170, 30))) {
                    replayPickerBusy = true;
                    UploadReplayForVerification();
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine(0, 8);
                if (ImGui::Button("Browse##conflict", ImVec2(70, 30)))
                    BrowseReplayAsync();

                if (reportReplayBuf[0]) {
                    std::string p  = reportReplayBuf;
                    auto        sl = p.find_last_of("\\/");
                    ImGui::TextDisabled("File: %s",
                        sl != std::string::npos ? p.substr(sl + 1).c_str() : p.c_str());
                } else {
                    ImGui::TextDisabled("(will auto-select newest replay)");
                }
            }

            // Forfeit is still available during a conflict
            ImGui::Separator();
            ImGui::Spacing();
            if (myForfeited) {
                forfeitConfirmPending = false;
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                    "⚑ Forfeit submitted — waiting for teammates...");
            } else if (forfeitConfirmPending) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                    "Confirm forfeit? All on your team must press.");
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("Yes, Forfeit##c", ImVec2(110, 28))) {
                    forfeitConfirmPending = false;
                    ForfeitMatch();
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                if (ImGui::Button("Cancel##fcancel2", ImVec2(70, 28)))
                    forfeitConfirmPending = false;
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("⚑ Forfeit##c", ImVec2(100, 28)))
                    forfeitConfirmPending = true;
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::TextDisabled("(all on your team must press)");
            }
            return;
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
            // Win / Loss / Draw buttons — greyed out until the host has confirmed
            // the lobby is ready (prevents submitting before the match starts)
            bool locked = !lobbyReady;
            if (locked) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
                ImGui::TextDisabled("Waiting for lobby before results can be submitted...");
                ImGui::Spacing();
            }

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.5f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.7f, 0.15f, 1.0f));
            if (ImGui::Button("Win", ImVec2(76, 30)) && !locked) {
                pendingOutcome = "win";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.05f, 0.05f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.05f, 0.05f, 1.0f));
            if (ImGui::Button("Loss", ImVec2(76, 30)) && !locked) {
                pendingOutcome = "loss";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.3f, 0.3f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.15f, 1.0f));
            if (ImGui::Button("Draw", ImVec2(76, 30)) && !locked) {
                pendingOutcome = "draw";
                outcomeConfirm = true;
            }
            ImGui::PopStyleColor(2);

            if (locked) ImGui::PopStyleVar();

            ImGui::Spacing();
            if (!locked)
                ImGui::TextDisabled("Press the result once the game is over.");
        }

        // Forfeit button
        ImGui::Separator();
        ImGui::Spacing();
        if (myForfeited) {
            forfeitConfirmPending = false;
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                "⚑ Forfeit submitted — waiting for teammates...");
        } else if (forfeitConfirmPending) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Confirm forfeit? All on your team must press.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Yes, Forfeit", ImVec2(110, 28))) {
                forfeitConfirmPending = false;
                ForfeitMatch();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel##fcancel", ImVec2(70, 28)))
                forfeitConfirmPending = false;
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("⚑ Forfeit", ImVec2(100, 28)))
                forfeitConfirmPending = true;
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
    if (platform == "Epic") {
        // ── Epic: display name is auto-applied — no manual input required ────────
        if (changingUsername) {
            // User clicked "Change" — show input to override their leaderboard name
            ImGui::Spacing();
            ImGui::Text("Leaderboard name");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            ImGui::InputText("##username", usernameInputBuf, sizeof(usernameInputBuf));
            ImGui::Spacing();
            if (registering) {
                ImGui::TextDisabled("Saving...");
            } else {
                bool hasName = usernameInputBuf[0] != '\0';
                if (!hasName) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.45f, 0.75f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.6f, 0.95f, 1.0f));
                if (ImGui::Button("Save", ImVec2(80, 0)) && hasName) {
                    registering      = true;
                    changingUsername = false;
                    displayName      = usernameInputBuf;
                    auto alive = pluginAlive;
                    gameWrapper->Execute([this, alive](GameWrapper* gw) {
                        if (!*alive) return;
                        FetchRealID();
                        RegisterWithServer();
                    });
                }
                ImGui::PopStyleColor(2);
                if (!hasName) ImGui::PopStyleVar();
                ImGui::SameLine(0, 8);
                if (ImGui::SmallButton("Cancel"))
                    changingUsername = false;
            }
        } else if (!displayName.empty()) {
            // Epic ID fetched, display name known — normal state
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", displayName.c_str());
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("(Epic)");
            ImGui::SameLine(0, 10);
            if (ImGui::SmallButton("Change")) {
                strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                          displayName.c_str(), _TRUNCATE);
                changingUsername = true;
            }
            if (registering)
                ImGui::TextDisabled("Connecting...");
        } else {
            // Epic ID is fetched but GetPlayerName() returned empty —
            // RL hasn't loaded the player name yet. Let them type one manually.
            ImGui::Spacing();
            ImGui::TextDisabled("(Epic) Enter a display name for the leaderboard:");
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
                    auto alive = pluginAlive;
                    gameWrapper->Execute([this, alive](GameWrapper* gw) {
                        if (!*alive) return;
                        FetchRealID();
                        RegisterWithServer();
                    });
                }
                ImGui::PopStyleColor(2);
                if (!hasName) ImGui::PopStyleVar();
            }
        }
    } else {
        // ── Steam (or unknown): manual username input ─────────────────────────────
        if (!displayName.empty() && !changingUsername) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", displayName.c_str());
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("(Steam)");
            ImGui::SameLine(0, 10);
            if (ImGui::SmallButton("Change")) {
                strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                          displayName.c_str(), _TRUNCATE);
                changingUsername = true;
            }
            if (registering)
                ImGui::TextDisabled("Connecting...");
        } else {
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
                    registering      = true;
                    changingUsername = false;
                    displayName      = usernameInputBuf;
                    auto alive = pluginAlive;
                    gameWrapper->Execute([this, alive](GameWrapper* gw) {
                        if (!*alive) return;
                        FetchRealID();
                        RegisterWithServer();
                    });
                }
                ImGui::PopStyleColor(2);
                if (!hasName) ImGui::PopStyleVar();
                if (changingUsername) {
                    ImGui::SameLine(0, 8);
                    if (ImGui::SmallButton("Cancel"))
                        changingUsername = false;
                }
            }
        }
    }

    // Website button — shown below the username in all states
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
    if (ImGui::SmallButton("Website")) {
        std::string url = SERVER_WEBSITE + "?pid=" + ServerID();
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor(2);
}

// ── queue actions ─────────────────────────────────────────────────────────────
void QueuePlugin::JoinQueue()
{
    if (!pluginEnabled) return;
    if (replayWatchActive.load()) { queueStatus = "Upload your replay to resolve the pending match first."; return; }
    if (inRankedQueue) { queueStatus = "Cancel your ranked search first."; return; }

    inQueue        = true;
    matchFound     = false;
    queueStartTime = time(nullptr);
    queueStatus    = "Searching... (" + std::string(MODE_IDS[selectedMode])
                   + " | " + REGIONS[selectedRegion] + ")";

    hasPriority = false;

    // FetchRealID accesses UObject pointers (GetSteamID / GetPlayerName) and must
    // run on the game thread.  Build the body and fire the HTTP request from inside
    // the same Execute callback so playerID is fully populated first.
    auto alive = pluginAlive;
    gameWrapper->Execute([this, alive](GameWrapper* gw) {
        if (!*alive) return;
        FetchRealID();

        std::string body = "{\"player_id\":\"" + ServerID() + "\","
                           "\"system_id\":\"" + systemID + "\","
                           "\"username\":\"" + JsonEscape(displayName) + "\","
                           "\"rl_display_name\":\"" + JsonEscape(platformDisplayName) + "\","
                           "\"region\":\"" + REGIONS[selectedRegion] + "\","
                           "\"mode\":\"" + MODE_IDS[selectedMode] + "\"}";

        HttpPostAsync("/queue/join", body, [this, alive](std::string resp) {
            gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
                if (!*alive) return;
                if (resp.empty()) {
                    inQueue     = false;
                    queueStatus = "Error: server unreachable";
                    return;
                }
                std::string status = JsonStr(resp, "status");
                if (status != "queued") {
                    inQueue = false;
                    // 409: server says we're already in a match we don't know about.
                    // This happens when a match_found SSE was missed (rare race condition).
                    // Recover by treating the server's match data as a match_found event.
                    std::string errType = JsonStr(resp, "error");
                    if (errType == "already_in_match") {
                        queueStatus = "Reconnecting to active match...";
                        OnMatchFound(resp);   // reuse same parsing path
                        StartSSE();
                        PollMatchStatus();
                        return;
                    }
                    std::string detail = JsonStr(resp, "detail");
                    queueStatus = detail.empty() ? "Could not join queue." : detail;
                    return;
                }

                // Open SSE stream. The server replays match state on every
                // (re)connect, so match_found is guaranteed even if the
                // matchmaker fires before this connection is established.
                StartSSE();
                // First heartbeat at 10s — catches a missed match_found SSE event
                // quickly if the VPS proxy is buffering the SSE response body.
                auto a2 = alive;
                gameWrapper->SetTimeout([this, a2](GameWrapper*) {
                    if (!*a2) return;
                    SendHeartbeat();
                }, 10.0f);
            });
        });
    }); // end gameWrapper->Execute
}

void QueuePlugin::LeaveQueue()
{
    StopSSE();
    inQueue        = false;
    matchFound     = false;
    matchID        = "";
    queueStartTime = 0;
    queueStatus    = "Not in queue";

    std::string body = "{\"player_id\":\"" + ServerID() + "\"}";
    HttpPostAsync("/queue/leave", body, [](std::string) {});
}

// ── SSE ────────────────────────────────────────────────────────────────────────
void QueuePlugin::StartSSE()
{
    if (sseActive.exchange(true)) return;  // already running
    auto alive = pluginAlive;
    std::thread([this, alive]() {
        SSELoop(alive);
    }).detach();
}

void QueuePlugin::StopSSE()
{
    sseActive.store(false);
}

void QueuePlugin::SSELoop(std::shared_ptr<std::atomic<bool>> alive)
{
    std::wstring host(SERVER_HOST.begin(), SERVER_HOST.end());

    HINTERNET hSession = WinHttpOpen(
        L"QueuePlugin-SSE/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { sseActive.store(false); return; }

    while (*alive && sseActive.load()) {
        std::string sid  = ServerID();
        std::string path = "/queue/events/" + sid;
        std::wstring wpath(path.begin(), path.end());

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), SERVER_PORT, 0);
        if (!hConnect) { Sleep(3000); continue; }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", wpath.c_str(),
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            Sleep(3000); continue;
        }

        // 90-second receive timeout — server sends a ping every 25s
        DWORD recvTimeout = 90000;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &recvTimeout, sizeof(recvTimeout));
        WinHttpAddRequestHeaders(hRequest,
            L"Accept: text/event-stream\r\nCache-Control: no-cache",
            (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        bool ok = WinHttpSendRequest(hRequest,
                      WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
               && WinHttpReceiveResponse(hRequest, NULL);

        if (!ok) {
            DWORD err = GetLastError();
            gameWrapper->Execute([this, alive, err](GameWrapper*) {
                if (!*alive) return;
                cvarManager->log("[RLCQ][SSE] connect failed, WinHttp error=" + std::to_string(err) + " — retry in 3s");
            });
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            Sleep(3000); continue;
        }

        gameWrapper->Execute([this, alive](GameWrapper*) {
            if (!*alive) return;
            cvarManager->log("[RLCQ][SSE] connected — streaming events");
        });

        // ── stream reading loop ──────────────────────────────────────────────
        std::string buf;
        while (*alive && sseActive.load()) {
            char chunk[4096] = {};
            DWORD bytesRead  = 0;
            if (!WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &bytesRead)
                || bytesRead == 0) {
                DWORD err = GetLastError();
                gameWrapper->Execute([this, alive, err, bytesRead](GameWrapper*) {
                    if (!*alive) return;
                    cvarManager->log("[RLCQ][SSE] read closed (bytesRead=" +
                        std::to_string(bytesRead) + " err=" + std::to_string(err) + ") — reconnecting");
                });
                break;
            }

            buf.append(chunk, bytesRead);

            // Normalise \r\n → \n so the parser works regardless of whether
            // sse-starlette uses CRLF or LF line endings.
            {
                std::string norm;
                norm.reserve(buf.size());
                for (size_t i = 0; i < buf.size(); i++) {
                    if (buf[i] == '\r') {
                        // skip lone \r or \r part of \r\n
                        if (i + 1 < buf.size() && buf[i + 1] == '\n')
                            continue;  // \n will be written on next iteration
                    } else {
                        norm += buf[i];
                    }
                }
                buf = std::move(norm);
            }

            // Parse complete SSE events (delimited by \n\n)
            size_t pos;
            while ((pos = buf.find("\n\n")) != std::string::npos) {
                std::string evt = buf.substr(0, pos);
                buf = buf.substr(pos + 2);

                // Find "data: " line
                const std::string prefix = "data: ";
                auto dp = evt.find(prefix);
                if (dp == std::string::npos) continue;  // ping / comment
                std::string json = evt.substr(dp + prefix.size());
                while (!json.empty() &&
                       (json.back() == '\n' || json.back() == '\r'))
                    json.pop_back();
                if (json.empty()) continue;

                gameWrapper->Execute([this, json, alive](GameWrapper*) {
                    if (!*alive) return;
                    std::string evtType = JsonStr(json, "event");
                    cvarManager->log("[RLCQ][SSE] event received: " + evtType);
                    HandleSSEEvent(json);
                });
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);

        if (*alive && sseActive.load())
            Sleep(2000);  // brief pause before reconnect
    }

    WinHttpCloseHandle(hSession);
    sseActive.store(false);
}

void QueuePlugin::HandleSSEEvent(const std::string& json)
{
    std::string type = JsonStr(json, "event");

    if (type == "match_found") {
        cvarManager->log("[RLCQ][SSE] match_found received — matchFound=" + std::string(matchFound ? "true" : "false"));
        if (!matchFound) OnMatchFound(json);
    }
    else if (type == "player_accepted") {
        acceptedCount = SafeStoi(JsonNum(json, "accepted_count"), acceptedCount);
        totalPlayers  = SafeStoi(JsonNum(json, "total"),          totalPlayers);
    }
    else if (type == "all_accepted") {
        allAccepted   = true;
        acceptedCount = totalPlayers;
    }
    else if (type == "match_cancelled") {
        bool priority = JsonBool(json, "priority");
        if (priority) hasPriority = true;
        std::string reason = JsonStr(json, "reason");
        CancelMatchLocally(reason.empty() ? "Match cancelled." : reason);
    }
    else if (type == "lobby_ready") {
        if (!lobbyReady) {
            lobbyReady     = true;
            lobbyReadyTime = time(nullptr);
            // Server re-sends lobby details in this event for guests
            std::string ln = JsonStr(json, "lobby_name");
            std::string lp = JsonStr(json, "lobby_password");
            if (!ln.empty()) lobbyName     = ln;
            if (!lp.empty()) lobbyPassword = lp;
        }
    }
    else if (type == "collect_replay") {
        // Server finished resolving via buttons; now collecting replays for verification
        std::string endsStr = JsonNum(json, "collection_ends_at");
        if (!endsStr.empty()) {
            try { collectionEndsAt = (time_t)std::stod(endsStr); } catch (...) {}
        }
        // Start watcher if not already running (it may have been started by SubmitOutcome)
        if (!replayWatchActive.load())
            StartReplayWatcher();
    }
    else if (type == "conflict") {
        // Server detected no vote majority — start replay watcher to determine winner
        awaitingReplay = true;
        std::string endsStr = JsonNum(json, "collection_ends_at");
        if (!endsStr.empty()) {
            try { collectionEndsAt = (time_t)std::stod(endsStr); } catch (...) {}
        }
        outcomeStatus  = "⚠ No majority — searching for your replay automatically...";
        if (!replayWatchActive.load())
            StartReplayWatcher();
    }
    else if (type == "match_resolved") {
        // Server resolved the match (via replay majority vote, draw, or admin)
        StopSSE();
        replayWatchActive.store(false);
        std::string outcome = JsonStr(json, "outcome");
        lastMatchID        = matchID;
        lastMatchTimestamp = time(nullptr);
        if (outcome == "disputed") {
            outcomeStatus = "⚠ Replays inconclusive — flagged for admin review.";
        } else if (outcome == "draw") {
            outcomeStatus = "Draw recorded — no MMR change.";
        } else {
            outcomeStatus = "✅ Match resolved — MMR updated!";
        }
        auto alive = pluginAlive;
        gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
            if (!*alive || !matchFound) return;
            CancelMatchLocally("Not in queue");
            FetchMMR();
            FetchHistory();
        }, 3.0f);
    }
    else if (type == "ping") { /* keep-alive, ignore */ }
}

// ── heartbeat ─────────────────────────────────────────────────────────────────
// Primary purpose: keep-alive for the server's stale-player janitor.
// Secondary purpose: fallback match delivery if SSE events are buffered/dropped
// by a reverse proxy (e.g. nginx without proxy_buffering off).  If the server
// reports "in_match" and the plugin hasn't seen a match_found SSE event yet,
// we recover immediately rather than waiting for the next SSE ping.
void QueuePlugin::SendHeartbeat()
{
    if (!inQueue && !matchFound) return;

    std::string body = "{\"player_id\":\"" + ServerID() + "\"}";
    auto alive = pluginAlive;
    HttpPostAsync("/queue/heartbeat", body, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            if (resp.empty()) {
                // Server unreachable — retry in 15s
                gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
                    if (!*alive) return;
                    SendHeartbeat();
                }, 15.0f);
                return;
            }
            std::string status = JsonStr(resp, "status");
            if (status == "in_match" && !matchFound) {
                // SSE match_found was not received — recover via heartbeat
                cvarManager->log("[RLCQ] heartbeat: in_match recovery (SSE event missed)");
                queueStatus = "Match found! (recovered)";
                inQueue     = false;
                OnMatchFound(resp);
                StopSSE();
                StartSSE();
                PollMatchStatus();
                return;
            }
            if (status == "not_in_queue" && inQueue && !matchFound) {
                inQueue     = false;
                queueStatus = "Removed from queue (timeout). Rejoin to continue.";
                StopSSE();
                return;
            }
            // Update total searching count for the UI
            int tot = SafeStoi(JsonNum(resp, "total_searching"), -1);
            if (tot >= 0) totalOnline = tot;
            // Poll every 10s while queued so a missed match_found is caught
            // within one heartbeat cycle (well inside the 60s accept window).
            // Slow to 60s once in a match — SSE handles events from that point.
            float nextInterval = (inQueue && !matchFound) ? 10.0f : 60.0f;
            gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                SendHeartbeat();
            }, nextInterval);
        });
    }, 10000);
}

void QueuePlugin::OnMatchFound(const std::string& resp)
{
    cvarManager->log("[RLCQ] OnMatchFound: matchID=" + JsonStr(resp, "match_id") +
                     " host=" + std::string(JsonBool(resp, "is_host") ? "yes" : "no"));
    pollEpoch++;       // invalidate any old poll chain from a previous match
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
    awaitingReplay     = false;
    matchFoundTime     = time(nullptr);   // stamp when match was found — filters replay picker
    reportSent         = false;
    reportStatus       = "";
    memset(reportReplayBuf, 0, sizeof(reportReplayBuf));
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
}

void QueuePlugin::AcceptMatch()
{
    myAccepted = true;
    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    auto alive = pluginAlive;
    HttpPostAsync("/match/accept", body, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
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
    StopSSE();          // no longer in queue or match — close idle SSE connection
    std::string mid    = matchID;
    matchFound         = false;
    myAccepted         = false;
    allAccepted        = false;
    matchID            = "";
    lobbyName          = "";
    lobbyPassword      = "";
    queueStatus        = "Not in queue";

    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"match_id\":\"" + mid + "\"}";
    HttpPostAsync("/match/decline", body, [](std::string) {});
}

void QueuePlugin::CancelMatchLocally(const std::string& reason)
{
    if (reason.find("declined") != std::string::npos)
        hasPriority = true;

    replayWatchActive.store(false);
    matchFound            = false;
    myAccepted            = false;
    allAccepted           = false;
    lobbyReady            = false;
    isHost                = false;
    myTeamIndex           = -1;
    myForfeited           = false;
    forfeitConfirmPending = false;
    drawCountdown         = -1;
    awaitingReplay        = false;
    matchFoundTime        = 0;
    lobbyReadyTime        = 0;
    collectionEndsAt      = 0;
    outcomeSent        = false;
    outcomeConfirm     = false;
    pendingOutcome     = "";
    outcomeStatus      = "";
    matchID            = "";
    lobbyName          = "";
    lobbyPassword      = "";
    queueStatus        = reason;
}

void QueuePlugin::PollMatchStatus(int epoch)
{
    if (!matchFound) return;

    // Epoch guard: each new "external" call to PollMatchStatus increments the epoch
    // so any older chain's callbacks see a stale epoch and stop scheduling new polls.
    // Recursive calls (from inside the chain) pass their own epoch in to skip the
    // increment — they only proceed if they still own the current epoch.
    if (epoch == -1) {
        epoch = ++pollEpoch;
    } else if (epoch != pollEpoch) {
        return;  // superseded by a newer chain — bail out silently
    }

    auto alive = pluginAlive;
    HttpGetAsync("/match/status/" + matchID, [this, alive, epoch](std::string resp) {
        gameWrapper->Execute([this, resp, alive, epoch](GameWrapper* gw) {
            if (!*alive || !matchFound || epoch != pollEpoch) return;

            std::string status = JsonStr(resp, "status");

            if (status == "cancelled") {
                std::string reason = JsonStr(resp, "reason");
                CancelMatchLocally(reason.empty() ? "Match cancelled." : reason);
                return;
            }

            if (status == "resolved") {
                // Match was fully resolved — clear UI, update MMR display
                StopSSE();
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
                StopSSE();
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
            }

            if (allAccepted && JsonBool(resp, "lobby_ready") && !lobbyReady) {
                lobbyReady     = true;
                lobbyReadyTime = time(nullptr);
            }

            // Server flagged no-majority — prompt player to upload replay
            if (!awaitingReplay && JsonBool(resp, "awaiting_replay")) {
                awaitingReplay = true;
                outcomeStatus  = "⚠ Conflict — please upload your replay";
            }

            // Polling rate:
            //   2s  — accept/deny phase (fast, time-sensitive)
            //   3s  — result submitted, waiting for other players (catch resolution quickly)
            //   60s — lobby ready, match in progress, no result yet (just a keep-alive)
            float nextPoll = !lobbyReady  ? 2.0f
                           : outcomeSent  ? 3.0f
                                          : 60.0f;
            auto alive2 = pluginAlive;
            gameWrapper->SetTimeout([this, alive2, epoch](GameWrapper* gw) {
                if (!*alive2) return;
                PollMatchStatus(epoch);  // pass epoch — keeps the same chain, no increment
            }, nextPoll);
        });
    });
}

// ── result submission ──────────────────────────────────────────────────────────
void QueuePlugin::SubmitOutcome(const std::string& outcome)
{
    if (outcomeSent || matchID.empty()) return;
    outcomeSent   = true;
    outcomeStatus = "Submitting...";

    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"match_id\":\"" + matchID + "\","
                       "\"outcome\":\"" + outcome + "\"}";

    auto alive = pluginAlive;
    HttpPostAsync("/match/result", body, [this, outcome, alive](std::string resp) {
        gameWrapper->Execute([this, resp, outcome, alive](GameWrapper* gw) {
            if (!*alive) return;
            if (resp.empty()) {
                outcomeSent   = false;
                outcomeStatus = "Server unreachable — try again.";
                return;
            }
            std::string s = JsonStr(resp, "status");

            // Read collection window deadline from any response that includes it
            std::string endsStr = JsonNum(resp, "collection_ends_at");
            if (!endsStr.empty()) {
                try { collectionEndsAt = (time_t)std::stod(endsStr); } catch (...) {}
            }

            if (s == "awarded") {
                outcomeStatus = "✅ Result accepted — MMR updated!";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                // Always start the replay watcher so we can verify the result
                if (!replayWatchActive.load()) StartReplayWatcher();
                gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
                    if (!*alive || !matchFound) return;
                    CancelMatchLocally("Not in queue");
                    FetchMMR();
                    FetchHistory();
                }, 3.0f);
            } else if (s == "draw_recorded") {
                outcomeStatus = "Draw recorded — no MMR change.";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                // Still collect replay for draw verification
                if (!replayWatchActive.load()) StartReplayWatcher();
                gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
                    if (!*alive || !matchFound) return;
                    CancelMatchLocally("Not in queue");
                    FetchHistory();
                }, 3.0f);
            } else if (s == "conflict") {
                awaitingReplay = true;
                outcomeStatus  = "⚠ No majority — searching for your replay automatically...";
                if (!replayWatchActive.load()) StartReplayWatcher();
            } else if (s == "disputed") {
                outcomeStatus = "⚠ Conflicting reports — flagged for admin review.";
                lastMatchID        = matchID;
                lastMatchTimestamp = time(nullptr);
                gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
                    if (!*alive || !matchFound) return;
                    CancelMatchLocally("Not in queue");
                }, 4.0f);
            } else if (s == "recorded" || s == "waiting") {
                std::string waitingStr = JsonNum(resp, "waiting");
                int waiting = waitingStr.empty() ? 0 : SafeStoi(waitingStr, 0);
                if (waiting > 0)
                    outcomeStatus = "Recorded — waiting for "
                                  + std::to_string(waiting) + " more player"
                                  + (waiting == 1 ? "" : "s") + " to submit...";
                else
                    outcomeStatus = "Recorded — waiting for other players...";
                // Kick off a fresh poll chain immediately — outcomeSent is now true
                // so PollMatchStatus will schedule follow-up polls every 3s instead
                // of the 60s rate used while the lobby was active.  Without this,
                // the player waits up to 60s for the existing chain to wake up.
                PollMatchStatus();
            } else {
                outcomeStatus = "Unexpected response: " + s;
            }
        });
    });
}

void QueuePlugin::UploadReplayForVerification()
{
    if (matchID.empty()) return;

    std::string mid         = matchID;
    std::string pid         = ServerID();
    time_t      mft         = matchFoundTime;
    // If the user browsed to a specific file, use that; otherwise auto-select
    std::string browsedPath = reportReplayBuf[0] ? std::string(reportReplayBuf) : "";

    // Helper lambda: reset the busy flag and show an error on the game thread.
    // Called from every failure path so the UI button never stays permanently greyed.
    auto alive = pluginAlive;
    auto failUpload = [this, mid, alive](const std::string& msg) {
        gameWrapper->Execute([this, mid, msg, alive](GameWrapper* gw) {
            if (!*alive || matchID != mid) return;
            replayPickerBusy = false;
            outcomeStatus    = msg;
        });
    };

    std::thread([this, mid, pid, mft, browsedPath, failUpload, alive]() {
        if (!browsedPath.empty()) {
            // User picked a file manually — use it immediately, no delay needed
        } else {
            // Auto-select: wait 8 seconds for RL to finish writing the replay file.
            // RL writes replays asynchronously after the match ends; uploading too
            // quickly picks up the previous match's file instead.
            std::this_thread::sleep_for(std::chrono::seconds(8));
        }
        if (!*alive) return;

        std::string path = !browsedPath.empty() ? browsedPath : FindNewestReplay(mft);
        if (path.empty()) {
            failUpload("No replay found — try Browse to pick one manually.");
            return;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            failUpload("Could not open replay file — try Browse to pick one manually.");
            return;
        }
        std::vector<char> data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        file.close();

        if (data.size() < 4096) {
            failUpload("Replay file is too small — it may not have saved yet. Try again.");
            return;
        }

        if (!*alive) return;

        // Upload to the verification endpoint
        HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { failUpload("Upload failed — could not open HTTP session."); return; }

        DWORD timeoutMs = 30000;   // 30 s — replay files are up to a few MB
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            failUpload("Upload failed — could not reach server.");
            return;
        }

        std::wstring wPath = L"/match/upload_replay/"
            + std::wstring(mid.begin(), mid.end())
            + L"?player_id=" + std::wstring(pid.begin(), pid.end());

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            failUpload("Upload failed — could not create HTTP request.");
            return;
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

        if (!*alive) return;

        gameWrapper->Execute([this, response, mid, alive](GameWrapper* gw) {
            if (!*alive || matchID != mid) return;

            replayPickerBusy = false;

            std::string status = JsonStr(response, "status");
            if (status == "auto_resolved") {
                // Conflict resolved by replay majority — server pushed match_resolved SSE
                outcomeStatus = "✅ Replay verified — match resolved!";
            } else if (status == "collected" || status == "verified") {
                // Replay stored and/or confirmed the button result
                std::string verdict = JsonStr(response, "verdict");
                if (verdict == "verified")
                    outcomeStatus = "✅ Replay uploaded and verified.";
                else
                    outcomeStatus = "Replay uploaded — verification pending.";
            } else if (status == "contradicts") {
                // Replay disagrees with button vote — trust penalised
                outcomeStatus = "⚠ Replay result differs from your button vote.";
            } else if (status == "pending_review") {
                // Everyone uploaded but no majority → admin review
                outcomeStatus = "⚠ All replays collected, no majority — sent to admin.";
            } else if (status == "already_uploaded") {
                outcomeStatus = "Replay already submitted for this match.";
            } else if (status == "collection_closed") {
                outcomeStatus = "Upload window closed — result already finalised.";
            } else if (status == "unverifiable") {
                std::string reason = JsonStr(response, "reason");
                outcomeStatus = "Replay could not be verified: " + reason
                              + "\nTry Browse to pick a different file.";
            } else if (!response.empty()) {
                outcomeStatus = "Replay: " + status;
            } else {
                outcomeStatus = "Upload failed — server unreachable.";
            }
        });
    }).detach();
}

// ── replay watcher ────────────────────────────────────────────────────────────
void QueuePlugin::StartReplayWatcher()
{
    if (replayWatchActive.load()) return;  // already running
    replayWatchActive.store(true);

    auto  alive     = pluginAlive;
    std::string mid = matchID;
    std::string pid = ServerID();

    // minReplayTime: oldest file timestamp we'll accept.
    // Use lobby_ready_at as the floor so we never upload a replay from a
    // previous match played in the same session.
    // Fall back to matchFoundTime if lobbyReadyTime hasn't been set yet.
    time_t minReplayTime = (lobbyReadyTime > 0)
        ? lobbyReadyTime
        : (matchFoundTime > 0 ? matchFoundTime : time(nullptr));

    // Deadline: use the server-supplied collection window, or fall back to 3 min
    // from now.  The server window is 3 min from the moment the result was recorded,
    // so there is always at least a few seconds of slack even when we start late.
    time_t deadline = (collectionEndsAt > time(nullptr))
        ? collectionEndsAt
        : (time(nullptr) + REPLAY_COLLECTION_WINDOW);

    std::thread([this, alive, mid, pid, minReplayTime, deadline]() {
        const int POLL_INTERVAL = 10;   // check every 10 seconds
        std::string uploadedPath;       // path we've already sent (one per match)

        while (true) {
            if (!*alive || !replayWatchActive.load()) return;

            time_t now = time(nullptr);

            // ── look for a new replay ──────────────────────────────────────
            if (uploadedPath.empty()) {
                std::string path = FindNewestReplay(minReplayTime);
                if (!path.empty()) {
                    uploadedPath = path;
                    replayWatchActive.store(false);
                    gameWrapper->Execute([this, path, mid, alive](GameWrapper* gw) {
                        if (!*alive || matchID != mid) return;
                        strncpy_s(reportReplayBuf, path.c_str(),
                                  sizeof(reportReplayBuf) - 1);
                        reportReplayBuf[sizeof(reportReplayBuf) - 1] = '\0';
                        replayPickerBusy = false;   // not blocked — auto-upload
                        outcomeStatus    = "Replay found — uploading for verification...";
                        UploadReplayForVerification();
                    });
                    return;
                }
            }

            // ── check if collection window has closed ──────────────────────
            if (now >= deadline) {
                replayWatchActive.store(false);
                if (uploadedPath.empty()) {
                    // No replay found in time — report to server
                    std::string path_no = "/match/no_replay/"
                        + mid + "?player_id=" + pid;
                    // Fire-and-forget HTTP GET (server doesn't need a body)
                    std::string resp = HttpGet(path_no, 10000);
                    gameWrapper->Execute([this, mid, alive](GameWrapper* gw) {
                        if (!*alive || matchID != mid) return;
                        outcomeStatus = "No replay found in time — "
                                        "result recorded without replay verification.";
                    });
                }
                return;
            }

            // ── update UI with time remaining ──────────────────────────────
            if (uploadedPath.empty()) {
                int remaining = (int)(deadline - now);
                gameWrapper->Execute([this, remaining, mid, alive](GameWrapper* gw) {
                    if (!*alive || matchID != mid || !replayWatchActive.load()) return;
                    int mins = remaining / 60;
                    int secs = remaining % 60;
                    if (mins > 0)
                        outcomeStatus = "Searching for replay... " + std::to_string(mins)
                                      + "m " + std::to_string(secs) + "s remaining";
                    else
                        outcomeStatus = "Searching for replay... " + std::to_string(secs) + "s remaining";
                });
            }

            std::this_thread::sleep_for(std::chrono::seconds(POLL_INTERVAL));
        }
    }).detach();
}

// ── lobby ─────────────────────────────────────────────────────────────────────
void QueuePlugin::NotifyLobbyReady()
{
    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    auto alive = pluginAlive;
    HttpPostAsync("/match/lobby_ready", body, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            if (!resp.empty() && !lobbyReady) {
                lobbyReady     = true;
                lobbyReadyTime = time(nullptr);
            }
        });
    });
}

void QueuePlugin::ForfeitMatch()
{
    if (myForfeited || matchID.empty()) return;
    myForfeited = true;
    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    auto alive = pluginAlive;
    HttpPostAsync("/match/forfeit", body, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            if (resp.empty()) {
                myForfeited = false;  // allow retry
            }
        });
    });
}

// ── match history ─────────────────────────────────────────────────────────────
void QueuePlugin::FetchHistory()
{
    if (historyFetching || ServerID().empty()) return;
    historyFetching = true;
    auto alive = pluginAlive;
    HttpGetAsync("/player/" + ServerID() + "/history", [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            historyFetching = false;
            if (resp.empty()) return;
            matchHistory.clear();
            size_t pos = 0;
            while ((pos = resp.find('{', pos)) != std::string::npos) {
                MatchHistoryEntry e;
                e.matchId      = JsonStr(resp.substr(pos), "match_id");
                e.mode         = JsonStr(resp.substr(pos), "mode");
                e.region       = JsonStr(resp.substr(pos), "region");
                e.outcome      = JsonStr(resp.substr(pos), "outcome");
                e.replayStatus = JsonStr(resp.substr(pos), "replay_status");
                e.won          = JsonBool(resp.substr(pos), "won");
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
            SaveConfig();  // persist history locally for offline display
        });
    });
}

// ── admin UI ───────────────────────────────────────────────────────────────────
void QueuePlugin::RenderAdminUI()
{
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "Admin Panel");
    ImGui::Spacing();

    if (ImGui::Button("Refresh##admin")) FetchAdminReports();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.4f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("Lock##admin")) {
        adminUnlocked = false;
        adminReports.clear();
        memset(adminPassBuf, 0, sizeof(adminPassBuf));
    }
    ImGui::PopStyleColor(2);

    if (!adminStatus.empty()) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", adminStatus.c_str());
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (adminFetching) {
        ImGui::TextDisabled("Loading...");
    } else if (adminReports.empty()) {
        ImGui::TextDisabled("No pending reports.");
    } else {
        for (auto& r : adminReports) {
            ImGui::PushID(r.id);
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                "Match: %s", r.matchId.c_str());
            ImGui::Text("Reporter: %s", r.reporterUsername.c_str());
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
            ImGui::Separator();
            ImGui::PopID();
        }
    }
}

// ── server status ─────────────────────────────────────────────────────────────
void QueuePlugin::CheckServerStatus()
{
    // Guard against duplicate calls from any path (RenderSettings, Enable button,
    // mini-window Enable).  serverCheckStarted is set here — first caller wins.
    // This function is ALWAYS called from the game thread, so starting
    // PollServerStatus() here (before the HTTP calls) is safe.
    if (serverCheckStarted) return;
    serverCheckStarted = true;

    // Start the 60-second poll loop exactly once, right now on the game thread.
    // We do this before the HTTP calls so there is no dependency on the callback.
    if (!pollServerStarted) {
        pollServerStarted = true;
        PollServerStatus();
    }

    // The HTTP callbacks only write simple bool/int values.  Writing these
    // directly from the background thread is safe on x86/x64 (aligned word
    // writes are atomic at the hardware level), and avoids calling
    // gameWrapper->Execute() from a background thread — which is the confirmed
    // cause of the BakkesMod crash when the game is transitioning states.
    auto alive = pluginAlive;
    HttpGetAsync("/health", [this, alive](std::string resp) {
        if (!*alive) return;
        serverChecked = true;
        serverOnline  = !resp.empty() && resp.find("ok") != std::string::npos;
    });
    HttpGetAsync("/queue/stats", [this, alive](std::string resp) {
        if (!*alive) return;
        int n = SafeStoi(JsonNum(resp, "total_searching"), 0);
        if (n >= 0) totalOnline = n;
    });
}

void QueuePlugin::PollServerStatus()
{
    auto alive = pluginAlive;
    gameWrapper->SetTimeout([this, alive](GameWrapper* gw) {
        if (!*alive) return;
        // Refresh server status while idle; heartbeat covers it when in-queue.
        if (!inQueue) {
            // Re-fetch without the one-shot guard — temporarily clear it so
            // CheckServerStatus will run, then reinstate the flag.
            serverCheckStarted = false;
            CheckServerStatus();
        }
        PollServerStatus();
    }, 60.0f);
}

// ── config ─────────────────────────────────────────────────────────────────────
void QueuePlugin::LoadConfig()
{
    std::string cfgFile = gameWrapper->GetBakkesModPath().string()
                        + "\\plugins\\rlcq_config.txt";
    std::ifstream in(cfgFile);
    if (!in.is_open()) return;

    // Legacy flat-key values — read for backward-compat migration only.
    std::string legacyUsername, legacyMmr1, legacyMmr2, legacyMmr3;

    // Temporary containers for history reconstruction
    std::map<int, MatchHistoryEntry> histMap;
    int historyCount = 0;

    std::string line;
    while (std::getline(in, line)) {
        auto sep = line.find('=');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);

        if (key == "replay_path") {
            replayPath = val;
            strncpy_s(replayPathBuf, sizeof(replayPathBuf), val.c_str(), _TRUNCATE);
        } else if (key == "plugin_enabled") {
            pluginEnabled = (val == "1");
        } else if (key == "player_id") {
            activeAccountID = val;
        } else if (key == "last_region") {
            int v = std::stoi(val);
            if (v >= 0 && v < IM_ARRAYSIZE(REGIONS)) selectedRegion = v;
        } else if (key == "last_mode") {
            int v = std::stoi(val);
            if (v >= 0 && v < IM_ARRAYSIZE(MODE_IDS)) selectedMode = v;
        // ── legacy flat keys (old format) ────────────────────────────────────
        } else if (key == "username") {
            legacyUsername = val;
        } else if (key == "mmr_1s") {
            legacyMmr1 = val;
        } else if (key == "mmr_2s") {
            legacyMmr2 = val;
        } else if (key == "mmr_3s") {
            legacyMmr3 = val;
        // ── per-account entries ───────────────────────────────────────────────
        // Two formats depending on platform:
        //   Steam: account_{digits}_{field}
        //   Epic:  account_epic_{hexchars}_{field}
        } else if (key.size() > 8 && key.rfind("account_", 0) == 0) {
            std::string rest = key.substr(8);
            std::string id, field;

            if (rest.rfind("epic_", 0) == 0) {
                // Epic — scan alphanumeric chars after the "epic_" prefix.
                // Epic account IDs are alphanumeric (e.g. "1kkpl6k..."), NOT
                // limited to hex digits, so use isalnum here.
                std::string epicRest = rest.substr(5);
                size_t idEnd = 0;
                while (idEnd < epicRest.size() && std::isalnum((unsigned char)epicRest[idEnd]))
                    ++idEnd;
                if (idEnd > 0 && idEnd + 1 < epicRest.size() && epicRest[idEnd] == '_') {
                    id    = "epic_" + epicRest.substr(0, idEnd);
                    field = epicRest.substr(idEnd + 1);
                }
            } else {
                // Steam — scan digits
                size_t idEnd = 0;
                while (idEnd < rest.size() && std::isdigit((unsigned char)rest[idEnd]))
                    ++idEnd;
                if (idEnd > 0 && idEnd + 1 < rest.size() && rest[idEnd] == '_') {
                    id    = rest.substr(0, idEnd);
                    field = rest.substr(idEnd + 1);
                }
            }

            if (!id.empty() && !field.empty()) {
                if      (field == "username")         accounts[id].displayName = val;
                else if (field == "rl_display_name")  accounts[id].platformDisplayName = val;
                else if (field == "mmr_1s")           accounts[id].mmr1s = val;
                else if (field == "mmr_2s")           accounts[id].mmr2s = val;
                else if (field == "mmr_3s")           accounts[id].mmr3s = val;
            }
        // ── match history cache ───────────────────────────────────────────────
        } else if (key == "history_count") {
            historyCount = SafeStoi(val, 0);
        } else if (key.size() > 8 && key.rfind("history_", 0) == 0) {
            std::string rest = key.substr(8);  // e.g. "0_matchid"
            size_t us = rest.find('_');
            if (us != std::string::npos) {
                int idx = SafeStoi(rest.substr(0, us), -1);
                std::string field = rest.substr(us + 1);
                if (idx >= 0 && idx < 10) {
                    auto& e = histMap[idx];
                    if      (field == "matchid")   e.matchId   = val;
                    else if (field == "mode")      e.mode      = val;
                    else if (field == "region")    e.region    = val;
                    else if (field == "outcome")   e.outcome   = val;
                    else if (field == "won")       e.won       = (val == "1");
                    else if (field == "mmrchange") {
                        try { e.mmrChange = std::stof(val); } catch (...) {}
                    }
                    else if (field == "timestamp") e.timestamp = (time_t)SafeStoi(val, 0);
                }
            }
        }
    }

    // Rebuild match history from loaded entries
    matchHistory.clear();
    for (int i = 0; i < historyCount && histMap.count(i); ++i) {
        if (!histMap[i].matchId.empty())
            matchHistory.push_back(histMap[i]);
    }

    // ── Migration: old flat-key config → per-account map ─────────────────────
    // If we have a known activeAccountID and legacy data but no account_ entry
    // yet, seed the map so the data survives the first SaveConfig() rewrite.
    if (!activeAccountID.empty()
        && !legacyUsername.empty()
        && accounts.count(activeAccountID) == 0) {
        accounts[activeAccountID] = { legacyUsername, legacyMmr1, legacyMmr2, legacyMmr3 };
    }

    // ── Restore active account data for immediate offline display ─────────────
    if (!activeAccountID.empty() && accounts.count(activeAccountID)) {
        auto& a = accounts[activeAccountID];
        displayName         = a.displayName;
        platformDisplayName = a.platformDisplayName;
        mmr1s = a.mmr1s;  mmr2s = a.mmr2s;  mmr3s = a.mmr3s;
        strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                  displayName.c_str(), _TRUNCATE);
    }

    // ── Derive platform from the saved account ID ─────────────────────────────
    // FetchRealID() updates this at runtime, but deriving it here means the
    // label shows correctly in offline mode before the main menu fires.
    // Steam IDs are all-digit strings; anything else is Epic or unknown.
    if (!activeAccountID.empty()
        && activeAccountID != systemID
        && std::isdigit((unsigned char)activeAccountID[0]))
        platform = "Steam";
    else if (!activeAccountID.empty())
        platform = "Epic";
    else
        platform = "";
}

void QueuePlugin::SaveConfig()
{
    // Keep the active account's current in-memory values in sync with the map
    // before writing, so every save captures the latest username and MMR.
    if (!activeAccountID.empty()) {
        accounts[activeAccountID] = { displayName, platformDisplayName, mmr1s, mmr2s, mmr3s };
    }

    std::string cfgFile = gameWrapper->GetBakkesModPath().string()
                        + "\\plugins\\rlcq_config.txt";
    std::ofstream out(cfgFile);

    // Global settings (not per-account)
    out << "plugin_enabled=" << (pluginEnabled ? "1" : "0") << "\n";
    out << "replay_path=" << replayPath << "\n";
    out << "player_id=" << activeAccountID << "\n";
    out << "last_region=" << selectedRegion << "\n";
    out << "last_mode=" << selectedMode << "\n";

    // Per-account data — one block per known Steam account
    for (auto& [id, a] : accounts) {
        if (a.displayName.empty()) continue;  // skip unregistered placeholders
        out << "account_" << id << "_username=" << a.displayName << "\n";
        if (!a.platformDisplayName.empty())
            out << "account_" << id << "_rl_display_name=" << a.platformDisplayName << "\n";
        if (!a.mmr1s.empty()) out << "account_" << id << "_mmr_1s=" << a.mmr1s << "\n";
        if (!a.mmr2s.empty()) out << "account_" << id << "_mmr_2s=" << a.mmr2s << "\n";
        if (!a.mmr3s.empty()) out << "account_" << id << "_mmr_3s=" << a.mmr3s << "\n";
    }

    // Match history — cached locally for offline display (last 10 entries)
    out << "history_count=" << matchHistory.size() << "\n";
    for (size_t i = 0; i < matchHistory.size(); ++i) {
        auto& e = matchHistory[i];
        out << "history_" << i << "_matchid="   << e.matchId   << "\n";
        out << "history_" << i << "_mode="      << e.mode      << "\n";
        out << "history_" << i << "_region="    << e.region    << "\n";
        out << "history_" << i << "_outcome="   << e.outcome   << "\n";
        out << "history_" << i << "_won="       << (e.won ? "1" : "0") << "\n";
        out << "history_" << i << "_mmrchange=" << e.mmrChange << "\n";
        out << "history_" << i << "_timestamp=" << e.timestamp << "\n";
    }
}

// ── dispute / replay ───────────────────────────────────────────────────────────
void QueuePlugin::BrowseReplayAsync()
{
    if (replayPickerBusy) return;
    replayPickerBusy = true;
    auto alive = pluginAlive;
    std::thread([this, alive]() {
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
            gameWrapper->Execute([this, path = std::string(filePath), alive](GameWrapper* gw) {
                if (!*alive) return;
                strncpy_s(reportReplayBuf, sizeof(reportReplayBuf),
                          path.c_str(), _TRUNCATE);
                replayPickerBusy = false;
            });
        } else {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
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
    std::string pid = ServerID();

    auto alive = pluginAlive;
    std::thread([this, path, mid, pid, alive]() {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
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
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                reportPending = false; reportStatus = "Server unreachable.";
            });
            return;
        }
        std::wstring wHost(SERVER_HOST.begin(), SERVER_HOST.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), SERVER_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
                reportPending = false; reportStatus = "Server unreachable.";
            });
            return;
        }
        std::wstring wPath = L"/match/report/"
            + std::wstring(mid.begin(), mid.end())
            + L"?reporter=" + std::wstring(pid.begin(), pid.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            gameWrapper->Execute([this, alive](GameWrapper* gw) {
                if (!*alive) return;
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

        gameWrapper->Execute([this, response, alive](GameWrapper* gw) {
            if (!*alive) return;
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

std::string QueuePlugin::FindNewestReplay(time_t minTime)
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

    // Convert minTime (Unix time_t) to FILETIME for comparison.
    // FILETIME counts 100-nanosecond intervals since 1601-01-01.
    // Unix epoch (1970-01-01) is 116444736000000000 intervals later.
    // NOTE: do NOT use Int32x32To64 here — time_t is 64-bit on modern Windows
    // and casting it to LONG truncates at 2038. Use a plain 64-bit multiply.
    FILETIME minFT = {};
    if (minTime > 0) {
        LONGLONG ll = ((LONGLONG)minTime) * 10000000LL + 116444736000000000LL;
        minFT.dwLowDateTime  = (DWORD)(ll & 0xFFFFFFFF);
        minFT.dwHighDateTime = (DWORD)(ll >> 32);
    }

    std::string newest;
    FILETIME latestTime = {};
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((folder + "*.replay").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return "";
    do {
        // Skip files written before the match was found — avoids picking up
        // replays from earlier ranked/casual games in the same session.
        if (minTime > 0 && CompareFileTime(&fd.ftLastWriteTime, &minFT) <= 0)
            continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &latestTime) > 0) {
            latestTime = fd.ftLastWriteTime;
            newest     = folder + fd.cFileName;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return newest;
}

// ── admin ──────────────────────────────────────────────────────────────────────
void QueuePlugin::TryAdminLogin(const std::string& password)
{
    // Brute-force protection: 3 failures → 10-minute lockout
    if (adminCooldownUntil > 0 && time(nullptr) < adminCooldownUntil) {
        int secs = (int)(adminCooldownUntil - time(nullptr));
        cvarManager->log("Admin locked. Try again in "
            + std::to_string(secs / 60) + "m "
            + std::to_string(secs % 60) + "s.");
        return;
    }

    if (password.empty()) {
        cvarManager->log("Usage: rlcq_admin <password>");
        return;
    }

    // Verify against server — valid password returns a JSON array,
    // wrong password returns {"detail":"Unauthorized"}
    auto alive = pluginAlive;
    HttpGetAsync("/admin/reports?password=" + password,
        [this, password, alive](std::string resp) {
            gameWrapper->Execute([this, resp, password, alive](GameWrapper* gw) {
                if (!*alive) return;

                bool ok = !resp.empty()
                       && resp.find("Unauthorized") == std::string::npos
                       && resp.find("\"detail\"")    == std::string::npos;

                if (ok) {
                    adminUnlocked      = true;
                    adminAttempts      = 0;
                    adminCooldownUntil = 0;
                    strncpy_s(adminPassBuf, sizeof(adminPassBuf),
                              password.c_str(), _TRUNCATE);

                    // Parse the reports already in the response
                    adminReports.clear();
                    size_t pos = 0;
                    while ((pos = resp.find("\"id\"", pos)) != std::string::npos) {
                        size_t sp = pos > 0 ? pos - 1 : 0;
                        ReportEntry e;
                        e.id               = SafeStoi(JsonNum(resp.substr(sp), "id"));
                        e.matchId          = JsonStr(resp.substr(sp), "match_id");
                        e.reporterUsername = JsonStr(resp.substr(sp), "reporter_username");
                        e.submittedAt      = JsonNum(resp.substr(sp), "submitted_at");
                        e.status           = JsonStr(resp.substr(sp), "status");
                        adminReports.push_back(e);
                        pos += 4;
                    }
                    adminStatus = adminReports.empty() ? "No pending reports." : "";
                    cvarManager->log("Admin panel unlocked. ("
                        + std::to_string(adminReports.size()) + " pending report(s))");
                } else {
                    adminAttempts++;
                    if (adminAttempts >= 3) {
                        adminCooldownUntil = time(nullptr) + 600;
                        adminAttempts      = 0;
                        cvarManager->log("Too many failed attempts. Admin locked for 10 minutes.");
                    } else {
                        cvarManager->log("Wrong password. "
                            + std::to_string(3 - adminAttempts)
                            + " attempt(s) remaining.");
                    }
                }
            });
        });
}

void QueuePlugin::FetchAdminReports()
{
    adminFetching = true;
    adminStatus   = "";
    std::string pass(adminPassBuf);
    auto alive = pluginAlive;
    HttpGetAsync("/admin/reports?password=" + pass, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
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
    auto alive = pluginAlive;
    HttpPostAsync("/admin/match/accept/" + matchId + "?password=" + pass, "{}",
        [this, alive](std::string resp) {
            gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
                if (!*alive) return;
                adminStatus = resp.find("accepted") != std::string::npos
                    ? "Result accepted." : "Failed.";
                FetchAdminReports();
            });
        });
}

void QueuePlugin::AdminCancelMatch(const std::string& matchId)
{
    std::string pass(adminPassBuf);
    auto alive = pluginAlive;
    HttpPostAsync("/admin/match/cancel/" + matchId + "?password=" + pass, "{}",
        [this, alive](std::string resp) {
            gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
                if (!*alive) return;
                adminStatus = resp.find("cancelled") != std::string::npos
                    ? "Match cancelled, MMR reversed." : "Failed.";
                FetchAdminReports();
            });
        });
}

// ── account ────────────────────────────────────────────────────────────────────
void QueuePlugin::LookupAccountByRealID()
{
    if (playerID.empty() || playerID == systemID) return;  // no valid player_id yet

    // Capture account-specific values by value now — the HTTP response arrives
    // asynchronously and the user may have switched accounts before it returns.
    // Using captured values guarantees we act on the correct account's data.
    std::string capturedPlayerID      = playerID;
    std::string capturedDisplayName = displayName;
    std::string capturedPlatform    = platform;
    std::string capturedPDN         = platformDisplayName;

    auto alive = pluginAlive;
    HttpGetAsync("/account/lookup?player_id=" + capturedPlayerID,
        [this, alive, capturedPlayerID, capturedDisplayName, capturedPlatform, capturedPDN]
        (std::string resp) {
        gameWrapper->Execute([this, resp, alive,
                              capturedPlayerID, capturedDisplayName,
                              capturedPlatform, capturedPDN]
            (GameWrapper* gw) {
            if (!*alive) return;
            if (resp.empty()) return;

            // Only apply if the active account is still the one we looked up
            if (activeAccountID != capturedPlayerID) {
                cvarManager->log("[RLCQ] Lookup response for " + capturedPlayerID.substr(0,12)
                                 + "... ignored — account already switched.");
                return;
            }

            bool found = JsonBool(resp, "found");
            if (!found) {
                cvarManager->log("[RLCQ] Lookup: no existing account — registering fresh.");
                if (!capturedDisplayName.empty() && pluginEnabled)
                    RegisterWithServer();
                return;
            }

            std::string serverUsername = JsonStr(resp, "username");
            std::string v1 = JsonNum(resp, "mmr_1s");
            std::string v2 = JsonNum(resp, "mmr_2s");
            std::string v3 = JsonNum(resp, "mmr_3s");
            cvarManager->log("[RLCQ] Lookup: found '" + serverUsername + "' — restoring.");

            // Restore username (server wins over local if we had nothing)
            if (!serverUsername.empty() && displayName.empty()) {
                displayName = serverUsername;
                strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                          displayName.c_str(), _TRUNCATE);
            }
            // Restore MMR
            if (!v1.empty()) mmr1s = v1;
            if (!v2.empty()) mmr2s = v2;
            if (!v3.empty()) mmr3s = v3;

            SaveConfig();

            // Re-register to link the player_id on the server
            if (pluginEnabled) RegisterWithServer();
        });
    });
}

void QueuePlugin::RegisterWithServer()
{
    std::string body = "{\"player_id\":\"" + ServerID() + "\","
                       "\"system_id\":\"" + systemID + "\","
                       "\"username\":\"" + JsonEscape(displayName) + "\","
                       "\"rl_display_name\":\"" + JsonEscape(platformDisplayName) + "\","
                       "\"platform\":\"" + platform + "\"}";
    auto alive = pluginAlive;
    HttpPostAsync("/account/register", body, [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            registering = false;
            if (!resp.empty()) {
                SaveConfig();
                FetchMMR();
                FetchHistory();  // pre-fill match history UI on enable/register
            }
        });
    });
}

void QueuePlugin::FetchMMR()
{
    auto alive = pluginAlive;
    HttpGetAsync("/player/" + ServerID() + "/mmr", [this, alive](std::string resp) {
        gameWrapper->Execute([this, resp, alive](GameWrapper* gw) {
            if (!*alive) return;
            std::string v1 = JsonNum(resp, "mmr_1s");
            std::string v2 = JsonNum(resp, "mmr_2s");
            std::string v3 = JsonNum(resp, "mmr_3s");
            bool changed = false;
            if (!v1.empty()) { mmr1s = v1; changed = true; }
            if (!v2.empty()) { mmr2s = v2; changed = true; }
            if (!v3.empty()) { mmr3s = v3; changed = true; }
            if (changed) SaveConfig();  // persist so ratings survive offline launches
        });
    });
}

// Retry every 5 seconds until Epic ID is available (no hard limit).
// Epic's auth can take anywhere from a few seconds to over a minute
// depending on network conditions, so we just keep polling.
static const float EPIC_ID_RETRY_INTERVAL = 5.0f;

void QueuePlugin::ApplyAccountSetup(std::shared_ptr<std::atomic<bool>> alive, int attempt)
{
    bool isEpicFallback = (playerID == systemID);
    if (isEpicFallback && platform == "Epic") {
        // Epic ID not ready yet — keep retrying every 5 seconds.
        cvarManager->log("[RLCQ] Epic ID not ready, retry #" + std::to_string(attempt + 1)
                         + " in " + std::to_string((int)EPIC_ID_RETRY_INTERVAL) + "s"
                         + "  GetEpicID='" + gameWrapper->GetEpicID() + "'"
                         + "  uid.str='" + gameWrapper->GetUniqueID().str() + "'");
        gameWrapper->SetTimeout([this, alive, attempt](GameWrapper*) {
            if (!*alive) return;
            FetchRealID();
            ApplyAccountSetup(alive, attempt + 1);
        }, EPIC_ID_RETRY_INTERVAL);
        return;
    }

    if (isEpicFallback) return; // Steam fallback path — shouldn't happen, bail

    // We have a valid playerID — run account switch / first-time setup.
    if (playerID != activeAccountID) {
        bool switched = !activeAccountID.empty();

        if (accounts.count(playerID)) {
            auto& a = accounts[playerID];
            displayName         = a.displayName;
            platformDisplayName = a.platformDisplayName;
            mmr1s = a.mmr1s;  mmr2s = a.mmr2s;  mmr3s = a.mmr3s;
            strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                      displayName.c_str(), _TRUNCATE);
            if (switched)
                cvarManager->log("[RLCQ] Account switched — loaded: " + displayName);
        } else {
            // First time seeing this account on this install.
            // Step 1: try to auto-fill from GetPlayerName() (Epic only — works
            //         once RL's session is loaded; may be empty on main menu).
            if (!platformDisplayName.empty()) {
                displayName = platformDisplayName;
                strncpy_s(usernameInputBuf, sizeof(usernameInputBuf),
                          displayName.c_str(), _TRUNCATE);
                cvarManager->log("[RLCQ] Auto-fill from platform name: '" + displayName + "'");
            } else {
                displayName = "";
                memset(usernameInputBuf, 0, sizeof(usernameInputBuf));
            }
            mmr1s = mmr2s = mmr3s = "";
        }

        changingUsername = false;
        activeAccountID  = playerID;
        SaveConfig();

        // Step 2: ask the server if this player_id already has a saved account
        // (covers re-installs, new PCs, account switches on the same machine).
        // LookupAccountByRealID() will overwrite the local display name with the
        // server's saved username if one exists, then call RegisterWithServer()
        // to register/update the player_id on the server.
        LookupAccountByRealID();
    } else {
        // Same account already active — no registration needed.
        // Still load fresh history from server when the plugin is enabled.
        if (pluginEnabled && !ServerID().empty())
            FetchHistory();
    }
}

void QueuePlugin::FetchRealID()
{
    // Grab the in-game display name for replay verification.
    // GetPlayerName() only returns a value once RL has fully loaded the player
    // session — it may be empty on the main menu and non-empty in-game.
    std::string nameStr = gameWrapper->GetPlayerName().ToString();
    cvarManager->log("[RLCQ] FetchRealID: GetPlayerName()='" + nameStr + "'");
    if (!nameStr.empty())
        platformDisplayName = nameStr;

    unsigned long long steamUID = gameWrapper->GetSteamID();
    if (steamUID != 0) {
        playerID   = std::to_string(steamUID);
        platform = "Steam";
        cvarManager->log("[RLCQ] FetchRealID: Steam UID = " + playerID
                         + " displayName='" + platformDisplayName + "'");
    } else {
        // Epic Games account — try both GetEpicID() and GetUniqueID()
        std::string epicID      = gameWrapper->GetEpicID();
        UniqueIDWrapper uid     = gameWrapper->GetUniqueID();
        std::string uidEpicID   = uid.GetEpicAccountID();
        std::string uidStr      = uid.str();
        std::string uidIdString = uid.GetIdString();

        cvarManager->log("[RLCQ] FetchRealID: GetEpicID()='" + epicID + "'"
                         + " uid.GetEpicAccountID()='" + uidEpicID + "'"
                         + " uid.str()='" + uidStr + "'"
                         + " uid.GetIdString()='" + uidIdString + "'");

        // Use GetEpicID() first, fall back to GetUniqueID().GetEpicAccountID()
        std::string best = !epicID.empty() ? epicID
                         : !uidEpicID.empty() ? uidEpicID
                         : "";

        if (!best.empty()) {
            playerID   = "epic_" + best;
            platform = "Epic";
            cvarManager->log("[RLCQ] FetchRealID: resolved Epic ID = " + playerID);
        } else {
            // Still empty — called too early before Epic auth finished.
            // Keep previous value; MainMenuAdded hook will retry.
            if (playerID.empty()) {
                playerID   = systemID;   // worst-case fallback: install ID
                platform = "Epic";
            }
            cvarManager->log("[RLCQ] FetchRealID: Epic ID not available yet, using = " + playerID);
        }
    }
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
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
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
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
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
    // Capture pluginAlive by value so the shared_ptr keeps the flag alive
    // even if the plugin is unloaded while the HTTP request is in flight.
    auto alive = pluginAlive;
    std::thread([this, path, body, callback, timeoutMs, alive]() {
        std::string resp = HttpPost(path, body, timeoutMs);
        if (!*alive) return;
        callback(resp);
    }).detach();
}

void QueuePlugin::HttpGetAsync(const std::string& path,
                               std::function<void(std::string)> callback,
                               DWORD timeoutMs)
{
    auto alive = pluginAlive;
    std::thread([this, path, callback, timeoutMs, alive]() {
        std::string resp = HttpGet(path, timeoutMs);
        if (!*alive) return;
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
