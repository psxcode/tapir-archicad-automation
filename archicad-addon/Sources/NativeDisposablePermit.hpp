#pragma once

#include "CommandBase.hpp"

// Packet 9A's APX-side unit is a compile-only contract marker.  It is kept
// behind its own isolated profile and deliberately has no Column handler,
// DevKit mutation call, Save/SaveAs path or ordinary CRUD dispatch.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)
class NativeDisposablePermitContractCommand : public CommandBase
{
public:
    NativeDisposablePermitContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif

