#pragma once

#include <cstdint>
#include <limits>

namespace NativeDimensionLayerControlPolicy {

constexpr std::uint64_t MaxLayers = 4096;
constexpr std::uint64_t MaxCombinations = 4096;
constexpr std::uint64_t MaxLayerEntries = 1ULL << 20;
constexpr std::uint64_t MaxSerializedFactBytes = 16ULL * 1024ULL * 1024ULL;

enum class Phase : std::uint8_t { Empty, Registered, Prepared, Hidden, Locked, Unknown };
enum class Action : std::uint8_t { Begin, Prepare, SetHidden, SetLocked, Inspect };

inline Phase NextPhase (const Phase phase, const Action action)
{
	switch (action) {
		case Action::Begin: return phase == Phase::Empty ? Phase::Registered : Phase::Unknown;
		case Action::Prepare: return phase == Phase::Registered ? Phase::Prepared : Phase::Unknown;
		case Action::SetHidden: return phase == Phase::Prepared ? Phase::Hidden : Phase::Unknown;
		case Action::SetLocked: return phase == Phase::Hidden ? Phase::Locked : Phase::Unknown;
		case Action::Inspect: return phase;
	}
	return Phase::Unknown;
}

inline bool IsWriteAction (const Action action)
{
	return action == Action::Prepare || action == Action::SetHidden || action == Action::SetLocked;
}

inline bool IsValidReadCount (const std::uint64_t count, const std::uint64_t limit)
{
	return count <= limit;
}

inline bool IsValidEntryCount (const std::uint64_t count, const std::uint64_t layerCount)
{
	return count <= layerCount && count <= MaxLayers;
}

inline bool HasStorageForEntries (const std::uint64_t handleBytes, const std::uint64_t entryCount, const std::uint64_t entrySize)
{
	return entrySize != 0 && entryCount <= std::numeric_limits<std::uint64_t>::max () / entrySize && handleBytes >= entryCount * entrySize;
}

enum class SlotState : std::uint8_t { Live, Tombstone, Absent };
enum class OwnedEntryChange : std::uint8_t { Add, Replace, Invalid };

inline OwnedEntryChange ClassifyOwnedEntryChange (const bool oldIndexWasLive, const bool oldEntryPresent,
	const SlotState oldEntryState, const bool oldEntryFlagsAndClassPreserved, const bool newEntryLive)
{
	if (oldIndexWasLive || !newEntryLive) return OwnedEntryChange::Invalid;
	if (!oldEntryPresent) return OwnedEntryChange::Add;
	if ((oldEntryState == SlotState::Tombstone || oldEntryState == SlotState::Absent) && oldEntryFlagsAndClassPreserved)
		return OwnedEntryChange::Replace;
	return OwnedEntryChange::Invalid;
}

inline bool IsValidAbsentOwnedEntryAddition (const std::int32_t rawFlags, const std::int32_t connectionClass)
{
	return (rawFlags == 0 || rawFlags == 1) && connectionClass == 1;
}

inline bool IsVisibleUnlockedStoredEntry (const std::int32_t rawFlags)
{
	return (rawFlags & 3) == 0;
}

inline bool IsExactNewOwnedLayerState (const std::int32_t rawFlags, const std::int32_t connectionClass)
{
	return rawFlags == 0 && connectionClass == 0;
}

inline bool IsConsistentSparseCount (const bool highestIndexIsBound, const std::uint64_t reportedCount,
	const std::uint64_t liveCount, const std::uint64_t tombstoneCount, const std::uint64_t slotCount,
	const std::uint64_t maximumObservedIndex)
{
	if (highestIndexIsBound)
		return reportedCount == slotCount && liveCount <= reportedCount && tombstoneCount <= reportedCount
			&& liveCount + tombstoneCount == reportedCount && maximumObservedIndex == reportedCount;
	return reportedCount == liveCount && slotCount == liveCount && tombstoneCount == 0
		&& ((slotCount == 0 && maximumObservedIndex == 0) || (slotCount > 0 && maximumObservedIndex > 0));
}

} // namespace NativeDimensionLayerControlPolicy
