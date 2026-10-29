#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementMutationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeUndoable.hpp"
#include "NotificationCommands.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"
#include "NativeDimensionCreateWire.hpp"
#include "NativeDimensionCreateWireObjectState.hpp"

using namespace NativeCrudInternal;

namespace {

// The factual result of one typed Native mutation before its receipt is
// published.  MutateNativeElements publishes it at once; a Native transaction
// (D-357) publishes every operation only after the one undo step committed,
// so a rolled-back operation never reaches the process-local receipt cache.
struct NativeMutationPending {
    bool ready = false;
    GS::ObjectState before;
    GS::ObjectState receipt;
    GS::ObjectState mutationResult;
    GS::ObjectState readback;
    GS::ObjectState deleteReadback;
    bool isDelete = false;
};

GS::ObjectState BuildNativeMutationResponse (const NativeMutationPending& outcome)
{
    GS::ObjectState response;
    bool groupModeRestored = false;
    if (outcome.mutationResult.Get ("groupModeRestored", groupModeRestored))
        response.Add ("groupModeRestored", groupModeRestored);
    Int32 groupModeRestoreError = 0;
    if (outcome.mutationResult.Get ("groupModeRestoreError", groupModeRestoreError))
        response.Add ("groupModeRestoreError", groupModeRestoreError);
    // BuildReceipt deliberately has no durable side effect.  Give the APX
    // response a process-local sequence for diagnostics and remember the
    // factual receipt before returning it so MCP can reconcile a lost HTTP
    // response without dispatching the Archicad command a second time.
    GS::ObjectState receipt = outcome.receipt;
    receipt.Add ("nativeSequence", NextCachedReceiptSequence ());
    RememberCachedReceipt (receipt);
    response.Add ("contractVersion", ContractVersion);
    response.Add ("status", "success");
    response.Add ("receipt", receipt);
    response.Add ("mutationResult", outcome.mutationResult);
    response.Add ("readback", outcome.readback);
    if (outcome.isDelete) response.Add ("deleteReadback", outcome.deleteReadback);
    return response;
}

GS::ObjectState ExecuteZoneMemoUpdate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl,
    bool allowAutomaticZone)
{
    // Reuse the existing SetDetails transaction so the candidate remains on
    // the same undoable native command and receipt/readback path as the
    // validated CRUD writers.  The SetDetails command receives only the
    // internal `geometry` object; no Zone stamp or scalar field is admitted.
    const GS::ObjectState nativeParameters = BuildNativeMutationParameters ("update", "Zone", items);
    GS::Array<GS::ObjectState> mappedItems;
    if (!nativeParameters.Get ("items", mappedItems) || mappedItems.GetSize () != 1)
        return CreateErrorResponse (APIERR_BADPARS, "The disposable Zone memo writer could not map its polygon patch.");
    const GS::ObjectState* mappedPayload = mappedItems[0].Get ("payload");
    const GS::ObjectState* elementId = mappedItems[0].Get ("elementId");
    if (mappedPayload == nullptr || elementId == nullptr || !mappedPayload->Contains ("geometry")
        || !HasOnlyObjectStateFields (*mappedPayload, { "polygonCoordinates", "polygonArcs", "holes", "geometry" }))
        return CreateErrorResponse (APIERR_BADPARS, "The disposable Zone memo writer mapped a non-polygon field.");

    GS::ObjectState setParameters;
    setParameters.Add ("nativeCrudZoneWriter", true);
    // D-357: Native CRUD writes are all-or-nothing.
    setParameters.Add ("nativeCrudAtomic", true);
    const auto& elementsWithDetails = setParameters.AddList<GS::ObjectState> ("elementsWithDetails");
    GS::ObjectState details;
    GS::ObjectState typeSpecificDetails = *mappedPayload;
    if (allowAutomaticZone) {
        if (!typeSpecificDetails.Contains ("geometry"))
            return CreateErrorResponse (APIERR_BADPARS, "The automatic Zone polygon probe requires a geometry object.");
        // Keep the probe marker outside the nested geometry object.  The
        // mapped payload already owns that key and GS::ObjectState::Add does
        // not replace an existing key; putting the marker at the details
        // level avoids a silent duplicate-key no-op before SetDetails reads
        // it.  This key is accepted only on the internal developer gate.
        typeSpecificDetails.Add ("allowAutomaticZonePolygonProbe", true);
    }
    details.Add ("typeSpecificDetails", typeSpecificDetails);
    elementsWithDetails (GS::ObjectState (
        "elementId", *elementId,
        "details", details));
    GS::ObjectState result = SetDetailsOfElementsCommand ().Execute (setParameters, processControl);

    GS::Array<GS::ObjectState> executionResults;
    bool complete = false;
    if (result.Get ("executionResults", executionResults) && executionResults.GetSize () == 1) {
        bool success = false;
        complete = executionResults[0].Get ("success", success) && success;
    }
    result.Add ("mutationComplete", complete);
    result.Add ("partial", !complete);
    return result;
}

GS::ObjectState ExecuteZoneTypedUpdate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl,
    const bool faultAfterFirstApplied)
{
    const GS::ObjectState nativeParameters = BuildNativeMutationParameters ("update", "Zone", items);
    GS::Array<GS::ObjectState> mappedItems;
    if (!nativeParameters.Get ("items", mappedItems) || mappedItems.GetSize () != items.GetSize ())
        return CreateErrorResponse (APIERR_BADPARS, "Zone typed UPDATE could not map its exact items.");

    GS::ObjectState setParameters;
    setParameters.Add ("nativeCrudAtomic", true);
    setParameters.Add ("nativeCrudZoneWriter", true);
    if (faultAfterFirstApplied) setParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
    const auto& elementsWithDetails = setParameters.AddList<GS::ObjectState> ("elementsWithDetails");
    for (const GS::ObjectState& mappedItem : mappedItems) {
        const GS::ObjectState* elementId = mappedItem.Get ("elementId");
        const GS::ObjectState* mappedPayload = mappedItem.Get ("payload");
        if (elementId == nullptr || mappedPayload == nullptr)
            return CreateErrorResponse (APIERR_BADPARS, "Zone typed UPDATE is missing elementId or sparse payload.");
        GS::ObjectState details;
        GS::ObjectState typeSpecificDetails = *mappedPayload;
        Int32 floorIndex = 0;
        if (typeSpecificDetails.Get ("floorIndex", floorIndex)) {
            details.Add ("floorIndex", floorIndex);
        }
        // D-264 permits a direct persisted polygon write on automatic Zones only
        // for the memo helper's exact topology checks.  This remains a direct
        // write and does not request Archicad recalculation/regeneration.
        if (typeSpecificDetails.Contains ("geometry"))
            typeSpecificDetails.Add ("allowAutomaticZonePolygonProbe", true);
        details.Add ("typeSpecificDetails", typeSpecificDetails);
        elementsWithDetails (GS::ObjectState (
            "elementId", *elementId,
            "details", details));
    }
    GS::ObjectState result = SetDetailsOfElementsCommand ().Execute (setParameters, processControl);
    GS::Array<GS::ObjectState> executionResults;
    bool complete = false;
    if (result.Get ("executionResults", executionResults) && executionResults.GetSize () == items.GetSize ()) {
        complete = true;
        for (const GS::ObjectState& executionResult : executionResults) {
            bool success = false;
            complete = complete && executionResult.Get ("success", success) && success;
        }
    }
    result.Add ("mutationComplete", complete);
    result.Add ("partial", !complete);
    return result;
}

GS::ObjectState DispatchMutation (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl,
    const GS::ObjectState* semanticMetadata)
{
    (void) operationId;
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (typeName);
    if (typeID == API_DoorID && HasNativeCrudTestFaultMetadata (semanticMetadata))
        return CreateErrorResponse (APIERR_BADPARS, "Door test-fault metadata is unavailable to Native CRUD writes (D-307).");
    if (typeID == API_WindowID && HasNativeCrudTestFaultMetadata (semanticMetadata))
        return CreateErrorResponse (APIERR_BADPARS, "Window test-fault metadata is unavailable to Native CRUD writes (D-307).");
    if (typeID == API_HatchID && HasNativeCrudTestFaultMetadata (semanticMetadata))
        return CreateErrorResponse (APIERR_BADPARS, "Hatch test-fault metadata is unavailable to Native CRUD writes (D-307).");
    if (typeID == API_DoorID && (operation == "create" || operation == "update")) {
        // Fault metadata was rejected above; the ordinary request must still
        // use the narrow Door payload contract and an exact source template
        // (marker-bearing templates are admitted under D-357).
        const GS::Optional<GS::UniString> writerError = ValidateDoorTypedPayloads (operation, items);
        if (writerError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, writerError.Get ());
        if (operation == "create") {
            for (const GS::ObjectState& item : items) {
                const GS::ObjectState* source = item.Get ("cloneSourceElementId");
                if (source == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door CREATE requires an exact source template (cloneSourceElementId).");
                const GS::Optional<GS::UniString> cloneError = ValidateDoorCloneSource (GetGuidFromObjectState (*source));
                if (cloneError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, cloneError.Get ());
            }
        }
    }
    if (typeID == API_WindowID && (operation == "create" || operation == "update")) {
        const GS::Optional<GS::UniString> writerError = ValidateWindowTypedPayloads (operation, items);
        if (writerError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, writerError.Get ());
        if (operation == "create") {
            for (const GS::ObjectState& item : items) {
                const GS::ObjectState* source = item.Get ("cloneSourceElementId");
                if (source == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Window CREATE requires an exact source template (cloneSourceElementId).");
                const GS::Optional<GS::UniString> cloneError = ValidateWindowCloneSource (GetGuidFromObjectState (*source));
                if (cloneError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, cloneError.Get ());
            }
        }
    }
    // D-357: Door/Window DELETE no longer requires a markerless target; the
    // fresh snapshot and fingerprint checks in the Execute path still bind
    // every requested GUID to its exact type and state.
    if (operation == "delete") {
        // ACAPI_Element_Delete expands an exact target to its complete group
        // when Suspend Groups is off. Native DELETE promises the requested
        // exact GUID set, so temporarily suspend groups and restore the
        // operator's prior mode on every exit path.
        bool groupsWereSuspended = false;
        GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsWereSuspended);
        if (groupModeError != NoError)
            return CreateErrorResponse (groupModeError, "Failed to read the Suspend Groups mode before exact Native DELETE.");
        bool groupModeNeedsRestore = !groupsWereSuspended;
        if (groupModeNeedsRestore) {
            groupModeError = TAPIR_Grouping_ChangeSuspendGroup (true);
            if (groupModeError != NoError)
                return CreateErrorResponse (groupModeError, "Failed to suspend groups for exact Native DELETE.");
        }
        const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
            if (groupModeNeedsRestore)
                TAPIR_Grouping_ChangeSuspendGroup (false);
        });

        GS::ObjectState mutationResult = DeleteElementsCommand ().Execute ([&]() {
            GS::ObjectState parameters;
            const auto& elements = parameters.AddList<GS::ObjectState> ("elements");
            for (const GS::ObjectState& item : items) {
                const GS::ObjectState* id = item.Get ("elementId");
                // DeleteElements expects each array row to retain the public
                // `{ elementId: { guid } }` envelope.  Passing only the
                // nested `{ guid }` object made GetGuidFromArrayItem return
                // APINULLGuid and surfaced as APIERR_BADID, even though the
                // exact target had just passed native snapshot validation.
                if (id != nullptr) elements (item);
            }
            return parameters;
        } (), processControl);

        // The DELETE may already be applied. Restore only the UI group mode;
        // never retry the element command if restoration fails.
        GSErrCode groupModeRestoreError = NoError;
        if (groupModeNeedsRestore) {
            groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (false);
            if (groupModeRestoreError == NoError)
                groupModeNeedsRestore = false;
        }
        mutationResult.Add ("groupModeRestored", groupModeRestoreError == NoError);
        if (groupModeRestoreError != NoError)
            mutationResult.Add ("groupModeRestoreError", groupModeRestoreError);
        return mutationResult;
    }
    // Reuse existing undoable typed routes for verified types and the
    // Door/Window template writers.
    if (typeID == API_WallID || typeID == API_ColumnID || typeID == API_TextID || typeID == API_DoorID || typeID == API_WindowID ||
        typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID) {
        GS::ObjectState nativeParameters = BuildNativeMutationParameters (operation, typeName, items);
        nativeParameters.Add ("elementType", typeName);
        // D-357: every Native CRUD batch is all-or-nothing.
        nativeParameters.Add ("nativeCrudAtomic", true);
        if (typeID == API_HatchID && (operation == "create" || operation == "update"))
            nativeParameters.Add ("nativeCrudHatchWriter", true);
        if (typeID == API_DoorID && (operation == "create" || operation == "update"))
            nativeParameters.Add ("nativeCrudDoorWriter", true);
        if (typeID == API_WindowID && (operation == "create" || operation == "update"))
            nativeParameters.Add ("nativeCrudWindowWriter", true);
        bool testFaultRequested = false;
        if ((typeID == API_ColumnID || typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID || typeID == API_DoorID || typeID == API_WindowID) && semanticMetadata != nullptr
            && semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", testFaultRequested)
            && testFaultRequested)
            nativeParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
        const GS::ObjectState nativeEnvelope = MutateElementsCommand ().Execute (nativeParameters, processControl);
        // MutateElements is itself an envelope: its actual typed command
        // result lives under `mutationResult`, while transaction/readback
        // status is kept beside it.  Native CRUD consumes the typed result
        // (`elements` for CREATE, `executionResults` for UPDATE) directly.
        // Returning the outer envelope here made every successful CREATE look
        // like it returned no GUIDs and therefore produced
        // `incomplete_evidence` receipts.  Flatten only the known control
        // fields; preserve error responses and direct DELETE responses as-is.
        const GS::ObjectState* typedResult = nativeEnvelope.Get ("mutationResult");
        if (typedResult == nullptr)
            return nativeEnvelope;
        GS::ObjectState normalized = *typedResult;
        bool mutationComplete = false;
        if (nativeEnvelope.Get ("mutationComplete", mutationComplete))
            normalized.Add ("mutationComplete", mutationComplete);
        bool readbackVerified = false;
        if (nativeEnvelope.Get ("readbackVerified", readbackVerified))
            normalized.Add ("readbackVerified", readbackVerified);
        bool partial = false;
        if (nativeEnvelope.Get ("partial", partial))
            normalized.Add ("partial", partial);
        bool transactionRolledBack = false;
        if (nativeEnvelope.Get ("transactionRolledBack", transactionRolledBack))
            normalized.Add ("transactionRolledBack", transactionRolledBack);
        Int32 transactionError = 0;
        if (nativeEnvelope.Get ("transactionError", transactionError))
            normalized.Add ("transactionError", transactionError);
        bool groupModeRestored = false;
        if (typedResult->Get ("groupModeRestored", groupModeRestored))
            normalized.Add ("groupModeRestored", groupModeRestored);
        Int32 groupModeRestoreError = 0;
        if (typedResult->Get ("groupModeRestoreError", groupModeRestoreError))
            normalized.Add ("groupModeRestoreError", groupModeRestoreError);
        const GS::ObjectState* testFaultEvidence = typedResult->Get ("testFaultEvidence");
        if (testFaultEvidence != nullptr)
            normalized.Add ("testFaultEvidence", *testFaultEvidence);
        return normalized;
    }
    if (typeID == API_ZoneID && HasAnyZoneMemoWriterGate (semanticMetadata)) {
        const GS::Optional<GS::UniString> candidateError = ValidateZoneMemoWriterCandidate (operation, items, semanticMetadata);
        if (candidateError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
        return ExecuteZoneMemoUpdate (items, processControl, HasAutomaticZonePolygonProbeGate (semanticMetadata));
    }
    if (typeID == API_ZoneID && (operation == "create" || operation == "update")) {
        // The proven fields are ordinary internal Native CRUD capability on
        // AC25/AC28; one typed executor serves CREATE and UPDATE.
        if (operation == "create") {
            GS::ObjectState nativeParameters = BuildNativeMutationParameters (operation, typeName, items);
            GS::Array<GS::ObjectState> mappedItems;
            if (!nativeParameters.Get ("items", mappedItems) || mappedItems.GetSize () != items.GetSize ())
                return CreateErrorResponse (APIERR_BADPARS, "Zone typed CREATE could not map its exact items.");
            GS::ObjectState createParameters;
            createParameters.Add ("nativeCrudAtomic", true);
            const auto& zoneData = createParameters.AddList<GS::ObjectState> ("zonesData");
            for (const GS::ObjectState& item : mappedItems) {
                const GS::ObjectState* payload = item.Get ("payload");
                if (payload == nullptr)
                    return CreateErrorResponse (APIERR_BADPARS, "Zone typed CREATE is missing its sparse payload.");
                zoneData (*payload);
            }
            GS::ObjectState result = CreateZonesCommand ().Execute (createParameters, processControl);
            GS::Array<GS::ObjectState> created;
            bool complete = result.Get ("elements", created) && created.GetSize () == items.GetSize ();
            if (complete) {
                for (const GS::ObjectState& row : created) {
                    const GS::ObjectState* id = row.Get ("elementId");
                    complete = complete && id != nullptr && GetGuidFromObjectState (*id) != APINULLGuid;
                }
            }
            bool rolledBack = false;
            result.Get ("transactionRolledBack", rolledBack);
            complete = complete && !rolledBack && result.Get ("error") == nullptr;
            result.Add ("mutationComplete", complete);
            result.Add ("partial", !complete);
            return result;
        }
        // Request metadata never injects a Zone first-applied fault: Execute
        // rejects that key for Zone before dispatch.
        return ExecuteZoneTypedUpdate (items, processControl, false);
    }
    return CreateErrorResponse (APIERR_BADPARS, "This element type has read snapshots but no validated native memo writer in this APX build.");
}

} // namespace

