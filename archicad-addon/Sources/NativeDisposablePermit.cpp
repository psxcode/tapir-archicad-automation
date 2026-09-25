#include "NativeDisposablePermit.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_PERMIT)

#include "permit.hpp"

NativeDisposablePermitContractCommand::NativeDisposablePermitContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposablePermitContractCommand::GetName () const
{
    return "NativeDisposablePermitContract";
}

GS::Optional<GS::UniString> NativeDisposablePermitContractCommand::GetInputParametersSchema () const
{
    // No JSON/HTTP/argv/environment field can create permit authority.  The
    // authenticated inherited channel and the native PermitVerifier remain a
    // separate future adapter boundary.
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposablePermitContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "bridgeVersion": { "enum": ["native-disposable-permit-bridge-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "bridgeVersion", "liveVerified", "mutationDispatched", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposablePermitContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // This command is intentionally an explicit unavailable marker for the
    // compile-only APX profile.  It does not consume a permit and cannot
    // dispatch NativeCrudCommands or any Column writer.
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("bridgeVersion", tapir::disposable_channel::kPermitBridgeVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_PERMIT_CHANNEL_NOT_CONFIGURED");
    return result;
}

#endif

