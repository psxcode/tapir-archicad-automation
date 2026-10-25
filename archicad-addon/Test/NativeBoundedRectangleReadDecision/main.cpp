#include "BoundedRectangleReadDecision.hpp"

#include <iostream>

namespace {

bool Require (bool condition, const char* message)
{
	if (!condition) std::cerr << message << '\n';
	return condition;
}

}

int main ()
{
	using namespace BoundedRectangleReadDecision;
	bool passed = true;

	passed &= Require (
		ClassifySelectionRead (true, false, false) == SelectionReadAction::CollectRows,
		"A successful bounded read below the limit should collect rows.");
	passed &= Require (
		ClassifySelectionRead (true, false, true) == SelectionReadAction::VerifyFocusAndRestore,
		"A successful over-limit bounded read must verify focus and restore before refusing the page.");
	passed &= Require (
		ClassifySelectionRead (false, false, false) == SelectionReadAction::VerifyFocusAfterSdkError,
		"A genuine SDK failure must retain the safe error-recovery path.");
	passed &= Require (
		ClassifySelectionRead (false, true, false) == SelectionReadAction::VerifyFocusAndRestore,
		"An empty selection must retain the safe restore path.");
	passed &= Require (
		ClassifyFinalRead (false, true, true) == FinalAction::FocusError,
		"Changed or unreadable focus must take precedence over page refusal.");
	passed &= Require (
		ClassifyFinalRead (false, false, true) == FinalAction::RestoreError,
		"Failed restoration after a changed or unreadable focus must remain explicit.");
	passed &= Require (
		ClassifyFinalRead (true, false, true) == FinalAction::RestoreError,
		"A failed focus restoration must take precedence over page refusal.");
	passed &= Require (
		ClassifyFinalRead (true, true, true) == FinalAction::PageRequired,
		"A verified, restored over-limit read must return the explicit page-required outcome.");
	passed &= Require (
		ClassifyFinalRead (true, true, false) == FinalAction::ReturnRows,
		"A verified, restored in-limit read may return rows.");

	passed &= Require (
		ClassifyCandidateHeader (true, false, 64, 64) == CandidateHeaderAction::Inspect,
		"A readable candidate header is inspected regardless of unresolved references.");
	passed &= Require (
		ClassifyCandidateHeader (false, true, 0, 64) == CandidateHeaderAction::ReportUnresolved,
		"An unresolved (not found) header is reported, not fatal.");
	passed &= Require (
		ClassifyCandidateHeader (false, true, 63, 64) == CandidateHeaderAction::ReportUnresolved,
		"The 64th unresolved reference is still reported.");
	passed &= Require (
		ClassifyCandidateHeader (false, true, 64, 64) == CandidateHeaderAction::RefuseTooManyUnresolved,
		"The 65th unresolved reference refuses the read.");
	passed &= Require (
		ClassifyCandidateHeader (false, false, 0, 64) == CandidateHeaderAction::Fail,
		"Any other header error still fails the read.");

	if (passed) std::cout << "Bounded rectangle read decision regression passed.\n";
	return passed ? 0 : 1;
}
