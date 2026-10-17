#pragma once

#include <cmath>
#include <cstdint>

namespace NativeDimensionAuditPolicy {

constexpr std::uint64_t MaxRawCandidates = 4096;
constexpr std::uint64_t MaxMatchedParents = 512;
constexpr std::uint64_t MaxBatchParents = 25;
constexpr std::uint64_t MaxStoredNotes = 16384;
constexpr std::uint64_t MaxSerializedFactBytes = 16ULL * 1024ULL * 1024ULL;

struct Bounds2D {
	double xMin;
	double yMin;
	double xMax;
	double yMax;
};

inline bool IsFiniteOrderedBounds (const Bounds2D& bounds)
{
	return std::isfinite (bounds.xMin) && std::isfinite (bounds.yMin)
		&& std::isfinite (bounds.xMax) && std::isfinite (bounds.yMax)
		&& bounds.xMin <= bounds.xMax && bounds.yMin <= bounds.yMax;
}

inline bool IntersectsInclusive (const Bounds2D& left, const Bounds2D& right)
{
	return IsFiniteOrderedBounds (left) && IsFiniteOrderedBounds (right)
		&& left.xMin <= right.xMax && left.xMax >= right.xMin
		&& left.yMin <= right.yMax && left.yMax >= right.yMin;
}

inline bool WithinCandidateLimit (std::uint64_t count) { return count <= MaxRawCandidates; }
inline bool WithinMatchedParentLimit (std::uint64_t count) { return count <= MaxMatchedParents; }
inline bool WithinStoredNoteLimit (std::uint64_t count) { return count <= MaxStoredNotes; }
inline bool WithinSerializedFactLimit (std::uint64_t byteCount) { return byteCount <= MaxSerializedFactBytes; }

} // namespace NativeDimensionAuditPolicy
