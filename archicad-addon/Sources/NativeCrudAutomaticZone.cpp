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
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using namespace NativeCrudInternal;

namespace {

constexpr const char* AutomaticZoneUpdateContractVersion = "native-zone-update-v1";
constexpr const char* AutomaticZoneUpdateDeveloperGate = "disposable-automatic-zone-recreate-v1";

// A replacement Zone is only safe after every source datum that can affect
// the element has been captured and can be restored.  The current bounded
// command can read the public scalar Zone state, polygon memo, and room
// relations, but it does not yet clone and verify the Zone stamp GDL
// parameters, element properties, and classifications.  Keep this explicit
// compile-time gate false until that complete manifest exists.  In
// particular, never let the create-then-delete body below turn a partial
// scalar comparison into a destructive update.
constexpr bool CompleteAutomaticZonePreservationManifestSupported = false;

GS::ObjectState AutomaticZoneVerification (
    const bool beforeReadbackComplete,
    const bool afterReadbackComplete,
    const bool oldZoneAbsent,
    const bool resultingZonePresent,
    const bool automaticModePreserved,
    const bool referencePositionPreserved,
    const bool persistedPolygonObserved,
    const bool preservationManifestComplete)
{
    return GS::ObjectState (
        "beforeReadbackComplete", beforeReadbackComplete,
        "afterReadbackComplete", afterReadbackComplete,
        "oldZoneAbsent", oldZoneAbsent,
        "resultingZonePresent", resultingZonePresent,
        "automaticModePreserved", automaticModePreserved,
        "referencePositionPreserved", referencePositionPreserved,
        "persistedPolygonObserved", persistedPolygonObserved,
        "preservationManifestComplete", preservationManifestComplete);
}

GS::ObjectState AutomaticZoneUpdateResult (
    const GS::UniString& operationId,
    const char* status,
    const API_Guid& previousGuid,
    const API_Guid& resultingGuid,
    const bool identityChanged,
    const bool geometryChanged,
    const char* preservationStatus,
    const GS::ObjectState& verification,
    const char* reason = nullptr,
    const std::initializer_list<const char*> warnings = {})
{
    GS::ObjectState result;
    result.Add ("contractVersion", AutomaticZoneUpdateContractVersion);
    result.Add ("operationId", operationId);
    result.Add ("status", status);
    result.Add ("mechanism", "create_then_delete");
    result.Add ("previousZoneId", CreateGuidObjectState (previousGuid));
    if (resultingGuid != APINULLGuid)
        result.Add ("resultingZoneId", CreateGuidObjectState (resultingGuid));
    result.Add ("identityChanged", identityChanged);
    result.Add ("geometryChanged", geometryChanged);
    result.Add ("preservationStatus", preservationStatus);
    result.Add ("verification", verification);
    result.AddList<GS::ObjectState> ("collateralChangedIds");
    const auto& warningList = result.AddList<GS::UniString> ("warnings");
    for (const char* warning : warnings)
        warningList (GS::UniString (warning));
    if (reason != nullptr && reason[0] != '\0')
        result.Add ("reason", GS::UniString (reason));
    return result;
}

bool SameAutomaticZoneCoordinates (const API_Coord& left, const API_Coord& right)
{
    return IsSame2DCoordinate (left, right);
}

bool SameAutomaticZonePolygons (const std::vector<PolygonData>& left, const std::vector<PolygonData>& right)
{
    if (left.size () != right.size ())
        return false;
    for (std::size_t polygonIndex = 0; polygonIndex < left.size (); ++polygonIndex) {
        const PolygonData& leftPolygon = left[polygonIndex];
        const PolygonData& rightPolygon = right[polygonIndex];
        if (leftPolygon.coords.size () != rightPolygon.coords.size ()
            || leftPolygon.arcs.size () != rightPolygon.arcs.size ())
            return false;
        for (std::size_t coordinateIndex = 0; coordinateIndex < leftPolygon.coords.size (); ++coordinateIndex) {
            if (!SameAutomaticZoneCoordinates (leftPolygon.coords[coordinateIndex], rightPolygon.coords[coordinateIndex]))
                return false;
        }
        for (std::size_t arcIndex = 0; arcIndex < leftPolygon.arcs.size (); ++arcIndex) {
            const API_PolyArc& leftArc = leftPolygon.arcs[arcIndex];
            const API_PolyArc& rightArc = rightPolygon.arcs[arcIndex];
            if (leftArc.begIndex != rightArc.begIndex || leftArc.endIndex != rightArc.endIndex
                || leftArc.arcAngle != rightArc.arcAngle)
                return false;
        }
    }
    return true;
}

bool HasAutomaticZonePolygon (const API_Guid& guid, std::vector<PolygonData>& polygons)
{
    polygons = GetPolygonsFromMemoCoords (guid);
    if (polygons.empty ())
        return false;
    for (const PolygonData& polygon : polygons) {
        if (polygon.coords.size () < 3 || !polygon.arcs.empty ())
            return false;
    }
    return true;
}

void CopyAutomaticZonePreservationFields (const API_Element& source, API_Element& target)
{
    target.header.floorInd = source.header.floorInd;
    target.header.layer = source.header.layer;

    target.zone.pen = source.zone.pen;
    target.zone.catInd = source.zone.catInd;
    target.zone.useStampPens = source.zone.useStampPens;
    target.zone.manual = false;
    target.zone.refLineFlag = source.zone.refLineFlag;
    target.zone.show_found_poly = source.zone.show_found_poly;
    std::copy (source.zone.roomName, source.zone.roomName + API_UniLongNameLen, target.zone.roomName);
    std::copy (source.zone.roomNoStr, source.zone.roomNoStr + 32, target.zone.roomNoStr);
    target.zone.roomBaseLev = source.zone.roomBaseLev;
    target.zone.roomHeight = source.zone.roomHeight;
    target.zone.roomTopOffset = source.zone.roomTopOffset;
    target.zone.roomFlThick = source.zone.roomFlThick;
    target.zone.roomLSize = source.zone.roomLSize;
    target.zone.reducePercent = source.zone.reducePercent;
    target.zone.material = source.zone.material;
    target.zone.stampAngle = source.zone.stampAngle;
    target.zone.fixedAngle = source.zone.fixedAngle;
    target.zone.oneMat = source.zone.oneMat;
    target.zone.floorFillPen = source.zone.floorFillPen;
    target.zone.floorFillInd = source.zone.floorFillInd;
    target.zone.floorFillBGPen = source.zone.floorFillBGPen;
    target.zone.floorContLPen = source.zone.floorContLPen;
    target.zone.floorContLType = source.zone.floorContLType;
    target.zone.useContourLine = source.zone.useContourLine;
    target.zone.useLocalOrigo = source.zone.useLocalOrigo;
    target.zone.use3DHatching = source.zone.use3DHatching;
    target.zone.useFloorFill = source.zone.useFloorFill;
    target.zone.libInd = source.zone.libInd;
    target.zone.linkToSettings = source.zone.linkToSettings;
    target.zone.relativeTopStory = source.zone.relativeTopStory;
    target.zone.pos = source.zone.pos;
    target.zone.refPos = source.zone.refPos;
    target.zone.locOrigo = source.zone.locOrigo;
    target.zone.fillAngle = source.zone.fillAngle;
    // stampGuid is generated for the replacement element and is therefore
    // intentionally excluded from the preservation manifest.
}

bool SameAutomaticZonePreservationFields (const API_Element& left, const API_Element& right)
{
    if (left.header.floorInd != right.header.floorInd || left.header.layer != right.header.layer)
        return false;
    if (left.zone.pen != right.zone.pen || left.zone.catInd != right.zone.catInd
        || left.zone.useStampPens != right.zone.useStampPens || left.zone.manual != right.zone.manual
        || left.zone.refLineFlag != right.zone.refLineFlag || left.zone.show_found_poly != right.zone.show_found_poly)
        return false;
    if (!std::equal (left.zone.roomName, left.zone.roomName + API_UniLongNameLen, right.zone.roomName)
        || !std::equal (left.zone.roomNoStr, left.zone.roomNoStr + 32, right.zone.roomNoStr))
        return false;
    if (left.zone.roomBaseLev != right.zone.roomBaseLev || left.zone.roomHeight != right.zone.roomHeight
        || left.zone.roomTopOffset != right.zone.roomTopOffset || left.zone.roomFlThick != right.zone.roomFlThick
        || left.zone.roomLSize != right.zone.roomLSize || left.zone.reducePercent != right.zone.reducePercent
        || left.zone.material != right.zone.material || left.zone.stampAngle != right.zone.stampAngle
        || left.zone.fixedAngle != right.zone.fixedAngle || left.zone.oneMat != right.zone.oneMat
        || left.zone.floorFillPen != right.zone.floorFillPen || left.zone.floorFillInd != right.zone.floorFillInd
        || left.zone.floorFillBGPen != right.zone.floorFillBGPen || left.zone.floorContLPen != right.zone.floorContLPen
        || left.zone.floorContLType != right.zone.floorContLType || left.zone.useContourLine != right.zone.useContourLine
        || left.zone.useLocalOrigo != right.zone.useLocalOrigo || left.zone.use3DHatching != right.zone.use3DHatching
        || left.zone.useFloorFill != right.zone.useFloorFill || left.zone.libInd != right.zone.libInd
        || left.zone.relativeTopStory != right.zone.relativeTopStory)
        return false;
    if (left.zone.linkToSettings.homeStoryDifference != right.zone.linkToSettings.homeStoryDifference
        || left.zone.linkToSettings.newCreationMode != right.zone.linkToSettings.newCreationMode)
        return false;
    return SameAutomaticZoneCoordinates (left.zone.pos, right.zone.pos)
        && SameAutomaticZoneCoordinates (left.zone.refPos, right.zone.refPos)
        && SameAutomaticZoneCoordinates (left.zone.locOrigo, right.zone.locOrigo)
        && left.zone.fillAngle == right.zone.fillAngle;
}

std::string AutomaticZoneGuidText (const API_Guid& guid)
{
    return std::string (APIGuidToString (guid).ToCStr (CC_UTF8).Get ());
}

bool ReadAutomaticZoneRelationManifest (const API_Guid& guid, std::vector<std::string>& manifest)
{
    API_RoomRelation relation = {};
    const GSErrCode error = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &relation, 0);
    const GS::OnExit dispose ([&relation] () { ACAPI_DisposeRoomRelationHdls (&relation); });
    if (error != NoError)
        return false;

