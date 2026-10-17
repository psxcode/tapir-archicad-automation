#include "NativeDimensionUtf16.hpp"

#if !defined(AC_VERSION) || (AC_VERSION != 25 && AC_VERSION != 28)
#error "Run the shared Unicode-copy proof separately against the AC25 and AC28 DevKits."
#endif

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool Require (const bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}

int main ()
{
    using NativeDimensionUtf16::CopyCallbackBuffer;
    using NativeDimensionUtf16::CopyStatus;
    constexpr unsigned short empty[] = { 0 };
    constexpr unsigned short ascii[] = { 'A', 'r', 'c', 0 };
    constexpr unsigned short unicode[] = { 0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442, 0 };
    constexpr unsigned short supplementary[] = { 0xD83D, 0xDE80, 0 };
    constexpr unsigned short unpairedHigh[] = { 0xD83D, 'x', 0 };
    constexpr unsigned short unpairedLow[] = { 0xDE80, 0 };
    constexpr std::size_t maxUnits = 4;
    constexpr unsigned short overLimit[] = { '1', '2', '3', '4', '5', 0 };
    constexpr unsigned short exactlyAtLimit[] = { '1', '2', '3', '4', 0 };

    bool ok = true;
    const auto nullResult = CopyCallbackBuffer (nullptr, maxUnits);
    ok &= Require (nullResult.status == CopyStatus::NullBuffer && nullResult.units.empty (), "null buffer must remain explicit");
    const auto emptyResult = CopyCallbackBuffer (empty, maxUnits);
    ok &= Require (emptyResult.status == CopyStatus::Empty && emptyResult.observedUnits == 0, "empty callback string must be distinct from no run");
    const auto asciiResult = CopyCallbackBuffer (ascii, maxUnits);
    ok &= Require (asciiResult.status == CopyStatus::Available && asciiResult.units.size () == 3 && asciiResult.units[0] == 'A', "ASCII callback text must copy exactly");
    const auto unicodeResult = CopyCallbackBuffer (unicode, maxUnits + 4);
    ok &= Require (unicodeResult.status == CopyStatus::Available && unicodeResult.units.front () == 0x041F && unicodeResult.units.back () == 0x0442, "BMP Unicode callback text must copy exactly");
    const auto supplementaryResult = CopyCallbackBuffer (supplementary, maxUnits);
    ok &= Require (supplementaryResult.status == CopyStatus::Available && supplementaryResult.units.size () == 2, "valid UTF-16 surrogate pair must be retained");
    ok &= Require (CopyCallbackBuffer (unpairedHigh, maxUnits).status == CopyStatus::MalformedSurrogates, "unpaired high surrogate must be rejected");
    ok &= Require (CopyCallbackBuffer (unpairedLow, maxUnits).status == CopyStatus::MalformedSurrogates, "unpaired low surrogate must be rejected");
    const auto exactResult = CopyCallbackBuffer (exactlyAtLimit, maxUnits);
    ok &= Require (exactResult.status == CopyStatus::Available && exactResult.units.size () == maxUnits, "exact limit must be accepted");
    const auto longResult = CopyCallbackBuffer (overLimit, maxUnits);
    ok &= Require (longResult.status == CopyStatus::OverLimit && longResult.observedUnits == maxUnits + 1 && longResult.units.empty (), "fully allocated over-limit input must not emit a prefix");
    ok &= Require (CopyCallbackBuffer (ascii, 0).status == CopyStatus::InvalidLimit, "zero bound must be rejected");
    return ok ? 0 : 1;
}
