#include "NativeDimensionLayerControlPolicy.hpp"

#include <cassert>

using namespace NativeDimensionLayerControlPolicy;

int main ()
{
	assert (NextPhase (Phase::Empty, Action::Begin) == Phase::Registered);
	assert (NextPhase (Phase::Registered, Action::Prepare) == Phase::Prepared);
	assert (NextPhase (Phase::Prepared, Action::SetHidden) == Phase::Hidden);
	assert (NextPhase (Phase::Hidden, Action::SetLocked) == Phase::Locked);
	assert (NextPhase (Phase::Locked, Action::Inspect) == Phase::Locked);
	assert (NextPhase (Phase::Registered, Action::SetHidden) == Phase::Unknown);
	assert (NextPhase (Phase::Unknown, Action::SetLocked) == Phase::Unknown);
	assert (IsWriteAction (Action::Prepare));
	assert (IsWriteAction (Action::SetHidden));
	assert (IsWriteAction (Action::SetLocked));
	assert (!IsWriteAction (Action::Begin));
	assert (!IsWriteAction (Action::Inspect));
	assert (IsValidReadCount (MaxLayers, MaxLayers));
	assert (!IsValidReadCount (MaxLayers + 1, MaxLayers));
	assert (IsValidEntryCount (MaxLayers, MaxLayers));
	assert (IsValidEntryCount (MaxLayers - 1, MaxLayers));
	assert (IsValidEntryCount (0, MaxLayers));
	assert (!IsValidEntryCount (MaxLayers + 1, MaxLayers));
	assert (HasStorageForEntries (sizeof (std::uint64_t) * 2, 2, sizeof (std::uint64_t)));
	assert (!HasStorageForEntries (sizeof (std::uint64_t), 2, sizeof (std::uint64_t)));
	assert (!HasStorageForEntries (std::numeric_limits<std::uint64_t>::max (), std::numeric_limits<std::uint64_t>::max (), 2));
	assert (ClassifyOwnedEntryChange (false, false, SlotState::Absent, true, true) == OwnedEntryChange::Add);
	assert (ClassifyOwnedEntryChange (false, true, SlotState::Tombstone, true, true) == OwnedEntryChange::Replace);
	assert (ClassifyOwnedEntryChange (false, true, SlotState::Absent, true, true) == OwnedEntryChange::Replace);
	assert (ClassifyOwnedEntryChange (false, true, SlotState::Tombstone, false, true) == OwnedEntryChange::Invalid);
	assert (ClassifyOwnedEntryChange (false, true, SlotState::Live, true, true) == OwnedEntryChange::Invalid);
	assert (ClassifyOwnedEntryChange (true, false, SlotState::Live, true, true) == OwnedEntryChange::Invalid);
	assert (ClassifyOwnedEntryChange (false, false, SlotState::Absent, true, false) == OwnedEntryChange::Invalid);
	assert (IsValidAbsentOwnedEntryAddition (0, 1));
	assert (IsValidAbsentOwnedEntryAddition (1, 1));
	assert (!IsValidAbsentOwnedEntryAddition (2, 1));
	assert (!IsValidAbsentOwnedEntryAddition (3, 1));
	assert (!IsValidAbsentOwnedEntryAddition (16, 1));
	assert (!IsValidAbsentOwnedEntryAddition (0, 0));
	assert (!IsValidAbsentOwnedEntryAddition (1, 0));
	assert (!IsValidAbsentOwnedEntryAddition (1, 2));
	assert (IsVisibleUnlockedStoredEntry (0));
	assert (IsVisibleUnlockedStoredEntry (16));
	assert (!IsVisibleUnlockedStoredEntry (1));
	assert (!IsVisibleUnlockedStoredEntry (2));
	assert (!IsVisibleUnlockedStoredEntry (3));
	assert (IsExactNewOwnedLayerState (0, 0));
	assert (!IsExactNewOwnedLayerState (16, 0));
	assert (!IsExactNewOwnedLayerState (0, 9));
	assert (IsConsistentSparseCount (true, 4, 3, 1, 4, 4));
	assert (!IsConsistentSparseCount (true, 4, 3, 0, 4, 4));
	assert (!IsConsistentSparseCount (true, 4, 3, 1, 3, 4));
	assert (IsConsistentSparseCount (false, 2, 2, 0, 2, 77));
	assert (!IsConsistentSparseCount (false, 2, 1, 1, 2, 77));
	assert (!IsConsistentSparseCount (false, 3, 2, 0, 2, 77));
	assert (IsConsistentSparseCount (false, 0, 0, 0, 0, 0));
	return 0;
}
