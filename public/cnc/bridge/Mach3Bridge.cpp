// ============================================================================
// CNC Dispenser V2.0 - Mach3 Native Win32 C++ Bridge (32-bit x86, No .NET)
// Target OS : Windows XP SP3 / Windows 7 / Windows 10/11 (32-bit x86 COM)
// Transport : Local HTTP server on http://127.0.0.1:8765 (Winsock2)
// Mach3 API : COM IDispatch Late Binding -> ProgID "Mach4.Document"
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WINVER
#define WINVER 0x0501
#endif
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#endif

// ============================================================================
// Configuration & Structures
// ============================================================================

static const char* BRIDGE_VERSION = "2.0.0";
static const int BRIDGE_PORT = 8765;
static const int MAX_HTTP_BODY_BYTES = 32 * 1024 * 1024; // 32 MB

struct DispenserRule {
    int id;
    std::string lowerCmd;
    std::string raiseCmd;
};

struct JobLineRule {
    int lineNum;
    std::string upperCmd;
};

struct MachineSnapshot {
    bool mach3Connected;
    std::string status; // DISCONNECTED | IDLE | RUNNING | HOLD | ESTOP
    double x;
    double y;
    double feedrate;
    int lineNum;
    int totalLines;
    bool valveOpen;
    std::vector<int> loweredDispensers;
    int activeDispenserId;
    bool isMoving;
    bool inReset;
    std::string error;
};

enum ActionType {
    ACT_NONE = 0,
    ACT_LOAD_AND_RUN,
    ACT_PAUSE,
    ACT_RESUME,
    ACT_STOP,
    ACT_RESET,
    ACT_JOG,
    ACT_ZERO,
    ACT_MDI,
    ACT_CONFIG
};

struct ActionRequest {
    ActionType type;
    std::string gcode;
    std::string filename;
    std::string axis;
    double step;
    std::string cmd;
    std::string openCmd;
    std::string closeCmd;
    std::vector<std::string> raiseCmds;
    std::vector<DispenserRule> dispensers;
    bool hasDispensers;

    HANDLE doneEvent;
    int httpCode;
    std::string errorMsg;
    MachineSnapshot resultSnap;

    ActionRequest()
        : type(ACT_NONE), step(0.0), hasDispensers(false),
          doneEvent(NULL), httpCode(200) {}
};

// ============================================================================
// Global State
// ============================================================================

static volatile LONG g_running = 1;
static SOCKET g_listenSock = INVALID_SOCKET;
static std::string g_baseDir;
static std::string g_tempDir;

static CRITICAL_SECTION g_snapCs;
static MachineSnapshot g_snapshot;

static CRITICAL_SECTION g_queueCs;
static HANDLE g_queueEvent = NULL;
static std::vector<ActionRequest*> g_actionQueue;

// Owned exclusively by COM worker thread:
static std::string g_valveOpenCmd = "M103";
static std::string g_valveCloseCmd = "M203";
static std::vector<DispenserRule> g_dispensers;

static bool g_stoppedLatch = false;
static DWORD g_stoppedTick = 0;
static bool g_valveOpen = false;
static std::vector<int> g_loweredDispensers;
static int g_activeDispenserId = 1;

static bool g_pulseActive = false;
static DWORD g_pulseStartTick = 0;
static DWORD g_pulseDurationMs = 100;
static std::string g_pulseCloseCmd = "M203";

static bool g_jobActive = false;
static bool g_jobSawRunning = false;
static DWORD g_jobStartTick = 0;
static int g_jobTotalLines = 0;
static int g_jobLastLineTracked = 0;
static std::vector<JobLineRule> g_jobLines;

// ============================================================================
// String / UTF-8 / File Helpers
// ============================================================================

static std::string TrimStr(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (unsigned char)s[start] <= ' ') start++;
    size_t end = s.size();
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    return s.substr(start, end - start);
}

static std::string ToUpperStr(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        out[i] = (char)toupper((unsigned char)out[i]);
    }
    return out;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    _snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

static void AppendUtf8Codepoint(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | ((cp >> 6) & 0x1F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | ((cp >> 12) & 0x0F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | ((cp >> 18) & 0x07));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static BSTR Utf8ToBstr(const std::string& utf8) {
    if (utf8.empty()) return SysAllocString(L"");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
        if (wlen <= 0) return SysAllocString(L"");
        std::vector<wchar_t> buf(wlen + 1, 0);
        MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), (int)utf8.size(), &buf[0], wlen);
        return SysAllocStringLen(&buf[0], wlen);
    }
    std::vector<wchar_t> buf(wlen + 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &buf[0], wlen);
    return SysAllocStringLen(&buf[0], wlen);
}

static void EnsureTempDir() {
    if (!g_tempDir.empty()) {
        CreateDirectoryA(g_tempDir.c_str(), NULL);
    }
}

static bool WriteFileAtomic(const std::string& targetPath, const std::string& content) {
    EnsureTempDir();
    std::string tmpPath = targetPath + ".tmp";
    HANDLE hFile = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    size_t writtenTotal = 0;
    bool writeOk = true;
    while (writtenTotal < content.size()) {
        DWORD chunk = (DWORD)(content.size() - writtenTotal);
        if (chunk > 65536) chunk = 65536;
        DWORD bytesWritten = 0;
        if (!WriteFile(hFile, content.data() + writtenTotal, chunk, &bytesWritten, NULL) || bytesWritten == 0) {
            writeOk = false;
            break;
        }
        writtenTotal += bytesWritten;
    }

    if (writeOk) {
        FlushFileBuffers(hFile);
    }
    CloseHandle(hFile);

    if (!writeOk) {
        DeleteFileA(tmpPath.c_str());
        return false;
    }

    if (!MoveFileExA(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(targetPath.c_str());
        if (!MoveFileA(tmpPath.c_str(), targetPath.c_str())) {
            DeleteFileA(tmpPath.c_str());
            return false;
        }
    }
    return true;
}

static bool ReadEntireFile(const std::string& path, std::string& outContent) {
    outContent.clear();
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize > (DWORD)MAX_HTTP_BODY_BYTES) {
        CloseHandle(hFile);
        return false;
    }

    outContent.resize(fileSize);
    DWORD totalRead = 0;
    while (totalRead < fileSize) {
        DWORD chunkRead = 0;
        if (!ReadFile(hFile, &outContent[0] + totalRead, fileSize - totalRead, &chunkRead, NULL) || chunkRead == 0) {
            break;
        }
        totalRead += chunkRead;
    }
    CloseHandle(hFile);
    if (totalRead != fileSize) {
        outContent.clear();
        return false;
    }
    return true;
}

static std::string NormalizeGcodeCrlf(const std::string& raw, int& outLineCount) {
    size_t trimmedEnd = raw.size();
    while (trimmedEnd > 0 && (raw[trimmedEnd - 1] == '\r' || raw[trimmedEnd - 1] == '\n')) {
        trimmedEnd--;
    }
    bool hasCode = false;
    for (size_t k = 0; k < trimmedEnd; k++) {
        if ((unsigned char)raw[k] > ' ') {
            hasCode = true;
            break;
        }
    }
    if (!hasCode || trimmedEnd == 0) {
        outLineCount = 0;
        return "";
    }

    std::string out;
    out.reserve(trimmedEnd + 256);
    int lines = 1;
    for (size_t i = 0; i < trimmedEnd; i++) {
        char c = raw[i];
        if (c == '\r') {
            if (i + 1 < trimmedEnd && raw[i + 1] == '\n') {
                i++;
            }
            out += "\r\n";
            lines++;
        } else if (c == '\n') {
            out += "\r\n";
            lines++;
        } else {
            out += c;
        }
    }
    out += "\r\n"; // Trailing CRLF ensures Mach3 executes the final M30 line
    outLineCount = lines;
    return out;
}

// ============================================================================
// Lightweight JSON Helpers (pure C++98, depth-aware string skipping)
// ============================================================================

