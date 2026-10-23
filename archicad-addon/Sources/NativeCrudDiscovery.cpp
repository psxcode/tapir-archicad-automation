#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace NativeCrudInternal;

namespace {

constexpr GSSize MaxDiscoveryCandidates = 4096;

GS::ObjectState CoordinateBounds (const API_Box3D& bounds)
{
    return GS::ObjectState (
        "minX", bounds.xMin,
        "minY", bounds.yMin,
        "maxX", bounds.xMax,
        "maxY", bounds.yMax);
}

bool IsFiniteOrderedDiscoveryBounds (double minX, double minY, double maxX, double maxY)
{
    return std::isfinite (minX) && std::isfinite (minY) && std::isfinite (maxX) && std::isfinite (maxY)
        && minX <= maxX && minY <= maxY;
}

bool DiscoveryBoundsIntersect (double minX, double minY, double maxX, double maxY,
    double queryMinX, double queryMinY, double queryMaxX, double queryMaxY)
{
    return maxX >= queryMinX && minX <= queryMaxX && maxY >= queryMinY && minY <= queryMaxY;
}

GS::ObjectState BuildReferencesFromSnapshots (const GS::ObjectState& snapshotsResponse)
{
    GS::ObjectState response;
    const auto& references = response.AddList<GS::ObjectState> ("references");
    GS::Array<GS::ObjectState> snapshots;
    snapshotsResponse.Get ("elements", snapshots);
    for (const GS::ObjectState& snapshot : snapshots) {
        GS::ObjectState reference;
        const GS::ObjectState* id = snapshot.Get ("elementId");
        if (id != nullptr) reference.Add ("elementId", *id);
        GS::UniString type;
        if (snapshot.Get ("elementType", type)) reference.Add ("elementType", type);
        Int32 story = 0;
        if (snapshot.Get ("storyIndex", story)) reference.Add ("storyIndex", story);
        const GS::ObjectState* bounds = snapshot.Get ("bounds");
        if (bounds != nullptr) reference.Add ("bounds", *bounds);
        references (reference);
    }
    response.Add ("count", static_cast<Int32> (snapshots.GetSize ()));
    return response;
}

GS::ObjectState CompactHistoryEntry (const GS::ObjectState& receipt, const bool detail)
{
    GS::ObjectState entry;
    Int64 nativeSequence = 0;
    GS::UniString operationId;
    GS::UniString terminalAt;
    GS::UniString description;
    GS::UniString origin;
    GS::UniString status;
    GS::UniString operation;
    GS::UniString elementType;
    receipt.Get ("nativeSequence", nativeSequence);
    receipt.Get ("operationId", operationId);
    receipt.Get ("terminalAt", terminalAt);
    receipt.Get ("description", description);
    receipt.Get ("origin", origin);
    receipt.Get ("status", status);
    receipt.Get ("operation", operation);
    receipt.Get ("elementType", elementType);
    entry.Add ("nativeSequence", nativeSequence);
    entry.Add ("operationId", operationId);
    entry.Add ("terminalAt", terminalAt);
    entry.Add ("description", description);
    entry.Add ("origin", origin);
    entry.Add ("status", status);
    entry.Add ("operation", operation);
    entry.Add ("elementType", elementType);
    GS::Array<GS::UniString> primaryGuids;
    if (receipt.Get ("primaryGuids", primaryGuids)) {
        const auto& primary = entry.AddList<GS::UniString> ("primaryGuids");
        for (const GS::UniString& guid : primaryGuids) primary (guid);
    } else {
        entry.AddList<GS::UniString> ("primaryGuids");
    }
    GS::Array<GS::UniString> collateralGuids;
    if (receipt.Get ("collateralGuids", collateralGuids)) {
        const auto& collateral = entry.AddList<GS::UniString> ("collateralGuids");
        for (const GS::UniString& guid : collateralGuids) collateral (guid);
    } else {
        entry.AddList<GS::UniString> ("collateralGuids");
    }
    const auto& changedFields = entry.AddList<GS::ObjectState> ("changedFields");
    const auto& changedFieldNames = entry.AddList<GS::UniString> ("changedFieldNames");
    const auto& actions = entry.AddList<GS::ObjectState> ("actions");
    std::vector<GS::UniString> emittedNames;
    GS::Array<GS::ObjectState> elements;
    if (receipt.Get ("elements", elements)) {
        for (const GS::ObjectState& element : elements) {
            GS::UniString action;
            const GS::ObjectState* elementId = element.Get ("elementId");
            if (element.Get ("action", action) && elementId != nullptr) {
                const API_Guid guid = GetGuidFromObjectState (*elementId);
                // Keep the compact history projection on the same stable
                // element-id shape as every other native boundary:
                // `{ elementId: { guid: string }, action }`.  A bare string
                // here is convenient for humans but cannot be consumed by
                // the versioned TypeScript NativeMutationAction contract.
                actions (GS::ObjectState ("elementId", CreateGuidObjectState (guid), "action", action));
            }
            GS::Array<GS::ObjectState> fields;
            if (element.Get ("changedFields", fields)) {
                for (const GS::ObjectState& field : fields) {
                    changedFields (field);
                    GS::UniString path;
                    if (field.Get ("path", path)
                        && std::find (emittedNames.begin (), emittedNames.end (), path) == emittedNames.end ()) {
                        emittedNames.push_back (path);
                        changedFieldNames (path);
                    }
                }
            }
        }
    }
    if (detail) entry.Add ("receipt", receipt);
    return entry;
}

} // namespace

