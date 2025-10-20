// BGvACC.cpp
// Custom EuroScope plugin for BGvACC. Checks sector file cycle/version and notifies user if newer exists.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#include <winhttp.h>
#include <string>
#include <regex>
#include <sstream>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#include <windows.h>

#include "EuroScopePlugIn.h"
using namespace EuroScopePlugIn;

static const char* DEFAULT_MANIFEST_URL = "https://raw.githubusercontent.com/andreitzenov/LBSR-UpdatePlugin/refs/heads/main/version.json";
static const char* WELCOME_JSON_URL = "https://raw.githubusercontent.com/andreitzenov/LBSR-UpdatePlugin/refs/heads/main/welcome.json";

struct ParsedLocal {
    int airac_cycle = 0;     // e.g. 2510
    int airac_version = 0;   // e.g. 2
    int package_version = 0; // e.g. 1
    std::string package_name;
};

struct RemoteLatest {
    int airac_cycle = 0;
    int airac_version = 0;
    int package_version = 0;
    std::string latest_package_name;
    std::string download_url;
    std::string notes;
};

static std::string ToString(int v) { std::ostringstream os; os << v; return os.str(); }

static bool ParseLocalFromInfoString(const std::string& s, ParsedLocal& out) {
    std::regex rx(R"((25\d{2})\s*/\s*([0-9]+)(?:-([0-9]+))?\s+[A-Z]{4}\s+(20\d{6}))");
    std::smatch m;
    if (!std::regex_search(s, m, rx)) return false;

    out.airac_cycle = std::stoi(m[1].str());
    out.airac_version = std::stoi(m[2].str());
    out.package_version = (m[3].matched ? std::stoi(m[3].str()) : 0);
    out.package_name = s;
    return true;
}

// Super-light JSON finder (no dependency): pull three fields via regex
static std::string RegGet(const std::string& s, const std::regex& rx) {
    std::smatch m; if (std::regex_search(s, m, rx)) return m[1].str();
    return "";
}