    manifest.clear ();
    for (const auto& pair : relation.elementsGroupedByType) {
#ifndef ServerMainVers_2700
        if (pair.key == nullptr || pair.value == nullptr)
            continue;
        const std::string typeName = std::string (GetElementTypeNonLocalizedName (*pair.key).ToCStr (CC_UTF8).Get ());
        for (const API_Guid& relatedGuid : *pair.value)
            manifest.push_back (typeName + ":" + AutomaticZoneGuidText (relatedGuid));
#else
        if (pair.value.IsEmpty ())
            continue;
        const std::string typeName = std::string (GetElementTypeNonLocalizedName (pair.key.typeID).ToCStr (CC_UTF8).Get ());
        for (const API_Guid& relatedGuid : pair.value)
            manifest.push_back (typeName + ":" + AutomaticZoneGuidText (relatedGuid));
#endif
    }
    for (const API_WallPart& wallPart : relation.wallPart) {
        std::ostringstream part;
        part << "wallPart:" << AutomaticZoneGuidText (wallPart.guid)
             << ":" << std::setprecision (17) << wallPart.roomEdge
             << ":" << wallPart.tBeg << ":" << wallPart.tEnd;
        manifest.push_back (part.str ());
    }
    std::sort (manifest.begin (), manifest.end ());
    return true;
}