DiscoverNativeElementsCommand::DiscoverNativeElementsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String DiscoverNativeElementsCommand::GetName () const { return "DiscoverNativeElements"; }
GS::Optional<GS::UniString> DiscoverNativeElementsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"fileScopeKey":{"type":"string"},"elementType":{"type":"string"},"documentTargetKey":{"type":"string"},"storyIndex":{"type":"integer"},"bounds":{"type":"object","properties":{"minX":{"type":"number"},"minY":{"type":"number"},"maxX":{"type":"number"},"maxY":{"type":"number"}},"required":["minX","minY","maxX","maxY"],"additionalProperties":false}},"additionalProperties":false,"required":["contractVersion","fileScopeKey"]})";
}
GS::Optional<GS::UniString> DiscoverNativeElementsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState DiscoverNativeElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    const GS::Optional<GS::UniString> scopeError = ValidateScope (parameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    const bool hasBounds = parameters.Get ("bounds") != nullptr;
    Int32 storyIndex = 0;
    const bool hasStory = parameters.Get ("storyIndex", storyIndex);
    GS::UniString documentTargetKey;
    const bool hasDocument = parameters.Get ("documentTargetKey", documentTargetKey) && !documentTargetKey.IsEmpty ();
    if (!hasBounds && !hasStory && !hasDocument)
        return CreateErrorResponse (APIERR_BADPARS, "A documentTargetKey, storyIndex or bounds scope is required; project-wide discovery is forbidden.");
    // A document locator is a semantic-layer identity, not an Archicad
    // element scope.  The APX cannot safely resolve it without the selected
    // document's bounded rectangle, so fail closed instead of silently
    // falling back to a project-wide GetAllElements sweep.
    if (hasDocument && !hasBounds && !hasStory)
        return CreateErrorResponse (APIERR_BADPARS, "NARROW_SCOPE_REQUIRED: documentTargetKey must be accompanied by a storyIndex or bounds rectangle for native discovery.");
    if (hasStory && !hasBounds)
        return CreateErrorResponse (APIERR_BADPARS, "NARROW_SCOPE_REQUIRED: storyIndex requires an explicit bounds rectangle; project-wide story sweeps are forbidden.");

    GS::UniString typeName;
    API_ElemTypeID typeID = API_ZombieElemID;
    bool requestedDimensionFamily = false;
    if (parameters.Get ("elementType", typeName)) {
        typeID = GetElementTypeFromNonLocalizedName (typeName);
        requestedDimensionFamily = typeID == API_DimensionID || typeID == API_RadialDimensionID
            || typeID == API_AngleDimensionID || typeID == API_LevelDimensionID;
        if (!IsSupportedNativeType (typeID) && !requestedDimensionFamily) return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    }

    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    GS::Array<GS::ObjectState> ids;
    if (hasBounds) {
        const GS::ObjectState* bounds = parameters.Get ("bounds");
        if (bounds == nullptr)
            return CreateErrorResponse (APIERR_BADPARS, "bounds is unavailable.");
        if (!bounds->Get ("minX", minX) || !bounds->Get ("minY", minY) ||
            !bounds->Get ("maxX", maxX) || !bounds->Get ("maxY", maxY))
            return CreateErrorResponse (APIERR_BADPARS, "bounds requires numeric minX, minY, maxX and maxY.");
        if (!IsFiniteOrderedDiscoveryBounds (minX, minY, maxX, maxY))
            return CreateErrorResponse (APIERR_BADPARS, "bounds must be finite and ordered.");
        GS::ObjectState rectParameters;
        rectParameters.Add ("bounds", GS::ObjectState (
            "xMin", minX, "yMin", minY,
            "xMax", maxX, "yMax", maxY));
        rectParameters.Add ("multiStory", true);
        rectParameters.Add ("pageSize", static_cast<Int32> (MaxDiscoveryCandidates));
        const GS::ObjectState rectResult = GetElementsInRectCommand ().Execute (rectParameters, processControl);
        if (rectResult.Get ("error") != nullptr)
            return rectResult;
        bool rawPageRequired = false;
        rectResult.Get ("pageRequired", rawPageRequired);
        if (rawPageRequired)
            return CreateErrorResponse (APIERR_BADPARS, "NARROW_SCOPE_REQUIRED: bounded discovery exceeded the finite raw-candidate limit.");
        if (!rectResult.Get ("elements", ids))
            return CreateErrorResponse (APIERR_BADPARS, "Bounded discovery did not return an element-reference list.");
        if (ids.GetSize () > MaxDiscoveryCandidates)
            return CreateErrorResponse (APIERR_BADPARS, "NARROW_SCOPE_REQUIRED: bounded discovery exceeded the finite raw-candidate limit.");

        // GetElementsInRect returns every element type intersecting the
        // rectangle. Apply exact type/story and native element bounds before
        // the 100-reference cap or exact snapshot reads, so unrelated types
        // and marquee false positives cannot crowd out the requested scope.
        GS::Array<GS::ObjectState> scopedIds;
        GS::Array<API_Guid> seenGuids;
        for (const GS::ObjectState& id : ids) {
            const API_Guid guid = GetRequestedElementGuid (id);
            if (guid == APINULLGuid)
                return CreateErrorResponse (APIERR_BADPARS, "Bounded discovery returned an invalid element GUID.");
            if (seenGuids.Contains (guid))
                continue;

            API_Elem_Head header = {};
            header.guid = guid;
            const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
            if (headerError != NoError)
                return CreateErrorResponse (headerError, "Bounded discovery could not verify a candidate element header; coverage is incomplete.");
            seenGuids.Push (guid);

            if (!typeName.IsEmpty () && GetElemTypeId (header) != typeID)
                continue;
            if (hasStory && static_cast<Int32> (header.floorInd) != storyIndex)
                continue;

            API_Box3D elementBounds = {};
            const GSErrCode boundsError = ACAPI_Element_CalcBounds (&header, &elementBounds);
            if (boundsError != NoError || !IsFiniteOrderedDiscoveryBounds (
                    elementBounds.xMin, elementBounds.yMin, elementBounds.xMax, elementBounds.yMax))
                return CreateErrorResponse (boundsError != NoError ? boundsError : APIERR_BADPARS,
                    "Bounded discovery could not verify finite native bounds for a candidate; coverage is incomplete.");
            if (!DiscoveryBoundsIntersect (elementBounds.xMin, elementBounds.yMin, elementBounds.xMax, elementBounds.yMax,
                    minX, minY, maxX, maxY))
                continue;
            scopedIds.Push (id);
        }
        ids = scopedIds;
    } else {
        GS::ObjectState listParameters;
        if (!typeName.IsEmpty ()) listParameters.Add ("elementType", typeName);
        listParameters.Add ("pageSize", static_cast<Int32> (MaxDiscoveryReferences + 1));
        const GS::ObjectState listResult = typeName.IsEmpty ()
            ? GetAllElementsCommand ().Execute (listParameters, processControl)
            : GetElementsByTypeCommand ().Execute (listParameters, processControl);
        listResult.Get ("elements", ids);
    }
    if (ids.GetSize () > MaxDiscoveryReferences)
        return CreateErrorResponse (APIERR_BADPARS, "NARROW_SCOPE_REQUIRED: discovery exceeded the 100-reference cap.");
    if (requestedDimensionFamily) {
        GS::Array<GS::ObjectState> detailRows;
        bool dimensionCoverageComplete = true;
        for (GSSize offset = 0; offset < static_cast<GSSize> (ids.GetSize ()); offset += MaxMutationItems) {
            GS::Array<GS::UniString> exactGuids;
            const GSSize end = std::min (static_cast<GSSize> (ids.GetSize ()), offset + MaxMutationItems);
            for (GSSize index = offset; index < end; ++index) {
                const API_Guid guid = GetRequestedElementGuid (ids[index]);
                if (guid == APINULLGuid) return CreateErrorResponse (APIERR_BADPARS, "Dimension discovery returned an invalid exact GUID.");
                exactGuids.Push (APIGuidToString (guid));
            }
            GS::ObjectState exactParameters;
            exactParameters.Add ("guids", exactGuids);
            const GS::ObjectState detailResponse = GetNativeDimensionDetailsCommand ().Execute (exactParameters, processControl);
            if (detailResponse.Get ("error") != nullptr) return detailResponse;
            GS::Array<GS::ObjectState> chunkRows;
            if (!detailResponse.Get ("elements", chunkRows) || chunkRows.GetSize () != exactGuids.GetSize ())
                return CreateErrorResponse (APIERR_BADID, "Dimension discovery did not preserve its bounded exact candidate set.");
            for (GSSize local = 0; local < static_cast<GSSize> (chunkRows.GetSize ()); ++local) {
                const GS::ObjectState& row = chunkRows[local];
                const API_Guid expectedGuid = GetRequestedElementGuid (ids[offset + local]);
                const GS::ObjectState* rowElementId = row.Get ("elementId");
                if (rowElementId == nullptr || GetGuidFromObjectState (*rowElementId) != expectedGuid)
                    return CreateErrorResponse (APIERR_BADID, "Dimension discovery exact read changed candidate identity or order.");
                GS::UniString rowStatus;
                row.Get ("status", rowStatus);
                if (rowStatus != "success") dimensionCoverageComplete = false;
                Int32 rowStory = 0;
                if (hasStory && rowStatus == "success" && (!row.Get ("storyIndex", rowStory) || rowStory != storyIndex))
                    return CreateErrorResponse (APIERR_BADID, "Dimension discovery exact read changed the requested story scope.");
                if (hasBounds && rowStatus == "success") {
                    const GS::ObjectState* rowBounds = row.Get ("bounds");
                    double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
                    if (rowBounds == nullptr || !rowBounds->Get ("xMin", xMin) || !rowBounds->Get ("yMin", yMin)
                        || !rowBounds->Get ("xMax", xMax) || !rowBounds->Get ("yMax", yMax)
                        || !DiscoveryBoundsIntersect (xMin, yMin, xMax, yMax, minX, minY, maxX, maxY))
                        return CreateErrorResponse (APIERR_BADID, "Dimension discovery exact read changed the requested bounds scope.");
                }
                GS::ObjectState tagged = row;
                tagged.Add ("canonicalSnapshotKind", "dimension");
                detailRows.Push (tagged);
            }
        }
        GS::ObjectState result;
        result.Add ("status", "success");
        result.Add ("contractVersion", ContractVersion);
        result.Add ("fileScopeKey", CurrentSoloFileScope ().Get ());
        result.Add ("scope", hasDocument ? "documentTargetKey" : hasStory ? "story" : "bounds");
        const auto& elements = result.AddList<GS::ObjectState> ("elements");
        for (const GS::ObjectState& row : detailRows) elements (row);
        result.Add ("complete", dimensionCoverageComplete);
        return result;
    }
    bool complete = false;
    const GS::UniString scope = CurrentSoloFileScope ().Get ();
    GS::ObjectState snapshots = ExactSnapshots (ids, scope, processControl, complete);
    if (snapshots.Get ("error") != nullptr)
        return snapshots;
    GS::Array<GS::ObjectState> snapshotRows;
    if (!snapshots.Get ("elements", snapshotRows))
        return CreateErrorResponse (APIERR_BADPARS, "Exact discovery snapshots did not return an element list.");
    GS::ObjectState filtered;
    const auto& elements = filtered.AddList<GS::ObjectState> ("elements");
    for (const GS::ObjectState& snapshot : snapshotRows) {
        GS::UniString snapshotType;
        if (!typeName.IsEmpty () && (!snapshot.Get ("elementType", snapshotType) || snapshotType != typeName))
            return CreateErrorResponse (APIERR_BADID, "An exact discovery snapshot does not match the requested elementType.");
        Int32 snapshotStory = 0;
        if (hasStory && (!snapshot.Get ("storyIndex", snapshotStory) || snapshotStory != storyIndex))
            return CreateErrorResponse (APIERR_BADID, "An exact discovery snapshot does not match the requested storyIndex.");
        if (hasBounds) {
            const GS::ObjectState* snapshotBounds = snapshot.Get ("bounds");
            double snapshotMinX = 0.0;
            double snapshotMinY = 0.0;
            double snapshotMaxX = 0.0;
            double snapshotMaxY = 0.0;
            if (snapshotBounds == nullptr || !snapshotBounds->Get ("minX", snapshotMinX) || !snapshotBounds->Get ("minY", snapshotMinY) ||
                !snapshotBounds->Get ("maxX", snapshotMaxX) || !snapshotBounds->Get ("maxY", snapshotMaxY) ||
                !IsFiniteOrderedDiscoveryBounds (snapshotMinX, snapshotMinY, snapshotMaxX, snapshotMaxY) ||
                !DiscoveryBoundsIntersect (snapshotMinX, snapshotMinY, snapshotMaxX, snapshotMaxY, minX, minY, maxX, maxY))
                return CreateErrorResponse (APIERR_BADID, "An exact discovery snapshot is missing finite bounds inside the requested rectangle.");
        }
        elements (snapshot);
    }
    filtered.Add ("complete", complete);
    filtered.Add ("contractVersion", ContractVersion);
    GS::ObjectState references = BuildReferencesFromSnapshots (filtered);
    GS::Array<GS::ObjectState> referenceRows;
    references.Get ("references", referenceRows);
    const auto& outputReferences = filtered.AddList<GS::ObjectState> ("references");
    for (const GS::ObjectState& reference : referenceRows) outputReferences (reference);
    filtered.Add ("scope", hasDocument ? "documentTargetKey" : hasStory ? "story" : "bounds");
    return filtered;
}

