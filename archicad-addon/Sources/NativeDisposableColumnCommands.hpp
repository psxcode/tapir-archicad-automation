#pragma once

#include "CommandBase.hpp"

// Packet 9B's APX-side unit is an isolated compile-only marker.  It has no
// live DevKit context yet, so it cannot reach ordinary CRUD dispatch or any
// Column writer.  A future authenticated adapter must bind the narrow
// executor to an explicitly injected native context before this can do more.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)
class NativeDisposableColumnContractCommand : public CommandBase
{
public:
    NativeDisposableColumnContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
