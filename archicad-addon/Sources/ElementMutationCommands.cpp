#include "ElementMutationCommands.hpp"

#include "ElementMutationInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementGDLParameterCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "NotificationCommands.hpp"

using namespace ElementMutationInternal;

MutateElementsCommand::MutateElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String MutateElementsCommand::GetName () const
{
    return "MutateElements";
}

GS::Optional<GS::UniString> MutateElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "description": "Typed native CRUD envelope. One operation and one element type per request.",
        "properties": {
            "operation": {
                "type": "string",
                "enum": ["create", "update", "delete"]
            },
            "elementType": {
                "type": "string",
                "enum": ["Wall", "Slab", "Column", "Beam", "Text", "Line", "Arc", "Hatch", "Door"]
            },
            "items": {
                "type": "array",
                "minItems": 1,
                "maxItems": 500,
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "cloneSourceElementId": { "$ref": "#/ElementId" },
                        "payload": {
                            "type": "object",
                            "properties": {
                                "begCoordinate": { "$ref": "#/Coordinate2D" },
                                "endCoordinate": { "$ref": "#/Coordinate2D" },
                                "coordinates": { "$ref": "#/Coordinate3D" },
                                "origin": { "$ref": "#/Coordinate2D" },
                                "floorIndex": { "type": "integer" },
                                "zCoordinate": { "type": "number" },
                                "level": { "type": "number" },
                                "height": { "type": "number" },
                                "width": { "type": "number" },
                                "depth": { "type": "number" },
                                "thickness": { "type": "number" },
                                "bottomOffset": { "type": "number" },
                                "offset": { "type": "number" },
                                "slantAngle": { "type": "number" },
                                "isSlanted": { "type": "boolean" },
                                "profileAngle": { "type": "number" },
                                "arcAngle": { "type": "number" },
                                "verticalCurveHeight": { "type": "number" },
                                "axisRotationAngle": { "type": "number" },
                                "referenceLineLocation": { "type": "string" },
                                "referencePlaneLocation": { "type": "string" },
                                "structureType": { "type": "string" },
                                "buildingMaterialId": { "$ref": "#/AttributeId" },
                                "compositeId": { "$ref": "#/AttributeId" },
                                "profileId": { "$ref": "#/AttributeId" },
                                "anchorPoint": { "type": "string" },
                                "coreAnchor": { "type": "string" },
                                "circleBased": { "type": "boolean" },
                                "isWidthAndHeightLinked": { "type": "boolean" },
                                "polygonCoordinates": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3
                                },
                                "polygonOutline": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3
                                },
                                "hatchCoordinates": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3,
                                    "maxItems": 64
                                },
                                "floorInd": { "type": "integer" },
                                "contourPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillBackgroundPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillAttributeIndex": { "type": "integer", "minimum": 1 },
                                "polygonArcs": {
                                    "type": "array",
                                    "items": { "$ref": "#/PolyArc" }
                                },
                                "holes": { "$ref": "#/Holes2D" }
                                ,"richText": {
                                    "type": "object",
                                    "description": "Versioned native Unicode Rich Text memo with paragraph and run style data.",
                                    "properties": {
                                        "schemaVersion": { "enum": ["native-rich-text-v1"] },
                                        "encoding": { "enum": ["unicode"] },
                                        "offsets": {
                                            "type": "object",
                                            "properties": {
                                                "unit": { "enum": ["archicad-unicode-character"] },
                                                "paragraph": { "enum": ["content"] },
                                                "run": { "enum": ["paragraph"] },
                                                "eol": { "enum": ["paragraph"] }
                                            },
                                            "additionalProperties": false,
                                            "required": ["unit", "paragraph", "run", "eol"]
                                        },
                                        "content": { "type": "string", "maxLength": 1000000 },
                                        "paragraphs": {
                                            "type": "array",
                                            "minItems": 1,
                                            "maxItems": 4096,
                                            "items": {
                                                "type": "object",
                                                "properties": {
                                                    "from": { "type": "integer", "minimum": 0 },
                                                    "range": { "type": "integer", "minimum": 0 },
                                                    "justification": { "type": "integer" },
                                                    "firstIndentMm": { "type": "number" },
                                                    "indentMm": { "type": "number" },
                                                    "rightIndentMm": { "type": "number" },
                                                    "spacing": { "type": "number" },
                                                    "tabs": {
                                                        "type": "array",
                                                        "maxItems": 256,
                                                        "items": {
                                                            "type": "object",
                                                            "properties": {
                                                                "type": { "type": "integer", "minimum": 0 },
                                                                "positionMm": { "type": "number" }
                                                            },
                                                            "additionalProperties": false,
                                                            "required": ["type", "positionMm"]
                                                        }
                                                    },
                                                    "runs": {
                                                        "type": "array",
                                                        "minItems": 1,
                                                        "maxItems": 4096,
                                                        "items": {
                                                            "type": "object",
                                                            "properties": {
                                                                "from": { "type": "integer", "minimum": 0 },
                                                                "range": { "type": "integer", "minimum": 0 },
                                                                "pen": { "type": "integer" },
                                                                "fontIndex": { "type": "integer" },
                                                                "sizeMm": { "type": "number" },
                                                                "faceBits": { "type": "integer", "minimum": 0 },
                                                                "effectBits": { "type": "integer", "minimum": 0 }
                                                            },
                                                            "additionalProperties": false,
                                                            "required": ["from", "range", "pen", "fontIndex", "sizeMm", "faceBits", "effectBits"]
                                                        }
                                                    },
                                                    "eolPositions": {
                                                        "type": "array",
                                                        "maxItems": 4096,
                                                        "items": { "type": "integer", "minimum": 0 }
                                                    },
                                                    "widthMm": { "type": "number", "description": "Output-only paragraph measurement; ignored on write." },
                                                    "heightMm": { "type": "number", "description": "Output-only paragraph measurement; ignored on write." }
                                                },
                                                "additionalProperties": false,
                                                "required": ["from", "range", "justification", "firstIndentMm", "indentMm", "rightIndentMm", "spacing", "tabs", "runs", "eolPositions"]
                                            }
                                        }
                                    },
                                    "additionalProperties": false,
                                    "required": ["schemaVersion", "encoding", "offsets", "content", "paragraphs"]
                                },
                                "coordinate": { "$ref": "#/Coordinate3D" },
                                "angle": { "type": "number" },
                                "anchor": { "type": "integer", "minimum": 0, "maximum": 8 },
                                "fixedSize": { "type": "boolean" },
                                "nonBreaking": { "type": "boolean" },
                                "pen": { "type": "integer" },
                                "linePen": { "type": "integer" },
                                "linePenIndex": { "type": "integer" },
                                "lineTypeIndex": { "type": "integer" },
                                "roomSeparator": { "type": "boolean" },
                                "radius": { "type": "number", "exclusiveMinimum": true },
                                "axisAngle": { "type": "number" },
                                "ratio": { "type": "number", "exclusiveMinimum": true },
                                "begAngle": { "type": "number" },
                                "beginAngle": { "type": "number" },
                                "endAngle": { "type": "number" },
                                "reflected": { "type": "boolean" }
                                ,"ownerWallId": { "$ref": "#/ElementId" }
                                ,"centerOffset": { "type": "number" }
                                ,"sillHeight": { "type": "number" }
                                ,"refSide": { "type": "boolean" }
                                ,"oSide": { "type": "boolean" }
                            },
                            "additionalProperties": false
                        }
                    },
                    "additionalProperties": false
                }
            }
        },
        "additionalProperties": false,
        "required": ["operation", "elementType", "items"]
    })";
}