static size_t SkipWs(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
        pos++;
    }
    return pos;
}

static bool ParseJsonStringAt(const std::string& s, size_t& pos, std::string& outStr) {
    if (pos >= s.size() || s[pos] != '\"') return false;
    pos++; // skip opening quote
    outStr.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '\"') {
            return true;
        }
        if (c == '\\') {
            if (pos >= s.size()) return false;
            char esc = s[pos++];
            switch (esc) {
                case '\"': outStr += '\"'; break;
                case '\\': outStr += '\\'; break;
                case '/':  outStr += '/'; break;
                case 'b':  outStr += '\b'; break;
                case 'f':  outStr += '\f'; break;
                case 'n':  outStr += '\n'; break;
                case 'r':  outStr += '\r'; break;
                case 't':  outStr += '\t'; break;
                case 'u': {
                    if (pos + 4 <= s.size()) {
                        unsigned int code = 0;
                        bool hexOk = true;
                        for (int h = 0; h < 4; h++) {
                            char hc = s[pos + h];
                            code <<= 4;
                            if (hc >= '0' && hc <= '9') code |= (hc - '0');
                            else if (hc >= 'a' && hc <= 'f') code |= (hc - 'a' + 10);
                            else if (hc >= 'A' && hc <= 'F') code |= (hc - 'A' + 10);
                            else { hexOk = false; break; }
                        }
                        if (hexOk) {
                            pos += 4;
                            AppendUtf8Codepoint(outStr, code);
                        }
                    }
                    break;
                }
                default: outStr += esc; break;
            }
        } else {
            outStr += c;
        }
    }
    return false;
}

static size_t SkipJsonValue(const std::string& s, size_t pos) {
    pos = SkipWs(s, pos);
    if (pos >= s.size()) return pos;
    char c = s[pos];
    if (c == '\"') {
        std::string dummy;
        ParseJsonStringAt(s, pos, dummy);
        return pos;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        while (pos < s.size()) {
            char ch = s[pos];
            if (ch == '\"') {
                std::string dummy;
                ParseJsonStringAt(s, pos, dummy);
                continue;
            }
            if (ch == '{' || ch == '[') depth++;
            else if (ch == '}' || ch == ']') {
                depth--;
                pos++;
                if (depth <= 0) return pos;
                continue;
            }
            pos++;
        }
        return pos;
    }
    while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
           s[pos] != ' ' && s[pos] != '\r' && s[pos] != '\n' && s[pos] != '\t') {
        pos++;
    }
    return pos;
}

static bool FindJsonValueSlice(const std::string& json, const char* targetKey, size_t& outStart, size_t& outEnd) {
    size_t pos = SkipWs(json, 0);
    if (pos >= json.size() || json[pos] != '{') return false;
    pos++;

    while (pos < json.size()) {
        pos = SkipWs(json, pos);
        if (pos >= json.size() || json[pos] == '}') return false;
        if (json[pos] == ',') { pos++; continue; }
        if (json[pos] != '\"') return false;

        std::string key;
        if (!ParseJsonStringAt(json, pos, key)) return false;
        pos = SkipWs(json, pos);
        if (pos >= json.size() || json[pos] != ':') return false;
        pos = SkipWs(json, pos + 1);

        size_t valStart = pos;
        size_t valEnd = SkipJsonValue(json, valStart);
        if (key == targetKey) {
            outStart = valStart;
            outEnd = valEnd;
            return true;
        }
        pos = valEnd;
    }
    return false;
}

static bool JsonExtractString(const std::string& json, const char* key, std::string& outVal) {
    size_t vs = 0, ve = 0;
    if (!FindJsonValueSlice(json, key, vs, ve)) return false;
    if (vs >= json.size() || json[vs] != '\"') return false;
    size_t p = vs;
    return ParseJsonStringAt(json, p, outVal);
}

static bool JsonExtractDouble(const std::string& json, const char* key, double& outVal) {
    size_t vs = 0, ve = 0;
    if (!FindJsonValueSlice(json, key, vs, ve)) return false;
    if (vs >= ve) return false;
    std::string raw = TrimStr(json.substr(vs, ve - vs));
    if (raw.empty() || (!isdigit((unsigned char)raw[0]) && raw[0] != '-' && raw[0] != '+')) return false;
    outVal = atof(raw.c_str());
    return true;
}

static bool JsonExtractStringArray(const std::string& json, const char* key, std::vector<std::string>& outArr) {
    outArr.clear();
    size_t vs = 0, ve = 0;
    if (!FindJsonValueSlice(json, key, vs, ve)) return false;
    if (vs >= json.size() || json[vs] != '[') return false;
    size_t pos = vs + 1;
    while (pos < ve) {
        pos = SkipWs(json, pos);
        if (pos >= ve || json[pos] == ']') break;
        if (json[pos] == ',') { pos++; continue; }
        if (json[pos] == '\"') {
            std::string item;
            if (ParseJsonStringAt(json, pos, item)) {
                outArr.push_back(item);
            }
        } else {
            pos = SkipJsonValue(json, pos);
        }
    }
    return true;
}

static bool JsonExtractDispensers(const std::string& json, std::vector<DispenserRule>& outList) {
    outList.clear();
    size_t vs = 0, ve = 0;
    if (!FindJsonValueSlice(json, "dispensers", vs, ve)) return false;
    if (vs >= json.size() || json[vs] != '[') return false;

    size_t pos = vs + 1;
    while (pos < ve) {
        pos = SkipWs(json, pos);
        if (pos >= ve || json[pos] == ']') break;
        if (json[pos] == ',') { pos++; continue; }
        if (json[pos] == '{') {
            size_t objStart = pos;
            size_t objEnd = SkipJsonValue(json, objStart);
            std::string objSlice = json.substr(objStart, objEnd - objStart);
            DispenserRule dr;
            double dId = 0;
            if (JsonExtractDouble(objSlice, "id", dId)) {
                dr.id = (int)(dId + 0.5);
                JsonExtractString(objSlice, "lowerCmd", dr.lowerCmd);
                JsonExtractString(objSlice, "raiseCmd", dr.raiseCmd);
                dr.lowerCmd = ToUpperStr(TrimStr(dr.lowerCmd));
                dr.raiseCmd = ToUpperStr(TrimStr(dr.raiseCmd));
                if (dr.id > 0) {
                    outList.push_back(dr);
                }
            }
            pos = objEnd;
        } else {
            pos = SkipJsonValue(json, pos);
        }
    }
    return !outList.empty();
}

// ============================================================================
// Dispenser & Valve State Tracking Helpers
// ============================================================================

static void InitDefaultDispensers() {
    g_dispensers.clear();
    for (int i = 1; i <= 5; i++) {
        DispenserRule dr;
        dr.id = i;
        char lBuf[16], rBuf[16];
        _snprintf(lBuf, sizeof(lBuf), "M%d", 103 + i);
        _snprintf(rBuf, sizeof(rBuf), "M%d", 203 + i);
        dr.lowerCmd = lBuf;
        dr.raiseCmd = rBuf;
        g_dispensers.push_back(dr);
    }
}

static void AddLoweredDispenser(int id) {
    if (id <= 0) return;
    g_activeDispenserId = id;
    for (size_t i = 0; i < g_loweredDispensers.size(); i++) {
        if (g_loweredDispensers[i] == id) return;
    }
    g_loweredDispensers.push_back(id);
    // Simple insertion sort ascending
    for (size_t i = 1; i < g_loweredDispensers.size(); i++) {
        int val = g_loweredDispensers[i];
        size_t j = i;
        while (j > 0 && g_loweredDispensers[j - 1] > val) {
            g_loweredDispensers[j] = g_loweredDispensers[j - 1];
            j--;
        }
        g_loweredDispensers[j] = val;
    }
}

