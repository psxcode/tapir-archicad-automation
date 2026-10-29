#pragma once

#include "ACAPinc.h"

#include <functional>

// D-357: one Native write call is one Archicad undo step.  Every undoable
// scope reachable from the Native CRUD path goes through this helper.  Outside
// a Native transaction it is exactly ACAPI_CallUndoableCommand.  Inside one
// (depth > 0) it runs the callback directly in the already open undo scope and
// returns the callback's own error code, so a nested failure propagates to the
// outer callback and rolls the whole transaction back.  Nested
// ACAPI_CallUndoableCommand calls from the same add-on are not documented by
// the DevKit; the counter keeps the transaction to exactly one host scope.
GSErrCode NativeUndoableCommand (const GS::UniString& undoName, const std::function<GSErrCode ()>& fn);

// True while a NativeUndoableCommand scope is open on this (main) thread.
bool NativeUndoableScopeIsOpen ();

// Work that publishes committed state (element New notifications).  With no
// open scope the action runs at once.  Inside an open scope it is queued and
// runs only after the outermost undo step returned NoError; a rolled-back
// step discards the queue, so nothing stale is ever published.
void NativeUndoableAfterCommit (const std::function<void ()>& action);