GS::Optional<GS::UniString> MutateElementsCommand::GetResponseSchema () const
{
    // The command returns either the typed success envelope below or the
    // framework-wide {"error": {code, message}} response from preflight.
    // Handing only the success schema to Archicad would make it discard the
    // useful error response as a schema-validation failure.
    return {};
}

GS::Optional<GS::UniString> MutateElementsCommand::GetRawResponseSchema () const
{
    // nativeDetails intentionally remains an open object.  It is the existing
    // detail projection and can contain bounded, type-specific fields.  The
    // envelope itself remains schema-checked, including explicit partial and
    // readback status.
    return R"({
        "type": "object",
        "properties": {
            "operation": { "type": "string", "enum": ["create", "update", "delete"] },
            "elementType": { "type": "string", "enum": ["Wall", "Slab", "Column", "Beam", "Text", "Line", "Arc", "Hatch"] },
            "requestedCount": { "type": "integer", "minimum": 0 },
            "appliedCount": { "type": "integer", "minimum": 0 },
            "mutationComplete": { "type": "boolean" },
            "readbackVerified": { "type": "boolean" },
            "partial": { "type": "boolean" },
            "mutationResult": { "type": "object" },
            "readback": { "type": "object" }
        },
        "additionalProperties": false,
        "required": [
            "operation", "elementType", "requestedCount", "appliedCount", "mutationComplete",
            "readbackVerified", "partial", "mutationResult", "readback"
        ]
    })";
}

