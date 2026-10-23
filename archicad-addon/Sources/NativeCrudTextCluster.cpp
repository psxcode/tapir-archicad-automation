#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

using namespace NativeCrudInternal;

namespace {

GS::Array<GS::ObjectState> NormalizeClusterIds (const GS::ObjectState& survivor, const GS::Array<GS::ObjectState>& deleted)
{
    GS::Array<GS::ObjectState> ids;
    const GS::ObjectState* survivorId = survivor.Get ("elementId");
    if (survivorId != nullptr) ids.Push (*survivorId);
    for (const GS::ObjectState& member : deleted) {
        const GS::ObjectState* id = member.Get ("elementId");
        if (id != nullptr) ids.Push (*id);
    }
    return ids;
}

void ApplyNormalizeTextPlacement (API_Element& element, API_Element& mask, const GS::ObjectState& payload)
{
    const GS::ObjectState* coordinate = payload.Get ("coordinate");
    if (coordinate != nullptr) {
        const API_Coord3D point = Get3DCoordinateFromObjectState (*coordinate);
        element.text.loc.x = point.x;
        element.text.loc.y = point.y;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, loc);
    }
    Int32 floorIndex = 0;
    // Native CRUD canonical payloads use storyIndex.  Accept floorIndex as a
    // compatibility alias for direct developer callers, but never require
    // the semantic adapter to translate the field a second time.
    if (payload.Get ("storyIndex", floorIndex) || payload.Get ("floorIndex", floorIndex)) {
        element.header.floorInd = static_cast<short> (floorIndex);
        ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
    }
    double angle = 0.0;
    if (payload.Get ("angle", angle)) {
        element.text.angle = angle;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, angle);
    }
    Int32 anchor = 0;
    if (payload.Get ("anchor", anchor)) {
        element.text.anchor = static_cast<API_AnchorID> (anchor);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, anchor);
    }
    double width = 0.0;
    if (payload.Get ("width", width)) {
        element.text.width = width;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, width);
    }
    double height = 0.0;
    if (payload.Get ("height", height)) {
        element.text.height = height;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, height);
    }
    Int32 pen = 0;
    if (payload.Get ("pen", pen)) {
        element.text.pen = static_cast<short> (pen);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, pen);
    }
    bool fixedSize = false;
    if (payload.Get ("fixedSize", fixedSize)) {
        element.text.fixedSize = fixedSize;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, fixedSize);
    }
    bool nonBreaking = false;
    if (payload.Get ("nonBreaking", nonBreaking)) {
        element.text.nonBreaking = nonBreaking;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, nonBreaking);
    }
}

GS::ObjectState NormalizeTextClusterError (const char* message)
{
    return CreateErrorResponse (APIERR_BADPARS, GS::UniString (message));
}

} // namespace

NormalizeTextClusterCommand::NormalizeTextClusterCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String NormalizeTextClusterCommand::GetName () const { return "NormalizeTextCluster"; }
GS::Optional<GS::UniString> NormalizeTextClusterCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"operationId":{"type":"string"},"expectedFileScopeKey":{"type":"string"},"requestHash":{"type":"string"},"operation":{"type":"string","enum":["normalize_multiline"]},"elementType":{"type":"string","enum":["Text"]},"description":{"type":"string"},"origin":{"type":"string","enum":["NATIVE","SEMANTIC"]},"operatorNote":{"type":"string","maxLength":1000},"semanticMetadata":{"type":"object"},"survivor":{"type":"object","properties":{"elementId":{"type":"object"},"payload":{"type":"object"},"nativePrecondition":{"type":"object","properties":{"expectedFingerprint":{"type":"string"}},"required":["expectedFingerprint"]}},"required":["elementId","payload","nativePrecondition"]},"deleteMembers":{"type":"array","minItems":1,"maxItems":24,"items":{"type":"object","properties":{"elementId":{"type":"object"},"nativePrecondition":{"type":"object","properties":{"expectedFingerprint":{"type":"string"}},"required":["expectedFingerprint"]}},"required":["elementId","nativePrecondition"]}}},"additionalProperties":false,"required":["contractVersion","operationId","expectedFileScopeKey","requestHash","operation","elementType","description","survivor","deleteMembers"]})";
}
GS::Optional<GS::UniString> NormalizeTextClusterCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> NormalizeTextClusterCommand::GetRawResponseSchema () const { return {}; }

