#pragma once

// PerfTrace.hpp - per-command performance tracing for the Tapir addon.
//
// Tracing is compile-time optional: every Execute keeps the same call sites,
// but normal/customer builds compile ScopeTimer and the output functions to
// no-ops. Diagnostic builds pass -DTAPIR_ENABLE_PERF_TRACE=1 and append ONE
// JSON object line per instrumented command.
//
// The trace file path is resolved at write time: the TAPIR_PERF_TRACE_FILE
// environment variable wins if set and non-empty; otherwise the file is
// %TEMP%\tapir-perf-trace.jsonl (C:\Windows\Temp\tapir-perf-trace.jsonl if
// TEMP is not set). The stream is flushed and closed per line so the data
// survives client-side timeouts and crashes; if the file cannot be opened the
// write fails silently.
//
// The JSON line is hand-rolled (not JSON::CreateFromObjectState): the DevKit
// converter is a compiled library whose double formatting is not guaranteed to
// be the exact "one decimal place" shape this format mandates, and the fixed
// line shape is small enough to emit directly with GS::UniString::Printf.

#include "UniString.hpp"

#ifndef TAPIR_ENABLE_PERF_TRACE
#define TAPIR_ENABLE_PERF_TRACE 0
#endif

#if TAPIR_ENABLE_PERF_TRACE
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#if defined (_WIN32)
#include <windows.h>
#endif
#endif

namespace PerfTrace {

#if TAPIR_ENABLE_PERF_TRACE

enum class Phase {
    ElemList,
    ParentResolve,
    ElementGet,
    MemoInfo,
    CalcBounds,
    TypeMemo,
    ShapePrims,
    AttrLookup,
    LibPart,
};

struct Accumulator {
    double elemListMs = 0.0;
    double parentResolveMs = 0.0;
    double elementGetMs = 0.0;
    double memoInfoMs = 0.0;
    double calcBoundsMs = 0.0;
    double typeMemoMs = 0.0;
    double shapePrimsMs = 0.0;
    double attrLookupMs = 0.0;
    double libpartMs = 0.0;
    std::map<std::string, std::pair<int, double>> types;    // type name -> {count, totalMs}
    int requested = 0;
    int returned = 0;
};

// AC25 builds compile with C++14: no inline variables, use accessor
// functions with function-local statics instead.
inline std::atomic<int>& SeqCounter ()
{
    static std::atomic<int> value (0);         // process-wide sequence counter
    return value;
}

inline bool& FirstWriteFlag ()
{
    static bool value = true;                   // truncate on the first write of the process
    return value;
}

inline Accumulator& AccumulatorInstance ()
{
    static thread_local Accumulator value;
    return value;
}

class ScopeTimer
{
public:
    explicit ScopeTimer (Phase phase) : m_phase (phase), m_isTypeTimer (false)
    {
        m_start = std::chrono::steady_clock::now ();
    }

    explicit ScopeTimer (const GS::UniString& typeName) :
        m_phase (Phase::ElemList),
        m_isTypeTimer (true),
        m_typeName (UniStringToStdString (typeName))
    {
        m_start = std::chrono::steady_clock::now ();
    }

    ~ScopeTimer ()
    {
        if (m_start == std::chrono::steady_clock::time_point ())
            return;
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - m_start).count ();
        if (m_isTypeTimer) {
            std::pair<int, double>& entry = AccumulatorInstance ().types[m_typeName];
            ++entry.first;
            entry.second += ms;
        } else {
            AddToPhase (m_phase, ms);
        }
    }

    ScopeTimer (const ScopeTimer&) = delete;
    ScopeTimer& operator= (const ScopeTimer&) = delete;

private:
    static std::string UniStringToStdString (const GS::UniString& source)
    {
        return std::string (source.ToCStr (CC_UTF8).Get ());
    }

