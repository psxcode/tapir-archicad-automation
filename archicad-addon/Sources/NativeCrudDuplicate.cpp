#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"

#include <chrono>
#include <cmath>

using namespace NativeCrudInternal;

namespace {

constexpr const char* DuplicateReceiptVersion = "native-duplicate-receipt-v1";

constexpr GSSize MaxDuplicateSources = 20000;

constexpr API_ElemFilterFlags DuplicateEditableFilter = APIFilt_IsEditable | APIFilt_InMyWorkspace | APIFilt_HasAccessRight;

constexpr GSFlags LiftedLayerFlags = APILay_Hidden | APILay_Locked;

GS::ObjectState DuplicateError (const char* message)
{
    return CreateErrorResponse (APIERR_BADPARS, GS::UniString (message));
}

bool IsOpeningType (const API_ElemTypeID typeID)
{
    return typeID == API_DoorID || typeID == API_WindowID || typeID == API_SkylightID || typeID == API_OpeningID;
}

GSErrCode OpeningOwner (const API_Guid& guid, const API_ElemTypeID typeID, API_Guid& owner)
{
    owner = APINULLGuid;
    API_Element element = {};
    element.header.guid = guid;
    const GSErrCode error = ACAPI_Element_Get (&element);
    if (error != NoError)
        return error;
    switch (typeID) {
        case API_DoorID:     owner = element.door.owner; break;
        case API_WindowID:   owner = element.window.owner; break;
        case API_SkylightID: owner = element.skylight.owner; break;
        case API_OpeningID:  owner = element.opening.owner; break;
        default:             break;
    }
    return NoError;
}

// The opening types a host owns, which Archicad copies with it: the owner connections the
// API documents.  A generic Opening is not owned this way (it lists the elements it cuts), so
// the caller refuses it instead of guessing what the Drag does with it.
GS::Array<API_ElemTypeID> CarriedOpeningTypes (const API_ElemTypeID hostType)
{
    switch (hostType) {
        case API_WallID:  return { API_WindowID, API_DoorID };
        case API_RoofID:
        case API_ShellID: return { API_SkylightID };
        default:          return {};
    }
}

// The owner Archicad keeps for an element it creates beside the paired copies: a library
// part's owner (zone stamps, detail markers) or a hierarchical part's main element (column and
// beam segments, curtain wall, stair and railing parts).
GSErrCode CreatedOwner (const API_Guid& guid, const API_ElemTypeID typeID, API_Guid& owner)
{
    owner = APINULLGuid;
    if (typeID == API_ObjectID) {
        API_Element element = {};
        element.header.guid = guid;
        const GSErrCode error = ACAPI_Element_Get (&element);
        if (error == NoError)
            owner = element.object.owner;
        return error;
    }
    API_HierarchicalOwnerType ownerType = API_RootHierarchicalOwner;
    API_HierarchicalElemType elemType = API_UnknownElemType;
    API_Guid elemGuid = guid;
#ifdef ServerMainVers_2700
    return ACAPI_HierarchicalEditing_GetHierarchicalElementOwner (&elemGuid, &ownerType, &elemType, &owner);
#else
    return ACAPI_Goodies (APIAny_GetHierarchicalElementOwnerID, &elemGuid, &ownerType, &elemType, &owner);
#endif
}

GSErrCode BoundsOf (const API_Guid& guid, API_Box3D& bounds)
{
    API_Elem_Head header = {};
    header.guid = guid;
    const GSErrCode error = ACAPI_Element_GetHeader (&header);
    return error != NoError ? error : ACAPI_Element_CalcBounds (&header, &bounds);
}

// Whether the copy's box is the source's box moved by the offset.
bool IsTranslatedBox (const API_Box3D& source, const API_Box3D& copy, const double dx, const double dy)
{
    constexpr double Tolerance = 1e-6;
    return std::fabs (copy.xMin - source.xMin - dx) <= Tolerance && std::fabs (copy.xMax - source.xMax - dx) <= Tolerance
        && std::fabs (copy.yMin - source.yMin - dy) <= Tolerance && std::fabs (copy.yMax - source.yMax - dy) <= Tolerance
        && std::fabs (copy.zMin - source.zMin) <= Tolerance && std::fabs (copy.zMax - source.zMax) <= Tolerance;
}

// A conflict token over the copied state: FNV-1a 64 of the fields fed in order.  It guards the
// copy against a change between the caller's dry run and the Drag, not against an adversary.
class StateDigest {
public:
    void Add (const void* data, const size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*> (data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    }
    void Add (const API_Guid& guid) { Add (&guid, sizeof (guid)); }
    void Add (const UInt64 value) { Add (&value, sizeof (value)); }
    GS::UniString Hex () const
    {
        // Spelled out: the AC25 headers redefine snprintf.
        static const char digits[] = "0123456789abcdef";
        char buffer[17] = {};
        for (int index = 0; index < 16; ++index)
            buffer[index] = digits[(hash >> (60 - 4 * index)) & 0xF];
        return GS::UniString (buffer);
    }
private:
    UInt64 hash = 14695981039346656037ULL;
};

// The flags of a layer that keep Archicad's Drag away from its elements, or zero.
GSFlags LayerBlockingFlags (const API_AttributeIndex& index)
{
    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    layer.header.index = index;
    if (ACAPI_Attribute_Get (&layer) != NoError)
        return 0;
    return layer.header.flags & LiftedLayerFlags;
}

// Why Archicad would leave an element on a shown, unlocked layer out of the Drag.
const char* NotEditableReason (const API_Elem_Head& header)
{
    if (!ACAPI_Element_Filter (header.guid, APIFilt_InMyWorkspace)) return "SOURCE_NOT_IN_WORKSPACE";
    if (!ACAPI_Element_Filter (header.guid, APIFilt_HasAccessRight)) return "SOURCE_NO_ACCESS_RIGHT";
    return "SOURCE_NOT_EDITABLE";
}

// A hidden or locked layer of the source set, with the flags it had before the copy.
struct LiftedLayer {
    API_AttributeIndex index;
    GSFlags flags = 0;
};

GSErrCode SetLayerFlags (const API_AttributeIndex& index, const GSFlags flags)
{
    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    layer.header.index = index;
    const GSErrCode error = ACAPI_Attribute_Get (&layer);
    if (error != NoError)
        return error;
    layer.header.flags = (layer.header.flags & ~LiftedLayerFlags) | (flags & LiftedLayerFlags);
    return ACAPI_Attribute_Modify (&layer, nullptr);
}

GSErrCode GroupOf (const API_Guid& guid, API_Guid& group)
{
    group = APINULLGuid;
    return ACAPI_Grouping_GetGroup (guid, &group);
}

GSErrCode DirectGroupSize (const API_Guid& group, GSSize& size)
{
    GS::Array<API_Guid> members;
    const GSErrCode error = ACAPI_Grouping_GetGroupedElems (group, &members);
    size = members.GetSize ();
    return error;
}

// The elements a copy refers to: the host of an opening, the parent of a label, the
// elements a dimension measures.  A failed read is an error, never an empty list.
GSErrCode LinkedElements (const API_Guid& guid, const API_ElemTypeID typeID, GS::Array<API_Guid>& linked)
{
    linked.Clear ();
    if (IsOpeningType (typeID)) {
        API_Guid owner = APINULLGuid;
        const GSErrCode error = OpeningOwner (guid, typeID, owner);
        if (error != NoError)
            return error;
        linked.Push (owner);
    } else if (typeID == API_LabelID) {
        API_Element element = {};
        element.header.guid = guid;
        const GSErrCode error = ACAPI_Element_Get (&element);
        if (error != NoError)
            return error;
        linked.Push (element.label.parent);
    } else if (typeID == API_DimensionID) {
        API_ElementMemo memo = {};
        NativeMemoGuard memoGuard (memo);
        const GSErrCode error = ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_All);
        if (error != NoError)
            return error;
        if (memo.dimElems != nullptr) {
            const GSSize count = BMGetHandleSize (reinterpret_cast<GSHandle> (memo.dimElems)) / sizeof (API_DimElem);
            for (GSSize index = 0; index < count; ++index)
                linked.Push ((*memo.dimElems)[index].base.base.guid);
        }
    }
    return NoError;
}

// Every element of the project.  The created elements are the difference around the edit,
// which is the only complete record of what Archicad adds besides the copies it returns:
// openings carried by their host, column segments and zone stamps.
GSErrCode AllElementGuids (GS::HashSet<API_Guid>& result)
{
    GS::Array<API_Guid> guids;
    const GSErrCode error = ACAPI_Element_GetElemList (API_ZombieElemID, &guids);
    if (error != NoError)
        return error;
    for (const API_Guid& guid : guids)
        result.Add (guid);
    return NoError;
}

struct DuplicateSource {
    API_Guid guid = APINULLGuid;
    API_ElemTypeID typeID = API_ZombieElemID;
    short floorIndex = 0;
    UInt64 modiStamp = 0;
    API_Guid rootGroup = APINULLGuid;
};

// An opening Archicad copies with a listed host.
struct CarriedOpening {
    API_Guid guid = APINULLGuid;
    API_ElemTypeID typeID = API_ZombieElemID;
    API_Guid host = APINULLGuid;
    UInt64 modiStamp = 0;
};

} // namespace