bool ReadAutomaticZoneSnapshotFingerprint (
    const GS::ObjectState& response,
    GS::UniString& fingerprint,
    bool& complete)
{
    complete = false;
    GS::Array<GS::ObjectState> rows;
    if (!response.Get ("elements", rows) || rows.GetSize () != 1)
        return false;
    const GS::ObjectState& row = rows[0];
    GS::UniString type;
    if (!row.Get ("elementType", type) || type != "Zone"
        || !row.Get ("fingerprint", fingerprint) || fingerprint.IsEmpty ())
        return false;
    complete = true;
    return true;
}

} // namespace

UpdateAutomaticZoneGeometryCommand::UpdateAutomaticZoneGeometryCommand () : CommandBase (CommonSchema::NotUsed) {}

GS::String UpdateAutomaticZoneGeometryCommand::GetName () const { return "UpdateAutomaticZoneGeometry"; }

GS::Optional<GS::UniString> UpdateAutomaticZoneGeometryCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"operationId":{"type":"string","maxLength":200},"expectedFileScopeKey":{"type":"string"},"zoneId":{"$ref":"#/ElementId"},"expectedFingerprint":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"},"allowIdentityReplacement":{"type":"boolean"},"developerGate":{"type":"string","enum":["disposable-automatic-zone-recreate-v1"]}},"additionalProperties":false,"required":["contractVersion","operationId","expectedFileScopeKey","zoneId","expectedFingerprint","allowIdentityReplacement","developerGate"]})";
}