    static void AddToPhase (Phase phase, double ms)
    {
        Accumulator& acc = AccumulatorInstance ();
        switch (phase) {
            case Phase::ElemList:       acc.elemListMs += ms;       break;
            case Phase::ParentResolve:  acc.parentResolveMs += ms;  break;
            case Phase::ElementGet:     acc.elementGetMs += ms;     break;
            case Phase::MemoInfo:       acc.memoInfoMs += ms;       break;
            case Phase::CalcBounds:     acc.calcBoundsMs += ms;     break;
            case Phase::TypeMemo:       acc.typeMemoMs += ms;       break;
            case Phase::ShapePrims:     acc.shapePrimsMs += ms;     break;
            case Phase::AttrLookup:     acc.attrLookupMs += ms;     break;
            case Phase::LibPart:        acc.libpartMs += ms;        break;
        }
    }

    Phase m_phase;
    bool m_isTypeTimer;
    std::string m_typeName;
    std::chrono::steady_clock::time_point m_start;
};

static void ResetAccumulator ()
{
    Accumulator& acc = AccumulatorInstance ();
    acc.elemListMs = 0.0;
    acc.parentResolveMs = 0.0;
    acc.elementGetMs = 0.0;
    acc.memoInfoMs = 0.0;
    acc.calcBoundsMs = 0.0;
    acc.typeMemoMs = 0.0;
    acc.shapePrimsMs = 0.0;
    acc.attrLookupMs = 0.0;
    acc.libpartMs = 0.0;
    acc.types.clear ();
    acc.requested = 0;
    acc.returned = 0;
}

static void SetRequestedReturned (int requested, int returned)
{
    Accumulator& acc = AccumulatorInstance ();
    acc.requested = requested;
    acc.returned = returned;
}

static int NextSeq ()
{
    return SeqCounter ().fetch_add (1, std::memory_order_relaxed);
}

static GS::UniString ResolveTraceFilePath ()
{
    const char* envPath = std::getenv ("TAPIR_PERF_TRACE_FILE");
    if (envPath != nullptr && envPath[0] != '\0')
        return GS::UniString (envPath, CC_UTF8);

    const char* temp = std::getenv ("TEMP");
    if (temp == nullptr || temp[0] == '\0')
        return GS::UniString ("C:\\Windows\\Temp\\tapir-perf-trace.jsonl", CC_UTF8);

    GS::UniString result (temp, CC_UTF8);
    if (result.GetLength () == 0 || result[result.GetLength () - 1] != '\\')
        result.Append ('\\');
    result.Append ("tapir-perf-trace.jsonl");
    return result;
}

#if defined (_WIN32)
static std::wstring ToNativePath (const GS::UniString& path)
{
    const char* utf8 = path.ToCStr (CC_UTF8).Get ();
    // std::ofstream has no UTF-8 path overload in the C++14 AC25 toolchain.
    // Convert properly so a diagnostic path under a non-ASCII user profile
    // does not silently become an invalid byte-by-byte Windows path.
    const int length = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (length <= 0) {
        std::wstring fallback;
        while (*utf8 != '\0')
            fallback.push_back (static_cast<wchar_t> (static_cast<unsigned char> (*utf8++)));
        return fallback;
    }
    std::wstring result (static_cast<std::size_t> (length), L'\0');
    MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, path.ToCStr (CC_UTF8).Get (), -1,
                         &result[0], length);
    if (!result.empty ())
        result.pop_back ();
    return result;
}
#else
static std::string ToNativePath (const GS::UniString& path)
{
    const char* utf8 = path.ToCStr (CC_UTF8).Get ();
    // std::ofstream accepts a UTF-8 narrow path on the non-Windows builds.
    std::string result;
    while (*utf8 != '\0')
        result.push_back (*utf8++);
    return result;
}
#endif

