#pragma once

#include "CommandBase.hpp"

// The isolated Arc profile is a compile-only unavailable marker.  It has no
// DevKit writer, permit bridge, broker route, ordinary element registration,
// generic mutation/delete route or legacy attribute-writer registration.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)
class NativeDisposableArcContractCommand : public CommandBase
{
public:
    NativeDisposableArcContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
