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

enum class CandidateHeaderAction {
	Inspect,
	ReportUnresolved,
	RefuseTooManyUnresolved,
	Fail
};

// D-288 in bounded discovery: a marquee reference whose header Archicad does
// not resolve (APIERR_BADID) is reported, up to a fixed bound, instead of
// failing the read. Every other header error still fails the read.
inline constexpr CandidateHeaderAction ClassifyCandidateHeader (
	bool headerRead,
	bool headerNotFound,
	unsigned long long unresolvedSoFar,
	unsigned long long unresolvedLimit)
{
	if (headerRead) return CandidateHeaderAction::Inspect;
	if (!headerNotFound) return CandidateHeaderAction::Fail;
	if (unresolvedSoFar >= unresolvedLimit) return CandidateHeaderAction::RefuseTooManyUnresolved;
	return CandidateHeaderAction::ReportUnresolved;
}

// D-321: bounded discovery keeps an enumerated element of the requested type
// and story when its native bounds touch or overlap the requested rectangle.
inline constexpr bool DiscoveryBoundsIntersect (
	double minX, double minY, double maxX, double maxY,
	double queryMinX, double queryMinY, double queryMaxX, double queryMaxY)
{
	return maxX >= queryMinX && minX <= queryMaxX && maxY >= queryMinY && minY <= queryMaxY;
}

} // namespace BoundedRectangleReadDecision
