#include "ElementMutationInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementGDLParameterCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "NotificationCommands.hpp"

namespace ElementMutationInternal {

bool ContainsGuid (const GS::Array<API_Guid>& guids, const API_Guid& candidate)
{
    for (const API_Guid& guid : guids) {
        if (guid == candidate) {
            return true;
        }
    }
    return false;
}

GS::ObjectState BuildArrayParameters (
    const char* fieldName,
    const GS::Array<GS::ObjectState>& items)
{
    GS::ObjectState parameters;
    const auto& array = parameters.AddList<GS::ObjectState> (fieldName);
    for (const GS::ObjectState& item : items) {
        array (item);
    }
    return parameters;
}

} // namespace ElementMutationInternal

namespace {

void AddSectionValue (GS::ObjectState& target, const API_AssemblySegmentData& segment)
{
    target.Add ("nominalWidth", segment.nominalWidth);
    target.Add ("nominalHeight", segment.nominalHeight);
    target.Add ("circleBased", segment.circleBased);
    target.Add ("isWidthAndHeightLinked", segment.isWidthAndHeightLinked);
    target.Add ("structureTypeCode", static_cast<Int32> (segment.modelElemStructureType));
    if (segment.buildingMaterial != APIInvalidAttributeIndex) {
        target.Add ("buildingMaterialId", CreateGuidObjectState (GetAttributeGuidFromIndex (API_BuildingMaterialID, segment.buildingMaterial)));
    }
    if (segment.profileAttr != APIInvalidAttributeIndex) {
        target.Add ("profileId", CreateGuidObjectState (GetAttributeGuidFromIndex (API_ProfileID, segment.profileAttr)));
    }
}

bool AddNativeSectionReadback (
    const API_Guid& elementGuid,
    const API_ElemTypeID typeID,
    GS::ObjectState& target)
{
    API_ElementMemo memo = {};
    NativeMemoGuard cleanup (memo);

    const UInt64 memoMask = typeID == API_ColumnID ? APIMemoMask_ColumnSegment : APIMemoMask_BeamSegment;
    const GSErrCode err = ACAPI_Element_GetMemo (elementGuid, &memo, memoMask);
    if (err != NoError) {
        target.Add ("errorCode", err);
        return false;
    }

    GSSize segmentCount = 0;
    if (typeID == API_ColumnID && memo.columnSegments != nullptr) {
        segmentCount = BMGetPtrSize (reinterpret_cast<GSPtr> (memo.columnSegments)) / sizeof (API_ColumnSegmentType);
    } else if (typeID == API_BeamID && memo.beamSegments != nullptr) {
        segmentCount = BMGetPtrSize (reinterpret_cast<GSPtr> (memo.beamSegments)) / sizeof (API_BeamSegmentType);
    }
    if (segmentCount == 0) {
        target.Add ("errorCode", APIERR_BADPARS);
        return false;
    }

    const auto& segments = target.AddList<GS::ObjectState> ("segments");
    for (GSSize i = 0; i < segmentCount; ++i) {
        GS::ObjectState segment;
        if (typeID == API_ColumnID) {
            AddSectionValue (segment, memo.columnSegments[i].assemblySegmentData);
        } else {
            AddSectionValue (segment, memo.beamSegments[i].assemblySegmentData);
        }
        segment.Add ("index", static_cast<Int32> (i));
        segments (segment);
    }

    return true;
}

} // namespace