MutateNativeElementsCommand::MutateNativeElementsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String MutateNativeElementsCommand::GetName () const { return "MutateNativeElements"; }
GS::Optional<GS::UniString> MutateNativeElementsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"operationId":{"type":"string"},"expectedFileScopeKey":{"type":"string"},"requestHash":{"type":"string"},"operation":{"type":"string","enum":["create","update","delete"]},"elementType":{"type":"string"},"items":{"type":"array","minItems":1,"maxItems":500,"items":{"type":"object"}},"origin":{"type":"string","enum":["NATIVE","SEMANTIC"]},"description":{"type":"string"},"operatorNote":{"type":"string","maxLength":1000},"semanticMetadata":{"type":"object"}},"additionalProperties":false,"required":["contractVersion","operationId","expectedFileScopeKey","requestHash","operation","elementType","items","description"]})";
}
GS::Optional<GS::UniString> MutateNativeElementsCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> MutateNativeElementsCommand::GetRawResponseSchema () const { return {}; }
namespace {

// One typed Native mutation (the former MutateNativeElements body).  With
// `pending == nullptr` the receipt is published and the full response is
// returned.  Inside a Native transaction `pending` receives the unpublished
// factual result and the call returns `{ status: "pending" }` or an error.
GS::ObjectState ExecuteNativeMutationRequest (
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl,
    NativeMutationPending* pending)
{
    // D-307: the candidate writer vocabulary is retired. Forged candidate
    // metadata fails closed before any read, write or Dimension routing.
    if (const GS::ObjectState* retiredMetadata = parameters.Get ("semanticMetadata")) {
        for (const GS::String& fieldName : retiredMetadata->GetFieldNames ()) {
            if (GS::UniString (fieldName).BeginsWith ("nativeCandidate"))
                return CreateErrorResponse (APIERR_BADPARS, "Native candidate writer metadata is retired (D-307).");
        }
    }
    GS::ObjectState scopeParameters;
    GS::UniString expectedScope;
    parameters.Get ("expectedFileScopeKey", expectedScope);
    scopeParameters.Add ("fileScopeKey", expectedScope);
    const GS::Optional<GS::UniString> scopeError = ValidateScope (scopeParameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    GS::UniString requestedType;
    if (parameters.Get ("elementType", requestedType) && requestedType == "Dimension") {
        // The Dimension candidate publishes its own receipt inside its own
        // command; it cannot be bound into one deferred transaction receipt.
        if (pending != nullptr)
            return CreateErrorResponse (APIERR_BADPARS, "NATIVE_TRANSACTION_UNSUPPORTED: Dimension writes cannot join a Native transaction; nothing was written.");
#if TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
        return ExecuteNativeDimensionCreateCandidate (parameters, processControl);
#else
        // An explicitly versioned private candidate envelope is refused by an
        // ordinary profile without entering the ordinary CRUD parser. Keep
        // historical ordinary Dimension denial unchanged for every other
        // payload shape.
        GS::Array<GS::ObjectState> dimensionItems;
        GS::UniString wireSchema;
        if (parameters.Get ("items", dimensionItems) && dimensionItems.GetSize () == 1
            && dimensionItems[0].Get ("schemaVersion", wireSchema)
            && wireSchema == NativeDimensionCreateWire::SchemaVersion)
            return ExecuteNativeDimensionCreateCandidate (parameters, processControl);
#endif
    }
    GS::UniString contract;
    if (!parameters.Get ("contractVersion", contract) || contract != ContractVersion)
        return CreateErrorResponse (APIERR_BADPARS, "contractVersion must be native-crud-v2.");
    GS::UniString operation;
    GS::UniString typeName;
    GS::UniString operationId;
    GS::UniString requestHash;
    GS::UniString description;
    GS::UniString operatorNote;
    GS::UniString origin;
    if (!parameters.Get ("operation", operation) || !parameters.Get ("elementType", typeName) ||
        !parameters.Get ("operationId", operationId) || !parameters.Get ("requestHash", requestHash) ||
        !parameters.Get ("description", description))
        return CreateErrorResponse (APIERR_BADPARS, "Native mutation envelope is missing identity fields.");
    if (operationId.IsEmpty () || description.IsEmpty () || !IsHex64 (requestHash))
        return CreateErrorResponse (APIERR_BADPARS, "operationId/description are required and requestHash must be a 64-hex value.");
    parameters.Get ("operatorNote", operatorNote);
    if (operationId.GetLength () > 200 || description.GetLength () > 500 || operatorNote.GetLength () > 1000)
        return CreateErrorResponse (APIERR_BADPARS, "operationId or description exceeds the native contract length limit.");

    parameters.Get ("origin", origin);
    const GS::ObjectState* semanticMetadata = parameters.Get ("semanticMetadata");
    if (operation != "create" && operation != "update" && operation != "delete")
        return CreateErrorResponse (APIERR_BADPARS, "operation must be create, update or delete.");
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (typeName);
    if (typeID == API_DimensionID || typeID == API_RadialDimensionID
        || typeID == API_AngleDimensionID || typeID == API_LevelDimensionID)
        return CreateErrorResponse (APIERR_BADPARS, "Dimension elements are read-only; no native mutation was dispatched.");
    if (!IsSupportedNativeType (typeID)) return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    // Keep the wire vocabulary canonical at the native boundary as well.  A
    // full Circle is exposed as Arc in native-crud-v2; accepting the alias
    // here would otherwise produce a receipt whose type disagrees with its
    // snapshots and capability row.
    typeName = CanonicalElementTypeName (typeName);

    GS::Array<GS::ObjectState> items;
    if (!parameters.Get ("items", items)) return CreateErrorResponse (APIERR_BADPARS, "items is required.");
    const GS::Optional<GS::UniString> itemError = ValidateMutationItems (operation, typeName, items);
    if (itemError.HasValue ()) return ScopeError (itemError);
    // D-347 stage 3b: the items carry no payload; the module holds the elements.
    const bool moduleRestore = operation == "create" && IsModuleRestoreRequest (items);
    if (operation == "create") {
        const GS::Optional<GS::UniString> placementError = ValidateNativeCreatePlacement (typeName, items);
        if (placementError.HasValue ()) return ScopeError (placementError);
    }
    if (typeID == API_ZoneID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested)
            return CreateErrorResponse (APIERR_BADPARS, "Zone test-fault metadata is unavailable to Native CRUD writes (D-307).");
    }
    if (typeID == API_ColumnID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested)
            return CreateErrorResponse (APIERR_BADPARS, "Column test-fault metadata is unavailable to Native CRUD writes (D-307).");
    }
    if (typeID == API_LineID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested)
            return CreateErrorResponse (APIERR_BADPARS, "Line test-fault metadata is unavailable to Native CRUD writes (D-307).");
    }
    if (typeID == API_ZoneID && (operation == "create" || operation == "update") && !moduleRestore) {
        const GS::Optional<GS::UniString> typedError = ValidateZoneTypedWriter (operation, items);
        if (typedError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
    }
    if (typeID == API_ArcID && (operation == "create" || operation == "update") && !moduleRestore) {
        // The proven Arc fields use the typed payload validation and the
        // native undoable dispatcher.
        const GS::Optional<GS::UniString> typedError = ValidateArcTypedPayloads (operation, items);
        if (typedError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
    }
    if (typeID == API_ArcID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested)
            return CreateErrorResponse (APIERR_BADPARS, "Arc test-fault metadata is unavailable to Native CRUD writes (D-307).");
    }
    if (typeID == API_HatchID && (operation == "create" || operation == "update") && !moduleRestore) {
        if (HasNativeCrudTestFaultMetadata (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Hatch test-fault metadata is unavailable to Native CRUD writes (D-307).");
        const GS::Optional<GS::UniString> typedError = ValidateHatchTypedPayloads (operation, items);
        if (typedError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
    }
    if (typeID == API_HatchID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested)
            return CreateErrorResponse (APIERR_BADPARS, "Hatch test-fault metadata is unavailable to Native CRUD writes (D-307).");
    }
    if (typeID == API_ZoneID && HasAnyZoneMemoWriterGate (semanticMetadata)) {
        const GS::Optional<GS::UniString> candidateError = ValidateZoneMemoWriterCandidate (operation, items, semanticMetadata);
        if (candidateError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
    }

    GS::Array<GS::ObjectState> requestedIds;
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* id = item.Get ("elementId");
        if (id != nullptr) requestedIds.Push (*id);
    }

    const GS::UniString scope = CurrentSoloFileScope ().Get ();
    NativeImpactEnvelope impact;
    const GS::Optional<GS::UniString> impactError = ParseNativeImpactEnvelope (
        semanticMetadata, scope, operationId, operation, typeName, requestedIds, impact);
    if (impactError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, impactError.Get ());
    const GS::Array<GS::ObjectState>& impactIds = impact.impactIds.IsEmpty () ? requestedIds : impact.impactIds;
    GS::UniString impactSchema;
    const bool hasImpactEnvelope = impact.envelope.Get ("schemaVersion", impactSchema);

    // Idempotency is owned durably by MCP SQLite.  APX keeps only a bounded
    // process-local receipt cache for response-loss reconciliation while this
    // Archicad process is alive.  A cache hit must still match the complete
    // request identity; an operation-id collision is never silently replayed.
    const GS::Optional<GS::ObjectState> cachedReceipt = FindCachedReceipt (operationId, scope);
    if (cachedReceipt.HasValue ()) {
        // A transaction never replays: a known operationId refuses the whole
        // transaction before (or, defensively, inside) its undo step.
        if (pending != nullptr)
            return NativeContractError ("DUPLICATE_OPERATION_CONFLICT", "operationId already has a native receipt; a Native transaction never replays an operation.");
        // The APX has no canonical request/patch hash implementation.  A
        // syntactically valid requestHash/patchHash is therefore not enough
        // to replay a Wall/Zone receipt: the MCP coordinator must reconcile
        // the exact persisted record instead of allowing an unverified cache
        // hit to acknowledge a different patch.
        if (semanticMetadata != nullptr && semanticMetadata->Get (ImpactMetadataKey) != nullptr)
            return NativeContractError (
                "NATIVE_IMPACT_REPLAY_UNVERIFIED",
                "nativeImpact cache replay is disabled because requestHash and patchHash cannot be recomputed from the exact request items in APX.");
        if (!ReceiptRequestIdentityMatches (cachedReceipt.Get (), scope, operation, typeName, requestHash, description, operatorNote, origin, requestedIds, semanticMetadata)) {
            return NativeContractError (
                "DUPLICATE_OPERATION_CONFLICT",
                "operationId already has a different complete native request identity.");
        }
        GS::ObjectState replay;
        replay.Add ("receipt", cachedReceipt.Get ());
        replay.Add ("readback", GS::ObjectState ("elements", GS::ObjectState ()));
        replay.Add ("duplicateOperation", true);
        return replay;
    }
    // D-347 stage 2b: a recreation is refused while the element it replaces exists (an Undo of the
    // deletion between the revert plan and this command must not leave a duplicate).
    if (operation == "create") {
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* absentId = item.Get ("absentElementId");
            if (absentId == nullptr) continue;
            // Only BADID proves absence; any other header error leaves it unproven.
            API_Elem_Head absentHeader = {};
            absentHeader.guid = GetGuidFromObjectState (*absentId);
            const GSErrCode absentError = ACAPI_Element_GetHeader (&absentHeader);
            if (absentError == NoError)
                return CreateErrorResponse (APIERR_BADID, "RECREATE_ORIGINAL_PRESENT: the element this CREATE recreates exists; nothing was created.");
            if (absentError != APIERR_BADID)
                return CreateErrorResponse (APIERR_BADID, "RECREATE_ABSENCE_UNPROVEN: the element this CREATE recreates could not be proven absent; nothing was created.");
        }
    }
    bool beforeComplete = operation == "create";
    GS::ObjectState before = operation == "create"
        ? GS::ObjectState ("elements", GS::ObjectState ())
        : ExactSnapshots (impactIds, scope, processControl, beforeComplete);
    if (!beforeComplete && operation != "create") return CreateErrorResponse (APIERR_BADID, "SNAPSHOT_COVERAGE_INCOMPLETE: native before-state is incomplete; no mutation was dispatched.");
    if (pending != nullptr) pending->before = before;

    if (operation == "update" || operation == "delete") {
        GS::Array<GS::ObjectState> beforeRows;
        before.Get ("elements", beforeRows);
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* id = item.Get ("elementId");
            GS::UniString expectedFingerprint;
            item.Get ("expectedFingerprint", expectedFingerprint);
            const API_Guid requestedGuid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
            const GS::ObjectState* matching = nullptr;
            for (const GS::ObjectState& snapshot : beforeRows) {
                const GS::ObjectState* snapshotId = snapshot.Get ("elementId");
                if (snapshotId != nullptr && GetGuidFromObjectState (*snapshotId) == requestedGuid) {
                    matching = &snapshot;
                    break;
                }
            }
            if (matching != nullptr) {
                GS::UniString actualType;
                if (!matching->Get ("elementType", actualType) || actualType != typeName)
                    return CreateErrorResponse (APIERR_BADID, "NATIVE_TYPE_MISMATCH: an exact GUID does not belong to the requested elementType.");
            }
            GS::UniString actualFingerprint;
            if (matching == nullptr || !matching->Get ("fingerprint", actualFingerprint) || actualFingerprint != expectedFingerprint)
                return CreateErrorResponse (APIERR_BADID, "STALE_NATIVE_SNAPSHOT: expectedFingerprint does not match the current native element.");
            // The layer is outside the fingerprint; a DELETE that names it is refused when it moved (D-347 stage 2b).
            if (item.Contains ("expectedLayerIndex")) {
                Int32 expectedLayerIndex = 0;
                Int32 actualLayerIndex = 0;
                item.Get ("expectedLayerIndex", expectedLayerIndex);
                if (!matching->Get ("layerIndex", actualLayerIndex) || actualLayerIndex != expectedLayerIndex)
                    return CreateErrorResponse (APIERR_BADID, "STALE_NATIVE_SNAPSHOT: expectedLayerIndex does not match the current native element.");
            }
        }
        const GS::Optional<GS::UniString> impactTypeError = ValidateNativeImpactSnapshotTypes (
            beforeRows,
            impactIds,
            requestedIds,
            typeName,
            hasImpactEnvelope ? &impact : nullptr);
        if (impactTypeError.HasValue ())
            return CreateErrorResponse (APIERR_BADID, impactTypeError.Get ());
    }

    GS::ObjectState mutationResult;
    bool moduleRollbackProven = false;
    if (moduleRestore) {
        ModuleRestoreOutcome restored = ExecuteModuleRestore (operationId, typeName, items);
        if (restored.refusal.HasValue ()) return restored.refusal.Get ();
        mutationResult = restored.mutationResult;
        moduleRollbackProven = restored.rollbackProven;
    } else {
        mutationResult = DispatchMutation (operation, operationId, typeName, items, processControl, semanticMetadata);
    }
    bool dispatchComplete = true;
    mutationResult.Get ("mutationComplete", dispatchComplete);
    if (mutationResult.Get ("error") != nullptr || mutationResult.Get ("transactionRolledBack") != nullptr) dispatchComplete = false;
    // Exact Native DELETE must not be reported complete if restoring the
    // operator's prior Suspend Groups mode failed.  The element mutation may
    // already be applied, so retain its factual readback/receipt and never
    // retry the element command; only mark acceptance incomplete.
    if (mutationResult.Contains ("groupModeRestored")) {
        bool groupModeRestored = false;
        if (!mutationResult.Get ("groupModeRestored", groupModeRestored) || !groupModeRestored)
            dispatchComplete = false;
    }
    if (mutationResult.Contains ("groupModeRestoreError"))
        dispatchComplete = false;

    GS::Array<GS::ObjectState> afterIds = requestedIds;
    bool createIdsComplete = operation != "create";
    if (operation == "create") {
        GS::Array<GS::ObjectState> created;
        mutationResult.Get ("elements", created);
        afterIds.Clear ();
        for (const GS::ObjectState& row : created) {
            const GS::ObjectState* id = row.Get ("elementId");
            if (id != nullptr) afterIds.Push (*id);
        }
        // A successful undoable command without one exact returned GUID per
        // requested CREATE item is not enough evidence to claim an applied
        // native operation.  Keep the receipt terminal but mark it
        // incomplete_evidence so the TypeScript lane blocks until a later
        // reconciliation can establish the factual after-state.
        createIdsComplete = afterIds.GetSize () == items.GetSize ();
    }
    bool afterComplete = operation == "delete";
    GS::ObjectState after;
    GS::ObjectState deleteReadback;
    if (operation == "delete") {
        // Deletion is only factual after every requested GUID is observed as
        // absent.  The old delete command's aggregate success is not enough:
        // a host can acknowledge an undoable command while one dependent or
        // invalid item remains.  Keep the per-GUID evidence in readback and
        // leave the receipt incomplete when any absence cannot be proven.
        bool deleteVerified = true;
        GSSize absentCount = 0;
        deleteReadback = BuildDeleteReadback (impactIds, typeID, deleteVerified, absentCount);
        afterComplete = deleteVerified && absentCount == static_cast<GSSize> (impactIds.GetSize ());
        after = GS::ObjectState ("elements", GS::ObjectState ());
    } else {
        const GS::Array<GS::ObjectState>& requestedAfterIds = operation == "create" ? afterIds : impactIds;
        after = ExactSnapshots (requestedAfterIds, scope, processControl, afterComplete);
        afterComplete = afterComplete && createIdsComplete;
    }
    const bool complete = dispatchComplete && afterComplete;
    const GS::Array<GS::ObjectState>& primaryAfterIds = operation == "create" ? afterIds : requestedIds;
    const GS::Array<GS::ObjectState>& receiptImpactIds = hasImpactEnvelope ? impactIds : primaryAfterIds;
    // D-350 R1: a module restore that did not complete is a known refusal only with the rollback proven.
    const char* statusOverride = moduleRestore && !dispatchComplete && !moduleRollbackProven ? "unknown" : nullptr;
    GS::ObjectState receipt = BuildReceipt (scope, operationId, operation, typeName, description, origin, requestHash, before, after, primaryAfterIds, receiptImpactIds, complete, dispatchComplete, semanticMetadata, operatorNote, &mutationResult, statusOverride, nullptr, hasImpactEnvelope ? &impact : nullptr);
    NativeMutationPending outcome;
    outcome.ready = true;
    outcome.receipt = receipt;
    outcome.mutationResult = mutationResult;
    outcome.readback = after;
    outcome.isDelete = operation == "delete";
    outcome.deleteReadback = deleteReadback;
    if (pending == nullptr) return BuildNativeMutationResponse (outcome);
    outcome.before = pending->before;
    *pending = outcome;
    return GS::ObjectState ("status", "pending");
}

} // namespace