static void RemoveLoweredDispenser(int id) {
    for (size_t i = 0; i < g_loweredDispensers.size(); i++) {
        if (g_loweredDispensers[i] == id) {
            g_loweredDispensers.erase(g_loweredDispensers.begin() + i);
            return;
        }
    }
}

static bool CmdStartsWithToken(const std::string& upperLine, const std::string& upperToken) {
    if (upperToken.empty()) return false;
    if (upperLine == upperToken) return true;
    if (upperLine.size() > upperToken.size() &&
        upperLine.compare(0, upperToken.size(), upperToken) == 0 &&
        (upperLine[upperToken.size()] == ' ' || upperLine[upperToken.size()] == '\t' || upperLine[upperToken.size()] == ';')) {
        return true;
    }
    return false;
}

static void ApplyTrackedMCode(const std::string& rawCmd) {
    std::string upper = ToUpperStr(TrimStr(rawCmd));
    size_t semi = upper.find(';');
    if (semi != std::string::npos) {
        upper = TrimStr(upper.substr(0, semi));
    }
    if (upper.empty()) return;

    if (CmdStartsWithToken(upper, g_valveOpenCmd)) {
        g_valveOpen = true;
        return;
    }
    if (CmdStartsWithToken(upper, g_valveCloseCmd)) {
        g_valveOpen = false;
        return;
    }
    if (CmdStartsWithToken(upper, "M5") || CmdStartsWithToken(upper, "M05") ||
        CmdStartsWithToken(upper, "M30") || CmdStartsWithToken(upper, "M2") || CmdStartsWithToken(upper, "M02")) {
        g_valveOpen = false;
        g_loweredDispensers.clear();
        return;
    }

    for (size_t i = 0; i < g_dispensers.size(); i++) {
        if (CmdStartsWithToken(upper, g_dispensers[i].lowerCmd)) {
            AddLoweredDispenser(g_dispensers[i].id);
            return;
        }
        if (CmdStartsWithToken(upper, g_dispensers[i].raiseCmd)) {
            RemoveLoweredDispenser(g_dispensers[i].id);
            return;
        }
    }
}

