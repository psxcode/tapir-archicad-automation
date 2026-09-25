#pragma once

#include "CommandBase.hpp"

// The isolated Line profile is a compile-only unavailable marker.  It has no
// DevKit writer, permit bridge, broker route or ordinary element registration.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)
class NativeDisposableLineContractCommand : public CommandBase
{
public:
    NativeDisposableLineContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif

