#pragma once

namespace BoundedRectangleReadDecision {

enum class SelectionReadAction {
	CollectRows,
	VerifyFocusAfterSdkError,
	VerifyFocusAndRestore
};

inline constexpr SelectionReadAction ClassifySelectionRead (
	bool readSucceeded,
	bool noSelection,
	bool pageRequired)
{
	if (readSucceeded && !pageRequired) return SelectionReadAction::CollectRows;
	if (!readSucceeded && !noSelection) return SelectionReadAction::VerifyFocusAfterSdkError;
	return SelectionReadAction::VerifyFocusAndRestore;
}

enum class FinalAction {
	FocusError,
	RestoreError,
	PageRequired,
	ReturnRows
};

inline constexpr FinalAction ClassifyFinalRead (
	bool focusVerifiedUnchanged,
	bool focusRestored,
	bool pageRequired)
{
	if (!focusRestored) return FinalAction::RestoreError;
	if (!focusVerifiedUnchanged) return FinalAction::FocusError;
	if (pageRequired) return FinalAction::PageRequired;
	return FinalAction::ReturnRows;
}

} // namespace BoundedRectangleReadDecision
