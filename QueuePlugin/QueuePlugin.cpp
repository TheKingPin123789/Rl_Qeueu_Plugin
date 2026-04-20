#include "QueuePlugin.h"
#include "imgui/imgui.h"
#include <windows.h>
#include <winhttp.h>
#include <thread>
#include <sstream>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

BAKKESMOD_PLUGIN(QueuePlugin, "RL Custom Queue", "0.1", PLUGINTYPE_FREEPLAY)

static std::string rand_str(int n)
{
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    srand((unsigned)GetTickCount64());
    for (int i = 0; i < n; i++)
        result += chars[rand() % (sizeof(chars) - 1)];
    return result;
}

static const char* REGIONS[] = { "NAE", "NAW", "EU", "OCE", "SAM", "ME", "ASIA" };
static const char* MODES[]   = { "1v1 (1s)", "2v2 (2s)", "3v3 (3s)" };
static const char* MODE_IDS[]= { "1s", "2s", "3s" };

// ── lifecycle ──────────────────────────────────────────────────────────────────
void QueuePlugin::onLoad()
{
    // permanent BakkesMod install ID — tied to this machine, not the RL account
    std::string idFile = gameWrapper->GetBakkesModPath().string() + "\\plugins\\rlcq_id.txt";
    std::ifstream in(idFile);
    if (in.is_open()) { std::getline(in, playerID); in.close(); }
    if (playerID.empty()) {
        playerID = "bm_" + rand_str(12);
        std::ofstream out(idFile);
        out << playerID;
    }

    // configurable server address — saved across sessions
    cvarManager->registerCvar("rlcq_host", "127.0.0.1", "Queue server IP address", true, false, 0, false, 0, true);
    cvarManager->registerCvar("rlcq_port", "8000",      "Queue server port",       true, false, 0, false, 0, true);

    cvarManager->getCvar("rlcq_host").addOnValueChanged([this](std::string, CVarWrapper cvar) {
        serverHost = cvar.getStringValue();
    });
    cvarManager->getCvar("rlcq_port").addOnValueChanged([this](std::string, CVarWrapper cvar) {
        serverPort = cvar.getIntValue();
    });

    serverHost = cvarManager->getCvar("rlcq_host").getStringValue();
    serverPort = cvarManager->getCvar("rlcq_port").getIntValue();

    // real ID = current RL account (Epic/Steam) — fetched safely once main menu is ready
    gameWrapper->HookEvent("Function TAGame.GFxData_MainMenu_TA.MainMenuAdded",
        [this](std::string) { FetchRealID(); });

    HookMatchEnd();
}

void QueuePlugin::onUnload()
{
    if (inQueue) LeaveQueue();
}

// ── PluginSettingsWindow ───────────────────────────────────────────────────────
std::string QueuePlugin::GetPluginName() { return "RL Custom Queue"; }
void QueuePlugin::RenderSettings() { RenderQueueUI(); }

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
    ImGui::SetNextWindowSize(ImVec2(340, 260), ImGuiCond_FirstUseEver);
    ImGui::Begin("Custom Queue", nullptr, ImGuiWindowFlags_NoResize);
    RenderQueueUI();
    ImGui::End();
}

