#include "NativeDisposableZoneCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_ZONE)

#include "zone-contract.hpp"

static_assert (tapir::disposable_channel::kNativeZoneMaxItems == 1, "Offline Zone operation bound changed");
static_assert (tapir::disposable_channel::kNativeZoneMaxVertices == 64, "Offline Zone vertex bound changed");

NativeDisposableZoneContractCommand::NativeDisposableZoneContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableZoneContractCommand::GetName () const
{
    return "NativeDisposableZoneContract";
}

GS::Optional<GS::UniString> NativeDisposableZoneContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableZoneContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-zone-executor-v1"] },
            "candidateVersion": { "enum": ["native-zone-candidate-v1"] },
            "readbackVersion": { "enum": ["native-zone-readback-v1"] },
            "receiptVersion": { "enum": ["native-zone-receipt-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "retryAllowed": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "candidateVersion", "readbackVersion", "receiptVersion", "liveVerified", "mutationDispatched", "retryAllowed", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposableZoneContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_channel::kNativeZoneExecutorVersion);
    result.Add ("candidateVersion", tapir::disposable_channel::kNativeZoneCandidateContractVersion);
    result.Add ("readbackVersion", tapir::disposable_channel::kNativeZoneReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_channel::kNativeZoneReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("retryAllowed", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_ZONE_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