DuplicateElementsByOffsetCommand::DuplicateElementsByOffsetCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String DuplicateElementsByOffsetCommand::GetName () const { return "DuplicateElementsByOffset"; }
GS::Optional<GS::UniString> DuplicateElementsByOffsetCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"operationId":{"type":"string"},"expectedFileScopeKey":{"type":"string"},"requestHash":{"type":"string"},"operation":{"type":"string","enum":["duplicate_by_offset"]},"mode":{"type":"string","enum":["dispatch","dryRun","lookup"]},"description":{"type":"string"},"origin":{"type":"string","enum":["NATIVE","SEMANTIC"]},"operatorNote":{"type":"string","maxLength":1000},"expectedSourceState":{"type":"string"},"semanticMetadata":{"type":"object"},"offset":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"required":["x","y"],"additionalProperties":false},"elements":{"type":"array","minItems":1,"maxItems":20000,"items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"required":["guid"]}},"required":["elementId"],"additionalProperties":false}}},"additionalProperties":false,"required":["contractVersion","operationId","expectedFileScopeKey","requestHash","operation","mode","description","offset","elements"]})";
}
GS::Optional<GS::UniString> DuplicateElementsByOffsetCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> DuplicateElementsByOffsetCommand::GetRawResponseSchema () const { return {}; }

GS::ObjectState DuplicateElementsByOffsetCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
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
    GS::UniString mode;
    GS::UniString operationId;
    GS::UniString requestHash;
    GS::UniString description;
    GS::UniString origin;
    GS::UniString operatorNote;
    if (!parameters.Get ("contractVersion", contract) || contract != ContractVersion
        || !parameters.Get ("operation", operation) || operation != "duplicate_by_offset"
        || !parameters.Get ("mode", mode) || (mode != "dispatch" && mode != "dryRun" && mode != "lookup")
        || !parameters.Get ("operationId", operationId) || operationId.IsEmpty ()
        || !parameters.Get ("requestHash", requestHash) || !IsHex64 (requestHash)
        || !parameters.Get ("description", description) || description.IsEmpty ())
        return DuplicateError ("DuplicateElementsByOffset identity is invalid.");
    parameters.Get ("origin", origin);
    parameters.Get ("operatorNote", operatorNote);
    if (operationId.GetLength () > 200 || description.GetLength () > 500 || operatorNote.GetLength () > 1000)
        return DuplicateError ("DuplicateElementsByOffset identity exceeds the native length limit.");

    const GS::ObjectState* offset = parameters.Get ("offset");
    double dx = 0.0;
    double dy = 0.0;
    if (offset == nullptr || !offset->Get ("x", dx) || !offset->Get ("y", dy)
        || !std::isfinite (dx) || !std::isfinite (dy) || (dx == 0.0 && dy == 0.0))
        return DuplicateError ("DuplicateElementsByOffset requires a finite, non-zero offset.");

    GS::Array<GS::ObjectState> elements;
    if (!parameters.Get ("elements", elements) || elements.IsEmpty () || elements.GetSize () > MaxDuplicateSources)
        return DuplicateError ("DuplicateElementsByOffset requires one to twenty thousand source elements.");

    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("status", "success");
    response.Add ("mode", mode);

    // A lookup only reads the process-local receipt: it is how an unknown outcome is
    // reconciled without dispatching the copy a second time.
    const GS::UniString scope = CurrentSoloFileScope ().Get ();
    const GS::Optional<GS::ObjectState> cached = FindCachedReceipt (operationId, scope);
    if (cached.HasValue () || mode == "lookup") {
        if (cached.HasValue ()) {
            GS::UniString recordedOperation;
            GS::UniString recordedHash;
            if (!cached.Get ().Get ("operation", recordedOperation) || !cached.Get ().Get ("requestHash", recordedHash)
                || recordedOperation != operation || LowerAscii (recordedHash) != LowerAscii (requestHash))
                return NativeContractError ("DUPLICATE_OPERATION_CONFLICT", "operationId already identifies a different duplicate request.");
            response.Add ("receipt", cached.Get ());
            response.Add ("duplicateOperation", true);
        }
        response.Add ("dispatched", false);
        response.Add ("found", cached.HasValue ());
        return response;
    }

    // Read every source fresh.  A copy is dispatched only when the whole set is copyable as
    // one piece: one story, editable, not hotlinked, groups complete and openings carried by
    // their host instead of listed on their own.
    GS::Array<DuplicateSource> sources;
    GS::HashSet<API_Guid> requested;
    const auto& refusals = response.AddList<GS::ObjectState> ("refusals");
    GSSize refusalCount = 0;
    Int32 liftedLayerSources = 0;
    Int32 groupedSources = 0;
    GS::Array<LiftedLayer> liftedLayers;
    GS::Array<API_Guid> liftedSources;
    // A refusal names the source and the reason; the TypeScript side turns the codes into
    // the operator-facing text and never shows the GUID.
    const auto refuse = [&] (const API_Guid& guid, const API_ElemTypeID typeID, const char* code) {
        refusals (GS::ObjectState (
            "elementId", CreateGuidObjectState (guid),
            "elementType", GetElementTypeNonLocalizedName (typeID),
            "code", code));
        ++refusalCount;
    };
    for (const GS::ObjectState& item : elements) {
        const GS::ObjectState* elementId = item.Get ("elementId");
        const API_Guid guid = elementId == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementId);
        if (guid == APINULLGuid || requested.Contains (guid))
            return DuplicateError ("DuplicateElementsByOffset source GUIDs must be valid and unique.");
        requested.Add (guid);
        API_Elem_Head header = {};
        header.guid = guid;
        if (ACAPI_Element_GetHeader (&header) != NoError) {
            refuse (guid, API_ZombieElemID, "SOURCE_MISSING");
            continue;
        }
        sources.Push ({ guid, GetElemTypeId (header), header.floorInd, header.modiStamp, APINULLGuid });
        if (header.hotlinkGuid != APINULLGuid)
            refuse (guid, GetElemTypeId (header), "SOURCE_HOTLINKED");
        // Hidden and locked layers are lifted for the copy only: hidden is not absent, and the
        // copies land on the same layer, which gets its flags back in the same undo step.  A
        // lifted source must still be in the operator's workspace and rights now, and fully
        // editable once its layer is lifted (checked again inside the undo step).
        const GSFlags layerFlags = LayerBlockingFlags (header.layer);
        if (layerFlags != 0) {
            ++liftedLayerSources;
            liftedSources.Push (guid);
            bool known = false;
            for (const LiftedLayer& lifted : liftedLayers)
                if (lifted.index == header.layer) known = true;
            if (!known)
                liftedLayers.Push ({ header.layer, layerFlags });
            if (!ACAPI_Element_Filter (guid, APIFilt_InMyWorkspace | APIFilt_HasAccessRight))
                refuse (guid, GetElemTypeId (header), NotEditableReason (header));
        } else if (!ACAPI_Element_Filter (guid, DuplicateEditableFilter)) {
            refuse (guid, GetElemTypeId (header), NotEditableReason (header));
        }
    }
    for (DuplicateSource& source : sources) {
        if (source.floorIndex != sources[0].floorIndex)
            refuse (source.guid, source.typeID, "SOURCE_OTHER_STORY");
        if (IsOpeningType (source.typeID)) {
            API_Guid owner = APINULLGuid;
            OpeningOwner (source.guid, source.typeID, owner);
            refuse (source.guid, source.typeID, owner != APINULLGuid && requested.Contains (owner) ? "OPENING_LISTED_WITH_HOST" : "OPENING_WITHOUT_HOST");
        }
        API_Guid group = APINULLGuid;
        if (ACAPI_Grouping_GetGroup (source.guid, &group) != NoError) {
            refuse (source.guid, source.typeID, "GROUP_UNREADABLE");
            continue;
        }
        if (group == APINULLGuid)
            continue;
        ++groupedSources;
        API_Guid rootGroup = APINULLGuid;
        GS::Array<API_Guid> members;
        if (ACAPI_Grouping_GetRootGroup (source.guid, &rootGroup) != NoError || rootGroup == APINULLGuid
            || ACAPI_Grouping_GetAllGroupedElems (rootGroup, &members) != NoError) {
            refuse (source.guid, source.typeID, "GROUP_UNREADABLE");
            continue;
        }
        source.rootGroup = rootGroup;
        for (const API_Guid& member : members) {
            if (requested.Contains (member))
                continue;
            API_Elem_Head outside = {};
            outside.guid = member;
            const bool readable = ACAPI_Element_GetHeader (&outside) == NoError;
            refusals (GS::ObjectState (
                "elementId", CreateGuidObjectState (source.guid),
                "elementType", GetElementTypeNonLocalizedName (source.typeID),
                "code", "GROUP_INCOMPLETE",
                "outsideMember", CreateGuidObjectState (member),
                "outsideMemberType", readable ? GetElementTypeNonLocalizedName (GetElemTypeId (outside)) : GS::UniString ("Unknown")));
            ++refusalCount;
            break;
        }
    }

    // The openings Archicad will copy with their hosts, and the state the copy is planned on:
    // each source's modification stamp and root group, each carried opening's stamp and host.
    // A dispatch names the state its dry run saw; any change since refuses it before the Drag.
    GS::Array<CarriedOpening> carried;
    GS::HashSet<API_Guid> carriedGuids;
    for (const DuplicateSource& source : sources) {
        for (const API_ElemTypeID openingType : CarriedOpeningTypes (source.typeID)) {
            GS::Array<API_Guid> connected;
            if (ACAPI_Grouping_GetConnectedElements (source.guid, openingType, &connected) != NoError) {
                refuse (source.guid, source.typeID, "CARRIED_OPENINGS_UNREADABLE");
                continue;
            }
            for (const API_Guid& opening : connected) {
                if (carriedGuids.Contains (opening))
                    continue;
                API_Elem_Head header = {};
                header.guid = opening;
                if (ACAPI_Element_GetHeader (&header) != NoError) {
                    refuse (source.guid, source.typeID, "CARRIED_OPENINGS_UNREADABLE");
                    continue;
                }
                carriedGuids.Add (opening);
                carried.Push ({ opening, GetElemTypeId (header), source.guid, header.modiStamp });
            }
        }
    }
    StateDigest digest;
    for (const DuplicateSource& source : sources) {
        digest.Add (source.guid);
        digest.Add (source.modiStamp);
        digest.Add (source.rootGroup);
    }
    for (const CarriedOpening& opening : carried) {
        digest.Add (opening.guid);
        digest.Add (opening.modiStamp);
        digest.Add (opening.host);
    }
    const GS::UniString sourceState = digest.Hex ();
    response.Add ("sourceState", sourceState);
    const auto& carriedList = response.AddList<GS::ObjectState> ("carried");
    for (const CarriedOpening& opening : carried)
        carriedList (GS::ObjectState (
            "elementId", CreateGuidObjectState (opening.guid),
            "elementType", GetElementTypeNonLocalizedName (opening.typeID),
            "host", CreateGuidObjectState (opening.host)));

    bool groupsSuspended = false;
    const GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsSuspended);
    if (groupModeError != NoError)
        return CreateErrorResponse (groupModeError, "Failed to read the Suspend Groups mode.");
    response.Add ("requestedCount", static_cast<Int32> (elements.GetSize ()));
    response.Add ("refusalCount", static_cast<Int32> (refusalCount));
    response.Add ("liftedLayerSources", liftedLayerSources);
    response.Add ("liftedLayerCount", static_cast<Int32> (liftedLayers.GetSize ()));
    response.Add ("groupedSources", groupedSources);
    if (mode == "dryRun" || refusalCount > 0) {
        response.Add ("dispatched", false);
        return response;
    }
    GS::UniString expectedSourceState;
    if (!parameters.Get ("expectedSourceState", expectedSourceState) || expectedSourceState.IsEmpty ())
        return DuplicateError ("DuplicateElementsByOffset dispatch requires the expectedSourceState of its dry run.");
    if (expectedSourceState != sourceState)
        return NativeContractError ("SOURCE_STATE_CONFLICT", "The source elements changed after the dry run; nothing was copied.");

    // Archicad copies the whole set in one Drag: links inside the set (dimensions, labels,
    // hosted openings) move to the copies.  The created elements are the difference of the
    // element lists around the edit; the edit fills each neig with its copy's GUID.  Without
    // the list before the edit nothing is dispatched.
    const auto listStarted = std::chrono::steady_clock::now ();
    GS::HashSet<API_Guid> before;
    const GSErrCode beforeError = AllElementGuids (before);
    double elementListMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - listStarted).count ();
    if (beforeError != NoError)
        return CreateErrorResponse (beforeError, "Failed to list the project's elements before the copy.");
    GS::Array<API_Neig> neigs;
    for (const DuplicateSource& source : sources)
        neigs.Push (API_Neig (source.guid));
    // A whole group is copied as a group: Suspend Groups is off for the Drag and comes back
    // as the operator had it.
    bool groupModeNeedsRestore = groupedSources > 0 && groupsSuspended;
    if (groupModeNeedsRestore) {
        const GSErrCode error = TAPIR_Grouping_ChangeSuspendGroup (false);
        if (error != NoError)
            return CreateErrorResponse (error, "Failed to turn Suspend Groups off for the copy.");
    }
    const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
        if (groupModeNeedsRestore)
            TAPIR_Grouping_ChangeSuspendGroup (true);
    });
    GSErrCode editError = NoError;
    GSErrCode layerError = NoError;
    bool layersRestored = true;
    const auto started = std::chrono::steady_clock::now ();
    GS::Array<API_Guid> notEditableAfterLift;
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Duplicate Elements By Offset", [&] () -> GSErrCode {
        UIndex lifted = 0;
        for (; lifted < liftedLayers.GetSize (); ++lifted) {
            layerError = SetLayerFlags (liftedLayers[lifted].index, 0);
            if (layerError != NoError) break;
        }
        // The set is copied whole or not at all: a lifted source that is still not editable
        // stops the Drag before it starts.
        if (layerError == NoError)
            for (const API_Guid& guid : liftedSources)
                if (!ACAPI_Element_Filter (guid, DuplicateEditableFilter)) notEditableAfterLift.Push (guid);
        if (layerError == NoError && !notEditableAfterLift.IsEmpty ())
            editError = APIERR_REFUSEDCMD;
        if (layerError == NoError && notEditableAfterLift.IsEmpty ()) {
            API_EditPars editPars = {};
            editPars.typeID = APIEdit_Drag;
            editPars.withDelete = false;
            editPars.begC = { 0.0, 0.0, 0.0 };
            editPars.endC = { dx, dy, 0.0 };
            editError = ACAPI_Element_Edit (&neigs, editPars);
        }
        for (UIndex index = 0; index < lifted; ++index)
            if (SetLayerFlags (liftedLayers[index].index, liftedLayers[index].flags) != NoError) layersRestored = false;
        return layerError != NoError ? layerError : editError;
    });
    const double elapsedMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
    GSErrCode groupModeRestoreError = NoError;
    if (groupModeNeedsRestore) {
        groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (true);
        if (groupModeRestoreError == NoError)
            groupModeNeedsRestore = false;
    }
    const auto afterListStarted = std::chrono::steady_clock::now ();
    GS::HashSet<API_Guid> after;
    const GSErrCode afterError = AllElementGuids (after);
    elementListMs += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - afterListStarted).count ();
    for (const API_Guid& guid : notEditableAfterLift) {
        API_Elem_Head header = {};
        header.guid = guid;
        const bool readable = ACAPI_Element_GetHeader (&header) == NoError;
        refuse (guid, readable ? GetElemTypeId (header) : API_ZombieElemID, "SOURCE_NOT_EDITABLE");
    }

    GS::ObjectState receipt;
    const auto& created = receipt.AddList<GS::ObjectState> ("created");
    GS::HashSet<API_Guid> createdGuids;
    for (const API_Guid& guid : after) {
        if (before.Contains (guid))
            continue;
        createdGuids.Add (guid);
        API_Elem_Head header = {};
        header.guid = guid;
        const bool readable = ACAPI_Element_GetHeader (&header) == NoError;
        created (GS::ObjectState (
            "elementId", CreateGuidObjectState (guid),
            "elementType", readable ? GetElementTypeNonLocalizedName (GetElemTypeId (header)) : GS::UniString ("Unknown"),
            "floorIndex", readable ? static_cast<Int32> (header.floorInd) : -1));
    }

    // A copy removes nothing; an element of the list before that is gone after is counted.
    Int32 removedCount = 0;
    if (afterError == NoError)
        for (const API_Guid& guid : before)
            if (!after.Contains (guid)) ++removedCount;

    // Each source pairs with the copy the edit returned; a source Archicad left out has none.
    const auto& pairs = receipt.AddList<GS::ObjectState> ("pairs");
    Int32 skippedSources = 0;
    Int32 missingSources = 0;
    GS::HashSet<API_Guid> sourceGuids;
    GS::HashTable<API_Guid, API_Guid> copyOfSource;
    for (UIndex index = 0; index < neigs.GetSize (); ++index) {
        sourceGuids.Add (sources[index].guid);
        if (!after.Contains (sources[index].guid)) ++missingSources;
        GS::ObjectState pair ("source", CreateGuidObjectState (sources[index].guid));
        if (neigs[index].guid == APINULLGuid || !createdGuids.Contains (neigs[index].guid)) {
            ++skippedSources;
        } else {
            pair.Add ("copy", CreateGuidObjectState (neigs[index].guid));
            copyOfSource.Add (sources[index].guid, neigs[index].guid);
        }
        pairs (pair);
    }

    // Each carried opening pairs with the one created opening of its type in its host's copy
    // whose box is its own moved by the offset; none or several is unmatched.
    const auto& carriedPairs = receipt.AddList<GS::ObjectState> ("carried");
    GS::HashTable<API_Guid, API_Guid> copyOfCarried;
    GS::HashSet<API_Guid> carriedCopies;
    Int32 carriedUnmatched = 0;
    for (const CarriedOpening& opening : carried) {
        GS::ObjectState pair ("source", CreateGuidObjectState (opening.guid));
        const API_Guid* hostCopy = copyOfSource.GetPtr (opening.host);
        API_Box3D sourceBox = {};
        GS::Array<API_Guid> candidates;
        API_Guid match = APINULLGuid;
        GSSize matches = 0;
        if (hostCopy != nullptr && BoundsOf (opening.guid, sourceBox) == NoError
            && ACAPI_Grouping_GetConnectedElements (*hostCopy, opening.typeID, &candidates) == NoError) {
            for (const API_Guid& candidate : candidates) {
                API_Box3D copyBox = {};
                if (!createdGuids.Contains (candidate) || carriedCopies.Contains (candidate)
                    || BoundsOf (candidate, copyBox) != NoError || !IsTranslatedBox (sourceBox, copyBox, dx, dy))
                    continue;
                match = candidate;
                ++matches;
            }
        }
        if (matches == 1) {
            carriedCopies.Add (match);
            copyOfCarried.Add (opening.guid, match);
            pair.Add ("copy", CreateGuidObjectState (match));
        } else {
            ++carriedUnmatched;
        }
        carriedPairs (pair);
    }

    // A copy that still refers to a source, rather than to a copy, was not re-linked.  Each
    // copy's links are checked against its source's: a link to a source or to a carried
    // opening becomes a link to that element's own copy, and a link outside the set stays as
    // it was (or is dropped, counted apart).  An opening created besides the paired and carried
    // copies is a mismatch.
    Int32 linksToCopies = 0;
    Int32 linksToSources = 0;
    Int32 linksElsewhere = 0;
    Int32 linksUnreadable = 0;
    Int32 linksMismatched = 0;
    Int32 linksExternalDropped = 0;
    GS::HashSet<API_Guid> pairedCopies;
    for (UIndex index = 0; index < neigs.GetSize (); ++index) {
        const API_Guid* copy = copyOfSource.GetPtr (sources[index].guid);
        if (copy == nullptr) continue;
        pairedCopies.Add (*copy);
        GS::Array<API_Guid> sourceLinks;
        GS::Array<API_Guid> copyLinks;
        if (LinkedElements (sources[index].guid, sources[index].typeID, sourceLinks) != NoError
            || LinkedElements (*copy, sources[index].typeID, copyLinks) != NoError) {
            ++linksUnreadable;
            continue;
        }
        bool matches = sourceLinks.GetSize () == copyLinks.GetSize ();
        for (UIndex link = 0; matches && link < sourceLinks.GetSize (); ++link) {
            const API_Guid& from = sourceLinks[link];
            const API_Guid& to = copyLinks[link];
            const API_Guid* mapped = copyOfSource.GetPtr (from);
            if (from == APINULLGuid)
                matches = to == APINULLGuid;
            else if (sourceGuids.Contains (from))
                matches = mapped != nullptr && *mapped == to;
            else if (carriedGuids.Contains (from))
                matches = copyOfCarried.ContainsKey (from) && copyOfCarried[from] == to;
            else if (to == APINULLGuid)
                ++linksExternalDropped;
            else
                matches = to == from;
        }
        if (!matches) ++linksMismatched;
    }
    for (const API_Guid& guid : createdGuids) {
        API_Elem_Head header = {};
        header.guid = guid;
        if (ACAPI_Element_GetHeader (&header) != NoError) {
            ++linksUnreadable;
            continue;
        }
        const API_ElemTypeID typeID = GetElemTypeId (header);
        GS::Array<API_Guid> linked;
        if (LinkedElements (guid, typeID, linked) != NoError) {
            ++linksUnreadable;
            continue;
        }
        if (IsOpeningType (typeID) && !pairedCopies.Contains (guid) && !carriedCopies.Contains (guid))
            ++linksMismatched;
        for (const API_Guid& target : linked) {
            if (target == APINULLGuid) continue;
            if (createdGuids.Contains (target)) ++linksToCopies;
            else if (sourceGuids.Contains (target)) ++linksToSources;
            else ++linksElsewhere;
        }
    }

    // Every created element is accounted for: a paired copy, a carried opening's copy, or an
    // element a copy owns.  Anything else is unexpected and fails the caller's check.
    Int32 ownedByCopies = 0;
    Int32 unexpectedCreated = 0;
    for (const API_Guid& guid : createdGuids) {
        if (pairedCopies.Contains (guid) || carriedCopies.Contains (guid))
            continue;
        API_Elem_Head header = {};
        header.guid = guid;
        API_Guid owner = APINULLGuid;
        if (ACAPI_Element_GetHeader (&header) == NoError && CreatedOwner (guid, GetElemTypeId (header), owner) == NoError
            && owner != APINULLGuid && createdGuids.Contains (owner))
            ++ownedByCopies;
        else
            ++unexpectedCreated;
    }

    // Each copy sits in a new group exactly when its source does.  Archicad does not
    // recreate a group of one element, so such a group is counted apart.
    Int32 groupStateDiffers = 0;
    Int32 singleMemberGroupsDropped = 0;
    Int32 copyInSourceGroup = 0;
    Int32 groupsUnreadable = 0;
    for (UIndex index = 0; index < neigs.GetSize (); ++index) {
        if (neigs[index].guid == APINULLGuid || !createdGuids.Contains (neigs[index].guid)) continue;
        API_Guid sourceGroup = APINULLGuid;
        API_Guid copyGroup = APINULLGuid;
        if (GroupOf (sources[index].guid, sourceGroup) != NoError || GroupOf (neigs[index].guid, copyGroup) != NoError) {
            ++groupsUnreadable;
            continue;
        }
        GSSize sourceGroupSize = 0;
        if (copyGroup != APINULLGuid && copyGroup == sourceGroup)
            ++copyInSourceGroup;
        else if (sourceGroup != APINULLGuid && copyGroup == APINULLGuid) {
            if (DirectGroupSize (sourceGroup, sourceGroupSize) != NoError)
                ++groupsUnreadable;
            else if (sourceGroupSize == 1)
                ++singleMemberGroupsDropped;
            else
                ++groupStateDiffers;
        } else if (sourceGroup == APINULLGuid && copyGroup != APINULLGuid)
            ++groupStateDiffers;
    }

    const bool dispatchComplete = undoError == NoError && editError == NoError && layerError == NoError;
    const bool createdComplete = afterError == NoError;
    const Int32 createdCount = static_cast<Int32> (createdGuids.GetSize ());
    // The status is what Archicad did; whether the copy is faithful is the caller's check of
    // the facts below.  Without the list after the edit the created set is not known.
    const char* status = !createdComplete ? "unknown"
        : dispatchComplete && createdCount > 0 ? "applied"
        : !dispatchComplete && createdCount == 0 ? "failed"
        : "unknown";
    receipt.Add ("schemaVersion", DuplicateReceiptVersion);
    receipt.Add ("operationId", operationId);
    receipt.Add ("fileScopeKey", scope);
    receipt.Add ("nativeSequence", NextCachedReceiptSequence ());
    receipt.Add ("terminalAt", GS::UniString (UtcTimestamp ().c_str (), CC_UTF8));
    receipt.Add ("operation", operation);
    receipt.Add ("status", status);
    receipt.Add ("description", description);
    if (!operatorNote.IsEmpty ()) receipt.Add ("operatorNote", operatorNote);
    receipt.Add ("origin", origin.IsEmpty () ? GS::UniString ("NATIVE") : origin);
    receipt.Add ("requestHash", requestHash);
    const GS::ObjectState* semanticMetadata = parameters.Get ("semanticMetadata");
    if (semanticMetadata != nullptr) receipt.Add ("semanticMetadata", *semanticMetadata);
    receipt.Add ("offset", GS::ObjectState ("x", dx, "y", dy));
    receipt.Add ("floorIndex", static_cast<Int32> (sources[0].floorIndex));
    receipt.Add ("sourceCount", static_cast<Int32> (sources.GetSize ()));
    receipt.Add ("createdCount", createdCount);
    receipt.Add ("createdComplete", createdComplete);
    receipt.Add ("removedCount", removedCount);
    receipt.Add ("ownedByCopies", ownedByCopies);
    receipt.Add ("unexpectedCreated", unexpectedCreated);
    receipt.Add ("skippedSourceCount", skippedSources);
    receipt.Add ("missingSourceCount", missingSources);
    receipt.Add ("liftedLayerCount", static_cast<Int32> (liftedLayers.GetSize ()));
    receipt.Add ("layersRestored", layersRestored);
    receipt.Add ("groupModeRestored", groupModeRestoreError == NoError);
    GS::ObjectState links ("toCopies", linksToCopies, "toSources", linksToSources, "elsewhere", linksElsewhere);
    links.Add ("unreadable", linksUnreadable);
    links.Add ("mismatched", linksMismatched);
    links.Add ("externalDropped", linksExternalDropped);
    receipt.Add ("links", links);
    GS::ObjectState groups ("groupedSources", groupedSources, "stateDiffers", groupStateDiffers, "singleMemberGroupsDropped", singleMemberGroupsDropped);
    groups.Add ("copyInSourceGroup", copyInSourceGroup);
    groups.Add ("unreadable", groupsUnreadable);
    receipt.Add ("groups", groups);
    receipt.Add ("sourceState", sourceState);
    receipt.Add ("carriedCount", static_cast<Int32> (carried.GetSize ()));
    receipt.Add ("carriedUnmatched", carriedUnmatched);
    receipt.Add ("errors", GS::ObjectState (
        "edit", static_cast<Int32> (editError),
        "layer", static_cast<Int32> (layerError),
        "undo", static_cast<Int32> (undoError),
        "elementList", static_cast<Int32> (afterError)));
    receipt.Add ("refusalCount", static_cast<Int32> (refusalCount));
    receipt.Add ("elapsedMs", elapsedMs);
    receipt.Add ("elementListMs", elementListMs);
    RememberCachedReceipt (receipt);
    response.Add ("dispatched", true);
    response.Add ("mutationComplete", dispatchComplete);
    response.Add ("receipt", receipt);
    return response;
}