GS::ObjectState MutateNativeElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    return ExecuteNativeMutationRequest (parameters, processControl, nullptr);
}

// ---------------------------------------------------------------------------
// D-357: MutateNativeTransaction.  One call may mix element types and
// CREATE/UPDATE/DELETE operations.  All operations run in order inside ONE
// NativeUndoableCommand (one Archicad undo step).  Any refused, failed,
// incomplete or partial operation makes the callback return an error, so
// Archicad rolls the whole step back; nothing is published and no receipt is
// remembered.  Receipts are published only after the undo step committed.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* DefaultTransactionUndoName = "Sheet patch";
constexpr USize MaxTransactionTextLength = 200;

struct TransactionLabels {
    GS::Array<GS::UniString> names;
    GS::Array<API_Guid> guids;

    bool Find (const GS::UniString& name, API_Guid& guid) const
    {
        for (UIndex index = 0; index < names.GetSize (); ++index) {
            if (names[index] == name) {
                guid = guids[index];
                return true;
            }
        }
        return false;
    }
};

struct TransactionFingerprints {
    GS::Array<API_Guid> guids;
    GS::Array<GS::UniString> values;

    void Set (const API_Guid& guid, const GS::UniString& value)
    {
        for (UIndex index = 0; index < guids.GetSize (); ++index) {
            if (guids[index] == guid) {
                values[index] = value;
                return;
            }
        }
        guids.Push (guid);
        values.Push (value);
    }