GS::ObjectState NormalizeTextClusterCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    GS::ObjectState scopeParameters;
    GS::UniString expectedScope;
    parameters.Get ("expectedFileScopeKey", expectedScope);
    scopeParameters.Add ("fileScopeKey", expectedScope);
    const GS::Optional<GS::UniString> scopeError = ValidateScope (scopeParameters);
    if (scopeError.HasValue ())
        return ScopeError (scopeError);

    GS::UniString contract;
    GS::UniString operation;
    GS::UniString elementType;
    GS::UniString operationId;
    GS::UniString requestHash;
    GS::UniString description;
    GS::UniString origin;
    GS::UniString operatorNote;
    if (!parameters.Get ("contractVersion", contract) || contract != ContractVersion
        || !parameters.Get ("operation", operation) || operation != "normalize_multiline"
        || !parameters.Get ("elementType", elementType) || elementType != "Text"
        || !parameters.Get ("operationId", operationId) || operationId.IsEmpty ()
        || !parameters.Get ("requestHash", requestHash) || !IsHex64 (requestHash)
        || !parameters.Get ("description", description) || description.IsEmpty ())
        return NormalizeTextClusterError ("NormalizeTextCluster identity is invalid.");
    parameters.Get ("origin", origin);
    parameters.Get ("operatorNote", operatorNote);
    if (operationId.GetLength () > 200 || description.GetLength () > 500 || operatorNote.GetLength () > 1000)
        return NormalizeTextClusterError ("NormalizeTextCluster identity exceeds the native length limit.");

    const GS::ObjectState* survivor = parameters.Get ("survivor");
    GS::Array<GS::ObjectState> deleteMembers;
    if (survivor == nullptr || !parameters.Get ("deleteMembers", deleteMembers)
        || deleteMembers.IsEmpty () || deleteMembers.GetSize () > 24)
        return NormalizeTextClusterError ("NormalizeTextCluster requires one survivor and one to twenty-four deleted Text members.");
    const GS::ObjectState* survivorId = survivor->Get ("elementId");
    const GS::ObjectState* survivorPayload = survivor->Get ("payload");
    const GS::ObjectState* survivorPrecondition = survivor->Get ("nativePrecondition");
    if (survivorId == nullptr || survivorPayload == nullptr || survivorPrecondition == nullptr)
        return NormalizeTextClusterError ("NormalizeTextCluster survivor requires elementId, payload and nativePrecondition.");
    const GS::ObjectState* richText = survivorPayload->Get ("richText");
    GS::UniString expectedFingerprint;
    if (richText == nullptr || !survivorPrecondition->Get ("expectedFingerprint", expectedFingerprint) || !IsHex64 (expectedFingerprint))
        return NormalizeTextClusterError ("NormalizeTextCluster survivor requires Rich Text and a 64-hex expectedFingerprint.");

    const GS::Array<GS::ObjectState> ids = NormalizeClusterIds (*survivor, deleteMembers);
    if (ids.GetSize () < 2 || ids.GetSize () > 25)
        return NormalizeTextClusterError ("NormalizeTextCluster requires two to twenty-five exact Text GUIDs.");
    GS::Array<API_Guid> seen;
    for (const GS::ObjectState& id : ids) {
        const API_Guid guid = GetRequestedElementGuid (id);
        if (guid == APINULLGuid || seen.Contains (guid)) return NormalizeTextClusterError ("NormalizeTextCluster GUIDs must be valid and unique.");
        seen.Push (guid);
    }
    for (const GS::ObjectState& member : deleteMembers) {
        const GS::ObjectState* memberPrecondition = member.Get ("nativePrecondition");
        if (member.Get ("elementId") == nullptr || memberPrecondition == nullptr) return NormalizeTextClusterError ("Every deleted member requires elementId and nativePrecondition.");
        GS::UniString memberFingerprint;
        if (!memberPrecondition->Get ("expectedFingerprint", memberFingerprint) || !IsHex64 (memberFingerprint))
            return NormalizeTextClusterError ("Every deleted member requires a 64-hex expectedFingerprint.");
    }

    const GS::UniString scope = CurrentSoloFileScope ().Get ();
    const GS::Optional<GS::ObjectState> cached = FindCachedReceipt (operationId, scope);
    if (cached.HasValue ()) {
        GS::UniString recordedOperation;
        GS::UniString recordedHash;
        if (!cached.Get ().Get ("operation", recordedOperation) || !cached.Get ().Get ("requestHash", recordedHash)
            || recordedOperation != operation || LowerAscii (recordedHash) != LowerAscii (requestHash))
            return NativeContractError ("DUPLICATE_OPERATION_CONFLICT", "operationId already identifies a different normalization request.");
        return GS::ObjectState ("status", "success", "receipt", cached.Get (), "duplicateOperation", true);
    }

    bool beforeComplete = false;
    const GS::ObjectState before = ExactSnapshots (ids, scope, processControl, beforeComplete);
    if (!beforeComplete) return CreateErrorResponse (APIERR_BADID, "SNAPSHOT_COVERAGE_INCOMPLETE: Text cluster before-state is incomplete; no mutation was dispatched.");
    GS::Array<GS::ObjectState> beforeRows;
    before.Get ("elements", beforeRows);
    for (GSSize index = 0; index < static_cast<GSSize> (ids.GetSize ()); ++index) {
        const GS::ObjectState& member = index == 0 ? *survivor : deleteMembers[index - 1];
        const GS::ObjectState* precondition = member.Get ("nativePrecondition");
        GS::UniString expected;
        precondition->Get ("expectedFingerprint", expected);
        const API_Guid guid = GetRequestedElementGuid (ids[index]);
        const GS::ObjectState* snapshot = nullptr;
        for (const GS::ObjectState& row : beforeRows) {
            const GS::ObjectState* rowId = row.Get ("elementId");
            if (rowId != nullptr && GetGuidFromObjectState (*rowId) == guid) snapshot = &row;
        }
        GS::UniString actualType;
        GS::UniString actualFingerprint;
        if (snapshot == nullptr || !snapshot->Get ("elementType", actualType) || actualType != "Text"
            || !snapshot->Get ("fingerprint", actualFingerprint) || LowerAscii (actualFingerprint) != LowerAscii (expected))
            return CreateErrorResponse (APIERR_BADID, "STALE_NATIVE_SNAPSHOT: Text cluster member type or fingerprint changed before dispatch.");
    }

    bool groupsWereSuspended = false;
    GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsWereSuspended);
    if (groupModeError != NoError)
        return CreateErrorResponse (groupModeError, "Failed to read the Suspend Groups mode.");
    bool groupModeNeedsRestore = !groupsWereSuspended;
    if (groupModeNeedsRestore) {
        groupModeError = TAPIR_Grouping_ChangeSuspendGroup (true);
        if (groupModeError != NoError)
            return CreateErrorResponse (groupModeError, "Failed to suspend groups for Text-cluster normalization.");
    }
    const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
        if (groupModeNeedsRestore)
            TAPIR_Grouping_ChangeSuspendGroup (false);
    });

    GS::ObjectState mutationResult;
    GSErrCode callbackError = NoError;
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Normalize Text Cluster", [&]() -> GSErrCode {
        API_Element element = {};
        API_Element mask = {};
        API_ElementMemo memo = {};
        NativeMemoGuard memoGuard (memo);
        element.header.guid = GetRequestedElementGuid (*survivorId);
        GSErrCode error = ACAPI_Element_Get (&element);
        if (error != NoError) return callbackError = error;
        GS::UniString richTextError;
        error = NativeRichTextMemo::BuildMemo (*richText, memo, element.text, richTextError);
        if (error != NoError) return callbackError = error;
        ApplyNormalizeTextPlacement (element, mask, *survivorPayload);
        error = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_TextContentUni | APIMemoMask_ParagraphUni, true);
        if (error != NoError) return callbackError = error;
        GS::Array<API_Guid> deletedGuids;
        for (const GS::ObjectState& member : deleteMembers)
            deletedGuids.Push (GetRequestedElementGuid (*member.Get ("elementId")));
        error = ACAPI_Element_Delete (deletedGuids);
        if (error != NoError) return callbackError = error;
        return NoError;
    });
    if (undoError != NoError) callbackError = undoError;
    const bool dispatchComplete = callbackError == NoError;

    // A failed UI-mode restoration is a post-dispatch side effect, not a
    // reason to retry the already completed element mutation.  Surface it in
    // the native result and keep the scope guard armed for one last retry.
    GSErrCode groupModeRestoreError = NoError;
    if (groupModeNeedsRestore) {
        groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (false);
        if (groupModeRestoreError == NoError)
            groupModeNeedsRestore = false;
    }
    mutationResult.Add ("groupModeRestored", groupModeRestoreError == NoError);
    if (groupModeRestoreError != NoError)
        mutationResult.Add ("groupModeRestoreError", groupModeRestoreError);

    GS::Array<GS::ObjectState> survivorIdArray;
    survivorIdArray.Push (*survivorId);
    bool afterComplete = false;
    const GS::ObjectState after = dispatchComplete
        ? ExactSnapshots (survivorIdArray, scope, processControl, afterComplete)
        : GS::ObjectState ("elements", GS::ObjectState ());
    bool deletedAbsent = dispatchComplete;
    const auto& deletedReadback = (mutationResult.AddList<GS::ObjectState> ("deleted"));
    if (dispatchComplete) {
        for (const GS::ObjectState& member : deleteMembers) {
            const API_Guid guid = GetRequestedElementGuid (*member.Get ("elementId"));
            API_Elem_Head header = {};
            header.guid = guid;
            const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
            const bool absent = headerError == APIERR_BADID;
            if (!absent) deletedAbsent = false;
            deletedReadback (GS::ObjectState ("elementId", *member.Get ("elementId"), "absent", absent));
        }
    }
    const bool complete = dispatchComplete && afterComplete && deletedAbsent;
    const API_Guid survivorGuid = GetRequestedElementGuid (*survivorId);
    GS::ObjectState receipt = BuildReceipt (
        scope, operationId, operation, elementType, description, origin, requestHash,
        before, after, ids, ids, complete, dispatchComplete, parameters.Get ("semanticMetadata"),
        operatorNote, nullptr, nullptr, &survivorGuid);
    receipt.Add ("nativeSequence", NextCachedReceiptSequence ());
    RememberCachedReceipt (receipt);
    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("status", "success");
    response.Add ("receipt", receipt);
    response.Add ("mutationComplete", dispatchComplete);
    response.Add ("readbackVerified", complete);
    response.Add ("mutationResult", mutationResult);
    response.Add ("readback", after);
    return response;
}
