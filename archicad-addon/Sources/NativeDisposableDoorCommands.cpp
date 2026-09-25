#include "NativeDisposableDoorCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_DOOR)

#include "door-contract.hpp"

static_assert (tapir::disposable_door::kNativeDoorMaxItems == 1, "Offline Door operation bound changed");
static_assert (tapir::disposable_door::kNativeDoorMaxClosureElements == 256, "Offline Door closure bound changed");

NativeDisposableDoorContractCommand::NativeDisposableDoorContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableDoorContractCommand::GetName () const
{
    return "NativeDisposableDoorContract";
}

GS::Optional<GS::UniString> NativeDisposableDoorContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableDoorContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-door-executor-v1"] },
            "candidateVersion": { "enum": ["native-door-candidate-v1"] },
            "readbackVersion": { "enum": ["native-door-readback-v1"] },
            "receiptVersion": { "enum": ["native-door-receipt-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "retryAllowed": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "source": { "enum": ["offline"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "candidateVersion", "readbackVersion", "receiptVersion", "liveVerified", "mutationDispatched", "retryAllowed", "authority", "source", "reason"]
    })";
}

GS::ObjectState NativeDisposableDoorContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_door::kNativeDoorExecutorVersion);
    result.Add ("candidateVersion", tapir::disposable_door::kNativeDoorCandidateContractVersion);
    result.Add ("readbackVersion", tapir::disposable_door::kNativeDoorReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_door::kNativeDoorReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("retryAllowed", false);
    result.Add ("authority", "none");
    result.Add ("source", "offline");
    result.Add ("reason", "NATIVE_DISPOSABLE_DOOR_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