GS::Optional<GS::UniString> UpdateAutomaticZoneGeometryCommand::GetResponseSchema () const { return {}; }
GS::Optional<GS::UniString> UpdateAutomaticZoneGeometryCommand::GetRawResponseSchema () const { return {}; }

GS::ObjectState UpdateAutomaticZoneGeometryCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    if (!HasOnlyObjectStateFields (parameters, {
        "contractVersion", "operationId", "expectedFileScopeKey", "zoneId",
        "expectedFingerprint", "allowIdentityReplacement", "developerGate"
    }))
        return CreateErrorResponse (APIERR_BADPARS, "Automatic Zone update contains unsupported fields.");

    GS::UniString expectedScope;
    parameters.Get ("expectedFileScopeKey", expectedScope);
    GS::ObjectState scopeParameters;
    scopeParameters.Add ("fileScopeKey", expectedScope);
    const GS::Optional<GS::UniString> scopeError = ValidateScope (scopeParameters);
    if (scopeError.HasValue ())
        return ScopeError (scopeError);

    GS::UniString contract;
    GS::UniString operationId;
    GS::UniString expectedFingerprint;
    GS::UniString developerGate;
    bool allowIdentityReplacement = false;
    const GS::ObjectState* zoneId = parameters.Get ("zoneId");
    if (!parameters.Get ("contractVersion", contract) || contract != AutomaticZoneUpdateContractVersion
        || !parameters.Get ("operationId", operationId) || operationId.IsEmpty () || operationId.GetLength () > 200
        || !parameters.Get ("expectedFingerprint", expectedFingerprint) || !IsHex64 (expectedFingerprint)
        || zoneId == nullptr || GetGuidFromObjectState (*zoneId) == APINULLGuid
        || !parameters.Get ("allowIdentityReplacement", allowIdentityReplacement)
        || !parameters.Get ("developerGate", developerGate) || developerGate != AutomaticZoneUpdateDeveloperGate)
        return CreateErrorResponse (APIERR_BADPARS, "Automatic Zone update identity, scope, fingerprint or developer gate is invalid.");

    const API_Guid previousGuid = GetGuidFromObjectState (*zoneId);
    if (!allowIdentityReplacement) {
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (false, false, false, false, false, false, false, false),
            "allowIdentityReplacement must be true because the portable fallback allocates a new Zone GUID.");
    }

    API_Element source = {};
    source.header.guid = previousGuid;
    GSErrCode error = ACAPI_Element_Get (&source);
    if (error != NoError)
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unknown",
            AutomaticZoneVerification (false, false, false, false, false, false, false, false),
            "The requested Zone could not be read.");
    if (GetElemTypeId (source.header) != API_ZoneID)
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (false, false, false, false, false, false, false, false),
            "The requested element is not a native Zone.");
    if (source.zone.manual)
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "Automatic Zone update accepts only Zones with manual=false.");
    if (!std::isfinite (source.zone.refPos.x) || !std::isfinite (source.zone.refPos.y))
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "The automatic Zone referencePosition is not finite.");

    GS::Array<GS::ObjectState> requestedIds;
    requestedIds.Push (GS::ObjectState ("elementId", CreateGuidObjectState (previousGuid)));
    bool beforeComplete = false;
    const GS::ObjectState beforeSnapshots = ExactSnapshots (requestedIds, expectedScope, processControl, beforeComplete);
    GS::UniString actualFingerprint;
    bool fingerprintRowComplete = false;
    if (!beforeComplete || !ReadAutomaticZoneSnapshotFingerprint (beforeSnapshots, actualFingerprint, fingerprintRowComplete)
        || !fingerprintRowComplete || LowerAscii (actualFingerprint) != LowerAscii (expectedFingerprint))
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unknown",
            AutomaticZoneVerification (beforeComplete && fingerprintRowComplete, false, false, false, false, false, false, false),
            "STALE_NATIVE_SNAPSHOT: the Zone fingerprint changed before dispatch.");

    // Re-read the source after the exact fingerprint check.  The replacement
    // must be built from the state that was validated by this command, rather
    // than from the earlier optimistic read performed before the snapshot.
    // If the element disappeared or changed type in the meantime, fail closed
    // before any create/delete transaction.
    API_Element validatedSource = {};
    validatedSource.header.guid = previousGuid;
    error = ACAPI_Element_Get (&validatedSource);
    if (error != NoError || GetElemTypeId (validatedSource.header) != API_ZoneID
        || validatedSource.zone.manual
        || !std::isfinite (validatedSource.zone.refPos.x)
        || !std::isfinite (validatedSource.zone.refPos.y))
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unknown",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "STALE_NATIVE_SNAPSHOT: the automatic Zone changed while the validated snapshot was being prepared.");
    source = validatedSource;

    std::vector<PolygonData> beforePolygons;
    if (!HasAutomaticZonePolygon (previousGuid, beforePolygons))
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "The source Zone polygon memo is unavailable or has unsupported topology.");

    std::vector<std::string> beforeRelations;
    if (!ReadAutomaticZoneRelationManifest (previousGuid, beforeRelations))
        return AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "incomplete",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "The source Zone relation manifest is unavailable; no replacement was attempted.");

    // Fail closed before entering the undoable transaction while the full
    // preservation manifest is not proven.  The transaction implementation
    // below remains the single future route for AC25/AC28, but it must not be
    // reachable with scalar-only preservation evidence.
    if (!CompleteAutomaticZonePreservationManifestSupported) {
        GS::ObjectState result = AutomaticZoneUpdateResult (
            operationId, "rejected", previousGuid, APINULLGuid, false, false, "unsupported",
            AutomaticZoneVerification (true, false, false, false, false, false, false, false),
            "PRESERVATION_MANIFEST_UNSUPPORTED: Zone stamp GDL parameters, properties and classifications are not yet captured and verified; no replacement was attempted.",
            { "The native fallback is disabled until the complete Zone preservation manifest is implemented." });
        return result;
    }

    API_Guid resultingGuid = APINULLGuid;
    bool callbackErrorSet = false;
    GSErrCode callbackError = NoError;
    bool candidatePolygonObserved = false;
    bool candidateAutomatic = false;
    bool candidateReferencePreserved = false;
    bool candidatePreservationComplete = false;
    bool candidateGeometryChanged = false;
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Update Automatic Zone Geometry", [&]() -> GSErrCode {
        API_Element replacement = {};
        API_ElementMemo memo = {};
        NativeMemoGuard memoGuard (memo);