GetNativeMutationHistoryCommand::GetNativeMutationHistoryCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeMutationHistoryCommand::GetName () const { return "GetNativeMutationHistory"; }
GS::Optional<GS::UniString> GetNativeMutationHistoryCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"fileScopeKey":{"type":"string"},"filter":{"type":"object"}},"additionalProperties":false,"required":["contractVersion","fileScopeKey"]})";
}
GS::Optional<GS::UniString> GetNativeMutationHistoryCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeMutationHistoryCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::Optional<GS::UniString> scopeError = ValidateScope (parameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    GS::UniString fileScopeKey;
    parameters.Get ("fileScopeKey", fileScopeKey);

    // Reconciliation is the one narrow exception to the MCP-owned history
    // boundary.  A response can be lost after Archicad has applied an
    // undoable command but before TypeScript folds the receipt into SQLite.
    // Keep exact operation lookup available from the bounded process-local
    // cache, while delegating all search/retention/list authority to MCP.
    const GS::ObjectState* filter = parameters.Get ("filter");
    GS::UniString exactOperationId;
    Int32 exactFirst = 1;
    bool exactDetail = false;
    if (filter != nullptr) {
        filter->Get ("operationId", exactOperationId);
        filter->Get ("first", exactFirst);
        filter->Get ("detail", exactDetail);
    }
    if (!exactOperationId.IsEmpty () && exactFirst >= 1 && exactFirst <= static_cast<Int32> (MaxMutationItems)) {
        const GS::Optional<GS::ObjectState> cached = FindCachedReceipt (exactOperationId, fileScopeKey);
        if (cached.HasValue ()) {
            GS::ObjectState history;
            history.Add ("contractVersion", ContractVersion);
            history.Add ("schemaVersion", HistoryVersion);
            history.Add ("status", "success");
            history.Add ("fileScopeKey", fileScopeKey);
            history.Add ("first", exactFirst);
            history.Add ("hasMore", false);
            history.Add ("complete", true);
            const auto& entries = history.AddList<GS::ObjectState> ("entries");
            entries (CompactHistoryEntry (cached.Get (), exactDetail));
            GS::ObjectState response;
            response.Add ("status", "success");
            response.Add ("mode", "mutationHistory");
            response.Add ("contractVersion", ContractVersion);
            response.Add ("fileScopeKey", fileScopeKey);
            response.Add ("history", history);
            response.Add ("source", "apx-process-receipt-cache");
            return response;
        }
    }

    // The MCP owns durable history and search.  APX only retains a bounded
    // process-local receipt cache for exact response-loss reconciliation.
    GS::ObjectState notFound;
    notFound.Add ("status", "not_found");
    notFound.Add ("code", "NATIVE_RECEIPT_NOT_FOUND");
    notFound.Add ("contractVersion", ContractVersion);
    notFound.Add ("schemaVersion", HistoryVersion);
    notFound.Add ("fileScopeKey", fileScopeKey);
    notFound.Add ("message", "No receipt for that operation is retained in the APX process cache.");
    return notFound;
}
