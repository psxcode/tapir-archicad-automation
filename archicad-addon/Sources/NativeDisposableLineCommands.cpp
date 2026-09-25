#include "NativeDisposableLineCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_LINE)

#include "line-contract.hpp"

static_assert (tapir::disposable_channel::kNativeLineMaxItems == 2, "Offline Line candidate batch bound changed");

NativeDisposableLineContractCommand::NativeDisposableLineContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableLineContractCommand::GetName () const
{
    return "NativeDisposableLineContract";
}

GS::Optional<GS::UniString> NativeDisposableLineContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableLineContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-line-executor-v1"] },
            "readbackVersion": { "enum": ["native-line-readback-v2"] },
            "receiptVersion": { "enum": ["native-line-receipt-v2"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "readbackVersion", "receiptVersion", "liveVerified", "mutationDispatched", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposableLineContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // The offline packet has no authenticated native context and therefore
    // cannot dispatch CREATE, UPDATE or DELETE.  This command is an explicit
    // unavailable marker, never a mock mutation success.
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_channel::kNativeLineExecutorVersion);
    result.Add ("readbackVersion", tapir::disposable_channel::kNativeLineReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_channel::kNativeLineReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_LINE_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
