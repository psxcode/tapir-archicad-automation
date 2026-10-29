#include "NativeUndoable.hpp"

#include <vector>

namespace {

// Archicad calls add-on commands on its main thread; the depth and the
// deferred queue are only ever touched there.  Plain state, not a lock.
int nativeUndoableDepth = 0;
std::vector<std::function<void ()>> afterCommitActions;

class DepthGuard final {
public:
    DepthGuard () { ++nativeUndoableDepth; }
    ~DepthGuard () { --nativeUndoableDepth; }
    DepthGuard (const DepthGuard&) = delete;
    DepthGuard& operator= (const DepthGuard&) = delete;
};

} // namespace

GSErrCode NativeUndoableCommand (const GS::UniString& undoName, const std::function<GSErrCode ()>& fn)
{
    if (nativeUndoableDepth > 0)
        return fn ();
    afterCommitActions.clear ();
    const GSErrCode error = ACAPI_CallUndoableCommand (undoName, [&fn] () -> GSErrCode {
        DepthGuard guard;
        return fn ();
    });
    std::vector<std::function<void ()>> actions;
    actions.swap (afterCommitActions);
    if (error == NoError) {
        for (const std::function<void ()>& action : actions)
            action ();
    }
    return error;
}

bool NativeUndoableScopeIsOpen ()
{
    return nativeUndoableDepth > 0;
}

void NativeUndoableAfterCommit (const std::function<void ()>& action)
{
    if (nativeUndoableDepth > 0)
        afterCommitActions.push_back (action);
    else
        action ();
}