    bool Find (const API_Guid& guid, GS::UniString& value) const
    {
        for (UIndex index = 0; index < guids.GetSize (); ++index) {
            if (guids[index] == guid) {
                value = values[index];
                return true;
            }
        }
        return false;
    }
};

// A label reference is an object with exactly one string field `label`.
bool IsLabelReference (const GS::ObjectState& value, GS::UniString& label)
{
    if (value.GetFieldCount () != 1 || !value.Contains ("label") || !value.IsString ("label"))
        return false;
    return value.Get ("label", label);
}

// Collects every label reference in one item.  At the item top level of a
// CREATE the string field `label` is a definition and is skipped.  Label
// references inside lists are refused: list rows cannot be rewritten in
// place, and silently ignoring them would write an unresolved reference.
GS::Optional<GS::UniString> CollectLabelReferences (
    const GS::ObjectState& value,
    bool itemTopLevel,
    GS::Array<GS::UniString>& references)
{
    for (const GS::String& fieldName : value.GetFieldNames ()) {
        if (itemTopLevel && fieldName == "label" && value.IsString (fieldName))
            continue;
        if (value.IsObject (fieldName)) {
            const GS::ObjectState* child = value.Get (fieldName);
            if (child == nullptr) continue;
            GS::UniString label;
            if (IsLabelReference (*child, label)) {
                references.Push (label);
                continue;
            }
            const GS::Optional<GS::UniString> nestedError = CollectLabelReferences (*child, false, references);
            if (nestedError.HasValue ()) return nestedError;
        } else if (value.IsList (fieldName)) {
            GS::Array<GS::ObjectState> rows;
            if (!value.Get (fieldName, rows)) continue;
            for (const GS::ObjectState& row : rows) {
                GS::Array<GS::UniString> nested;
                GS::UniString label;
                const GS::Optional<GS::UniString> nestedError = CollectLabelReferences (row, false, nested);
                if (nestedError.HasValue ()) return nestedError;
                if (IsLabelReference (row, label) || !nested.IsEmpty ())
                    return GS::UniString ("label references are accepted only in object positions (elementId or a GUID object), not inside lists.");
            }
        }
    }
    return {};
}

// Rewrites every label reference of an owned deep copy in place into the
// element-id shape `{ "guid": "<created GUID>" }`.
GS::Optional<GS::UniString> ResolveLabelReferences (
    GS::ObjectState& value,
    bool itemTopLevel,
    const TransactionLabels& labels)
{
    for (const GS::String& fieldName : value.GetFieldNames ()) {
        if (itemTopLevel && fieldName == "label" && value.IsString (fieldName))
            continue;
        if (!value.IsObject (fieldName)) continue;
        GS::ObjectState* child = const_cast<GS::ObjectState*> (value.Get (fieldName));
        if (child == nullptr) continue;
        GS::UniString label;
        if (IsLabelReference (*child, label)) {
            API_Guid guid = APINULLGuid;
            if (!labels.Find (label, guid))
                return GS::UniString ("label \"") + label + "\" is not defined by an earlier CREATE item of this transaction.";
            child->Clear ();
            child->Add ("guid", APIGuidToString (guid));
            continue;
        }
        const GS::Optional<GS::UniString> nestedError = ResolveLabelReferences (*child, false, labels);
        if (nestedError.HasValue ()) return nestedError;
    }
    return {};
}

