#pragma once

#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace NativeDimensionUtf16 {

enum class CopyStatus {
    Available,
    Empty,
    NullBuffer,
    OverLimit,
    MalformedSurrogates,
    AllocationFailure,
    InvalidLimit
};

struct CopyResult {
    CopyStatus status = CopyStatus::NullBuffer;
    std::vector<std::uint16_t> units;
    std::size_t observedUnits = 0;
};

inline bool IsWellFormed (const std::vector<std::uint16_t>& units) noexcept
{
    for (std::size_t index = 0; index < units.size (); ++index) {
        const std::uint16_t unit = units[index];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 1 >= units.size () || units[index + 1] < 0xDC00 || units[index + 1] > 0xDFFF)
                return false;
            ++index;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

// API_PrimTextID supplies par2 as a direct NUL-terminated unsigned-short
// buffer for the duration of the callback. Call this only during that callback.
// A bounded scan cannot establish that an arbitrary invalid pointer is readable;
// callers must honor the DevKit's callback lifetime contract.
inline CopyResult CopyCallbackBuffer (const unsigned short* buffer, const std::size_t maxUnits) noexcept
{
    CopyResult result;
    if (buffer == nullptr) {
        result.status = CopyStatus::NullBuffer;
        return result;
    }
    if (maxUnits == 0 || maxUnits == std::numeric_limits<std::size_t>::max ()) {
        result.status = CopyStatus::InvalidLimit;
        return result;
    }

    try {
        result.units.reserve (maxUnits);
        for (std::size_t index = 0; index <= maxUnits; ++index) {
            const std::uint16_t unit = static_cast<std::uint16_t> (buffer[index]);
            if (unit == 0) {
                result.observedUnits = index;
                if (!IsWellFormed (result.units)) {
                    result.units.clear ();
                    result.status = CopyStatus::MalformedSurrogates;
                } else {
                    result.status = result.units.empty () ? CopyStatus::Empty : CopyStatus::Available;
                }
                return result;
            }
            if (index == maxUnits) {
                result.observedUnits = maxUnits + 1;
                result.units.clear ();
                result.status = CopyStatus::OverLimit;
                return result;
            }
            result.units.push_back (unit);
        }
    } catch (...) {
        result.units.clear ();
        result.observedUnits = 0;
        result.status = CopyStatus::AllocationFailure;
    }
    return result;
}

} // namespace NativeDimensionUtf16
