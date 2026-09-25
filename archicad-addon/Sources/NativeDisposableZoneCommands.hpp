#pragma once

#include "CommandBase.hpp"

// The isolated Zone profile is a compile-only unavailable marker.  It has no
// ordinary Zone writer, generic mutation/delete route, permit, broker or live
// authority.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)
class NativeDisposableZoneContractCommand : public CommandBase
{
public:
    NativeDisposableZoneContractCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
