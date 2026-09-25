#pragma once

#include "CommandBase.hpp"

// The isolated Hatch profile is a compile-only unavailable marker.  It has
// no ordinary Hatch writer, generic mutation/delete route, permit, broker
// dispatch or legacy CreateHatches registration.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)
class NativeDisposableHatchContractCommand : public CommandBase
{
public:
    NativeDisposableHatchContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