static void IndexActiveJobLines(const std::string& crlfGcode) {
    g_jobLines.clear();
    int lineNum = 1;
    size_t start = 0;
    while (start <= crlfGcode.size()) {
        size_t nl = crlfGcode.find('\n', start);
        std::string line = (nl == std::string::npos) ? crlfGcode.substr(start) : crlfGcode.substr(start, nl - start);
        line = ToUpperStr(TrimStr(line));
        size_t semi = line.find(';');
        if (semi != std::string::npos) line = TrimStr(line.substr(0, semi));
        if (!line.empty()) {
            JobLineRule r;
            r.lineNum = lineNum;
            r.upperCmd = line;
            g_jobLines.push_back(r);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
        lineNum++;
    }
}

static bool IsMotionCommand(const std::string& upperCmd) {
    // Allow pure M-codes and G4 dwells without blocking; block G0/G1/G2/G3/G28/G28.1/X/Y positioning
    std::string c = upperCmd;
    size_t semi = c.find(';');
    if (semi != std::string::npos) c = c.substr(0, semi);
    c = TrimStr(c);
    if (c.empty()) return false;
    if (c[0] == 'M') return false;
    if (CmdStartsWithToken(c, "G4") || CmdStartsWithToken(c, "G04")) return false;
    if (c.find("G28") != std::string::npos ||
        c.find("G0") != std::string::npos ||
        c.find("G1") != std::string::npos ||
        c.find('X') != std::string::npos ||
        c.find('Y') != std::string::npos) {
        return true;
    }
    return false;
}

// ============================================================================
// Mach3 COM Late-Binding Wrapper (Runs strictly on dedicated COM STA Thread)
// ============================================================================

class Mach3ComClient {
public:
    Mach3ComClient() : m_pMach4(NULL), m_pScript(NULL), m_loggedAttachError(false) {}

    ~Mach3ComClient() {
        Disconnect();
    }

    bool IsAttached() const {
        return m_pMach4 != NULL;
    }

    void Disconnect() {
        if (m_pScript) {
            m_pScript->Release();
            m_pScript = NULL;
        }
        if (m_pMach4) {
            m_pMach4->Release();
            m_pMach4 = NULL;
        }
    }

    bool EnsureAttached() {
        if (m_pMach4 != NULL) return true;

        CLSID clsid;
        HRESULT hr = CLSIDFromProgID(L"Mach4.Document", &clsid);
        if (FAILED(hr)) {
            if (!m_loggedAttachError) {
                printf("[COM] Mach4.Document ProgID not registered yet (HRESULT=0x%08lX). Waiting for Mach3...\n", (unsigned long)hr);
                m_loggedAttachError = true;
            }
            return false;
        }

        IUnknown* pUnk = NULL;
        hr = GetActiveObject(clsid, NULL, &pUnk);
        if (FAILED(hr) || !pUnk) {
            if (!m_loggedAttachError) {
                printf("[COM] Mach3 is not running yet (waiting for Mach4.Document in ROT)...\n");
                m_loggedAttachError = true;
            }
            return false;
        }

        IDispatch* pMach4 = NULL;
        hr = pUnk->QueryInterface(IID_IDispatch, (void**)&pMach4);
        pUnk->Release();
        if (FAILED(hr) || !pMach4) {
            return false;
        }

        m_pMach4 = pMach4;
        m_pScript = ObtainScriptDispatch(pMach4);
        m_loggedAttachError = false;
        printf("[COM] Attached to active Mach3 instance (%s)!\n",
               m_pScript ? "IMach4 + ScriptDispatch" : "IMach4 direct");
        return true;
    }

    bool IsRpcDisconnectHr(HRESULT hr) const {
        return hr == (HRESULT)0x800706BA || // RPC_S_SERVER_UNAVAILABLE
               hr == (HRESULT)0x800706BE || // RPC_S_CALL_FAILED
               hr == (HRESULT)0x80010108 || // RPC_E_DISCONNECTED
               hr == (HRESULT)0x800706BF;
    }

    bool GetOEMDRO(short droNum, double& outVal) {
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_I2;
        arg.iVal = droNum;

        VARIANT res;
        VariantInit(&res);
        HRESULT hr = InvokeAuto(L"GetOEMDRO", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &arg, 1, &res, true);
        if (hr == DISP_E_TYPEMISMATCH) {
            arg.vt = VT_I4;
            arg.lVal = (long)droNum;
            VariantClear(&res);
            hr = InvokeAuto(L"GetOEMDRO", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &arg, 1, &res, true);
        }
        if (FAILED(hr)) {
            VariantClear(&res);
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        VARIANT dblRes;
        VariantInit(&dblRes);
        if (SUCCEEDED(VariantChangeType(&dblRes, &res, 0, VT_R8))) {
            outVal = dblRes.dblVal;
            VariantClear(&dblRes);
            VariantClear(&res);
            return true;
        }
        VariantClear(&dblRes);
        VariantClear(&res);
        return false;
    }

    bool GetOEMLED(short ledNum, bool& outVal) {
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_I2;
        arg.iVal = ledNum;

        VARIANT res;
        VariantInit(&res);
        HRESULT hr = InvokeAuto(L"GetOEMLED", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &arg, 1, &res, true);
        if (hr == DISP_E_TYPEMISMATCH) {
            arg.vt = VT_I4;
            arg.lVal = (long)ledNum;
            VariantClear(&res);
            hr = InvokeAuto(L"GetOEMLED", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &arg, 1, &res, true);
        }
        if (FAILED(hr)) {
            VariantClear(&res);
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        VARIANT boolRes;
        VariantInit(&boolRes);
        if (SUCCEEDED(VariantChangeType(&boolRes, &res, 0, VT_BOOL))) {
            outVal = (boolRes.boolVal != VARIANT_FALSE);
            VariantClear(&boolRes);
            VariantClear(&res);
            return true;
        }
        VariantClear(&boolRes);
        VariantClear(&res);
        return false;
    }

    bool IsMoving(bool& outMoving) {
        VARIANT res;
        VariantInit(&res);
        HRESULT hr = InvokeAuto(L"IsMoving", DISPATCH_METHOD | DISPATCH_PROPERTYGET, NULL, 0, &res, true);
        if (FAILED(hr)) {
            VariantClear(&res);
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        VARIANT boolRes;
        VariantInit(&boolRes);
        if (SUCCEEDED(VariantChangeType(&boolRes, &res, 0, VT_BOOL))) {
            outMoving = (boolRes.boolVal != VARIANT_FALSE);
            VariantClear(&boolRes);
            VariantClear(&res);
            return true;
        }
        VariantClear(&boolRes);
        VariantClear(&res);
        return false;
    }

    bool IsLoading(bool& outLoading) {
        outLoading = false;
        VARIANT res;
        VariantInit(&res);
        HRESULT hr = InvokeAuto(L"IsLoading", DISPATCH_METHOD | DISPATCH_PROPERTYGET, NULL, 0, &res, true);
        if (FAILED(hr)) {
            VariantClear(&res);
            return false;
        }
        VARIANT boolRes;
        VariantInit(&boolRes);
        if (SUCCEEDED(VariantChangeType(&boolRes, &res, 0, VT_BOOL))) {
            outLoading = (boolRes.boolVal != VARIANT_FALSE);
            VariantClear(&boolRes);
            VariantClear(&res);
            return true;
        }
        VariantClear(&boolRes);
        VariantClear(&res);
        return false;
    }

    bool DoOEMButton(short btnNum) {
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_I2;
        arg.iVal = btnNum;

        HRESULT hr = InvokeAuto(L"DoOEMButton", DISPATCH_METHOD, &arg, 1, NULL, true);
        if (hr == DISP_E_TYPEMISMATCH) {
            arg.vt = VT_I4;
            arg.lVal = (long)btnNum;
            hr = InvokeAuto(L"DoOEMButton", DISPATCH_METHOD, &arg, 1, NULL, true);
        }
        if (FAILED(hr)) {
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        return true;
    }

    bool Code(const std::string& gcodeLine) {
        BSTR bCmd = Utf8ToBstr(gcodeLine);
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_BSTR;
        arg.bstrVal = bCmd;

        HRESULT hr = InvokeAuto(L"Code", DISPATCH_METHOD, &arg, 1, NULL, true);
        SysFreeString(bCmd);
        if (FAILED(hr)) {
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        return true;
    }

    bool LoadFile(const std::string& fullFilePath) {
        BSTR bPath = Utf8ToBstr(fullFilePath);
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_BSTR;
        arg.bstrVal = bPath;

        // LoadFile lives on IMach4; fall back cleanly to pScript if needed
        HRESULT hr = InvokeAuto(L"LoadFile", DISPATCH_METHOD, &arg, 1, NULL, false);
        if (FAILED(hr)) {
            hr = InvokeAuto(L"LoadFile", DISPATCH_METHOD, &arg, 1, NULL, true);
        }
        SysFreeString(bPath);
        if (FAILED(hr)) {
            if (IsRpcDisconnectHr(hr)) Disconnect();
            return false;
        }
        return true;
    }

private:
    IDispatch* m_pMach4;
    IDispatch* m_pScript;
    bool m_loggedAttachError;

    IDispatch* ObtainScriptDispatch(IDispatch* pMach4) {
        if (!pMach4) return NULL;
        LPOLESTR name = (LPOLESTR)L"GetScriptDispatch";
        DISPID dispid = DISPID_UNKNOWN;
        HRESULT hr = pMach4->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispid);
        if (FAILED(hr)) return NULL;

        DISPPARAMS dp;
        dp.rgvarg = NULL;
        dp.rgdispidNamedArgs = NULL;
        dp.cArgs = 0;
        dp.cNamedArgs = 0;

        VARIANT res;
        VariantInit(&res);
        hr = pMach4->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &res, NULL, NULL);
        if (SUCCEEDED(hr) && res.vt == VT_DISPATCH && res.pdispVal != NULL) {
            IDispatch* pOut = res.pdispVal;
            pOut->AddRef();
            VariantClear(&res);
            return pOut;
        }
        VariantClear(&res);
        return NULL;
    }

    HRESULT InvokeOn(IDispatch* pDisp, LPCWSTR methodName, WORD flags, VARIANT* args, UINT cArgs, VARIANT* outRes) {
        if (!pDisp) return E_POINTER;
        LPOLESTR olestr = (LPOLESTR)methodName;
        DISPID dispid = DISPID_UNKNOWN;
        HRESULT hr = pDisp->GetIDsOfNames(IID_NULL, &olestr, 1, LOCALE_USER_DEFAULT, &dispid);
        if (FAILED(hr)) return hr;

        DISPPARAMS dp;
        dp.rgvarg = args;
        dp.rgdispidNamedArgs = NULL;
        dp.cArgs = cArgs;
        dp.cNamedArgs = 0;

        return pDisp->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, outRes, NULL, NULL);
    }

    HRESULT InvokeAuto(LPCWSTR methodName, WORD flags, VARIANT* args, UINT cArgs, VARIANT* outRes, bool preferScript) {
        IDispatch* primary = preferScript ? m_pScript : m_pMach4;
        IDispatch* secondary = preferScript ? m_pMach4 : m_pScript;

        HRESULT hr = E_FAIL;
        if (primary) {
            hr = InvokeOn(primary, methodName, flags, args, cArgs, outRes);
            if (SUCCEEDED(hr) || hr == DISP_E_TYPEMISMATCH) return hr;
            if (outRes) VariantClear(outRes);
        }
        if (secondary && secondary != primary) {
            if (outRes) VariantInit(outRes);
            hr = InvokeOn(secondary, methodName, flags, args, cArgs, outRes);
        }
        return hr;
    }
};

// ============================================================================
// COM Thread Poll & Command Execution
// ============================================================================

static MachineSnapshot RefreshSnapshotFromMach3(Mach3ComClient& com) {
    MachineSnapshot s;
    s.mach3Connected = com.IsAttached();
    s.x = 0.0;
    s.y = 0.0;
    s.feedrate = 0.0;
    s.lineNum = 0;
    s.totalLines = g_jobTotalLines;
    s.valveOpen = g_valveOpen;
    s.loweredDispensers = g_loweredDispensers;
    s.activeDispenserId = g_activeDispenserId;
    s.isMoving = false;
    s.inReset = false;

    if (!s.mach3Connected) {
        s.status = "ESTOP";
        s.error = "\xD0\x9C\xD0\xB0\xD1\x88\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0 \xD0\xBD\xD0\xB5 \xD0\xB5 \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD1\x82\xD1\x83\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB0 \xD0\xB2\xD0\xBE Mach3. Mach3Bridge.exe \xD1\x87\xD0\xB5\xD0\xBA\xD0\xB0 Mach3...";
        return s;
    }

    double xVal = 0.0, yVal = 0.0, fVal = 0.0, lineVal = 0.0;
    bool ledReset = false, ledRun = false, ledHold = false, moving = false;

    if (!com.GetOEMLED(800, ledReset)) {
        // Detached due to RPC failure
        s.mach3Connected = false;
        s.status = "ESTOP";
        s.error = "\xD0\x98\xD0\xB7\xD0\xB3\xD1\x83\xD0\xB1\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xB5 COM \xD0\xB2\xD1\x80\xD1\x81\xD0\xBA\xD0\xB0\xD1\x82\xD0\xB0 \xD1\x81\xD0\xBE Mach3.";
        return s;
    }

    com.GetOEMDRO(800, xVal);
    com.GetOEMDRO(801, yVal);
    com.GetOEMDRO(818, fVal);
    com.GetOEMDRO(816, lineVal);
    com.GetOEMLED(804, ledRun);
    com.GetOEMLED(805, ledHold);
    com.IsMoving(moving);

    s.x = xVal;
    s.y = yVal;
    s.feedrate = fVal;
    s.lineNum = (int)(lineVal + 0.5);
    if (s.lineNum < 0) s.lineNum = 0;
    s.isMoving = moving;
    s.inReset = ledReset;

    if (g_stoppedLatch && !ledReset && (ledRun || moving) && (DWORD)(GetTickCount() - g_stoppedTick) > 600) {
        g_stoppedLatch = false;
    }

    if (ledReset || g_stoppedLatch) {
        s.status = "ESTOP";
    } else if (ledHold) {
        s.status = "HOLD";
    } else if (ledRun || moving) {
        s.status = "RUNNING";
        if (g_jobActive) g_jobSawRunning = true;
    } else {
        s.status = "IDLE";
    }

    // Track dispenser/valve state dynamically while active_job.tap progresses
    if (g_jobActive) {
        if (s.lineNum > g_jobLastLineTracked) {
            for (size_t i = 0; i < g_jobLines.size(); i++) {
                if (g_jobLines[i].lineNum > g_jobLastLineTracked && g_jobLines[i].lineNum <= s.lineNum) {
                    ApplyTrackedMCode(g_jobLines[i].upperCmd);
                }
            }
            g_jobLastLineTracked = s.lineNum;
        }

        DWORD elapsed = GetTickCount() - g_jobStartTick;
        if (s.status == "IDLE" && (g_jobSawRunning || elapsed > 800)) {
            // Job finished cleanly
            g_jobActive = false;
            g_jobSawRunning = false;
            g_valveOpen = false;
            g_loweredDispensers.clear();
        } else if (s.status == "ESTOP") {
            g_jobActive = false;
            g_jobSawRunning = false;
            g_valveOpen = false;
            g_loweredDispensers.clear();
        }
    }

    s.valveOpen = g_valveOpen;
    s.loweredDispensers = g_loweredDispensers;
    s.activeDispenserId = g_activeDispenserId;
    return s;
}

static void ExecuteActionOnComThread(Mach3ComClient& com, ActionRequest* req) {
    if (!req) return;
    req->httpCode = 200;

    if (req->type == ACT_CONFIG) {
        if (!req->openCmd.empty()) g_valveOpenCmd = ToUpperStr(TrimStr(req->openCmd));
        if (!req->closeCmd.empty()) g_valveCloseCmd = ToUpperStr(TrimStr(req->closeCmd));
        if (req->hasDispensers && !req->dispensers.empty()) {
            g_dispensers = req->dispensers;
        }
        req->resultSnap = RefreshSnapshotFromMach3(com);
        return;
    }

    if (!com.EnsureAttached()) {
        req->httpCode = 503;
        req->errorMsg = "\xD0\x9D\xD0\xB5 \xD0\xB5 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xBD\xD0\xB0\xD1\x98\xD0\xB4\xD0\xB5\xD0\xBD \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD1\x82\xD1\x83\xD0\xB2\xD0\xB0\xD0\xBD Mach3 \xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD1\x81.";
        req->resultSnap = RefreshSnapshotFromMach3(com);
        return;
    }

    // Apply any bundled dispenser/valve configuration
    if (!req->openCmd.empty()) g_valveOpenCmd = ToUpperStr(TrimStr(req->openCmd));
    if (!req->closeCmd.empty()) g_valveCloseCmd = ToUpperStr(TrimStr(req->closeCmd));
    if (req->hasDispensers && !req->dispensers.empty()) {
        g_dispensers = req->dispensers;
    }

    MachineSnapshot cur = RefreshSnapshotFromMach3(com);

    switch (req->type) {
        case ACT_LOAD_AND_RUN: {
            if (cur.inReset || g_stoppedLatch || cur.status == "ESTOP") {
                req->httpCode = 409;
                req->errorMsg = "\xD0\x9C\xD0\xB0\xD1\x88\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0 \xD0\xB5 \xD0\xB2\xD0\xBE E-STOP / Reset \xD1\x81\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD1\x98\xD0\xB1\xD0\xB0. \xD0\x9F\xD1\x80\xD0\xB8\xD1\x82\xD0\xB8\xD1\x81\xD0\xBD\xD0\xB5\xD1\x82\xD0\xB5 Reset \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4 \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD1\x82.";
                break;
            }
            if (cur.status == "RUNNING" || cur.status == "HOLD") {
                req->httpCode = 409;
                req->errorMsg = "\xD0\x9C\xD0\xB0\xD1\x88\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0 \xD0\xB2\xD0\xB5\xD1\x9C\xD0\xB5 \xD0\xB8\xD0\xB7\xD0\xB2\xD1\x80\xD1\x88\xD1\x83\xD0\xB2\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB3\xD1\x80\xD0\xB0\xD0\xBC\xD0\xB0.";
                break;
            }
            if (!g_loweredDispensers.empty() || g_valveOpen) {
                req->httpCode = 409;
                req->errorMsg = "\xD0\x91\xD0\xB5\xD0\xB7\xD0\xB1\xD0\xB5\xD0\xB4\xD0\xBD\xD0\xBE\xD1\x81\xD0\xBD\xD0\xB0 \xD0\xB1\xD0\xBB\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xB4\xD0\xB0: \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xB8\xD0\xB3\xD0\xBD\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB3\xD0\xB8 \xD1\x81\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB4\xD0\xB8\xD1\x81\xD0\xBF\xD0\xB5\xD0\xBD\xD0\xB7\xD0\xB5\xD1\x80\xD0\xB8 \xD0\xB8 \xD0\xB7\xD0\xB0\xD1\x82\xD0\xB2\xD0\xBE\xD1\x80\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB3\xD0\xBE \xD0\xB2\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBB\xD0\xBE\xD1\x82 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4 \xD1\x81\xD1\x82\xD0\xB0\xD1\x80\xD1\x82!";
                break;
            }

            int linesCount = 0;
            std::string normalized = NormalizeGcodeCrlf(req->gcode, linesCount);
            if (linesCount == 0) {
                req->httpCode = 400;
                req->errorMsg = "G-\xD0\xBA\xD0\xBE\xD0\xB4\xD0\xBE\xD1\x82 \xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB5\xD0\xBD.";
                break;
            }

            std::string tapPath = g_tempDir + "\\active_job.tap";
            if (!WriteFileAtomic(tapPath, normalized)) {
                req->httpCode = 500;
                req->errorMsg = "\xD0\x9D\xD0\xB5 \xD0\xBC\xD0\xBE\xD0\xB6\xD0\xB5 \xD0\xB4\xD0\xB0 \xD1\x81\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x88\xD0\xB5 temp/active_job.tap";
                break;
            }

            if (!com.LoadFile(tapPath)) {
                req->httpCode = 500;
                req->errorMsg = "Mach3 LoadFile \xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD0\xB0 \xD0\xB4\xD0\xB0 \xD1\x98\xD0\xB0 \xD0\xB2\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0 \xD0\xB4\xD0\xB0\xD1\x82\xD0\xBE\xD1\x82\xD0\xB5\xD0\xBA\xD0\xB0\xD1\x82\xD0\xB0.";
                break;
            }

            Sleep(200);
            DWORD loadWaitStart = GetTickCount();
            bool loadingNow = false;
            while (com.IsLoading(loadingNow) && loadingNow && (DWORD)(GetTickCount() - loadWaitStart) < 1500) {
                Sleep(40);
            }

            bool stillReset = false;
            com.GetOEMLED(800, stillReset);
            if (stillReset) {
                req->httpCode = 409;
                req->errorMsg = "Mach3 \xD0\xB2\xD0\xBB\xD0\xB5\xD0\xB7\xD0\xB5 \xD0\xB2\xD0\xBE Reset \xD0\xBF\xD0\xBE \xD0\xB2\xD1\x87\xD0\xB8\xD1\x82\xD1\x83\xD0\xB2\xD0\xB0\xD1\x9A\xD0\xB5 \xD0\xBD\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB3\xD1\x80\xD0\xB0\xD0\xBC\xD0\xB0\xD1\x82\xD0\xB0.";
                break;
            }

            g_jobTotalLines = linesCount;
            g_jobLastLineTracked = 0;
            g_jobActive = true;
            g_jobSawRunning = false;
            g_jobStartTick = GetTickCount();
            IndexActiveJobLines(normalized);

            if (!com.DoOEMButton(1000)) { // Cycle Start
                g_jobActive = false;
                req->httpCode = 500;
                req->errorMsg = "Mach3 CycleStart (OEM 1000) \xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD0\xB0.";
                break;
            }
            printf("[JOB] Started %s (%d lines) via %s\n", req->filename.c_str(), linesCount, tapPath.c_str());
            Sleep(50);
            break;
        }

        case ACT_PAUSE:
            com.DoOEMButton(1001); // Feed Hold
            Sleep(30);
            break;

        case ACT_RESUME:
            com.DoOEMButton(1000); // Cycle Start
            Sleep(30);
            break;

        case ACT_STOP:
            com.DoOEMButton(1003); // Stop
            g_pulseActive = false;
            g_valveOpen = false;
            g_loweredDispensers.clear();
            g_jobActive = false;
            g_jobSawRunning = false;
            g_stoppedLatch = true;
            g_stoppedTick = GetTickCount();
            Sleep(40);
            break;

        case ACT_RESET: {
            g_stoppedLatch = false;
            g_pulseActive = false;
            g_valveOpen = false;
            g_loweredDispensers.clear();
            g_jobActive = false;
            g_jobSawRunning = false;
            bool ledReset = false;
            if (com.GetOEMLED(800, ledReset) && ledReset) {
                com.DoOEMButton(1021); // Toggle Reset only when Reset LED is lit
                Sleep(120);
            }
            break;
        }

        case ACT_JOG: {
            if (cur.status != "IDLE") {
                req->httpCode = 409;
                req->errorMsg = "Jog \xD0\xB5 \xD0\xB4\xD0\xBE\xD0\xB7\xD0\xB2\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xBD \xD1\x81\xD0\xB0\xD0\xBC\xD0\xBE \xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB0 Mach3 \xD0\xB5 IDLE.";
                break;
            }
            std::string ax = ToUpperStr(TrimStr(req->axis));
            if (ax != "X" && ax != "Y") {
                req->httpCode = 400;
                req->errorMsg = "\xD0\x9D\xD0\xB5\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xB4\xD1\x80\xD0\xB6\xD0\xB0\xD0\xBD\xD0\xB0 \xD0\xBE\xD1\x81\xD0\xBA\xD0\xB0 (\xD1\x81\xD0\xB0\xD0\xBC\xD0\xBE X \xD0\xB8 Y).";
                break;
            }
            // Use G90 absolute target move from live DRO so Mach3 NEVER stays stuck in G91
            double target = (ax == "X") ? (cur.x + req->step) : (cur.y + req->step);
            char mdiBuf[96];
            _snprintf(mdiBuf, sizeof(mdiBuf), "G90 G1 %s%.3f F2000", ax.c_str(), target);
            if (!com.Code(mdiBuf)) {
                req->httpCode = 500;
                req->errorMsg = "Mach3 Jog MDI \xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD0\xB0.";
            } else {
                Sleep(40);
            }
            break;
        }

        case ACT_ZERO: {
            if (cur.status != "IDLE") {
                req->httpCode = 409;
                req->errorMsg = "Zero \xD0\xB5 \xD0\xB4\xD0\xBE\xD0\xB7\xD0\xB2\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE \xD1\x81\xD0\xB0\xD0\xBC\xD0\xBE \xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB0 Mach3 \xD0\xB5 IDLE.";
                break;
            }
            std::string ax = ToUpperStr(TrimStr(req->axis));
            if (ax == "X" || ax == "XY" || ax == "ALL") {
                com.DoOEMButton(1008); // Zero X
            }
            if (ax == "Y" || ax == "XY" || ax == "ALL") {
                com.DoOEMButton(1009); // Zero Y
            }
            Sleep(50);
            break;
        }

        case ACT_MDI: {
            if (cur.status != "IDLE") {
                req->httpCode = 409;
                req->errorMsg = "\xD0\xA0\xD0\xB0\xD1\x87\xD0\xBD\xD0\xB8 \xD0\xBA\xD0\xBE\xD0\xBC\xD0\xB0\xD0\xBD\xD0\xB4\xD0\xB8 \xD1\x81\xD0\xB5 \xD0\xB4\xD0\xBE\xD0\xB7\xD0\xB2\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8 \xD1\x81\xD0\xB0\xD0\xBC\xD0\xBE \xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB0 Mach3 \xD0\xB5 IDLE.";
                break;
            }
            std::string cmd = TrimStr(req->cmd);
            std::string upper = ToUpperStr(cmd);

            if (upper.compare(0, 6, "PULSE:") == 0) {
                int ms = atoi(upper.c_str() + 6);
                if (ms <= 0) ms = 100;
                if (ms > 5000) ms = 5000;
                std::string oCmd = !req->openCmd.empty() ? ToUpperStr(TrimStr(req->openCmd)) : g_valveOpenCmd;
                std::string cCmd = !req->closeCmd.empty() ? ToUpperStr(TrimStr(req->closeCmd)) : g_valveCloseCmd;
                if (com.Code(oCmd)) {
                    g_valveOpen = true;
                    g_pulseActive = true;
                    g_pulseStartTick = GetTickCount();
                    g_pulseDurationMs = (DWORD)ms;
                    g_pulseCloseCmd = cCmd;
                } else {
                    req->httpCode = 500;
                    req->errorMsg = "Mach3 Code(\"" + oCmd + "\") \xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD0\xB0.";
                }
                break;
            }

            if (upper == "RAISE_ALL") {
                if (!req->raiseCmds.empty()) {
                    for (size_t r = 0; r < req->raiseCmds.size(); r++) {
                        std::string rc = TrimStr(req->raiseCmds[r]);
                        if (!rc.empty()) {
                            com.Code(rc);
                            Sleep(25);
                        }
                    }
                } else {
                    for (size_t i = 0; i < g_dispensers.size(); i++) {
                        bool isLow = false;
                        for (size_t k = 0; k < g_loweredDispensers.size(); k++) {
                            if (g_loweredDispensers[k] == g_dispensers[i].id) { isLow = true; break; }
                        }
                        if (isLow || g_loweredDispensers.empty()) {
                            com.Code(g_dispensers[i].raiseCmd);
                            Sleep(25);
                        }
                    }
                }
                g_loweredDispensers.clear();
                break;
            }

            // Safety interlock against horizontal motion when dispensers are lowered or valve open
            if (IsMotionCommand(upper) && (!g_loweredDispensers.empty() || g_valveOpen)) {
                req->httpCode = 409;
                req->errorMsg = "\xD0\x91\xD0\xB5\xD0\xB7\xD0\xB1\xD0\xB5\xD0\xB4\xD0\xBD\xD0\xBE\xD1\x81\xD0\xBD\xD0\xB0 \xD0\xB1\xD0\xBB\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xB4\xD0\xB0: \xD0\xBF\xD1\x80\xD0\xB2\xD0\xBE \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xB8\xD0\xB3\xD0\xBD\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB3\xD0\xB8 \xD1\x81\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB4\xD0\xB8\xD1\x81\xD0\xBF\xD0\xB5\xD0\xBD\xD0\xB7\xD0\xB5\xD1\x80\xD0\xB8 \xD0\xB8 \xD0\xB7\xD0\xB0\xD1\x82\xD0\xB2\xD0\xBE\xD1\x80\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB3\xD0\xBE \xD0\xB2\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBB\xD0\xBE\xD1\x82!";
                break;
            }

            if (!cmd.empty()) {
                if (!com.Code(cmd)) {
                    req->httpCode = 500;
                    req->errorMsg = "Mach3 MDI \xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD0\xBF\xD0\xB5\xD0\xB0.";
                } else {
                    ApplyTrackedMCode(cmd);
                    Sleep(35);
                }
            }
            break;
        }

        default:
            break;
    }

    req->resultSnap = RefreshSnapshotFromMach3(com);
}

static DWORD WINAPI ComWorkerThreadProc(LPVOID) {
    CoInitialize(NULL);
    Mach3ComClient com;
    DWORD lastAttachAttempt = 0;

    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        // Wait up to 20ms for queued action or timeout to poll Mach3 / check pulse timer
        WaitForSingleObject(g_queueEvent, 20);

        DWORD now = GetTickCount();

        if (!com.IsAttached() && (now - lastAttachAttempt) >= 500) {
            lastAttachAttempt = now;
            com.EnsureAttached();
        }

        // Complete pending 100ms valve test pulse
        if (g_pulseActive && (DWORD)(now - g_pulseStartTick) >= g_pulseDurationMs) {
            g_pulseActive = false;
            if (com.IsAttached()) {
                com.Code(g_pulseCloseCmd);
            }
            g_valveOpen = false;
        }

        // Drain pending action requests strictly on COM STA thread
        while (true) {
            ActionRequest* req = NULL;
            EnterCriticalSection(&g_queueCs);
            if (!g_actionQueue.empty()) {
                req = g_actionQueue.front();
                g_actionQueue.erase(g_actionQueue.begin());
            }
            LeaveCriticalSection(&g_queueCs);

            if (!req) break;
            ExecuteActionOnComThread(com, req);
            SetEvent(req->doneEvent);
        }

        // Periodic snapshot refresh
        MachineSnapshot fresh = RefreshSnapshotFromMach3(com);
        EnterCriticalSection(&g_snapCs);
        g_snapshot = fresh;
        LeaveCriticalSection(&g_snapCs);
    }

    com.Disconnect();
    CoUninitialize();
    return 0;
}

static bool SubmitComAction(ActionRequest& req, DWORD timeoutMs = 2500) {
    req.doneEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!req.doneEvent) return false;

    EnterCriticalSection(&g_queueCs);
    g_actionQueue.push_back(&req);
    LeaveCriticalSection(&g_queueCs);
    SetEvent(g_queueEvent);

    DWORD waitRes = WaitForSingleObject(req.doneEvent, timeoutMs);
    CloseHandle(req.doneEvent);
    req.doneEvent = NULL;
    return (waitRes == WAIT_OBJECT_0);
}

// ============================================================================
// JSON Serialization & HTTP Framing
// ============================================================================

static std::string BuildSnapshotJson(const MachineSnapshot& s, bool ok, const std::string& overrideErr) {
    std::string finalErr = !overrideErr.empty() ? overrideErr : s.error;
    char numBuf[128];
    std::string out = "{";
    out += "\"ok\":"; out += (ok ? "true" : "false");
    out += ",\"bridgeVersion\":\""; out += BRIDGE_VERSION; out += "\"";
    out += ",\"mach3Connected\":"; out += (s.mach3Connected ? "true" : "false");
    out += ",\"status\":\""; out += JsonEscape(s.status); out += "\"";

    _snprintf(numBuf, sizeof(numBuf), "%.3f", s.x);
    out += ",\"x\":"; out += numBuf;
    _snprintf(numBuf, sizeof(numBuf), "%.3f", s.y);
    out += ",\"y\":"; out += numBuf;
    _snprintf(numBuf, sizeof(numBuf), "%.0f", s.feedrate);
    out += ",\"feedrate\":"; out += numBuf;
    _snprintf(numBuf, sizeof(numBuf), "%d", s.lineNum);
    out += ",\"lineNum\":"; out += numBuf;
    _snprintf(numBuf, sizeof(numBuf), "%d", s.totalLines);
    out += ",\"totalLines\":"; out += numBuf;
    out += ",\"valveOpen\":"; out += (s.valveOpen ? "true" : "false");

    out += ",\"loweredDispensers\":[";
    for (size_t i = 0; i < s.loweredDispensers.size(); i++) {
        if (i > 0) out += ",";
        _snprintf(numBuf, sizeof(numBuf), "%d", s.loweredDispensers[i]);
        out += numBuf;
    }
    out += "]";

    _snprintf(numBuf, sizeof(numBuf), "%d", s.activeDispenserId);
    out += ",\"activeDispenserId\":"; out += numBuf;
    out += ",\"isMoving\":"; out += (s.isMoving ? "true" : "false");
    out += ",\"inReset\":"; out += (s.inReset ? "true" : "false");

    if (finalErr.empty()) {
        out += ",\"error\":null";
    } else {
        out += ",\"error\":\""; out += JsonEscape(finalErr); out += "\"";
    }
    out += "}";
    return out;
}

static void SendHttpResponse(SOCKET sock, int statusCode, const char* statusText,
                             const char* contentType, const std::string& body) {
    char header[512];
    _snprintf(header, sizeof(header),
              "HTTP/1.1 %d %s\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
              "Access-Control-Allow-Headers: Content-Type\r\n"
              "Cache-Control: no-store\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %lu\r\n"
              "Connection: close\r\n\r\n",
              statusCode, statusText, contentType, (unsigned long)body.size());

    std::string full = std::string(header) + body;
    size_t sentTotal = 0;
    while (sentTotal < full.size()) {
        int chunk = (int)(full.size() - sentTotal);
        int sent = send(sock, full.data() + sentTotal, chunk, 0);
        if (sent <= 0) break;
        sentTotal += (size_t)sent;
    }
}

static bool ReadHttpRequest(SOCKET sock, std::string& method, std::string& path, std::string& body) {
    std::string buf;
    buf.reserve(4096);
    char tmp[4096];

    size_t headerEnd = std::string::npos;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (buf.size() > 65536 && buf.find("\r\n\r\n") == std::string::npos) {
            return false; // Header too large
        }
    }

    std::string headerStr = buf.substr(0, headerEnd);
    std::string remainder = buf.substr(headerEnd + 4);

    // Parse request line: METHOD /path HTTP/1.x
    size_t lineEnd = headerStr.find("\r\n");
    std::string reqLine = (lineEnd == std::string::npos) ? headerStr : headerStr.substr(0, lineEnd);
    size_t sp1 = reqLine.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = reqLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) sp2 = reqLine.size();
    method = ToUpperStr(reqLine.substr(0, sp1));
    path = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qPos = path.find('?');
    if (qPos != std::string::npos) path = path.substr(0, qPos);

    // Find Content-Length (case-insensitive)
    int contentLen = 0;
    std::string upperHeader = ToUpperStr(headerStr);
    size_t clPos = upperHeader.find("\r\nCONTENT-LENGTH:");
    if (clPos != std::string::npos) {
        contentLen = atoi(upperHeader.c_str() + clPos + 17);
    }
    if (contentLen < 0 || contentLen > MAX_HTTP_BODY_BYTES) return false;

    body = remainder;
    while ((int)body.size() < contentLen) {
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body.append(tmp, n);
    }
    if ((int)body.size() > contentLen) {
        body.resize((size_t)contentLen);
    }
    return true;
}