static void WriteLine (const GS::UniString& cmd, int seq, double totalMs)
{
    const Accumulator& acc = AccumulatorInstance ();
    const double sumPhases = acc.elemListMs + acc.parentResolveMs +
                             acc.elementGetMs + acc.memoInfoMs +
                             acc.calcBoundsMs + acc.typeMemoMs +
                             acc.shapePrimsMs + acc.attrLookupMs +
                             acc.libpartMs;
    const double unattributedMs = (totalMs > sumPhases) ? (totalMs - sumPhases) : 0.0;

    // UniString::Append returns void; build the line with sequential appends.
    GS::UniString line;
    line.Append ("{\"cmd\":\"");
    line.Append (cmd);
    line.Append ("\",\"seq\":");
    line.Append (GS::UniString::Printf ("%d", seq));
    line.Append (",\"requested\":");
    line.Append (GS::UniString::Printf ("%d", acc.requested));
    line.Append (",\"returned\":");
    line.Append (GS::UniString::Printf ("%d", acc.returned));
    line.Append (",\"totalMs\":");
    line.Append (GS::UniString::Printf ("%.1f", totalMs));
    line.Append (",\"elemListMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.elemListMs));
    line.Append (",\"parentResolveMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.parentResolveMs));
    line.Append (",\"elementGetMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.elementGetMs));
    line.Append (",\"memoInfoMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.memoInfoMs));
    line.Append (",\"calcBoundsMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.calcBoundsMs));
    line.Append (",\"typeMemoMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.typeMemoMs));
    line.Append (",\"shapePrimsMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.shapePrimsMs));
    line.Append (",\"attrLookupMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.attrLookupMs));
    line.Append (",\"libpartMs\":");
    line.Append (GS::UniString::Printf ("%.1f", acc.libpartMs));
    line.Append (",\"unattributedMs\":");
    line.Append (GS::UniString::Printf ("%.1f", unattributedMs));
    line.Append (",\"types\":{");
    bool firstType = true;
    for (const auto& entry : acc.types) {
        if (!firstType)
            line.Append (",");
        firstType = false;
        line.Append ("\"");
        line.Append (GS::UniString (entry.first.c_str (), CC_UTF8));
        line.Append ("\":{\"count\":");
        line.Append (GS::UniString::Printf ("%d", entry.second.first));
        line.Append (",\"ms\":");
        line.Append (GS::UniString::Printf ("%.1f", entry.second.second));
        line.Append ("}");
    }
    line.Append ("}}");

    // Add-on commands are normally serialized by Archicad, but keeping the
    // file transition guarded makes the diagnostic path correct if a future
    // command is dispatched from another worker thread.
    static std::mutex writeMutex;
    const std::lock_guard<std::mutex> lock (writeMutex);
    std::ofstream stream;
    if (FirstWriteFlag ()) {
        FirstWriteFlag () = false;
        stream.open (ToNativePath (ResolveTraceFilePath ()), std::ios::out | std::ios::trunc);
    } else {
        stream.open (ToNativePath (ResolveTraceFilePath ()), std::ios::out | std::ios::app);
    }
    if (!stream.is_open ()) {
        return;                 // silent disable: no crash, no report spam
    }
    stream << line.ToCStr (CC_UTF8).Get () << std::endl;
    stream.flush ();
    stream.close ();
}

#else

// Keep the public call surface identical when tracing is compiled out.  These
// constructors are intentionally empty: the normal add-on pays no clock,
// allocation, map, or file-I/O cost for the diagnostic instrumentation.
enum class Phase {
    ElemList,
    ParentResolve,
    ElementGet,
    MemoInfo,
    CalcBounds,
    TypeMemo,
    ShapePrims,
    AttrLookup,
    LibPart,
};

class ScopeTimer {
public:
    explicit ScopeTimer (Phase) {}
    explicit ScopeTimer (const GS::UniString&) {}
};

inline void ResetAccumulator () {}
inline void SetRequestedReturned (int, int) {}
inline int NextSeq () { return 0; }
inline void WriteLine (const GS::UniString&, int, double) {}

#endif

}   // namespace PerfTrace
