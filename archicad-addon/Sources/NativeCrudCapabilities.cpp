#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"
#include "NativeCrudSchema.generated.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

using namespace NativeCrudSchema;
using namespace NativeCrudInternal;

namespace {

constexpr const char* CapabilitySchemaVersion = "native-crud-capabilities-v2";

constexpr const char* CapabilitySchemaV3Version = "native-crud-capabilities-v3";

void AddCanonicalWritableFields (GS::ObjectState& target, const GS::UniString& typeName)
{
    const TypeFieldLists* lists = nullptr;
    for (const TypeFieldLists& row : TypeFields)
        if (typeName == row.typeName) lists = &row;
    const auto& writableFields = target.AddList<GS::UniString> ("writableFields");
    const auto& createFieldList = target.AddList<GS::UniString> ("createFields");
    const auto& createPlacementFields = target.AddList<GS::UniString> ("createPlacementFields");
    const auto& updateFields = target.AddList<GS::UniString> ("updateFields");
    for (GSSize i = 0; i < CreatePlacementFieldCount; ++i) createPlacementFields (CreatePlacementFields[i]);
    if (lists == nullptr) return;
    for (GSSize i = 0; i < lists->writable.count; ++i) writableFields (lists->writable.names[i]);
    for (GSSize i = 0; i < lists->create.count; ++i) createFieldList (lists->create.names[i]);
    for (GSSize i = 0; i < lists->update.count; ++i) updateFields (lists->update.names[i]);
}

} // namespace

GetNativeCrudCapabilitiesCommand::GetNativeCrudCapabilitiesCommand () : CommandBase (CommonSchema::NotUsed) {}

GS::String GetNativeCrudCapabilitiesCommand::GetName () const { return "GetNativeCrudCapabilities"; }

GS::Optional<GS::UniString> GetNativeCrudCapabilitiesCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"}},"additionalProperties":false,"required":["contractVersion"]})";
}

GS::Optional<GS::UniString> GetNativeCrudCapabilitiesCommand::GetResponseSchema () const { return {}; }

GS::ObjectState GetNativeCrudCapabilitiesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("snapshotVersion", SnapshotVersion);
    response.Add ("receiptVersion", ReceiptVersion);
    response.Add ("historyVersion", HistoryVersion);
    response.Add ("capabilityIdentity", GS::ObjectState (
        "schemaVersion", CapabilitySchemaV3Version,
        "schemaHash", CapabilitySchemaV3Hash,
        "source", "apx"));
    response.Add ("fileScope", "saved Solo PLN canonical path");
    response.Add ("limits", GS::ObjectState (
        "maxItems", static_cast<Int32> (MaxMutationItems),
        "maxDiscoverReferences", static_cast<Int32> (MaxDiscoveryReferences),
        "maxHistoryEntries", static_cast<Int32> (NativeCrudInternal::MaxHistoryEntries),
        "impactClosure", static_cast<Int32> (MaxImpactElements)));
    const auto& elementTypes = response.AddList<GS::ObjectState> ("elementTypes");
    for (const TypeCapabilities& row : CrudTypes) {
        const char* typeName = row.typeName;
        GS::ObjectState type;
        type.Add ("elementType", typeName);
        type.Add ("contracted", GS::ObjectState (
            "create", true,
            "read", true,
            "update", true,
            "delete", true));
        type.Add ("read", true);
        // The verified writer bits come from the typed schema (NATIVE_VERIFIED_CAPABILITY_BITS).
        const bool validatedWriter = row.create;
        const bool updateValidatedWriter = row.update;
        type.Add ("create", validatedWriter);
        type.Add ("update", updateValidatedWriter);
        type.Add ("delete", true);
        type.Add ("writerStatus", validatedWriter ? "validated" : "read_only_until_memo_writer_gate");
        type.Add ("capabilityState", GS::ObjectState (
            "create", validatedWriter ? "verified" : "unavailable",
            "read", "verified",
            "update", updateValidatedWriter ? "verified" : "unavailable",
            "delete", "verified"));
        AddCanonicalWritableFields (type, GS::UniString (typeName));
        elementTypes (type);
    }
    for (const char* typeName : { "Dimension", "RadialDimension", "AngleDimension", "LevelDimension" }) {
        GS::ObjectState type;
        type.Add ("elementType", typeName);
        type.Add ("contracted", GS::ObjectState ("create", false, "read", true, "update", false, "delete", false));
        type.Add ("create", false);
        type.Add ("read", true);
        type.Add ("update", false);
        type.Add ("delete", false);
        type.Add ("writerStatus", "read_only_dimension");
        type.Add ("capabilityState", GS::ObjectState (
            "create", "unavailable", "read", "verified", "update", "unavailable", "delete", "unavailable"));
        type.Add ("writableFields", GS::Array<GS::UniString> {});
        type.Add ("createFields", GS::Array<GS::UniString> {});
        type.Add ("createPlacementFields", GS::Array<GS::UniString> {});
        type.Add ("updateFields", GS::Array<GS::UniString> {});
        elementTypes (type);
    }
    response.Add ("safety", GS::ObjectState (
        "previewRequired", false,
        "singleStepAtomicPatch", true,
        "expectedOldValues", true,
        "authorizationRequired", true,
        "authorizationMode", "server-configured",
        "atomicBatch", true,
        "sparseUpdate", true,
        "noRestoreReplayApi", true,
        "unknownOutcomeNoRetry", true));
    return response;
}