// ── UI ─────────────────────────────────────────────────────────────────────────
void QueuePlugin::RenderQueueUI()
{
    if (matchFound) { RenderMatchFoundUI(); return; }

    // server settings
    ImGui::TextDisabled("Server");
    static char hostBuf[128];
    static bool hostInit = false;
    if (!hostInit) { strncpy_s(hostBuf, serverHost.c_str(), sizeof(hostBuf)); hostInit = true; }
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("IP##host", hostBuf, sizeof(hostBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        cvarManager->getCvar("rlcq_host").setValue(std::string(hostBuf));
    }
    ImGui::SameLine();
    static char portBuf[8];
    static bool portInit = false;
    if (!portInit) { strncpy_s(portBuf, std::to_string(serverPort).c_str(), sizeof(portBuf)); portInit = true; }
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Port##port", portBuf, sizeof(portBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
        cvarManager->getCvar("rlcq_port").setValue(std::string(portBuf));
    }
    ImGui::TextDisabled("BM ID: %s", playerID.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    if (inQueue) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);

    ImGui::Text("Region");
    ImGui::SetNextItemWidth(200);
    if (!inQueue)
        ImGui::Combo("##region", &selectedRegion, REGIONS, IM_ARRAYSIZE(REGIONS));
    else
        ImGui::TextDisabled("%s", REGIONS[selectedRegion]);

    ImGui::Spacing();
    ImGui::Text("Game Mode");
    for (int i = 0; i < IM_ARRAYSIZE(MODES); i++)
    {
        if (i > 0) ImGui::SameLine();
        if (!inQueue) {
            if (ImGui::RadioButton(MODES[i], selectedMode == i)) selectedMode = i;
        } else {
            ImGui::RadioButton(MODES[i], selectedMode == i);
        }
    }

    if (inQueue) ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::TextColored(
        inQueue ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "%s", queueStatus.c_str()
    );
    ImGui::Spacing();

    if (!inQueue) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
        if (ImGui::Button("Join Queue", ImVec2(140, 32))) JoinQueue();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("Leave Queue", ImVec2(140, 32))) LeaveQueue();
        ImGui::PopStyleColor(2);
    }
}

void QueuePlugin::RenderMatchFoundUI()
{
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "MATCH FOUND!");
    ImGui::Separator();
    ImGui::Spacing();

    if (isHost) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "You are the HOST");
        ImGui::TextWrapped("Create a private match with:");
    } else {
        ImGui::TextWrapped("Join the private match:");
    }

    ImGui::Spacing();
    ImGui::Text("Lobby Name:"); ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", lobbyName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##name")) CopyToClipboard(lobbyName);

    ImGui::Text("Password:  "); ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", lobbyPassword.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##pass")) CopyToClipboard(lobbyPassword);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
    if (ImGui::Button("Accept", ImVec2(100, 30))) AcceptMatch();
    ImGui::PopStyleColor(2);

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("Decline", ImVec2(100, 30))) DeclineMatch();
    ImGui::PopStyleColor(2);
}

// ── queue actions ──────────────────────────────────────────────────────────────
void QueuePlugin::JoinQueue()
{

    inQueue     = true;
    matchFound  = false;
    queueStatus = "Searching... (" + std::string(MODE_IDS[selectedMode])
                + " | " + REGIONS[selectedRegion] + ")";

    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"real_id\":\"" + realID + "\","
                       "\"region\":\"" + REGIONS[selectedRegion] + "\","
                       "\"mode\":\"" + MODE_IDS[selectedMode] + "\"}";

    HttpPostAsync("/queue/join", body, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (resp.empty()) queueStatus = "Error: server unreachable";
            else StartPolling();
        });
    });
}

void QueuePlugin::LeaveQueue()
{
    inQueue    = false;
    matchFound = false;
    matchID    = "";
    queueStatus = "Not in queue";

    std::string body = "{\"player_id\":\"" + playerID + "\"}";
    HttpPostAsync("/queue/leave", body, [](std::string) {});
}

// ── polling ────────────────────────────────────────────────────────────────────
void QueuePlugin::StartPolling()
{
    gameWrapper->SetTimeout([this](GameWrapper* gw) { PollOnce(); }, 3.0f);
}

void QueuePlugin::PollOnce()
{
    if (!inQueue || matchFound) return;

    HttpGetAsync("/queue/status/" + playerID, [this](std::string resp) {
        gameWrapper->Execute([this, resp](GameWrapper* gw) {
            if (resp.empty()) { StartPolling(); return; }

            std::string status = JsonStr(resp, "status");
            if (status == "match_found") {
                OnMatchFound(resp);
            } else {
                StartPolling();
            }
        });
    });
}