GS::ObjectState MutateElementsCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl) const
{
    for (const GS::String& fieldName : parameters.GetFieldNames ()) {
        if (GS::UniString (fieldName).BeginsWith ("nativeCandidate"))
            return CreateErrorResponse (APIERR_BADPARS, "Native candidate writer metadata is retired (D-307).");
    }

    GS::UniString operation;
    if (!parameters.Get ("operation", operation) || !IsOneOf (operation, { "create", "update", "delete" })) {
        return CreateErrorResponse (APIERR_BADPARS, "'operation' must be one of 'create', 'update' or 'delete'.");
    }

    GS::UniString elementTypeName;
    if (!parameters.Get ("elementType", elementTypeName)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'elementType' field.");
    }
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (elementTypeName);
    const bool isText = typeID == API_TextID;
    bool nativeCrudAtomic = false;
    parameters.Get ("nativeCrudAtomic", nativeCrudAtomic);
    bool nativeCrudHatchWriter = false;
    parameters.Get ("nativeCrudHatchWriter", nativeCrudHatchWriter);
    if (nativeCrudHatchWriter && (typeID != API_HatchID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Hatch typed writer marker is valid only for Hatch CREATE or UPDATE.");
    bool nativeCrudDoorWriter = false;
    const bool hasNativeCrudDoorWriter = parameters.Get ("nativeCrudDoorWriter", nativeCrudDoorWriter);
    if (parameters.Contains ("nativeCrudDoorWriter") && !hasNativeCrudDoorWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Door typed writer marker must be a boolean.");
    if (nativeCrudDoorWriter && (typeID != API_DoorID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Door typed writer marker is valid only for Door CREATE or UPDATE.");
    if (typeID == API_DoorID && (operation == "create" || operation == "update")
        && !nativeCrudDoorWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Door CREATE/UPDATE requires the typed Native CRUD writer route.");
    bool nativeCrudWindowWriter = false;
    const bool hasNativeCrudWindowWriter = parameters.Get ("nativeCrudWindowWriter", nativeCrudWindowWriter);
    if (parameters.Contains ("nativeCrudWindowWriter") && !hasNativeCrudWindowWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Window typed writer marker must be a boolean.");
    if (nativeCrudWindowWriter && (typeID != API_WindowID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Window typed writer marker is valid only for Window CREATE or UPDATE.");
    if (typeID == API_WindowID && (operation == "create" || operation == "update") && !nativeCrudWindowWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Window CREATE/UPDATE requires its typed Native CRUD writer route.");
    if (typeID == API_HatchID && (operation == "create" || operation == "update") && !nativeCrudHatchWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Hatch CREATE/UPDATE requires the validated typed Native CRUD writer route.");
    bool nativeCrudTestFaultAfterFirstApplied = false;
    const bool hasNativeCrudTestFaultAfterFirstApplied = parameters.Get ("nativeCrudTestFaultAfterFirstApplied", nativeCrudTestFaultAfterFirstApplied);
    if (parameters.Contains ("nativeCrudTestFaultAfterFirstApplied") && !hasNativeCrudTestFaultAfterFirstApplied)
        return CreateErrorResponse (APIERR_BADPARS, "Native CRUD first-applied fault metadata must be a boolean.");
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (hasNativeCrudTestFaultAfterFirstApplied)
        return CreateErrorResponse (APIERR_BADPARS, "Native CRUD first-applied fault metadata is unavailable in this ordinary APX build.");
#else
    if (nativeCrudTestFaultAfterFirstApplied && ((typeID != API_ColumnID && typeID != API_LineID && typeID != API_ArcID && typeID != API_HatchID && typeID != API_DoorID && typeID != API_WindowID) || operation != "update"))
        return CreateErrorResponse (APIERR_BADPARS, "First-applied fault seam is valid only for Column, Line, Arc, Hatch, Door or Window UPDATE.");
#endif
    const bool isDoor = typeID == API_DoorID;
    const bool isWindow = typeID == API_WindowID;
    const ElementTypeSpec* typeSpec = (isText || isDoor || isWindow) ? nullptr : GetElementTypeSpec (typeID);
    if (!isText && !isDoor && !isWindow && typeSpec == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "'elementType' must be Wall, Slab, Column, Beam, Text, Line, Arc, Hatch or guarded Door/Window.");
    }

    GS::Array<GS::ObjectState> items;
    if (!parameters.Get ("items", items)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required array field 'items'.");
    }
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) {
        return CreateErrorResponse (APIERR_BADPARS, "'items' must contain between 1 and 500 entries.");
    }
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (nativeCrudTestFaultAfterFirstApplied && items.GetSize () != 2)
        return CreateErrorResponse (APIERR_BADPARS, "First-applied fault seam requires exactly two UPDATE items.");
#endif

    GS::Array<GS::ObjectState> normalizedItems;
    GS::Array<API_Guid> requestedElementGuids;
    for (const GS::ObjectState& item : items) {
        auto error = ValidateEnvelopeItem (item, operation, typeID, normalizedItems, requestedElementGuids);
        if (error.HasValue ()) {
            // The complete preflight runs before any delegated executor is
            // called.  Invalid enum/range/reference/conflict input therefore
            // cannot result in a native mutation.
            return CreateErrorResponse (APIERR_BADPARS, error.Get ());
        }
    }

    // Validate the exact angle-only target before touching the operator's
    // Suspend Groups mode.  The edit lane repeats this check after entering
    // the undoable command because the native element must still be fresh at
    // the actual edit boundary.
    if (isText && operation == "update" && normalizedItems.GetSize () == 1
        && IsAngleOnlyTextItem (normalizedItems[0])) {
        const GSErrCode rotationPreflightError = ValidateAngleOnlyTextItem (normalizedItems[0]);
        if (rotationPreflightError != NoError)
            return CreateErrorResponse (rotationPreflightError, "Text angle-only update target is fixed, linked, grouped, or unavailable.");
    }

    // Archicad rejects exact changes to members of a group while Suspend
    // Groups is off.  Mutations are already serialized on Archicad's main
    // thread, so temporarily suspend groups for this synchronous command and
    // restore the operator's prior UI mode on every exit path.  This changes
    // neither group membership nor the undo payload.
    bool groupModeNeedsRestore = false;
    if (operation != "create") {
        bool groupsWereSuspended = false;
        GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsWereSuspended);
        if (groupModeError != NoError)
            return CreateErrorResponse (groupModeError, "Failed to read the Suspend Groups mode.");
        groupModeNeedsRestore = !groupsWereSuspended;
        if (groupModeNeedsRestore) {
            groupModeError = TAPIR_Grouping_ChangeSuspendGroup (true);
            if (groupModeError != NoError)
                return CreateErrorResponse (groupModeError, "Failed to suspend groups for exact element mutation.");
        }
    }
    const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
        if (groupModeNeedsRestore)
            TAPIR_Grouping_ChangeSuspendGroup (false);
    });

    GS::ObjectState mutationParameters;
    if ((isDoor || isWindow) && operation == "create") {
        mutationParameters = BuildArrayParameters ("items", normalizedItems);
    } else if (!isText && !isDoor && !isWindow && operation == "create") {
        mutationParameters = BuildArrayParameters (typeSpec->createArrayName, normalizedItems);
        if (nativeCrudHatchWriter)
            mutationParameters.Add ("nativeCrudHatchWriter", true);
    } else if ((isDoor || isWindow) && operation == "update") {
        mutationParameters = BuildArrayParameters ("items", normalizedItems);
        mutationParameters.Add ("nativeCrudAtomic", true);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (nativeCrudTestFaultAfterFirstApplied) mutationParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
#endif
    } else if (!isText && !isDoor && !isWindow && operation == "update") {
        mutationParameters = BuildArrayParameters (typeSpec->updateArrayName, normalizedItems);
        if (nativeCrudAtomic)
            mutationParameters.Add ("nativeCrudAtomic", true);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        // This top-level internal key is not part of the MutateElements input
        // schema. NativeCrudCommands adds it only after exact candidate
        // manifest validation; copy it solely into existing atomic typed
        // executors that implement the first-applied rollback seam.
        if ((typeID == API_ColumnID || typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID) && nativeCrudTestFaultAfterFirstApplied)
            mutationParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
#endif
    }

    GS::ObjectState mutationResult;
    if (isText && operation == "create") {
        mutationResult = ExecuteTextCreate (normalizedItems, processControl);
    } else if (isText && operation == "update") {
        mutationResult = ExecuteTextUpdate (normalizedItems, processControl);
    } else if (isWindow && operation == "create") {
        mutationResult = ExecuteWindowCreate (normalizedItems, processControl);
    } else if (isWindow && operation == "update") {
        mutationResult = ExecuteWindowUpdate (normalizedItems, nativeCrudTestFaultAfterFirstApplied, processControl);
    } else if (isDoor && operation == "create") {
        mutationResult = ExecuteDoorCreate (normalizedItems, processControl);
    } else if (isDoor && operation == "update") {
        mutationResult = ExecuteDoorUpdate (normalizedItems, nativeCrudTestFaultAfterFirstApplied, processControl);
    } else if (operation == "create") {
        mutationResult = ExecuteCreate (typeID, mutationParameters, processControl);
    } else if (operation == "update") {
        mutationResult = ExecuteUpdate (typeID, mutationParameters, processControl);
    } else {
        mutationResult = ExecuteDelete (normalizedItems, processControl);
    }

    // Restore the operator's UI mode before producing the response so a
    // failure is factual and observable.  The OnExit guard remains armed as
    // one final best-effort retry, but the element mutation is never retried.
    GSErrCode groupModeRestoreError = NoError;
    if (groupModeNeedsRestore) {
        groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (false);
        if (groupModeRestoreError == NoError)
            groupModeNeedsRestore = false;
    }
    mutationResult.Add ("groupModeRestored", groupModeRestoreError == NoError);
    if (groupModeRestoreError != NoError)
        mutationResult.Add ("groupModeRestoreError", groupModeRestoreError);

    GS::Array<API_Guid> changedGuids;
    if (operation == "create") {
        changedGuids = CollectCreatedGuids (mutationResult);
    } else if (operation == "update") {
        changedGuids = CollectUpdatedGuids (mutationResult, requestedElementGuids);
    } else {
        changedGuids = CollectDeletedGuids (mutationResult, requestedElementGuids);
    }

    const bool mutationComplete = MutationResultIsComplete (operation, mutationResult, items.GetSize ());
    // A partial update must still expose the observed state of every requested
    // element, including items whose delegated executor reported failure.  A
    // readback of successful items only can make a failed scalar/memo update
    // look as if the element had never been part of the operation.
    GS::Array<API_Guid> readbackGuids = changedGuids;
    if ((operation == "update" || operation == "delete") && !mutationComplete) {
        readbackGuids = requestedElementGuids;
    }

    // An empty readback cannot prove a non-empty request.  In particular, a
    // failed create may yield zero changed GUIDs; reporting verified=true in
    // that case would turn a missing postcondition into green evidence.
    bool readbackVerified = items.IsEmpty () || !readbackGuids.IsEmpty ();
    GSSize appliedCount = static_cast<GSSize> (changedGuids.GetSize ());
    GS::ObjectState readback;
    if (operation == "delete") {
        GSSize absentCount = 0;
        readback = BuildDeleteReadback (readbackGuids, typeID, readbackVerified, absentCount);
        // DeleteElements exposes one aggregate host result, so a failed
        // aggregate cannot identify applied items on its own.  The immediate
        // per-GUID readback is the authoritative evidence: count only GUIDs
        // whose absence was explicitly confirmed with APIERR_BADID.
        appliedCount = absentCount;
    } else {
        readback = BuildElementReadback (readbackGuids, typeID, processControl, readbackVerified);
    }

    GS::ObjectState response;
    response.Add ("operation", operation);
    response.Add ("elementType", elementTypeName);
    response.Add ("requestedCount", static_cast<Int32> (items.GetSize ()));
    response.Add ("appliedCount", static_cast<Int32> (appliedCount));
    response.Add ("mutationComplete", mutationComplete);
    response.Add ("readbackVerified", readbackVerified);
    response.Add ("partial", !mutationComplete || !readbackVerified);
    response.Add ("mutationResult", mutationResult);
    response.Add ("readback", readback);
    return response;
}