namespace ElementMutationInternal {

GS::ObjectState BuildElementReadback (
    const GS::Array<API_Guid>& elementGuids,
    const API_ElemTypeID typeID,
    GS::ProcessControl& processControl,
    bool& verified)
{
    GS::ObjectState readback;
    const auto& elements = readback.AddList<GS::ObjectState> ("elements");

    GS::Array<GS::ObjectState> detailItems;
    for (const API_Guid& guid : elementGuids) {
        detailItems.Push (CreateElementIdObjectState (guid));
    }
    GS::ObjectState detailsParameters = BuildArrayParameters ("elements", detailItems);
    GS::ObjectState nativeDetails = GetDetailsOfElementsCommand ().Execute (detailsParameters, processControl);
    GS::Array<GS::ObjectState> detailRows;
    if (!nativeDetails.Get ("detailsOfElements", detailRows) ||
        static_cast<GSSize> (detailRows.GetSize ()) != static_cast<GSSize> (elementGuids.GetSize ())) {
        verified = false;
    } else {
        GS::Array<API_Guid> returnedDetailGuids;
        for (const GS::ObjectState& detailRow : detailRows) {
            const GS::ObjectState* detailElementID = detailRow.Get ("elementId");
            const GS::ObjectState* detail = detailRow.Get ("details");
            const API_Guid detailGuid = detailElementID == nullptr
                ? APINULLGuid
                : GetGuidFromObjectState (*detailElementID);
            if (detailGuid == APINULLGuid
                || !ContainsGuid (elementGuids, detailGuid)
                || ContainsGuid (returnedDetailGuids, detailGuid)
                || detail == nullptr
                || detail->Get ("error") != nullptr) {
                verified = false;
                break;
            }
            if (typeID == API_TextID) {
                GS::UniString richTextStatus;
                const GS::ObjectState* richText = detail->Get ("richText");
                if (richText == nullptr || !detail->Get ("richTextStatus", richTextStatus)
                    || richTextStatus != "available") {
                    // A structural detail row is not sufficient evidence for
                    // Text mutations: the Unicode paragraph/run memo must be
                    // present and explicitly marked available.
                    verified = false;
                    break;
                }
            }
            returnedDetailGuids.Push (detailGuid);
        }
        if (static_cast<GSSize> (returnedDetailGuids.GetSize ()) != static_cast<GSSize> (elementGuids.GetSize ())) {
            verified = false;
        }
    }
    readback.Add ("nativeDetails", nativeDetails);

    if (typeID == API_ColumnID || typeID == API_BeamID) {
        const auto& nativeSections = readback.AddList<GS::ObjectState> ("nativeSections");
        for (const API_Guid& guid : elementGuids) {
            GS::ObjectState status;
            status.Add ("elementId", CreateGuidObjectState (guid));
            const bool exists = DoesElementExist (guid, typeID);
            status.Add ("exists", exists);
            if (!exists) {
                verified = false;
                nativeSections (status);
                continue;
            }
            const bool sectionAvailable = AddNativeSectionReadback (guid, typeID, status);
            status.Add ("available", sectionAvailable);
            if (!sectionAvailable) {
                verified = false;
            }
            nativeSections (status);
        }
    }

    for (const API_Guid& guid : elementGuids) {
        GS::ObjectState status;
        status.Add ("elementId", CreateGuidObjectState (guid));
        API_Elem_Head header = {};
        header.guid = guid;
        const bool exists = ACAPI_Element_GetHeader (&header) == NoError;
        const bool typeMatches = exists && GetElemTypeId (header) == typeID;
        status.Add ("exists", exists);
        status.Add ("elementTypeMatches", typeMatches);
        if (!typeMatches) {
            verified = false;
        }
        elements (status);
    }

    return readback;
}

GS::ObjectState BuildDeleteReadback (
    const GS::Array<API_Guid>& elementGuids,
    const API_ElemTypeID typeID,
    bool& verified,
    GSSize& absentCount)
{
    GS::ObjectState readback;
    const auto& deleted = readback.AddList<GS::ObjectState> ("deleted");
    absentCount = 0;
    for (const API_Guid& guid : elementGuids) {
        API_Elem_Head header = {};
        header.guid = guid;
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        const bool stillExists = headerError == NoError;
        // APIERR_BADID is the documented not-found result for a deleted
        // element.  Any other error is an indeterminate readback failure,
        // not proof that the element is absent.
        const bool absent = headerError == APIERR_BADID;
        GS::ObjectState status;
        status.Add ("elementId", CreateGuidObjectState (guid));
        status.Add ("absent", absent);
        if (headerError != NoError && headerError != APIERR_BADID) {
            status.Add ("readbackErrorCode", headerError);
            verified = false;
        }
        if (absent) {
            ++absentCount;
        }
        if (stillExists) {
            status.Add ("elementType", GetElementTypeNonLocalizedName (GetElemTypeId (header)));
        }
        if (!absent) {
            verified = false;
        }
        deleted (status);
    }
    (void) typeID;
    return readback;
}

GS::Array<API_Guid> CollectCreatedGuids (const GS::ObjectState& mutationResult)
{
    GS::Array<API_Guid> guids;
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return guids;
    }
    GS::Array<GS::ObjectState> elements;
    if (!mutationResult.Get ("elements", elements)) {
        return guids;
    }
    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementID = element.Get ("elementId");
        if (elementID != nullptr) {
            const API_Guid guid = GetGuidFromObjectState (*elementID);
            if (guid != APINULLGuid && !ContainsGuid (guids, guid)) {
                guids.Push (guid);
            }
        }
    }
    return guids;
}

