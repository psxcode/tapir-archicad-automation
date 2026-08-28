#pragma once

#include "ACAPinc.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

// API_LibPart::location is allocated by the legacy Archicad API. Keep the
// required API ownership rule behind a small RAII guard so call sites do not
// manually delete API-owned pointers or forget cleanup on an early return.
class LibraryPartLocationGuard final {
public:
    explicit LibraryPartLocationGuard (API_LibPart& libPartIn) : libPart (libPartIn) {}

    ~LibraryPartLocationGuard ()
    {
        std::unique_ptr<IO::Location> location (libPart.location);
        libPart.location = nullptr;
    }

    LibraryPartLocationGuard (const LibraryPartLocationGuard&) = delete;
    LibraryPartLocationGuard& operator= (const LibraryPartLocationGuard&) = delete;

private:
    API_LibPart& libPart;
};

// API_SelectionInfo may contain an API-owned coordinate handle.  Selection
// snapshots are therefore never copied with the default shallow copy when a
// command has to restore focus after a temporary marquee operation.
inline void ReleaseSelectionInfoHandles (API_SelectionInfo& selection)
{
    if (selection.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&selection.marquee.coords));
    selection.marquee.coords = nullptr;
}

inline bool CloneSelectionInfoHandles (const API_SelectionInfo& source, API_SelectionInfo& destination)
{
    if (&source == &destination)
        return true;

    // The destination may already own a memo handle.  Release it before the
    // shallow metadata copy below; otherwise a second clone leaks that handle.
    ReleaseSelectionInfoHandles (destination);
    destination = source;
    destination.marquee.coords = nullptr;
    if (source.marquee.coords == nullptr)
        return true;
    if (*source.marquee.coords == nullptr)
        return false;

    constexpr GSSize maxSelectionCoordinateBytes = 16 * 1024 * 1024;
    const GSSize bytes = BMhGetSize (reinterpret_cast<GSHandle> (source.marquee.coords));
    if (bytes < static_cast<GSSize> (sizeof (API_Coord)) ||
        bytes % static_cast<GSSize> (sizeof (API_Coord)) != 0 ||
        bytes > maxSelectionCoordinateBytes)
        return false;
    if (source.typeID == API_MarqueePoly) {
        if (source.marquee.nCoords < 1 ||
            static_cast<GSSize> (source.marquee.nCoords) >
                std::numeric_limits<GSSize>::max () / static_cast<GSSize> (sizeof (API_Coord)) - 1)
            return false;
        const GSSize requiredBytes = (static_cast<GSSize> (source.marquee.nCoords) + 1) *
            static_cast<GSSize> (sizeof (API_Coord));
        if (requiredBytes > bytes)
            return false;
    }

    const GSSize coordinateCount = bytes / static_cast<GSSize> (sizeof (API_Coord));
    for (GSSize index = 0; index < coordinateCount; ++index) {
        const API_Coord& coordinate = (*source.marquee.coords)[index];
        if (!std::isfinite (coordinate.x) || !std::isfinite (coordinate.y))
            return false;
    }

    destination.marquee.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle (bytes, ALLOCATE_CLEAR, 0));
    if (destination.marquee.coords == nullptr || *destination.marquee.coords == nullptr) {
        if (destination.marquee.coords != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&destination.marquee.coords));
        return false;
    }
    std::memcpy (*destination.marquee.coords, *source.marquee.coords, bytes);
    return true;
}