#ifdef ServerMainVers_2600
        replacement.header.type = API_ZoneID;
#else
        replacement.header.typeID = API_ZoneID;
#endif
        GSErrCode transactionError = ACAPI_Element_GetDefaults (&replacement, &memo);
        if (transactionError != NoError) {
            callbackError = transactionError;
            callbackErrorSet = true;
            return transactionError;
        }
        CopyAutomaticZonePreservationFields (source, replacement);
        replacement.zone.manual = false;
        replacement.zone.refPos = source.zone.refPos;
        replacement.zone.pos = source.zone.pos;
        transactionError = ACAPI_Element_Create (&replacement, &memo);
        if (transactionError != NoError || replacement.header.guid == APINULLGuid || replacement.header.guid == previousGuid) {
            callbackError = transactionError != NoError ? transactionError : APIERR_BADID;
            callbackErrorSet = true;
            return callbackError;
        }
        resultingGuid = replacement.header.guid;

        API_Element candidate = {};
        candidate.header.guid = resultingGuid;
        transactionError = ACAPI_Element_Get (&candidate);
        if (transactionError != NoError) {
            callbackError = transactionError;
            callbackErrorSet = true;
            return transactionError;
        }
        candidateAutomatic = GetElemTypeId (candidate.header) == API_ZoneID && !candidate.zone.manual;
        candidateReferencePreserved = candidateAutomatic && SameAutomaticZoneCoordinates (candidate.zone.refPos, source.zone.refPos);
        std::vector<PolygonData> candidatePolygons;
        candidatePolygonObserved = HasAutomaticZonePolygon (resultingGuid, candidatePolygons);
        candidateGeometryChanged = candidatePolygonObserved && !SameAutomaticZonePolygons (beforePolygons, candidatePolygons);
        candidatePreservationComplete = candidateAutomatic && candidateReferencePreserved
            && candidatePolygonObserved && SameAutomaticZonePreservationFields (source, candidate);
        if (!candidatePreservationComplete) {
            callbackError = APIERR_BADPARS;
            callbackErrorSet = true;
            return callbackError;
        }

        GS::Array<API_Guid> oldIds;
        oldIds.Push (previousGuid);
        transactionError = ACAPI_Element_Delete (oldIds);
        if (transactionError != NoError) {
            callbackError = transactionError;
            callbackErrorSet = true;
            return transactionError;
        }
        API_Elem_Head oldHeader = {};
        oldHeader.guid = previousGuid;
        if (ACAPI_Element_GetHeader (&oldHeader) != APIERR_BADID) {
            callbackError = APIERR_BADID;
            callbackErrorSet = true;
            return callbackError;
        }

        API_Element afterDelete = {};
        afterDelete.header.guid = resultingGuid;
        transactionError = ACAPI_Element_Get (&afterDelete);
        if (transactionError != NoError) {
            callbackError = transactionError;
            callbackErrorSet = true;
            return transactionError;
        }
        std::vector<PolygonData> afterPolygons;
        std::vector<std::string> afterRelations;
        const bool afterPolygonObserved = HasAutomaticZonePolygon (resultingGuid, afterPolygons);
        const bool afterRelationsReadable = ReadAutomaticZoneRelationManifest (resultingGuid, afterRelations);
        const bool afterPreserved = GetElemTypeId (afterDelete.header) == API_ZoneID && !afterDelete.zone.manual
            && SameAutomaticZoneCoordinates (afterDelete.zone.refPos, source.zone.refPos)
            && afterPolygonObserved && SameAutomaticZonePreservationFields (source, afterDelete)
            && afterRelationsReadable && afterRelations == beforeRelations;
        if (!afterPreserved) {
            callbackError = APIERR_BADPARS;
            callbackErrorSet = true;
            return callbackError;
        }
        candidatePreservationComplete = true;
        return NoError;
    });
    if (undoError != NoError && !callbackErrorSet)
        callbackError = undoError;
    if (callbackError != NoError) {
        // ACAPI_CallUndoableCommand should restore the source on every
        // callback error, but do not infer that from the error code alone.
        // Read the two identities after the transaction and downgrade to an
        // unknown outcome if rollback cannot be independently observed.
        API_Elem_Head rollbackSourceHeader = {};
        rollbackSourceHeader.guid = previousGuid;
        const bool rollbackSourcePresent = ACAPI_Element_GetHeader (&rollbackSourceHeader) == NoError;
        bool rollbackReplacementAbsent = true;
        if (resultingGuid != APINULLGuid) {
            API_Elem_Head rollbackReplacementHeader = {};
            rollbackReplacementHeader.guid = resultingGuid;
            rollbackReplacementAbsent = ACAPI_Element_GetHeader (&rollbackReplacementHeader) == APIERR_BADID;
        }
        const bool rollbackVerified = rollbackSourcePresent && rollbackReplacementAbsent;
        return AutomaticZoneUpdateResult (
            operationId, rollbackVerified ? "failed_rolled_back" : "unknown", previousGuid,
            rollbackVerified ? APINULLGuid : resultingGuid, !rollbackVerified, false, "unknown",
            AutomaticZoneVerification (true, false, !rollbackSourcePresent, !rollbackReplacementAbsent,
                                       candidateAutomatic, candidateReferencePreserved,
                                       candidatePolygonObserved, false),
            rollbackVerified
                ? "Automatic Zone replacement failed and independent identity readback confirmed rollback."
                : "Automatic Zone replacement failed and rollback could not be independently confirmed.",
            rollbackVerified ? std::initializer_list<const char*> {
                "No replacement was committed; the original Zone remains."
            } : std::initializer_list<const char*> {
                "Do not retry this operation; inspect the source and candidate identities before deciding on repair."
            });
    }

    // The callback has committed.  This is a second, independent readback;
    // an incomplete result is unknown and is never retried by this command.
    API_Elem_Head oldHeader = {};
    oldHeader.guid = previousGuid;
    const bool oldZoneAbsent = ACAPI_Element_GetHeader (&oldHeader) == APIERR_BADID;
    API_Element resultingElement = {};
    resultingElement.header.guid = resultingGuid;
    const bool resultingZonePresent = ACAPI_Element_Get (&resultingElement) == NoError
        && GetElemTypeId (resultingElement.header) == API_ZoneID;
    GS::Array<GS::ObjectState> resultingIds;
    resultingIds.Push (GS::ObjectState ("elementId", CreateGuidObjectState (resultingGuid)));
    bool afterComplete = false;
    const GS::ObjectState afterSnapshots = resultingZonePresent
        ? ExactSnapshots (resultingIds, expectedScope, processControl, afterComplete)
        : GS::ObjectState ();
    GS::UniString resultingFingerprint;
    bool resultingRowComplete = false;
    const bool resultingSnapshotReadable = resultingZonePresent
        && afterComplete && ReadAutomaticZoneSnapshotFingerprint (afterSnapshots, resultingFingerprint, resultingRowComplete);
    std::vector<PolygonData> afterPolygons;
    const bool persistedPolygonObserved = resultingZonePresent && HasAutomaticZonePolygon (resultingGuid, afterPolygons);
    candidateGeometryChanged = persistedPolygonObserved && !SameAutomaticZonePolygons (beforePolygons, afterPolygons);
    const bool automaticModePreserved = resultingZonePresent && !resultingElement.zone.manual;
    const bool referencePositionPreserved = automaticModePreserved
        && SameAutomaticZoneCoordinates (source.zone.refPos, resultingElement.zone.refPos);
    const bool scalarPreserved = resultingZonePresent && SameAutomaticZonePreservationFields (source, resultingElement);
    std::vector<std::string> afterRelations;
    const bool relationReadable = resultingZonePresent && ReadAutomaticZoneRelationManifest (resultingGuid, afterRelations);
    const bool preservationComplete = resultingSnapshotReadable && scalarPreserved
        && relationReadable && afterRelations == beforeRelations;
    const bool readbackComplete = oldZoneAbsent && resultingZonePresent && resultingSnapshotReadable
        && automaticModePreserved && referencePositionPreserved && persistedPolygonObserved && preservationComplete;
    if (!readbackComplete) {
        GS::ObjectState result = AutomaticZoneUpdateResult (
            operationId, "unknown", previousGuid, resultingGuid, true, candidateGeometryChanged,
            preservationComplete ? "complete" : "incomplete",
            AutomaticZoneVerification (true, afterComplete, oldZoneAbsent, resultingZonePresent,
                                       automaticModePreserved, referencePositionPreserved,
                                       persistedPolygonObserved, preservationComplete),
            "Automatic Zone transaction committed but independent native readback is incomplete.",
            { "Do not retry this operation; inspect the old/new Zone identities before deciding on a forward repair." });
        return result;
    }
    GS::ObjectState result = AutomaticZoneUpdateResult (
        operationId,
        // A committed create-then-delete transaction is an applied update,
        // even when Archicad returns a geometrically identical polygon.  The
        // `unchanged` status is reserved for a proven native refresh route.
        "applied",
        previousGuid,
        resultingGuid,
        true,
        candidateGeometryChanged,
        "complete",
        AutomaticZoneVerification (true, true, true, true, true, true, true, true),
        nullptr,
        {
            "Zone GUID changed because this portable fallback used create_then_delete.",
            "Collateral Zone/dependency enumeration remains outside this one-zone native command."
        });
    return result;
}