static DWORD WINAPI ClientThreadProc(LPVOID param) {
    SOCKET client = (SOCKET)(UINT_PTR)param;
    DWORD timeoutMs = 4000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    std::string method, path, body;
    if (ReadHttpRequest(client, method, path, body)) {
        if (method == "OPTIONS") {
            SendHttpResponse(client, 204, "No Content", "text/plain", "");
        } else if (method == "GET" && (path == "/status" || path == "/")) {
            EnterCriticalSection(&g_snapCs);
            MachineSnapshot snap = g_snapshot;
            LeaveCriticalSection(&g_snapCs);
            SendHttpResponse(client, 200, "OK", "application/json; charset=utf-8",
                             BuildSnapshotJson(snap, true, ""));
        } else if (method == "GET" && path == "/temp/autosave") {
            std::string savedJson;
            std::string filePath = g_tempDir + "\\tabs_autosave.json";
            if (ReadEntireFile(filePath, savedJson) && !savedJson.empty()) {
                SendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", savedJson);
            } else {
                SendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", "{\"exists\":false}");
            }
        } else if (method == "POST" && path == "/temp/autosave") {
            std::string filePath = g_tempDir + "\\tabs_autosave.json";
            if (body.empty()) {
                SendHttpResponse(client, 400, "Bad Request", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"error\":\"Empty body\"}");
            } else if (WriteFileAtomic(filePath, body)) {
                SendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true}");
            } else {
                SendHttpResponse(client, 500, "Internal Server Error", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"error\":\"Failed writing temp/tabs_autosave.json\"}");
            }
        } else if (method == "POST") {
            ActionRequest req;
            if (path == "/config") {
                req.type = ACT_CONFIG;
                JsonExtractString(body, "valveOpenCmd", req.openCmd);
                JsonExtractString(body, "valveCloseCmd", req.closeCmd);
                req.hasDispensers = JsonExtractDispensers(body, req.dispensers);
            } else if (path == "/load_and_run") {
                req.type = ACT_LOAD_AND_RUN;
                JsonExtractString(body, "gcode", req.gcode);
                JsonExtractString(body, "filename", req.filename);
                JsonExtractString(body, "valveOpenCmd", req.openCmd);
                JsonExtractString(body, "valveCloseCmd", req.closeCmd);
                req.hasDispensers = JsonExtractDispensers(body, req.dispensers);
            } else if (path == "/pause") {
                req.type = ACT_PAUSE;
            } else if (path == "/resume") {
                req.type = ACT_RESUME;
            } else if (path == "/stop") {
                req.type = ACT_STOP;
            } else if (path == "/reset") {
                req.type = ACT_RESET;
            } else if (path == "/jog") {
                req.type = ACT_JOG;
                JsonExtractString(body, "axis", req.axis);
                JsonExtractDouble(body, "step", req.step);
            } else if (path == "/zero") {
                req.type = ACT_ZERO;
                JsonExtractString(body, "axis", req.axis);
            } else if (path == "/mdi") {
                req.type = ACT_MDI;
                JsonExtractString(body, "cmd", req.cmd);
                JsonExtractString(body, "openCmd", req.openCmd);
                JsonExtractString(body, "closeCmd", req.closeCmd);
                JsonExtractStringArray(body, "raiseCmds", req.raiseCmds);
            } else {
                SendHttpResponse(client, 404, "Not Found", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"error\":\"Unknown endpoint\"}");
                shutdown(client, SD_BOTH);
                closesocket(client);
                return 0;
            }

            DWORD comTimeout = (req.type == ACT_LOAD_AND_RUN) ? 5000 : 3000;
            if (!SubmitComAction(req, comTimeout)) {
                SendHttpResponse(client, 504, "Gateway Timeout", "application/json; charset=utf-8",
                                 "{\"ok\":false,\"error\":\"COM thread timeout\"}");
            } else {
                bool ok = (req.httpCode >= 200 && req.httpCode < 300);
                const char* stText = (req.httpCode == 200) ? "OK" :
                                     (req.httpCode == 400) ? "Bad Request" :
                                     (req.httpCode == 409) ? "Conflict" :
                                     (req.httpCode == 503) ? "Service Unavailable" : "Error";
                SendHttpResponse(client, req.httpCode, stText, "application/json; charset=utf-8",
                                 BuildSnapshotJson(req.resultSnap, ok, req.errorMsg));
            }
        } else {
            SendHttpResponse(client, 404, "Not Found", "application/json; charset=utf-8",
                             "{\"ok\":false,\"error\":\"Not found\"}");
        }
    }

    shutdown(client, SD_BOTH);
    closesocket(client);
    return 0;
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_running, 0);
        if (g_listenSock != INVALID_SOCKET) {
            closesocket(g_listenSock);
            g_listenSock = INVALID_SOCKET;
        }
        return TRUE;
    }
    return FALSE;
}