static bool ParseRemoteJson(const std::string& json, RemoteLatest& out) {
    std::string cyc = RegGet(json, std::regex(R"json("airac_cycle"\s*:\s*(\d+))json"));
    std::string ver = RegGet(json, std::regex(R"json("airac_version"\s*:\s*(\d+))json"));
    std::string pver = RegGet(json, std::regex(R"json("package_version"\s*:\s*(\d+))json"));
    std::string name = RegGet(json, std::regex(R"json("latest_package_name"\s*:\s*"([^"]*)")json"));
    std::string url = RegGet(json, std::regex(R"json("download_url"\s*:\s*"([^"]*)")json"));
    std::string nts = RegGet(json, std::regex(R"json("notes"\s*:\s*"([^"]*)")json"));
                if (cyc.empty() || ver.empty()) return false;
    out.airac_cycle = std::stoi(cyc);
    out.airac_version = std::stoi(ver);
    out.package_version = pver.empty() ? 0 : std::stoi(pver);
    out.latest_package_name = name;
    out.download_url = url;
    out.notes = nts;
    return true;
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Helper to retrieve the currently loaded sector file name, even if not logged in
static std::string GetLoadedSectorName(CPlugIn* self) {
    // 1) Try your own controller slot
    CController me = self->ControllerMyself();
    const char* s = me.GetSectorFileName();
    if (s && *s) return s;

    // 2) Fall back: scan all controllers (local + remote)
    for (CController c = self->ControllerSelectFirst();
        c.IsValid();
        c = self->ControllerSelectNext(c)) {
        s = c.GetSectorFileName();
        if (s && *s) return s;
    }
    return "";
}

// Naive GET with WinHTTP
static bool HttpGet(const std::string& url, std::string& bodyOut) {
    bodyOut.clear();
    // crude URL splitter
    std::regex rx(R"(^(https?)://([^/]+)(/.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, rx)) return false;
    bool isHttps = (m[1].str() == "https");
    std::string host = m[2].str();
    std::string path = m[3].str().empty() ? "/" : m[3].str();

    HINTERNET hSession = WinHttpOpen(L"BGvACC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, Widen(host).c_str(), isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", Widen(path).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD dwSize = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (!dwSize) break;
            std::string buf; buf.resize(dwSize);
            DWORD dwRead = 0;
            if (!WinHttpReadData(hRequest, &buf[0], dwSize, &dwRead)) break;
            buf.resize(dwRead);
            bodyOut.append(buf);
        } while (dwSize > 0);
        ok = !bodyOut.empty();
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Update alert
static void ShowUpdateAlert(EuroScopePlugIn::CPlugIn* self, const std::string& text) {
    // Still put it in the EuroScope chat log with ack
    self->DisplayUserMessage(self->GetPlugInName(), "BGvACC", text.c_str(),
        true,  // ShowHandler
        true,  // ShowUnread
        true,  // ShowUnreadEvenIfBusy
        true,  // StartFlashing
        true); // NeedConfirmation

    // ALSO pop up a Windows modal box
    MessageBoxA(
        NULL,                     // no parent window - system modal
        text.c_str(),             // message text
        "BGvACC Updater",         // title of the dialog
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SYSTEMMODAL
    );
}

// Welcome message
static void ShowWelcomeIfAvailable(EuroScopePlugIn::CPlugIn* self, bool& flagShown) {
    if (flagShown) return;

    try {
        std::string body;
        if (!HttpGet(WELCOME_JSON_URL, body)) return;

        auto extract = [](const std::string& s, const char* key) -> std::string {
            // Build a simple, safe pattern:  "key" : "value"
            std::string pat = std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"";
            std::regex rx(pat, std::regex::ECMAScript);
            std::smatch m;
            if (std::regex_search(s, m, rx)) return m[1].str();
            return {};
            };

        const std::string title = extract(body, "title");
        const std::string msg = extract(body, "message");

        // Nothing to show? just return quietly.
        if (title.empty() && msg.empty()) return;

        const std::string boxTitle = title.empty() ? "BGvACC" : title;
        const std::string text = (title.empty() ? "BGvACC" : title) +
            std::string(" ") +
            (msg.empty() ? "Welcome!" : msg);

        // Chat (ack required) + modal popup
        self->DisplayUserMessage(self->GetPlugInName(), "BGvACC", text.c_str(),
            true, true, true, true, true);

        flagShown = true;  // prevent future attempts this session
    }
    catch (const std::exception&) {
        // Swallow any regex/parse errors to keep EuroScope stable
        // (Optionally log a minimal message in chat)
        // self->DisplayUserMessage(self->GetPlugInName(), "Welcome", "welcome.json parse error", true,true,false,false,false);
    }
    catch (...) {
    }
}

class BGvACC : public CPlugIn {
public:
    BGvACC()
        : CPlugIn(COMPATIBILITY_CODE, "BGvACC", "1.0.0", "BGvACC", "© BGvACC")
    {
        m_LastCheckCounter = 0;
        m_IntervalSec = 5;
        m_AutoChecksStopped = false;
        
        m_WelcomeShown = false;

        m_LastConnType = -1;
        m_OnlineSeconds = 0;
        m_ReminderMinutes = 120;   // default 120 minutes
        m_NextReminderAtSec = m_ReminderMinutes * 60;
        m_LastReminderBucket = 0;
    }

    virtual ~BGvACC() {}

    // Called once per second by EuroScope (Counter = seconds since app start).
    void OnTimer(int Counter) override {
        
        static bool firstTick = true;
        if (firstTick) {
            firstTick = false;
            ShowWelcomeIfAvailable(this, m_WelcomeShown);
            DoCheck(true);
            DisplayUserMessage(GetPlugInName(), "BGvACC",
                "To run a manual check: .bgvacc-update-check",
                true, true, false, false, false);
        }
        
        if (!m_AutoChecksStopped) {
            if (Counter - m_LastCheckCounter >= m_IntervalSec) {
                DoCheck(false);
                m_LastCheckCounter = Counter;
            }
        }

        int ct = GetConnectionType();                // 0 = offline, non-zero = connected

        // Detect transitions
        bool wentOnline = (m_LastConnType == 0 || m_LastConnType == -1) && (ct != 0);
        bool wentOffline = (m_LastConnType != 0) && (ct == 0);

        if (wentOnline) {
            // First tick after connecting
            m_OnlineSeconds = 0;
            m_LastReminderBucket = 0;
        }

        if (ct != 0) {
            // Connected: advance the session timer
            ++m_OnlineSeconds; // OnTimer is 1 Hz

            int intervalSec = m_ReminderMinutes * 60;
            if (intervalSec < 60) intervalSec = 60;  // safety

            // How many full intervals have elapsed (0,1,2,...)
            int bucket = m_OnlineSeconds / intervalSec;

            if (bucket > m_LastReminderBucket) {
                
                m_LastReminderBucket = bucket;  // latch: only once per interval
                
                const char* msg = "You have been online for a long session! Time for a break.";
                DisplayUserMessage(GetPlugInName(), "Reminder", msg,
                    true, true, true, true, true);
                MessageBoxA(NULL, msg, "BGvACC Reminder",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SYSTEMMODAL);
            }
        }
        else {
            // Offline: reset counters for next connection
            m_OnlineSeconds = 0;
            m_LastReminderBucket = 0;
        }

        m_LastConnType = ct;
    }

    // Custom commands
    bool OnCompileCommand(const char* sLine) override {
        std::string line = sLine ? sLine : "";
        if (line == ".bgvacc-update-check") {
            DoCheck(true);
            return true;
        }
        if (line == ".bgvacc-update-open") {
            if (!m_LastDownloadUrl.empty()) ShellExecuteA(NULL, "open", m_LastDownloadUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
            else DisplayUserMessage(GetPlugInName(), "BGvACC", "No download URL in manifest.", true, true, false, false, false);
            return true;
        }
        if (line == ".bgvacc-status") {
            std::ostringstream os;

            // Local sector info
            std::string sfn = GetLoadedSectorName(this);
            os << "[Local] Sector string: " << (sfn.empty() ? "<none>" : sfn) << " ";

            ParsedLocal local{};
            if (!sfn.empty() && ParseLocalFromInfoString(sfn, local)) {
                os << "AIRAC " << local.airac_cycle
                    << "/" << local.airac_version
                    << " (Package " << local.package_version << "). ";
            }
            else {
                os << "Could not parse INFO format. ";
            }

            // Remote manifest info
            os << "[Remote]";
            RemoteLatest remote{};
            std::string body;
            if (HttpGet(DEFAULT_MANIFEST_URL, body) && ParseRemoteJson(body, remote)) {
                os << " AIRAC " << remote.airac_cycle
                    << "/" << remote.airac_version
                    << " (Package " << remote.package_version << ") "
                    << "Name: " << (remote.latest_package_name.empty() ? "<none> " : remote.latest_package_name) << " ";
            }
            else {
                os << " <fetch/parse failed>";
            }

            DisplayUserMessage(GetPlugInName(), "BGvACC", os.str().c_str(),
                true, true, false, false, false);
            return true;
        }
        if (line == ".bgvacc-hey") {
            const char* msg = "Zdravei! Welcome to BGvACC!";
            DisplayUserMessage(GetPlugInName(), "???", msg, true, true, false, false, false);
            return true;
        }
        if (line == ".bgvacc-coffee") {
            const char* msg = "Coffee delivered to your scope. Clear skies!";
            DisplayUserMessage(GetPlugInName(), "???", msg, true, true, false, false, false);
            MessageBoxA(NULL, msg, "BGvACC Café", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SYSTEMMODAL);
            return true;
        }
        if (line == ".bgvacc-falcon") {
            const char* msg = "The BGvACC falcon watches over your skies.";
            DisplayUserMessage(GetPlugInName(), "???", msg, true, true, false, false, false);
            return true;
        }
        return false;
    }

private:
    int  m_LastCheckCounter = -20;   // force first check after 20 sec
    int  m_IntervalSec = 5;    // check every 20 sec
    bool m_AutoChecksStopped = false; // stop after sector is detected
    std::string m_LastDownloadUrl;

    int   m_LastConnType = -1;        // last known connection type
    int   m_OnlineSeconds = 0;        // seconds online in current connection
    int   m_ReminderMinutes = 120;    // interval minutes (default: 2 hours)
    int   m_NextReminderAtSec = 120 * 60; // when to fire next reminder (seconds since connect)
    int   m_LastReminderBucket = 0;

    bool m_WelcomeShown = false; // welcome toggle

    static std::string Trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void DoCheck(bool verbose) {
        // 1) Get the sector name string (from EuroScope API)
        std::string sfn = GetLoadedSectorName(this);
        if (sfn.empty()) {
            if (verbose) {
                DisplayUserMessage(GetPlugInName(), "BGvACC",
                    "No sector file detected yet. Connect to VATSIM.",
                    true, true, false, false, false);
            }
            return;
        }
        m_AutoChecksStopped = true;
        ParsedLocal local{};
        if (!ParseLocalFromInfoString(sfn, local)) {
            if (verbose) {
                std::string msg = std::string("Could not parse INFO format. Got: ") + sfn +
                    "  | Expected like: '... 2510/2-2 LBSR 20251013'";
                DisplayUserMessage(GetPlugInName(), "BGvACC", msg.c_str(),
                    true, true, false, false, true);
            }
            return;
        }

        // 2) Get manifest URL
        const std::string manifestUrl = DEFAULT_MANIFEST_URL;

        // 3) Fetch manifest
        std::string body;
        if (!HttpGet(manifestUrl, body)) {
            if (verbose) DisplayUserMessage(GetPlugInName(), "BGvACC", "Failed to fetch manifest.", true, true, false, false, true);
            return;
        }

        // 4) Parse remote
        RemoteLatest remote{};
        if (!ParseRemoteJson(body, remote)) {
            if (verbose) DisplayUserMessage(GetPlugInName(), "BGvACC", "Manifest JSON missing fields.", true, true, false, false, true);
            return;
        }
        m_LastDownloadUrl = remote.download_url;

        // 5) Compare only (airac_cycle, airac_version, and package_version)
        bool newer =
            (remote.airac_cycle > local.airac_cycle) ||
            (remote.airac_cycle == local.airac_cycle && remote.airac_version > local.airac_version) ||
            (remote.airac_cycle == local.airac_cycle && remote.airac_version == local.airac_version &&
                remote.package_version > local.package_version);

        if (newer) {
            std::ostringstream os;
            os << "New sector available: AIRAC " << remote.airac_cycle
                << "/" << remote.airac_version
                << " (Package " << remote.package_version << "). \n"
                << "You have AIRAC " << local.airac_cycle
                << "/" << local.airac_version
                << " (Package " << local.package_version << "). \n"
                << "Type .bgvacc-update-open to get it.";
            if (!remote.notes.empty()) os << " \nRelease notes: " << remote.notes;

            ShowUpdateAlert(this, os.str());
        }
        else if (verbose) {
            std::ostringstream os;
            os << "Up to date. Local AIRAC " << local.airac_cycle
                << "/" << local.airac_version
                << " (Package " << local.package_version << ").";
            DisplayUserMessage(GetPlugInName(), "BGvACC", os.str().c_str(),
                true, true, false, false, false);
        }
    }
};

// Exported entry points required by EuroScope
static BGvACC* g_p = nullptr;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance) {
    g_p = new BGvACC();
    *ppPlugInInstance = g_p;
}

void __declspec(dllexport) EuroScopePlugInExit(void) {
    delete g_p; g_p = nullptr;
}