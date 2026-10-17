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

	if (passed) std::cout << "Bounded rectangle read decision regression passed.\n";
	return passed ? 0 : 1;
}