// GS::ObjectState has no Remove; rebuild a CREATE item without its `label`.
GS::Optional<GS::UniString> CopyItemWithoutLabel (const GS::ObjectState& source, GS::ObjectState& target)
{
    target.Clear ();
    for (const GS::String& fieldName : source.GetFieldNames ()) {
        if (fieldName == "label") continue;
        switch (source.GetType (fieldName)) {
            case GS::ObjectState::Bool: { bool v = false; source.Get (fieldName, v); target.Add (fieldName, v); break; }
            case GS::ObjectState::Int: { Int64 v = 0; source.Get (fieldName, v); target.Add (fieldName, v); break; }
            case GS::ObjectState::UInt: { UInt64 v = 0; source.Get (fieldName, v); target.Add (fieldName, v); break; }
            case GS::ObjectState::Real: { double v = 0.0; source.Get (fieldName, v); target.Add (fieldName, v); break; }
            case GS::ObjectState::String: { GS::UniString v; source.Get (fieldName, v); target.Add (fieldName, v); break; }
            case GS::ObjectState::Object: {
                const GS::ObjectState* child = source.Get (fieldName);
                if (child == nullptr) return GS::UniString ("A labelled CREATE item has an unreadable object field.");
                target.Add (fieldName, *child);
                break;
            }
            case GS::ObjectState::List: {
                GS::Array<GS::ObjectState> rows;
                if (!source.Get (fieldName, rows))
                    return GS::UniString ("A labelled CREATE item may carry only object lists beside its label.");
                target.Add (fieldName, rows);
                break;
            }
        }
    }
    return {};
}

API_Guid RowGuid (const GS::ObjectState& row)
{
    const GS::ObjectState* id = row.Get ("elementId");
    return id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
}

GS::ObjectState MakeTransactionError (const GS::UniString& code, const GS::UniString& message)
{
    GS::ObjectState error;
    error.Add ("code", code);
    error.Add ("message", message);
    return error;
}

GS::ObjectState MakeTransactionError (GSErrCode code, const GS::UniString& message)
{
    GS::ObjectState error;
    error.Add ("code", static_cast<Int32> (code));
    error.Add ("message", message);
    return error;
}

GS::ObjectState ErrorObjectOf (const GS::ObjectState& response, const GS::UniString& fallback)
{
    const GS::ObjectState* error = response.Get ("error");
    if (error != nullptr) return *error;
    return MakeTransactionError (GS::UniString ("NATIVE_TRANSACTION_OPERATION_FAILED"), fallback);
}

GS::ObjectState TransactionFailure (
    const GS::UniString& transactionId,
    const char* outcome,
    const GS::ObjectState& error,
    Int32 failedOperationIndex,
    const GS::UniString& failedOperationId)
{
    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("transactionId", transactionId);
    response.Add ("status", "failed");
    response.Add ("committed", false);
    response.Add ("outcome", outcome);
    // Not "error": Archicad's JSON API turns an add-on answer with a
    // top-level "error" into a failed command and drops everything else.
    response.Add ("failure", error);
    if (failedOperationIndex >= 0) {
        response.Add ("failedOperationIndex", failedOperationIndex);
        response.Add ("failedOperationId", failedOperationId);
    }
    return response;
}

GS::ObjectState TransactionRefusal (
    const GS::UniString& transactionId,
    const GS::UniString& message,
    Int32 failedOperationIndex = -1,
    const GS::UniString& failedOperationId = GS::UniString ())
{
    return TransactionFailure (transactionId, "refused", MakeTransactionError (APIERR_BADPARS, message), failedOperationIndex, failedOperationId);
}

// Independent postcondition of a rollback: every GUID created inside the
// transaction is absent again, every pre-existing target is back at its first
// observed fingerprint, and every target known only by GUID still exists.  An
// empty fingerprint means "existence only".  `inspected` counts the elements
// actually checked; a verdict with nothing inspected is not evidence.
struct RollbackInspection {
    UInt32 inspected = 0;
    bool passed = false;
};

RollbackInspection VerifyTransactionRolledBack (
    const GS::Array<API_Guid>& createdGuids,
    const GS::Array<GS::ObjectState>& originalIds,
    const GS::Array<GS::UniString>& originalFingerprints,
    const GS::UniString& scope,
    GS::ProcessControl& processControl)
{
    RollbackInspection inspection;
    for (const API_Guid& guid : createdGuids) {
        API_Elem_Head header = {};
        header.guid = guid;
        ++inspection.inspected;
        if (ACAPI_Element_GetHeader (&header) != APIERR_BADID) return inspection;
    }
    if (!originalIds.IsEmpty ()) {
        bool complete = false;
        const GS::ObjectState snapshots = ExactSnapshots (originalIds, scope, processControl, complete);
        if (!complete) return inspection;
        GS::Array<GS::ObjectState> rows;
        snapshots.Get ("elements", rows);
        for (UIndex index = 0; index < originalIds.GetSize (); ++index) {
            const API_Guid guid = GetGuidFromObjectState (originalIds[index]);
            ++inspection.inspected;
            bool matched = false;
            for (const GS::ObjectState& row : rows) {
                if (RowGuid (row) != guid) continue;
                GS::UniString fingerprint;
                matched = originalFingerprints[index].IsEmpty ()
                    || (row.Get ("fingerprint", fingerprint) && fingerprint == originalFingerprints[index]);
                break;
            }
            if (!matched) return inspection;
        }
    }
    inspection.passed = inspection.inspected > 0;
    return inspection;
}

} // namespace