// ============================================================================
// Entry Point
// ============================================================================

int main(int argc, char** argv) {
    // Allow "--once" self-test check or single-instance protection
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Local\\CNCDispenser_Mach3Bridge_V2");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("[Mach3Bridge] Another instance is already running on http://127.0.0.1:%d.\n", BRIDGE_PORT);
        CloseHandle(hMutex);
        return 0;
    }

    char modPath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, modPath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string full(modPath, len);
        size_t slash = full.find_last_of("\\/");
        g_baseDir = (slash != std::string::npos) ? full.substr(0, slash) : ".";
    } else {
        g_baseDir = ".";
    }
    g_tempDir = g_baseDir + "\\temp";
    EnsureTempDir();
    InitDefaultDispensers();

    g_snapshot.mach3Connected = false;
    g_snapshot.status = "ESTOP";
    g_snapshot.x = 0.0;
    g_snapshot.y = 0.0;
    g_snapshot.feedrate = 0.0;
    g_snapshot.lineNum = 0;
    g_snapshot.totalLines = 0;
    g_snapshot.valveOpen = false;
    g_snapshot.activeDispenserId = 1;
    g_snapshot.isMoving = false;
    g_snapshot.inReset = false;

    InitializeCriticalSection(&g_snapCs);
    InitializeCriticalSection(&g_queueCs);
    g_queueEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[ERROR] WSAStartup failed!\n");
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) {
        printf("[ERROR] Cannot create TCP socket.\n");
        WSACleanup();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    int opt = 1;
    setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(BRIDGE_PORT);

    if (bind(g_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[ERROR] Port %d is already in use or blocked.\n", BRIDGE_PORT);
        closesocket(g_listenSock);
        WSACleanup();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    if (listen(g_listenSock, 16) == SOCKET_ERROR) {
        printf("[ERROR] listen() failed.\n");
        closesocket(g_listenSock);
        WSACleanup();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    HANDLE hComThread = CreateThread(NULL, 0, ComWorkerThreadProc, NULL, 0, NULL);

    printf("============================================================\n");
    printf("  CNC Dispenser V%s - Mach3 Native C++ Bridge (Win32)\n", BRIDGE_VERSION);
    printf("============================================================\n");
    printf("  HTTP API : http://127.0.0.1:%d\n", BRIDGE_PORT);
    printf("  Temp Dir : %s\n", g_tempDir.c_str());
    printf("  Arch     : %d-bit (Mach3 COM Mach4.Document)\n", (int)(sizeof(void*) * 8));
    printf("  Status   : Listening... Minimize this window, do not close.\n");
    printf("============================================================\n");

    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        Sleep(250);
        InterlockedExchange(&g_running, 0);
        closesocket(g_listenSock);
        g_listenSock = INVALID_SOCKET;
    }

    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(g_listenSock, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (InterlockedCompareExchange(&g_running, 1, 1) == 0) break;
            Sleep(10);
            continue;
        }
        HANDLE hClient = CreateThread(NULL, 0, ClientThreadProc, (LPVOID)(UINT_PTR)clientSock, 0, NULL);
        if (hClient) {
            CloseHandle(hClient);
        } else {
            closesocket(clientSock);
        }
    }

    if (hComThread) {
        SetEvent(g_queueEvent);
        WaitForSingleObject(hComThread, 2000);
        CloseHandle(hComThread);
    }
    if (g_queueEvent) CloseHandle(g_queueEvent);
    DeleteCriticalSection(&g_queueCs);
    DeleteCriticalSection(&g_snapCs);
    WSACleanup();
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
