#include "NativeDisposableArcCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_ARC)

#include "arc-contract.hpp"

static_assert (tapir::disposable_channel::kNativeArcMaxItems == 2, "Offline Arc candidate batch bound changed");

NativeDisposableArcContractCommand::NativeDisposableArcContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableArcContractCommand::GetName () const
{
    return "NativeDisposableArcContract";
}

GS::Optional<GS::UniString> NativeDisposableArcContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableArcContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-arc-executor-v1"] },
            "candidateVersion": { "enum": ["native-arc-candidate-v1"] },
            "readbackVersion": { "enum": ["native-arc-readback-v1"] },
            "receiptVersion": { "enum": ["native-arc-receipt-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "candidateVersion", "readbackVersion", "receiptVersion", "liveVerified", "mutationDispatched", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposableArcContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // This profile has no authenticated native context and therefore cannot
    // dispatch CREATE, UPDATE or DELETE.  It is an explicit unavailable
    // marker, never a mock mutation success.
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_channel::kNativeArcExecutorVersion);
    result.Add ("candidateVersion", tapir::disposable_channel::kNativeArcCandidateContractVersion);
    result.Add ("readbackVersion", tapir::disposable_channel::kNativeArcReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_channel::kNativeArcReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_ARC_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