MutateNativeTransactionCommand::MutateNativeTransactionCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String MutateNativeTransactionCommand::GetName () const { return "MutateNativeTransaction"; }
GS::Optional<GS::UniString> MutateNativeTransactionCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"transactionId":{"type":"string","minLength":1,"maxLength":200},"undoName":{"type":"string","minLength":1,"maxLength":200},"expectedFileScopeKey":{"type":"string"},"operations":{"type":"array","minItems":1,"items":{"type":"object","properties":{"operationId":{"type":"string","minLength":1,"maxLength":200},"operation":{"type":"string","enum":["create","update","delete"]},"elementType":{"type":"string"},"items":{"type":"array","minItems":1,"maxItems":500,"items":{"type":"object"}},"requestHash":{"type":"string"},"description":{"type":"string"},"origin":{"type":"string","enum":["NATIVE","SEMANTIC"]},"operatorNote":{"type":"string","maxLength":1000},"semanticMetadata":{"type":"object"}},"additionalProperties":false,"required":["operationId","operation","elementType","items"]}}},"additionalProperties":false,"required":["transactionId","expectedFileScopeKey","operations"]})";
}
GS::Optional<GS::UniString> MutateNativeTransactionCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> MutateNativeTransactionCommand::GetRawResponseSchema () const { return {}; }
GS::ObjectState MutateNativeTransactionCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    GS::UniString transactionId;
    parameters.Get ("transactionId", transactionId);
    if (transactionId.IsEmpty () || transactionId.GetLength () > MaxTransactionTextLength)
        return TransactionRefusal (transactionId, "transactionId is required (1..200 characters).");
    GS::UniString contract;
    if (parameters.Get ("contractVersion", contract) && contract != ContractVersion)
        return TransactionRefusal (transactionId, "contractVersion must be native-crud-v2 when present.");

    // Same scope boundary as MutateNativeElements.
    GS::ObjectState scopeParameters;
    GS::UniString expectedScope;
    parameters.Get ("expectedFileScopeKey", expectedScope);
    scopeParameters.Add ("fileScopeKey", expectedScope);
    const GS::Optional<GS::UniString> scopeError = ValidateScope (scopeParameters);
    if (scopeError.HasValue ())
        return TransactionRefusal (transactionId, scopeError.Get ());
    const GS::UniString scope = CurrentSoloFileScope ().Get ();

    GS::UniString undoName (DefaultTransactionUndoName);
    if (parameters.Contains ("undoName")) {
        parameters.Get ("undoName", undoName);
        if (undoName.IsEmpty () || undoName.GetLength () > MaxTransactionTextLength)
            return TransactionRefusal (transactionId, "undoName must contain 1..200 characters.");
    }

    GS::Array<GS::ObjectState> operations;
    if (!parameters.Get ("operations", operations) || operations.IsEmpty ())
        return TransactionRefusal (transactionId, "operations must contain at least one operation.");

    // Static preflight, before the undo step opens: identities, label
    // definitions and backward-only label references.
    GS::Array<GS::UniString> operationIds;
    GS::Array<GS::UniString> definedLabels;
    bool needsGroupSuspension = false;
    for (UIndex opIndex = 0; opIndex < operations.GetSize (); ++opIndex) {
        const GS::ObjectState& op = operations[opIndex];
        const Int32 index = static_cast<Int32> (opIndex);
        GS::UniString operationId;
        GS::UniString operation;
        GS::UniString elementType;
        GS::Array<GS::ObjectState> items;
        if (!op.Get ("operationId", operationId) || operationId.IsEmpty () || operationId.GetLength () > MaxTransactionTextLength)
            return TransactionRefusal (transactionId, "Every operation needs an operationId (1..200 characters).", index);
        if (operationIds.Contains (operationId))
            return TransactionRefusal (transactionId, "operationId appears twice in one transaction.", index, operationId);
        operationIds.Push (operationId);
        if (!op.Get ("operation", operation) || (operation != "create" && operation != "update" && operation != "delete"))
            return TransactionRefusal (transactionId, "operation must be create, update or delete.", index, operationId);
        if (!op.Get ("elementType", elementType) || elementType.IsEmpty ())
            return TransactionRefusal (transactionId, "elementType is required.", index, operationId);
        if (!op.Get ("items", items) || items.IsEmpty () || items.GetSize () > static_cast<USize> (MaxNativeWriteItems))
            return TransactionRefusal (transactionId, "items must contain between 1 and 500 entries.", index, operationId);
        if (operation != "create") needsGroupSuspension = true;
        if (FindCachedReceipt (operationId, scope).HasValue ()) {
            return TransactionFailure (transactionId, "refused",
                MakeTransactionError (GS::UniString ("DUPLICATE_OPERATION_CONFLICT"), "operationId already has a native receipt; a Native transaction never replays an operation."),
                index, operationId);
        }
        GS::Array<GS::UniString> labelsOfThisOperation;
        for (const GS::ObjectState& item : items) {
            GS::Array<GS::UniString> references;
            const GS::Optional<GS::UniString> collectError = CollectLabelReferences (item, true, references);
            if (collectError.HasValue ()) return TransactionRefusal (transactionId, collectError.Get (), index, operationId);
            for (const GS::UniString& reference : references) {
                if (!definedLabels.Contains (reference))
                    return TransactionRefusal (transactionId, GS::UniString ("label \"") + reference + "\" must be defined by a CREATE item of an earlier operation.", index, operationId);
            }
            if (item.Contains ("moduleRestore")) {
                // Module restore proves its own rollback with a separate
                // undo step; inside the transaction step that proof is not
                // real, so the request is refused instead of weakened.
                return TransactionFailure (transactionId, "refused",
                    MakeTransactionError (GS::UniString ("NATIVE_TRANSACTION_UNSUPPORTED"), "moduleRestore items cannot join a Native transaction; nothing was written."),
                    index, operationId);
            }
            if (!item.Contains ("label")) continue;
            GS::UniString label;
            if (operation != "create" || !item.IsString ("label") || !item.Get ("label", label))
                return TransactionRefusal (transactionId, "label is accepted only as a string on CREATE items.", index, operationId);
            if (label.IsEmpty () || label.GetLength () > MaxTransactionTextLength)
                return TransactionRefusal (transactionId, "label must contain 1..200 characters.", index, operationId);
            if (definedLabels.Contains (label) || labelsOfThisOperation.Contains (label))
                return TransactionRefusal (transactionId, "label is defined twice in one transaction.", index, operationId);
            labelsOfThisOperation.Push (label);
        }
        for (const GS::UniString& label : labelsOfThisOperation) definedLabels.Push (label);
    }

    // Exact UPDATE/DELETE needs Suspend Groups.  Switch the UI mode once,
    // outside the undo step, so no inner writer toggles it inside the step;
    // restore the operator's prior mode on every exit path.
    bool groupModeNeedsRestore = false;
    if (needsGroupSuspension) {
        bool groupsWereSuspended = false;
        GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsWereSuspended);
        if (groupModeError != NoError)
            return TransactionFailure (transactionId, "refused", MakeTransactionError (groupModeError, "Failed to read the Suspend Groups mode; nothing was written."), -1, GS::UniString ());
        if (!groupsWereSuspended) {
            groupModeError = TAPIR_Grouping_ChangeSuspendGroup (true);
            if (groupModeError != NoError)
                return TransactionFailure (transactionId, "refused", MakeTransactionError (groupModeError, "Failed to suspend groups for the Native transaction; nothing was written."), -1, GS::UniString ());
            groupModeNeedsRestore = true;
        }
    }
    const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
        if (groupModeNeedsRestore)
            TAPIR_Grouping_ChangeSuspendGroup (false);
    });

    TransactionLabels labels;
    TransactionFingerprints fingerprints;
    GS::Array<NativeMutationPending> completed;
    GS::Array<GS::UniString> completedOperations;
    NativeMutationPending failing;
    GS::UniString failingOperation;
    Int32 failedIndex = -1;
    GS::UniString failedOperationId;
    GS::ObjectState failedError;
    GS::Array<API_Guid> failingTargets;
    bool callbackRan = false;
    bool callbackSucceeded = false;

    // D-357: one element-event bracket for the whole step.  Nested writers
    // skip their own bracket and queue New notifications until the commit.
    API_NotifyElementType eventBracket = {};
    eventBracket.notifID = APINotifyElement_BeginEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&eventBracket);
    const GSErrCode undoError = NativeUndoableCommand (undoName, [&] () -> GSErrCode {
        callbackRan = true;
        for (UIndex opIndex = 0; opIndex < operations.GetSize (); ++opIndex) {
            failingTargets.Clear ();
            const GS::ObjectState& op = operations[opIndex];
            GS::UniString operationId;
            GS::UniString operation;
            GS::UniString elementType;
            GS::Array<GS::ObjectState> items;
            op.Get ("operationId", operationId);
            op.Get ("operation", operation);
            op.Get ("elementType", elementType);
            op.Get ("items", items);
            const auto fail = [&] (const GS::ObjectState& error) -> GSErrCode {
                failedIndex = static_cast<Int32> (opIndex);
                failedOperationId = operationId;
                failingOperation = operation;
                failedError = error;
                return APIERR_CANCEL;
            };

            GS::ObjectState opParameters;
            opParameters.Add ("contractVersion", ContractVersion);
            opParameters.Add ("operationId", operationId);
            opParameters.Add ("expectedFileScopeKey", expectedScope);
            opParameters.Add ("operation", operation);
            opParameters.Add ("elementType", elementType);
            GS::UniString requestHash;
            if (!op.Get ("requestHash", requestHash) || requestHash.IsEmpty ())
                requestHash = GS::UniString ("0000000000000000000000000000000000000000000000000000000000000000");
            opParameters.Add ("requestHash", requestHash);
            GS::UniString description;
            if (!op.Get ("description", description) || description.IsEmpty ())
                description = undoName;
            opParameters.Add ("description", description);
            GS::UniString origin;
            if (op.Get ("origin", origin)) opParameters.Add ("origin", origin);
            GS::UniString operatorNote;
            if (op.Get ("operatorNote", operatorNote)) opParameters.Add ("operatorNote", operatorNote);
            const GS::ObjectState* semanticMetadata = op.Get ("semanticMetadata");
            if (semanticMetadata != nullptr) opParameters.Add ("semanticMetadata", *semanticMetadata);

            // Resolve labels into created GUIDs and complete omitted
            // fingerprints of elements this transaction already changed.
            GS::Array<GS::UniString> itemLabels;
            GS::Array<GS::ObjectState> resolvedItems;
            for (const GS::ObjectState& item : items) {
                GS::UniString label;
                const bool labelled = item.Contains ("label") && item.Get ("label", label);
                itemLabels.Push (labelled ? label : GS::UniString ());
                GS::ObjectState resolved;
                if (labelled) {
                    const GS::Optional<GS::UniString> copyError = CopyItemWithoutLabel (item, resolved);
                    if (copyError.HasValue ()) return fail (MakeTransactionError (APIERR_BADPARS, copyError.Get ()));
                } else {
                    resolved = item;
                }
                const GS::Optional<GS::UniString> resolveError = ResolveLabelReferences (resolved, true, labels);
                if (resolveError.HasValue ()) return fail (MakeTransactionError (APIERR_BADPARS, resolveError.Get ()));
                if (operation != "create" && !resolved.Contains ("expectedFingerprint")) {
                    const GS::ObjectState* id = resolved.Get ("elementId");
                    GS::UniString fingerprint;
                    if (id != nullptr && fingerprints.Find (GetGuidFromObjectState (*id), fingerprint))
                        resolved.Add ("expectedFingerprint", fingerprint);
                }
                resolvedItems.Push (resolved);
            }
            opParameters.Add ("items", resolvedItems);
            failingTargets.Clear ();
            if (operation != "create") {
                for (const GS::ObjectState& resolved : resolvedItems) {
                    const GS::ObjectState* id = resolved.Get ("elementId");
                    const API_Guid guid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
                    if (guid != APINULLGuid && !failingTargets.Contains (guid)) failingTargets.Push (guid);
                }
            }

            NativeMutationPending pending;
            const GS::ObjectState opResponse = ExecuteNativeMutationRequest (opParameters, processControl, &pending);
            if (opResponse.Get ("error") != nullptr || !pending.ready) {
                failing = pending;
                return fail (ErrorObjectOf (opResponse, "The operation returned no factual result."));
            }
            GS::UniString receiptStatus;
            pending.receipt.Get ("status", receiptStatus);
            if (receiptStatus != "applied") {
                failing = pending;
                GS::ObjectState error = MakeTransactionError (GS::UniString ("NATIVE_TRANSACTION_OPERATION_NOT_APPLIED"),
                    GS::UniString ("The operation finished with receipt status \"") + receiptStatus + "\"; the whole transaction was rolled back.");
                error.Add ("receiptStatus", receiptStatus);
                const GS::ObjectState* diagnostics = pending.receipt.Get ("transaction");
                if (diagnostics != nullptr) error.Add ("operationDiagnostics", *diagnostics);
                return fail (error);
            }

            if (operation == "create") {
                GS::Array<GS::ObjectState> created;
                pending.mutationResult.Get ("elements", created);
                if (created.GetSize () != items.GetSize ()) {
                    failing = pending;
                    return fail (MakeTransactionError (GS::UniString ("NATIVE_TRANSACTION_CREATE_IDS_INCOMPLETE"), "CREATE did not return one exact GUID per item; labels cannot be bound."));
                }
                for (UIndex itemIndex = 0; itemIndex < items.GetSize (); ++itemIndex) {
                    const API_Guid guid = RowGuid (created[itemIndex]);
                    if (guid == APINULLGuid) {
                        failing = pending;
                        return fail (MakeTransactionError (GS::UniString ("NATIVE_TRANSACTION_CREATE_IDS_INCOMPLETE"), "CREATE returned an item without an exact GUID."));
                    }
                    if (!itemLabels[itemIndex].IsEmpty ()) {
                        labels.names.Push (itemLabels[itemIndex]);
                        labels.guids.Push (guid);
                    }
                }
            }
            GS::Array<GS::ObjectState> afterRows;
            pending.readback.Get ("elements", afterRows);
            for (const GS::ObjectState& row : afterRows) {
                GS::UniString fingerprint;
                const API_Guid guid = RowGuid (row);
                if (guid != APINULLGuid && row.Get ("fingerprint", fingerprint))
                    fingerprints.Set (guid, fingerprint);
            }
            completed.Push (pending);
            completedOperations.Push (operation);
        }
        callbackSucceeded = true;
        return NoError;
    });
    eventBracket = {};
    eventBracket.notifID = APINotifyElement_EndEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&eventBracket);

    GSErrCode groupModeRestoreError = NoError;
    if (groupModeNeedsRestore) {
        groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (false);
        if (groupModeRestoreError == NoError)
            groupModeNeedsRestore = false;
    }

    if (undoError == NoError && callbackSucceeded) {
        GS::ObjectState response;
        response.Add ("contractVersion", ContractVersion);
        response.Add ("transactionId", transactionId);
        response.Add ("status", "success");
        response.Add ("committed", true);
        response.Add ("undoName", undoName);
        const auto& operationResponses = response.AddList<GS::ObjectState> ("operations");
        for (const NativeMutationPending& pending : completed)
            operationResponses (BuildNativeMutationResponse (pending));
        GS::ObjectState labelObject;
        for (UIndex index = 0; index < labels.names.GetSize (); ++index)
            labelObject.Add (GS::String (labels.names[index].ToCStr (CC_UTF8).Get ()), APIGuidToString (labels.guids[index]));
        response.Add ("labels", labelObject);
        if (needsGroupSuspension) {
            response.Add ("groupModeRestored", groupModeRestoreError == NoError);
            if (groupModeRestoreError != NoError)
                response.Add ("groupModeRestoreError", static_cast<Int32> (groupModeRestoreError));
        }
        return response;
    }

    if (!callbackRan)
        return TransactionFailure (transactionId, "refused", MakeTransactionError (undoError, "Archicad did not open the undo step; nothing was written."), -1, GS::UniString ());

    // The callback ran.  Either it failed (Archicad rolls the step back) or
    // the host reported an error after a successful callback.  In both cases
    // verify the rollback independently; an unproven rollback is "unknown"
    // and is never retried (D-281).
    GS::Array<API_Guid> createdGuids;
    GS::Array<GS::ObjectState> originalIds;
    GS::Array<GS::UniString> originalFingerprints;
    const auto collect = [&] (const NativeMutationPending& pending, const GS::UniString& operation) {
        if (operation == "create" && pending.ready) {
            GS::Array<GS::ObjectState> created;
            pending.mutationResult.Get ("elements", created);
            for (const GS::ObjectState& row : created) {
                const API_Guid guid = RowGuid (row);
                if (guid != APINULLGuid && !createdGuids.Contains (guid)) createdGuids.Push (guid);
            }
        }
        GS::Array<GS::ObjectState> beforeRows;
        pending.before.Get ("elements", beforeRows);
        for (const GS::ObjectState& row : beforeRows) {
            const API_Guid guid = RowGuid (row);
            GS::UniString fingerprint;
            if (guid == APINULLGuid || createdGuids.Contains (guid) || !row.Get ("fingerprint", fingerprint)) continue;
            bool seen = false;
            for (const GS::ObjectState& id : originalIds) {
                if (GetGuidFromObjectState (id) == guid) { seen = true; break; }
            }
            if (seen) continue;
            originalIds.Push (CreateGuidObjectState (guid));
            originalFingerprints.Push (fingerprint);
        }
    };
    for (UIndex index = 0; index < completed.GetSize (); ++index) collect (completed[index], completedOperations[index]);
    collect (failing, failingOperation);
    if (failedIndex >= 0) {
        // Targets of the failing operation that no snapshot covered: the op
        // failed before its write could start, so existence is the proof.
        for (const API_Guid& guid : failingTargets) {
            if (createdGuids.Contains (guid)) continue;
            bool seen = false;
            for (const GS::ObjectState& id : originalIds) {
                if (GetGuidFromObjectState (id) == guid) { seen = true; break; }
            }
            if (seen) continue;
            originalIds.Push (CreateGuidObjectState (guid));
            originalFingerprints.Push (GS::UniString ());
        }
    }
    const RollbackInspection inspection = VerifyTransactionRolledBack (createdGuids, originalIds, originalFingerprints, scope, processControl);
    const bool rollbackVerified = inspection.passed;
    const bool noEvidence = inspection.inspected == 0;
    // With nothing to inspect, a failed callback plus an error from the undo
    // step is still a rollback, but it is not claimed as verified.
    const bool rolledBack = rollbackVerified || (noEvidence && !callbackSucceeded && undoError != NoError);
    const char* outcome = rolledBack ? "rolled_back" : "unknown";
    const auto addEvidence = [&] (GS::ObjectState& response) {
        response.Add ("rollbackVerified", rollbackVerified);
        response.Add ("rollbackEvidence", noEvidence ? "none" : "inspected");
        response.Add ("rollbackInspectedElements", static_cast<Int32> (inspection.inspected));
    };

    if (callbackSucceeded) {
        GS::ObjectState response = TransactionFailure (transactionId, outcome,
            MakeTransactionError (undoError, rolledBack
                ? GS::UniString ("Archicad reported an error after every operation succeeded; the rollback was verified, nothing is applied.")
                : GS::UniString ("Archicad reported an error after every operation succeeded; the outcome is unknown and must not be retried.")),
            -1, GS::UniString ());
        addEvidence (response);
        return response;
    }
    GS::ObjectState response = TransactionFailure (transactionId, outcome, failedError, failedIndex, failedOperationId);
    addEvidence (response);
    if (undoError != NoError) response.Add ("transactionError", static_cast<Int32> (undoError));
    return response;
}
