#include "NativeDisposableWindowCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_WINDOW)

#include "window-contract.hpp"

static_assert (tapir::disposable_window::kNativeWindowMaxItems == 1, "Offline Window operation bound changed");
static_assert (tapir::disposable_window::kNativeWindowMaxClosureElements == 256, "Offline Window closure bound changed");

NativeDisposableWindowContractCommand::NativeDisposableWindowContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableWindowContractCommand::GetName () const
{
    return "NativeDisposableWindowContract";
}

GS::Optional<GS::UniString> NativeDisposableWindowContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableWindowContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-window-executor-v1"] },
            "candidateVersion": { "enum": ["native-window-candidate-v1"] },
            "readbackVersion": { "enum": ["native-window-readback-v1"] },
            "receiptVersion": { "enum": ["native-window-receipt-v1"] },
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

GS::ObjectState NativeDisposableWindowContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_window::kNativeWindowExecutorVersion);
    result.Add ("candidateVersion", tapir::disposable_window::kNativeWindowCandidateContractVersion);
    result.Add ("readbackVersion", tapir::disposable_window::kNativeWindowReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_window::kNativeWindowReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("retryAllowed", false);
    result.Add ("authority", "none");
    result.Add ("source", "offline");
    result.Add ("reason", "NATIVE_DISPOSABLE_WINDOW_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
