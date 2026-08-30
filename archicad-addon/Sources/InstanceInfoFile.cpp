#include "InstanceInfoFile.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "MigrationHelper.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(WIN32)
#include <windows.h>
#endif

namespace {

#if defined(WIN32)

// Escapes a GS::UniString into a JSON string body (without the surrounding
// quotes). Converts to UTF-8 first so non-ASCII project names stay readable,
// then escapes the JSON-significant ASCII control characters and quotes.
std::string EscapeJsonString (const GS::UniString& value)
{
    const std::string utf8 (value.ToCStr (CC_UTF8).Get ());
    std::string result;
    result.reserve (utf8.size ());
    for (unsigned char ch : utf8) {
        switch (ch) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b";  break;
            case '\f': result += "\\f";  break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:
                if (ch < 0x20) {
                    char buf[8] = {0};
                    std::snprintf (buf, sizeof (buf), "\\u%04x", ch);
                    result += buf;
                } else {
                    result += static_cast<char> (ch);
                }
                break;
        }
    }
    return result;
}

// Formats a UTC FILETIME as an ISO 8601 "YYYY-MM-DDTHH:MM:SS.fffZ" string.
std::string FormatIsoUtc (const FILETIME& ft)
{
    SYSTEMTIME st = {0};
    if (!FileTimeToSystemTime (&ft, &st)) {
        return std::string ("");
    }
    char buf[40] = {0};
    std::snprintf (buf, sizeof (buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string (buf);
}

std::string GetProcessStartedIso ()
{
    FILETIME creation = {0}, exitT = {0}, kernel = {0}, user = {0};
    if (GetProcessTimes (GetCurrentProcess (), &creation, &exitT, &kernel, &user)) {
        std::string formatted = FormatIsoUtc (creation);
        if (!formatted.empty ()) {
            return formatted;
        }
    }
    // Fallback: current UTC time if the process times query is unavailable.
    SYSTEMTIME st = {0};
    GetSystemTime (&st);
    char buf[40] = {0};
    std::snprintf (buf, sizeof (buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string (buf);
}

bool EnsureDirectory (const std::string& path)
{
    // Recursive CreateDirectoryA; ignore ERROR_ALREADY_EXISTS.
    if (path.empty ()) return false;
    DWORD attr = GetFileAttributesA (path.c_str ());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    const size_t pos = path.find_last_of ('\\');
    if (pos != std::string::npos && pos > 0) {
        if (!EnsureDirectory (path.substr (0, pos))) {
            // Parent failed; still attempt to create this level below.
        }
    }
    if (CreateDirectoryA (path.c_str (), nullptr)) {
        return true;
    }
    return GetLastError () == ERROR_ALREADY_EXISTS;
}

std::string GetPortsDirectory ()
{
    char localAppData[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA ("LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // Fallback to %USERPROFILE%\AppData\Local.
        len = GetEnvironmentVariableA ("USERPROFILE", localAppData, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return std::string ();
        }
        std::string base (localAppData);
        base += "\\AppData\\Local\\ArchicadTapirMcp\\ports";
        return base;
    }
    std::string base (localAppData);
    base += "\\ArchicadTapirMcp\\ports";
    return base;
}

std::string BuildInstanceJson ()
{
    const DWORD pid = GetCurrentProcessId ();

    GS::UShort port = 0;
    const GSErrCode portErr = ACAPI_Command_GetHttpConnectionPort (&port);
    (void) portErr; // Port may be unavailable at very early load; refresh on project events.

    const std::string startedAt = GetProcessStartedIso ();

    bool isUntitled = true;
    bool isTeamwork = false;
    std::string projectName;
    std::string projectPath;
    std::string projectLocation;

    API_ProjectInfo projectInfo = {};
    if (ACAPI_ProjectOperation_Project (&projectInfo) == NoError) {
        isUntitled = projectInfo.untitled != 0;
        isTeamwork = projectInfo.teamwork != 0;
        if (!isUntitled) {
            if (projectInfo.projectName) {
                projectName = EscapeJsonString (*projectInfo.projectName);
            }
            if (projectInfo.projectPath) {
                projectPath = EscapeJsonString (*projectInfo.projectPath);
            }
            if (projectInfo.location) {
                projectLocation = EscapeJsonString (projectInfo.location->ToDisplayText ());
            }
        }
    }

    std::string json;
    json.reserve (256);
    char header[96] = {0};
    std::snprintf (header, sizeof (header),
                   "{\"pid\":%lu,\"port\":%lu,\"startedAt\":\"%s\"",
                   static_cast<unsigned long> (pid),
                   static_cast<unsigned long> (port),
                   startedAt.c_str ());
    json += header;

    json += ",\"isUntitled\":";
    json += (isUntitled ? "true" : "false");
    json += ",\"isTeamwork\":";
    json += (isTeamwork ? "true" : "false");

    if (!projectName.empty ()) {
        json += ",\"projectName\":\"";
        json += projectName;
        json += "\"";
    }
    if (!projectPath.empty ()) {
        json += ",\"projectPath\":\"";
        json += projectPath;
        json += "\"";
    }
    if (!projectLocation.empty ()) {
        json += ",\"projectLocation\":\"";
        json += projectLocation;
        json += "\"";
    }
    json += "}";
    return json;
}

#endif // defined(WIN32)

} // namespace

namespace InstanceInfoFile {

GSErrCode WriteInstanceInfo ()
{
#if defined(WIN32)
    const std::string dir = GetPortsDirectory ();
    if (dir.empty ()) {
        return APIERR_GENERAL;
    }
    if (!EnsureDirectory (dir)) {
        return APIERR_GENERAL;
    }

    const DWORD pid = GetCurrentProcessId ();
    char pidBuf[24] = {0};
    std::snprintf (pidBuf, sizeof (pidBuf), "%lu", static_cast<unsigned long> (pid));
    const std::string filePath = dir + "\\" + pidBuf + ".json";
    // Include process/thread/tick identity so concurrent project/open events
    // never share one predictable temporary file.  The final MoveFileEx is
    // still the atomic publication boundary consumed by the registry reader.
    const std::string tmpPath = filePath + "." + std::to_string (static_cast<unsigned long> (pid))
        + "-" + std::to_string (static_cast<unsigned long> (GetCurrentThreadId ()))
        + "-" + std::to_string (static_cast<unsigned long long> (GetTickCount64 ())) + ".tmp";

    const std::string json = BuildInstanceJson ();

    FILE* file = nullptr;
    if (fopen_s (&file, tmpPath.c_str (), "wb") != 0 || file == nullptr) {
        return APIERR_GENERAL;
    }
    const size_t written = std::fwrite (json.data (), 1, json.size (), file);
    std::fclose (file);
    if (written != json.size ()) {
        DeleteFileA (tmpPath.c_str ());
        return APIERR_GENERAL;
    }

    // Atomic replace so readers never observe a half-written file.
    if (!MoveFileExA (tmpPath.c_str (), filePath.c_str (), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA (tmpPath.c_str ());
        return APIERR_GENERAL;
    }
    return NoError;
#else
    return NoError;
#endif
}

GSErrCode Delete ()
{
#if defined(WIN32)
    const std::string dir = GetPortsDirectory ();
    if (dir.empty ()) {
        return NoError;
    }
    const DWORD pid = GetCurrentProcessId ();
    char pidBuf[24] = {0};
    std::snprintf (pidBuf, sizeof (pidBuf), "%lu", static_cast<unsigned long> (pid));
    const std::string filePath = dir + "\\" + pidBuf + ".json";
    if (!DeleteFileA (filePath.c_str ())) {
        const DWORD err = GetLastError ();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            return APIERR_GENERAL;
        }
    }
    return NoError;
#else
    return NoError;
#endif
}

} // namespace InstanceInfoFile
