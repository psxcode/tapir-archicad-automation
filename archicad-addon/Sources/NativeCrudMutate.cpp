#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementMutationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"
#include "NativeDimensionCreateWire.hpp"
#include "NativeDimensionCreateWireObjectState.hpp"

using namespace NativeCrudInternal;

namespace {

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
        // Ordinary builds admit only the same markerless typed writer. Fault
        // metadata was rejected above; the ordinary request must still use
        // the narrow Door payload contract and exact source.
        const GS::Optional<GS::UniString> writerError = ValidateDoorTypedPayloads (operation, items);
        if (writerError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, writerError.Get ());
        if (operation == "create") {
            for (const GS::ObjectState& item : items) {
                const GS::ObjectState* source = item.Get ("cloneSourceElementId");
                if (source == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door CREATE requires an exact markerless source template.");
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
                if (source == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Window CREATE requires an exact markerless source template.");
                const GS::Optional<GS::UniString> cloneError = ValidateWindowCloneSource (GetGuidFromObjectState (*source));
                if (cloneError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, cloneError.Get ());
            }
        }
    }
    if (operation == "delete" && typeID == API_DoorID) {
        // Door DELETE is markerless-only even for direct APX callers. Read
        // every requested target immediately before entering the generic
        // delete writer so the TypeScript preflight is not the only guard.
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* id = item.Get ("elementId");
            const API_Guid requestedGuid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
            if (requestedGuid == APINULLGuid)
                return CreateErrorResponse (APIERR_BADPARS, "Door DELETE requires an exact elementId GUID.");
            API_Element target = {};
            target.header.guid = requestedGuid;
            if (ACAPI_Element_Get (&target) != NoError
                || target.header.guid != requestedGuid
                || GetElemTypeId (target.header) != API_DoorID
                || target.door.openingBase.markGuid != APINULLGuid)
                return CreateErrorResponse (APIERR_BADID, "Door DELETE requires a freshly resolved exact markerless target.");
        }
    }
    if (operation == "delete" && typeID == API_WindowID) {
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* id = item.Get ("elementId");
            const API_Guid requestedGuid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
            if (requestedGuid == APINULLGuid) return CreateErrorResponse (APIERR_BADPARS, "Window DELETE requires an exact elementId GUID.");
            API_Element target = {};
            target.header.guid = requestedGuid;
            if (ACAPI_Element_Get (&target) != NoError || target.header.guid != requestedGuid
                || GetElemTypeId (target.header) != API_WindowID || target.window.openingBase.markGuid != APINULLGuid)
                return CreateErrorResponse (APIERR_BADID, "Window DELETE requires a freshly resolved exact markerless target.");
        }
    }
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
    // markerless Door writer.
    // guard before this same typed Door path.
    if (typeID == API_WallID || typeID == API_ColumnID || typeID == API_TextID || typeID == API_DoorID || typeID == API_WindowID ||
        typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID) {
        GS::ObjectState nativeParameters = BuildNativeMutationParameters (operation, typeName, items);
        nativeParameters.Add ("elementType", typeName);
        if (operation == "update")
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
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"operationId":{"type":"string"},"expectedFileScopeKey":{"type":"string"},"requestHash":{"type":"string"},"operation":{"type":"string","enum":["create","update","delete"]},"elementType":{"type":"string"},"items":{"type":"array","minItems":1,"maxItems":25,"items":{"type":"object"}},"origin":{"type":"string","enum":["NATIVE","SEMANTIC"]},"description":{"type":"string"},"operatorNote":{"type":"string","maxLength":1000},"semanticMetadata":{"type":"object"}},"additionalProperties":false,"required":["contractVersion","operationId","expectedFileScopeKey","requestHash","operation","elementType","items","description"]})";
}
GS::Optional<GS::UniString> MutateNativeElementsCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> MutateNativeElementsCommand::GetRawResponseSchema () const { return {}; }
GS::ObjectState MutateNativeElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
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
    GS::ObjectState response;
    bool groupModeRestored = false;
    if (mutationResult.Get ("groupModeRestored", groupModeRestored))
        response.Add ("groupModeRestored", groupModeRestored);
    Int32 groupModeRestoreError = 0;
    if (mutationResult.Get ("groupModeRestoreError", groupModeRestoreError))
        response.Add ("groupModeRestoreError", groupModeRestoreError);
    // BuildReceipt deliberately has no durable side effect.  Give the APX
    // response a process-local sequence for diagnostics and remember the
    // factual receipt before returning it so MCP can reconcile a lost HTTP
    // response without dispatching the Archicad command a second time.
    receipt.Add ("nativeSequence", NextCachedReceiptSequence ());
    RememberCachedReceipt (receipt);
    response.Add ("contractVersion", ContractVersion);
    response.Add ("status", "success");
    response.Add ("receipt", receipt);
    response.Add ("mutationResult", mutationResult);
    response.Add ("readback", after);
    if (operation == "delete") response.Add ("deleteReadback", deleteReadback);
    return response;
}
