#pragma once

#include "CommandBase.hpp"

// This command exists only in the explicitly selected disposable channel
// compile profile.  It is a zero-write APX skeleton: it cannot create an
// authority from JSON, environment, argv, a port or copied identity data.
#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)
class NativeDisposableChannelProbeCommand : public CommandBase
{
public:
    NativeDisposableChannelProbeCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
#endif
