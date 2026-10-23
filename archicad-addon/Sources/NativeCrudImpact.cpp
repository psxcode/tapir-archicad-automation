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

constexpr const char* ImpactEnvelopeVersion = "native-impact-envelope-v1";

constexpr const char* ImpactScope = "wall-zone-pair-v1";

GS::ObjectState IdFromGuid (const API_Guid& guid)
{
    return CreateElementIdObjectState (guid);
}

bool GuidListMatchesRequested (
    const GS::Array<GS::UniString>& recorded,
    const GS::Array<GS::ObjectState>& requested)
{
    if (recorded.GetSize () != requested.GetSize ()) return false;
    GS::Array<API_Guid> seen;
    for (const GS::UniString& value : recorded) {
        const API_Guid guid = APIGuidFromString (value.ToCStr ());
        if (guid == APINULLGuid || seen.Contains (guid)) return false;
        seen.Push (guid);
    }
    for (const GS::ObjectState& requestedId : requested) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        if (guid == APINULLGuid || !seen.Contains (guid)) return false;
    }
    return true;
}

} // namespace

namespace NativeCrudInternal {

GS::Optional<GS::UniString> ParseNativeImpactEnvelope (
    const GS::ObjectState* semanticMetadata,
    const GS::UniString& scope,
    const GS::UniString& operationId,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& requestedIds,
    NativeImpactEnvelope& output)
{
    if (semanticMetadata == nullptr) return {};
    const GS::ObjectState* raw = semanticMetadata->Get (ImpactMetadataKey);
    if (raw == nullptr) return {};
    output.envelope = *raw;
    GS::UniString schemaVersion;
    GS::UniString fileScopeKey;
    GS::UniString bindingId;
    GS::UniString recordedOperationId;
    GS::UniString recordedOperation;
    GS::UniString recordedType;
    GS::UniString patchHash;
    Int32 archicadMajor = 0;
    Int32 maxElements = 0;
    if (!raw->Get ("schemaVersion", schemaVersion)
        || !raw->Get ("fileScopeKey", fileScopeKey)
        || !raw->Get ("archicadMajor", archicadMajor)
        || !raw->Get ("bindingId", bindingId)
        || !raw->Get ("operationId", recordedOperationId)
        || !raw->Get ("operation", recordedOperation)
        || !raw->Get ("elementType", recordedType)
        || !raw->Get ("patchHash", patchHash)
        || !raw->Get ("maxElements", maxElements))
        return GS::UniString ("nativeImpact envelope is missing required identity fields.");
    if (schemaVersion != ImpactEnvelopeVersion || fileScopeKey != scope || archicadMajor != TAPIR_AC_VERSION
        || bindingId.IsEmpty () || recordedOperationId != operationId || recordedOperation != operation
        || recordedType != typeName || !IsHex64 (patchHash)
        || maxElements < 1 || maxElements > static_cast<Int32> (MaxImpactElements))
        return GS::UniString ("nativeImpact envelope identity or bounded version is invalid.");
    GS::Array<GS::UniString> primaryGuids;
    if (!raw->Get ("primaryGuids", primaryGuids) || !GuidListMatchesRequested (primaryGuids, requestedIds))
        return GS::UniString ("nativeImpact primaryGuids do not match the requested native targets.");
    GS::Array<GS::UniString> collateralGuids;
    if (!raw->Get ("collateralGuids", collateralGuids))
        return GS::UniString ("nativeImpact collateralGuids are required.");
    GS::Array<GS::UniString> collateralElementTypes;
    if (!raw->Get ("collateralElementTypes", collateralElementTypes)
        || collateralElementTypes.GetSize () != collateralGuids.GetSize ())
        return GS::UniString ("nativeImpact collateralElementTypes must match collateralGuids.");
    if (operation != "update" || (typeName != "Wall" && typeName != "Zone")
        || primaryGuids.GetSize () != 1 || collateralGuids.GetSize () != 1
        || maxElements < 2)
        return GS::UniString ("wall-zone-pair-v1 requires exactly one primary and one collateral member.");
    GS::Array<API_Guid> primary;
    for (const GS::ObjectState& requestedId : requestedIds) primary.Push (GetRequestedElementGuid (requestedId));
    GS::Array<API_Guid> collateral;
    for (const GS::UniString& value : collateralGuids) {
        const API_Guid guid = APIGuidFromString (value.ToCStr ());
        if (guid == APINULLGuid || collateral.Contains (guid) || primary.Contains (guid))
            return GS::UniString ("nativeImpact collateralGuids are malformed or overlap primaryGuids.");
        collateral.Push (guid);
        output.collateralIds.Push (IdFromGuid (guid));
    }
    const GSSize maxImpactElements = static_cast<GSSize> (maxElements);
    const GSSize primaryCount = static_cast<GSSize> (primary.GetSize ());
    const GSSize collateralCount = static_cast<GSSize> (collateral.GetSize ());
    if (primaryCount > maxImpactElements
        || collateralCount > maxImpactElements - primaryCount)
        return GS::UniString ("nativeImpact closure exceeds its declared cap.");
    for (const GS::UniString& collateralType : collateralElementTypes) {
        const bool expected = typeName == "Wall" ? collateralType == "Zone" : typeName == "Zone" ? collateralType == "Wall" : false;
        if (!expected) return GS::UniString ("nativeImpact collateralElementTypes do not match the bounded Wall/Zone pair.");
    }
    GS::ObjectState evidence;
    GS::UniString evidenceVersion;
    GS::UniString source;
    GS::UniString impactScope;
    GS::Array<GS::UniString> nativeCommands;
    GS::Array<GS::ObjectState> relationEdges;
    GS::Array<GS::ObjectState> ownershipEdges;
    bool relationComplete = false;
    bool ownershipComplete = false;
    bool frontierComplete = false;
    if (!raw->Get ("evidence", evidence)
        || !evidence.Get ("schemaVersion", evidenceVersion)
        || !evidence.Get ("source", source)
        || !evidence.Get ("scope", impactScope)
        || !evidence.Get ("nativeCommands", nativeCommands)
        || !evidence.Get ("relationComplete", relationComplete)
        || !evidence.Get ("ownershipComplete", ownershipComplete)
        || !evidence.Get ("frontierComplete", frontierComplete)
        || !evidence.Get ("relationEdges", relationEdges)
        || !evidence.Get ("ownershipEdges", ownershipEdges)
        || evidenceVersion != ImpactEvidenceVersion
        || source != "native-wall-zone-relations"
        || impactScope != ImpactScope
        || nativeCommands.IsEmpty ()
        || relationEdges.IsEmpty ()
        || !relationComplete || !ownershipComplete || !frontierComplete)
        return GS::UniString ("nativeImpact evidence does not prove a complete native relation frontier.");
    if (relationEdges.GetSize () != 1 || !ownershipEdges.IsEmpty ())
        return GS::UniString ("wall-zone-pair-v1 requires exactly one relation edge and no ownership edges.");
    const API_Guid expectedZoneGuid = typeName == "Wall"
        ? APIGuidFromString (collateralGuids[0].ToCStr ())
        : APIGuidFromString (primaryGuids[0].ToCStr ());
    const API_Guid expectedWallGuid = typeName == "Wall"
        ? APIGuidFromString (primaryGuids[0].ToCStr ())
        : APIGuidFromString (collateralGuids[0].ToCStr ());
    for (const GS::ObjectState& edge : relationEdges) {
        GS::UniString sourceGuid;
        GS::UniString targetGuid;
        GS::UniString relation;
        if (!edge.Get ("sourceGuid", sourceGuid) || !edge.Get ("targetGuid", targetGuid) || !edge.Get ("relation", relation))
            return GS::UniString ("nativeImpact relation edge is malformed.");
        const API_Guid source = APIGuidFromString (sourceGuid.ToCStr ());
        const API_Guid target = APIGuidFromString (targetGuid.ToCStr ());
        if (relation != "zone.wallPart" || source != expectedZoneGuid || target != expectedWallGuid
            || relation.IsEmpty () || source == APINULLGuid || target == APINULLGuid
            || (!primary.Contains (source) && !collateral.Contains (source))
            || (!primary.Contains (target) && !collateral.Contains (target)))
            return GS::UniString ("nativeImpact relation edge is malformed.");
    }
    for (const GS::ObjectState& edge : ownershipEdges) {
        GS::UniString childGuid;
        GS::UniString ownerGuid;
        GS::UniString relation;
        if (!edge.Get ("childGuid", childGuid) || !edge.Get ("ownerGuid", ownerGuid) || !edge.Get ("relation", relation))
            return GS::UniString ("nativeImpact ownership edge is malformed.");
        const API_Guid child = APIGuidFromString (childGuid.ToCStr ());
        const API_Guid owner = APIGuidFromString (ownerGuid.ToCStr ());
        if (relation.IsEmpty () || child == APINULLGuid || owner == APINULLGuid
            || (!primary.Contains (child) && !collateral.Contains (child))
            || (!primary.Contains (owner) && !collateral.Contains (owner)))
            return GS::UniString ("nativeImpact ownership edge is malformed.");
    }
    output.collateralGuids = collateralGuids;
    output.collateralElementTypes = collateralElementTypes;
    output.impactIds = requestedIds;
    for (const GS::ObjectState& id : output.collateralIds) output.impactIds.Push (id);
    return {};
}

GS::Optional<GS::UniString> ValidateNativeImpactSnapshotTypes (
    const GS::Array<GS::ObjectState>& beforeRows,
    const GS::Array<GS::ObjectState>& impactIds,
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::UniString& typeName,
    const NativeImpactEnvelope* impact)
{
    if (impact == nullptr) return {};
    if (beforeRows.GetSize () != impactIds.GetSize ())
        return GS::UniString ("SNAPSHOT_COVERAGE_INCOMPLETE: nativeImpact before-state does not cover the exact hashed member set.");

    GS::Array<API_Guid> seen;
    const GSSize collateralIdCount = static_cast<GSSize> (impact->collateralIds.GetSize ());
    const GSSize collateralTypeCount = static_cast<GSSize> (impact->collateralElementTypes.GetSize ());
    for (GSSize rowIndex = 0; rowIndex < static_cast<GSSize> (beforeRows.GetSize ()); ++rowIndex) {
        const GS::ObjectState& row = beforeRows[rowIndex];
        const GS::ObjectState* id = row.Get ("elementId");
        const API_Guid guid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
        if (guid == APINULLGuid || seen.Contains (guid))
            return GS::UniString ("NATIVE_TYPE_MISMATCH: nativeImpact before-state contains an invalid or duplicate member GUID.");
        seen.Push (guid);

        GS::UniString expectedType = typeName;
        bool selected = false;
        for (GSSize primaryIndex = 0; primaryIndex < static_cast<GSSize> (requestedIds.GetSize ()); ++primaryIndex) {
            if (GetRequestedElementGuid (requestedIds[primaryIndex]) == guid) {
                selected = true;
                break;
            }
        }
        if (!selected) {
            for (GSSize collateralIndex = 0; collateralIndex < collateralIdCount; ++collateralIndex) {
                if (GetRequestedElementGuid (impact->collateralIds[collateralIndex]) == guid) {
                    if (collateralIndex >= collateralTypeCount)
                        return GS::UniString ("NATIVE_TYPE_MISMATCH: nativeImpact collateral type index is incomplete.");
                    expectedType = impact->collateralElementTypes[collateralIndex];
                    selected = true;
                    break;
                }
            }
        }
        if (!selected)
            return GS::UniString ("NATIVE_TYPE_MISMATCH: nativeImpact before-state contains a GUID outside the hashed primary/collateral set.");

        GS::UniString actualType;
        if (!row.Get ("elementType", actualType) || actualType != expectedType)
            return GS::UniString ("NATIVE_TYPE_MISMATCH: nativeImpact before-state GUID does not match its hashed native element type.");
    }
    return {};
}

} // namespace NativeCrudInternal
