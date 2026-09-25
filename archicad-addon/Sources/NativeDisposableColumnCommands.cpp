#include "NativeDisposableColumnCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_COLUMN)

#include "column.hpp"

static_assert (tapir::disposable_channel::kDisposableColumnMaxItems == 2, "Packet 9B batch bound changed");

NativeDisposableColumnContractCommand::NativeDisposableColumnContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableColumnContractCommand::GetName () const
{
    return "NativeDisposableColumnContract";
}

GS::Optional<GS::UniString> NativeDisposableColumnContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableColumnContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-column-executor-v1"] },
            "receiptVersion": { "enum": ["native-disposable-column-receipt-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "receiptVersion", "liveVerified", "mutationDispatched", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposableColumnContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // No authenticated APX/native context is proven in Packet 9B.  This is
    // deliberately not a mock success and never calls the ordinary mutation
    // or broad native-writer route.
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_channel::kDisposableColumnExecutorVersion);
    result.Add ("receiptVersion", tapir::disposable_channel::kDisposableColumnReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_COLUMN_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