void QueuePlugin::OnMatchFound(const std::string& resp)
{
    matchFound    = true;
    inQueue       = false;
    matchID       = JsonStr(resp, "match_id");
    lobbyName     = JsonStr(resp, "lobby_name");
    lobbyPassword = JsonStr(resp, "lobby_password");
    isHost        = JsonBool(resp, "is_host");
    queueStatus   = "Match found!";

    // parse player real IDs array: "real_ids":["id1","id2",...]
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

    if (isHost) SendPartyInvites();
}

void QueuePlugin::AcceptMatch()
{
    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    HttpPostAsync("/match/accept", body, [](std::string) {});
}

void QueuePlugin::DeclineMatch()
{
    matchFound  = false;
    matchID     = "";
    lobbyName   = "";
    lobbyPassword = "";
    queueStatus = "Not in queue";

    std::string body = "{\"player_id\":\"" + playerID + "\","
                       "\"match_id\":\"" + matchID + "\"}";
    HttpPostAsync("/match/decline", body, [](std::string) {});
}

// ── replay ─────────────────────────────────────────────────────────────────────
void QueuePlugin::HookMatchEnd()
{
    gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
        [this](std::string) {
            if (matchID.empty()) return;
            // wait for RL to finish saving the replay
            gameWrapper->SetTimeout([this](GameWrapper* gw) {
                UploadNewestReplay();
            }, 6.0f);
        });
}

void QueuePlugin::UploadNewestReplay()
{
    std::string path = FindNewestReplay();
    if (path.empty()) return;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;
    std::vector<char> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    file.close();

    // send as raw bytes; backend reads request body
    std::thread([this, path, data]() {
        HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return;

        std::wstring wHost(serverHost.begin(), serverHost.end());
        HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), serverPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return; }

        std::wstring wPath = L"/match/replay/" + std::wstring(matchID.begin(), matchID.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

        WinHttpSendRequest(hRequest,
            L"Content-Type: application/octet-stream\r\n", -1,
            (LPVOID)data.data(), (DWORD)data.size(), (DWORD)data.size(), 0);
        WinHttpReceiveResponse(hRequest, nullptr);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        gameWrapper->Execute([this](GameWrapper* gw) {
            matchFound  = false;
            matchID     = "";
            queueStatus = "Replay uploaded!";
        });
    }).detach();
}

std::string QueuePlugin::FindNewestReplay()
{
    char userprofile[MAX_PATH];
    GetEnvironmentVariableA("USERPROFILE", userprofile, MAX_PATH);
    std::string folder = std::string(userprofile) +
        "\\Documents\\My Games\\Rocket League\\TAGame\\Demos\\";

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

// ── HTTP ───────────────────────────────────────────────────────────────────────
std::string QueuePlugin::HttpPost(const std::string& path, const std::string& body)
{
    HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    std::wstring wHost(serverHost.begin(), serverHost.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), serverPort, 0);
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

std::string QueuePlugin::HttpGet(const std::string& path)
{
    HINTERNET hSession = WinHttpOpen(L"QueuePlugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    std::wstring wHost(serverHost.begin(), serverHost.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), serverPort, 0);
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
                                std::function<void(std::string)> callback)
{
    std::thread([this, path, body, callback]() {
        callback(HttpPost(path, body));
    }).detach();
}

void QueuePlugin::HttpGetAsync(const std::string& path,
                               std::function<void(std::string)> callback)
{
    std::thread([this, path, callback]() {
        callback(HttpGet(path));
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

// ── misc ───────────────────────────────────────────────────────────────────────
void QueuePlugin::FetchRealID()
{
    // safe — only called from MainMenuAdded hook when game is fully ready
    try {
        auto uid = gameWrapper->GetSteamID();
        if (uid != 0) { realID = std::to_string(uid); return; }
    } catch (...) {}

    try {
        auto pc = gameWrapper->GetPlayerController();
        if (!pc.IsNull()) {
            auto pri = pc.GetPRI();
            if (!pri.IsNull()) {
                auto idStr = pri.GetUniqueIdWrapper().GetIdString();
                if (!idStr.empty() && idStr != "0") { realID = idStr; }
            }
        }
    } catch (...) {}
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
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}
