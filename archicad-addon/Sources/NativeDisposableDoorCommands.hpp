#pragma once

#include "CommandBase.hpp"

// The isolated Door profile is a compile-only unavailable marker.  It has no
// ordinary Door/Window writer, generic mutation/delete route, permit, broker
// dispatch or favorite/default selector input.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)
class NativeDisposableDoorContractCommand : public CommandBase
{
public:
    NativeDisposableDoorContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