GS::Array<API_Guid> CollectUpdatedGuids (
    const GS::ObjectState& mutationResult,
    const GS::Array<API_Guid>& requestedGuids)
{
    GS::Array<API_Guid> guids;
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return guids;
    }
    GS::Array<GS::ObjectState> results;
    if (!mutationResult.Get ("executionResults", results)) {
        return guids;
    }
    const GSSize resultCount = static_cast<GSSize> (results.GetSize ());
    const GSSize requestedGuidCount = static_cast<GSSize> (requestedGuids.GetSize ());
    const GSSize count = resultCount < requestedGuidCount ? resultCount : requestedGuidCount;
    for (GSSize i = 0; i < count; ++i) {
        bool success = false;
        if (results[i].Get ("success", success) && success) {
            guids.Push (requestedGuids[i]);
        }
    }
    return guids;
}

GS::Array<API_Guid> CollectDeletedGuids (
    const GS::ObjectState& mutationResult,
    const GS::Array<API_Guid>& requestedGuids)
{
    bool success = false;
    if (mutationResult.Get ("success", success) && success) {
        return requestedGuids;
    }
    // DeleteElements returns one aggregate result for the supplied GUID list.
    // A failed aggregate must not be reported as if every requested element
    // had been deleted; the readback then remains an explicit failed/partial
    // postcondition instead of manufacturing positive evidence.
    return {};
}

bool MutationResultIsComplete (
    const GS::UniString& operation,
    const GS::ObjectState& mutationResult,
    const GSSize requestedCount)
{
    if (mutationResult.Get ("error") != nullptr) {
        return false;
    }
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return false;
    }
    if (mutationResult.Contains ("groupModeRestored")) {
        bool groupModeRestored = false;
        if (!mutationResult.Get ("groupModeRestored", groupModeRestored) || !groupModeRestored)
            return false;
    }
    if (mutationResult.Contains ("groupModeRestoreError")) {
        return false;
    }
    if (operation == "create") {
        GS::Array<GS::ObjectState> elements;
        if (!mutationResult.Get ("elements", elements) || static_cast<GSSize> (elements.GetSize ()) != requestedCount) {
            return false;
        }
        GS::Array<API_Guid> createdGuids;
        for (const GS::ObjectState& element : elements) {
            const GS::ObjectState* elementID = element.Get ("elementId");
            if (elementID == nullptr) {
                return false;
            }
            const API_Guid guid = GetGuidFromObjectState (*elementID);
            if (guid == APINULLGuid || ContainsGuid (createdGuids, guid)) {
                return false;
            }
            createdGuids.Push (guid);
        }
        return static_cast<GSSize> (createdGuids.GetSize ()) == requestedCount;
    }
    if (operation == "update") {
        GS::Array<GS::ObjectState> results;
        if (!mutationResult.Get ("executionResults", results) || static_cast<GSSize> (results.GetSize ()) != requestedCount) {
            return false;
        }
        for (const GS::ObjectState& result : results) {
            bool success = false;
            if (!result.Get ("success", success) || !success) {
                return false;
            }
        }
        return true;
    }

    bool success = false;
    return mutationResult.Get ("success", success) && success;
}

} // namespace ElementMutationInternal
