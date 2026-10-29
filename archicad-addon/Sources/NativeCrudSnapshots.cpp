#include "NativeCrudCommands.hpp"

#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <string>

using namespace NativeCrudInternal;

namespace {

bool IsSupportedNativeTypeName (const GS::UniString& typeName)
{
    return IsSupportedNativeType (GetElementTypeFromNonLocalizedName (typeName));
}

} // namespace

namespace NativeCrudInternal {

// DetailsOfElements is a stable native read DTO, but its historical field
// names are not the public native-crud-v2 writable schema.  Keep the wire
// snapshot canonical so an agent can copy a `beforeState.writableState` value
// into a sparse UPDATE without learning a second Archicad vocabulary.  Only
// the allowlisted fields are projected; raw DevKit/detail objects are not
// exposed through writableState.
void CopyStringField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    GS::UniString value;
    if (source.Get (sourceName, value)) target.Add (targetName, value);
}

void CopyIntField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    Int32 value = 0;
    if (source.Get (sourceName, value)) target.Add (targetName, value);
}

void CopyDoubleField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    double value = 0.0;
    if (source.Get (sourceName, value)) target.Add (targetName, value);
}

void CopyBoolField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    bool value = false;
    if (source.Get (sourceName, value)) target.Add (targetName, value);
}

void CopyObjectField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    const GS::ObjectState* value = source.Get (sourceName);
    if (value != nullptr) target.Add (targetName, *value);
}

void CopyArrayField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    GS::Array<GS::ObjectState> values;
    if (!source.Get (sourceName, values)) return;
    const auto& output = target.AddList<GS::ObjectState> (targetName);
    for (const GS::ObjectState& value : values) output (value);
}

} // namespace NativeCrudInternal

namespace {

void CopyIntArrayField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    GS::Array<Int32> values;
    if (!source.Get (sourceName, values)) return;
    const auto& output = target.AddList<Int32> (targetName);
    for (const Int32 value : values) output (value);
}

void CopyPolygonField (const GS::ObjectState& source, GS::ObjectState& target)
{
    GS::Array<GS::ObjectState> coordinates;
    if (!source.Get ("polygonOutline", coordinates)) return;
    GS::ObjectState polygon;
    const auto& outputCoordinates = polygon.AddList<GS::ObjectState> ("coordinates");
    for (const GS::ObjectState& coordinate : coordinates) outputCoordinates (coordinate);
    CopyArrayField (source, "polygonArcs", polygon, "arcs");
    CopyArrayField (source, "holes", polygon, "holes");
    target.Add ("polygon", polygon);
}

GS::ObjectState CanonicalWritableState (
    const GS::UniString& typeName,
    const GS::ObjectState& details,
    const GS::ObjectState& row)
{
    GS::ObjectState state;
    Int32 storyIndex = 0;
    if (row.Get ("floorIndex", storyIndex)) state.Add ("storyIndex", storyIndex);

    if (typeName == "Zone") {
        CopyStringField (details, "name", state, "name");
        CopyStringField (details, "numberStr", state, "number");
        CopyObjectField (details, "categoryAttributeId", state, "category");
        CopyObjectField (details, "referencePosition", state, "referencePosition");
        // `isManual` is factual mode evidence, not an accepted writable
        // field.  Keeping it in the canonical snapshot makes the automatic
        // Zone precondition part of the fingerprint instead of relying on a
        // caller-invented detail value.
        CopyBoolField (details, "isManual", state, "isManual");
        GS::ObjectState stamp;
        CopyObjectField (details, "stampPosition", stamp, "position");
        CopyDoubleField (details, "stampAngle", stamp, "angle");
        CopyBoolField (details, "fixedStampAngle", stamp, "fixedAngle");
        if (!stamp.IsEmpty ()) state.Add ("stampPlacement", stamp);
        CopyPolygonField (details, state);
    } else if (typeName == "Wall") {
        CopyObjectField (details, "begCoordinate", state, "begCoordinate");
        CopyObjectField (details, "endCoordinate", state, "endCoordinate");
        // `elevation` is the bottom level above the Wall's own story (D-306).
        CopyDoubleField (details, "bottomOffset", state, "elevation");
        CopyDoubleField (details, "height", state, "height");
        CopyDoubleField (details, "begThickness", state, "thickness");
        CopyDoubleField (details, "offset", state, "offset");
        CopyStringField (details, "structureType", state, "structure");
        CopyObjectField (details, "compositeId", state, "composite");
        CopyStringField (details, "referenceLineLocation", state, "referenceLine");
        // Which side of the reference line the Wall body lies on (V1-022).
        CopyBoolField (details, "flipped", state, "flipped");
    } else if (typeName == "Column") {
        CopyObjectField (details, "origin", state, "origin");
        // Story-relative like the Wall's (D-306); factualState keeps the
        // absolute level.
        CopyDoubleField (details, "bottomOffset", state, "elevation");
        CopyDoubleField (details, "height", state, "height");
        CopyDoubleField (details, "axisRotationAngle", state, "rotation");
        CopyDoubleField (details, "slantAngle", state, "slant");
        // Column fingerprints always include factual memo topology and
        // linkage. Expose writable dimensions only for the exact supported
        // one-segment, vertical, rectangular Basic section; never synthesize
        // dimensions when memo evidence is missing or outside that shape.
        const GS::ObjectState* section = details.Get ("columnSectionReadback");
        if (section != nullptr) {
            state.Add ("section", *section);
            GS::UniString sectionStatus;
            GS::Array<GS::ObjectState> segments;
            bool simple = false;
            Int32 segmentCount = 0;
            if (section->Get ("status", sectionStatus) && sectionStatus == "available"
                && section->Get ("simpleVerticalSingleSegment", simple) && simple
                && section->Get ("segmentCount", segmentCount) && segmentCount == 1
                && section->Get ("segments", segments) && segments.GetSize () == 1) {
                GS::UniString shape;
                GS::UniString structure;
                GS::ObjectState dimensions;
                bool circleBased = true;
                if (segments[0].Get ("shape", shape) && shape == "rectangular"
                    && segments[0].Get ("structure", structure) && structure == "Basic"
                    && segments[0].Get ("circleBased", circleBased) && !circleBased
                    && segments[0].Get ("dimensions", dimensions))
                    state.Add ("dimensions", dimensions);
            }
        }
    } else if (typeName == "Text") {
        CopyObjectField (details, "richText", state, "richText");
        CopyObjectField (details, "anchorCoordinate", state, "coordinate");
        CopyIntField (row, "floorIndex", state, "storyIndex");
        CopyIntField (details, "anchor", state, "anchor");
        CopyDoubleField (details, "angle", state, "angle");
        CopyDoubleField (details, "width", state, "width");
        CopyDoubleField (details, "height", state, "height");
        CopyIntField (details, "pen", state, "pen");
        CopyBoolField (details, "fixedSize", state, "fixedSize");
        CopyBoolField (details, "nonBreaking", state, "nonBreaking");
    } else if (typeName == "Line") {
        CopyObjectField (details, "begCoordinate", state, "begin");
        CopyObjectField (details, "endCoordinate", state, "end");
        CopyStringField (details, "database", state, "database");
        CopyIntField (details, "lineTypeIndex", state, "lineType");
        CopyIntField (details, "linePen", state, "pen");
        CopyBoolField (details, "roomSeparator", state, "roomSeparator");
        CopyObjectField (details, "arrowData", state, "arrowSettings");
    } else if (typeName == "Arc") {
        CopyObjectField (details, "origin", state, "origin");
        CopyDoubleField (details, "radius", state, "radius");
        CopyDoubleField (details, "ratio", state, "ratio");
        CopyDoubleField (details, "axisAngle", state, "axisAngle");
        CopyDoubleField (details, "beginAngle", state, "beginAngle");
        CopyDoubleField (details, "endAngle", state, "endAngle");
        CopyBoolField (details, "whole", state, "whole");
        CopyBoolField (details, "reflected", state, "reflected");
        CopyIntField (details, "lineTypeIndex", state, "lineType");
        CopyIntField (details, "linePen", state, "pen");
        CopyBoolField (details, "roomSeparator", state, "roomSeparator");
        CopyObjectField (details, "arrowData", state, "arrowSettings");
    } else if (typeName == "Hatch") {
        CopyPolygonField (details, state);
        CopyIntField (details, "fillAttributeIndex", state, "fill");
        CopyIntField (details, "contourPen", state, "contourPen");
        CopyIntField (details, "fillBackgroundPen", state, "backgroundPen");
        CopyIntField (details, "fillPen", state, "foregroundPen");
        CopyIntArrayField (details, "memoVertexIds", state, "memoVertexIds");
        CopyStringField (details, "memoVertexIdsStatus", state, "memoVertexIdsStatus");
        GS::ObjectState hatch;
        CopyStringField (details, "hatchType", hatch, "type");
        CopyStringField (details, "determination", hatch, "determination");
        CopyIntField (details, "contourPenBase", hatch, "contourPenBase");
        CopyIntField (details, "contourPenOverride", hatch, "contourPenOverride");
        CopyIntField (details, "fillPenBase", hatch, "fillPenBase");
        CopyIntField (details, "fillPenOverride", hatch, "fillPenOverride");
        CopyIntField (details, "buildingMaterialIndex", hatch, "buildingMaterialIndex");
        CopyIntField (details, "hatchFlags", hatch, "hatchFlags");
        CopyBoolField (details, "contourPenOverridden", hatch, "contourPenOverridden");
        CopyBoolField (details, "fillPenOverridden", hatch, "fillPenOverridden");
        CopyBoolField (details, "hasForegroundRGB", hatch, "hasForegroundRGB");
        CopyBoolField (details, "hasBackgroundRGB", hatch, "hasBackgroundRGB");
        CopyIntField (details, "hatchOrientationType", hatch, "orientationType");
        CopyIntField (details, "hatchOrientationFlags", hatch, "orientationFlags");
        if (!hatch.IsEmpty ()) state.Add ("hatch", hatch);
    } else if (typeName == "Door" || typeName == "Window") {
        CopyObjectField (details, "ownerElementId", state, "ownerWallId");
        CopyDoubleField (details, "centerOffset", state, "offset");
        CopyDoubleField (details, "sillHeight", state, "sill");
        CopyDoubleField (details, "width", state, "width");
        CopyDoubleField (details, "height", state, "height");
        CopyObjectField (details, "libPart", state, "libraryPart");
        CopyObjectField (details, "stamp", state, "stamp");
        CopyObjectField (details, "openingTopology", state, "openingTopology");
        GS::ObjectState orientation;
        CopyBoolField (details, "reflected", orientation, "reflected");
        CopyBoolField (details, "refSide", orientation, "refSide");
        CopyBoolField (details, "oSide", orientation, "oSide");
        if (!orientation.IsEmpty ()) state.Add ("orientation", orientation);
    }
    return state;
}

// Column placement/section observations are deliberately kept outside
// writableState.  In particular, origin is the native UPDATE placement
// projection and is never presented as CREATE coordinates or a recreateState.
GS::ObjectState FactualColumnReadbackState (const GS::ObjectState& details, const GS::ObjectState& row)
{
    GS::ObjectState readback;
    CopyObjectField (details, "origin", readback, "origin");
    CopyIntField (row, "floorIndex", readback, "storyIndex");
    CopyDoubleField (details, "zCoordinate", readback, "absoluteElevation");
    CopyDoubleField (details, "bottomOffset", readback, "bottomOffset");
    CopyDoubleField (details, "height", readback, "height");
    CopyDoubleField (details, "axisRotationAngle", readback, "rotation");
    CopyIntField (details, "relativeTopStory", readback, "relativeTopStory");
    CopyBoolField (details, "isSlanted", readback, "isSlanted");
    CopyObjectField (details, "columnSectionReadback", readback, "section");

    double absoluteElevation = 0.0;
    double storyLevel = 0.0;
    double bottomOffset = 0.0;
    GS::ObjectState elevationRelation;
    if (details.Get ("zCoordinate", absoluteElevation)
        && details.Get ("storyLevel", storyLevel)
        && details.Get ("bottomOffset", bottomOffset)
        && std::isfinite (absoluteElevation)
        && std::isfinite (storyLevel)
        && std::isfinite (bottomOffset)) {
        const bool relationVerified = std::abs ((storyLevel + bottomOffset) - absoluteElevation) <= 1.0e-9;
        elevationRelation.Add ("status", relationVerified ? "verified" : "unavailable");
        elevationRelation.Add ("rule", "absoluteElevation=storyLevel+bottomOffset");
        elevationRelation.Add ("storyLevel", storyLevel);
        elevationRelation.Add ("bottomOffset", bottomOffset);
        elevationRelation.Add ("absoluteElevation", absoluteElevation);
    } else {
        elevationRelation.Add ("status", "unavailable");
        elevationRelation.Add ("rule", "absoluteElevation=storyLevel+bottomOffset");
    }
    readback.Add ("elevationRelation", elevationRelation);
    return readback;
}

GS::ObjectState SnapshotFromDetailRow (const GS::ObjectState& row, const GS::UniString& scope)
{
    const GS::ObjectState* elementId = row.Get ("elementId");
    const GS::ObjectState* details = row.Get ("details");
    GS::UniString typeName;
    row.Get ("type", typeName);
    if (elementId == nullptr || details == nullptr || !IsSupportedNativeTypeName (typeName))
        return GS::ObjectState ("error", "Native detail row is unavailable or has an unsupported type.");
    const GS::UniString canonicalTypeName = CanonicalElementTypeName (typeName);

    GS::ObjectState snapshot;
    snapshot.Add ("schemaVersion", SnapshotVersion);
    snapshot.Add ("elementId", *elementId);
    snapshot.Add ("elementType", canonicalTypeName);
    snapshot.Add ("fileScopeKey", scope);
    Int32 floorIndex = 0;
    if (row.Get ("floorIndex", floorIndex))
        snapshot.Add ("storyIndex", floorIndex);
    // D-323: the element's layer attribute index rides on the snapshot top
    // level, outside writableState, so the fingerprint stays unchanged. The
    // key is omitted when the detail row does not carry it (for example a
    // drafting-profile row); a client must not guess a layer.
    Int32 layerIndex = 0;
    if (row.Get ("layerIndex", layerIndex) && layerIndex > 0)
        snapshot.Add ("layerIndex", layerIndex);
    const GS::ObjectState* bounds = row.Get ("bounds");
    if (bounds != nullptr) {
        double xMin = 0.0;
        double yMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
        if (bounds->Get ("xMin", xMin) && bounds->Get ("yMin", yMin) &&
            bounds->Get ("xMax", xMax) && bounds->Get ("yMax", yMax)) {
            snapshot.Add ("bounds", GS::ObjectState ("minX", xMin, "minY", yMin, "maxX", xMax, "maxY", yMax));
        }
    }
    // Hosted elements (notably Door/Window) expose their parent in the
    // type-specific detail object.  Preserve that relation in the versioned
    // snapshot so a native impact collector can build a bounded parent/child
    // closure without leaking a raw DevKit memo.
    const GS::ObjectState* owner = details->Get ("ownerElementId");
    if (owner != nullptr && GetGuidFromObjectState (*owner) != APINULLGuid)
        snapshot.Add ("ownerElementId", *owner);
    const GS::ObjectState writableState = CanonicalWritableState (canonicalTypeName, *details, row);
    snapshot.Add ("writableState", writableState);
    if (canonicalTypeName == "Column") {
        snapshot.Add ("factualState", FactualColumnReadbackState (*details, row));
        // A Column UPDATE origin/elevation projection is not the CREATE
        // coordinates vocabulary.  Omit recreateState until a complete,
        // factual candidate writer exists.
    } else {
        snapshot.Add ("recreateState", writableState);
    }
    const bool columnSnapshot = canonicalTypeName == "Column";
    snapshot.Add ("fingerprintAlgorithm", columnSnapshot ? "native-json-fnv1a-64-column-factual-v2" : "native-json-fnv1a-64-v1");
    // The TypeScript adapter treats this as an opaque precondition.  The
    // deterministic FNV value is intentionally derived from the serialized
    // canonical detail object; a future APX can upcast it to SHA-256 without
    // changing the surrounding snapshot shape.
    GS::ObjectState fingerprintState = writableState;
    if (columnSnapshot) {
        GS::ObjectState columnFingerprint;
        columnFingerprint.Add ("writableState", writableState);
        const GS::ObjectState* factualState = snapshot.Get ("factualState");
        if (factualState != nullptr) columnFingerprint.Add ("factualState", *factualState);
        fingerprintState = columnFingerprint;
    }
    GS::UniString serialized;
    JSON::CreateFromObjectState (fingerprintState, serialized);
    const std::string bytes (serialized.ToCStr (CC_UTF8).Get ());
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream fingerprint;
    fingerprint << std::hex << std::setfill ('0') << std::setw (64) << hash;
    // Keep a fixed 64-character representation so clients can carry the
    // value through the same strict hash field as SHA-256 snapshots.
    snapshot.Add ("fingerprint", GS::UniString (fingerprint.str ().c_str (), CC_UTF8));
    return snapshot;
}

// GetDetailsOfElements keeps the positional slot for an element that cannot
// be loaded by returning a schema-valid `type:Object` row whose details carry
// an error string.  That is only a candidate for absence evidence.  A
// malformed or unrequested row must remain a coverage failure, never become a
// synthetic missing GUID.
bool IsUnavailableObjectDetailRow (
    const GS::ObjectState& row,
    const GS::Array<API_Guid>& requested,
    API_Guid& guid)
{
    GS::UniString typeName;
    const GS::ObjectState* elementId = row.Get ("elementId");
    const GS::ObjectState* details = row.Get ("details");
    GS::UniString error;
    if (!row.Get ("type", typeName) || typeName != "Object"
        || elementId == nullptr || details == nullptr || !details->Get ("error", error)
        || error.IsEmpty ())
        return false;

    guid = GetGuidFromObjectState (*elementId);
    return guid != APINULLGuid && requested.Contains (guid);
}

GS::ObjectState BuildSnapshotsFromDetails (
    const GS::ObjectState& detailResponse,
    const GS::UniString& scope,
    const GS::Array<GS::ObjectState>& requestedIds,
    bool& complete)
{
    GS::Array<GS::ObjectState> rows;
    if (!detailResponse.Get ("detailsOfElements", rows)) {
        complete = false;
        return CreateErrorResponse (APIERR_BADID, "Native detail response did not contain detailsOfElements.");
    }

    GS::ObjectState response;
    const auto& snapshots = response.AddList<GS::ObjectState> ("elements");
    GS::Array<API_Guid> seen;
    GS::Array<API_Guid> requested;
    GS::Array<API_Guid> unavailable;
    bool invalidRow = false;
    complete = rows.GetSize () == requestedIds.GetSize ();
    for (const GS::ObjectState& requestedId : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        if (guid == APINULLGuid || requested.Contains (guid)) {
            complete = false;
            continue;
        }
        requested.Push (guid);
    }
    for (const GS::ObjectState& row : rows) {
        const GS::ObjectState snapshot = SnapshotFromDetailRow (row, scope);
        if (snapshot.Contains ("error")) {
            API_Guid unavailableGuid = APINULLGuid;
            if (IsUnavailableObjectDetailRow (row, requested, unavailableGuid)
                && !seen.Contains (unavailableGuid)
                && !unavailable.Contains (unavailableGuid)) {
                unavailable.Push (unavailableGuid);
            } else {
                complete = false;
                invalidRow = true;
            }
            continue;
        }
        const GS::ObjectState* elementId = snapshot.Get ("elementId");
        const API_Guid guid = elementId == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementId);
        if (guid == APINULLGuid || !requested.Contains (guid)
            || seen.Contains (guid) || unavailable.Contains (guid)) {
            complete = false;
            invalidRow = true;
            continue;
        }
        seen.Push (guid);
        snapshots (snapshot);
    }
    if (seen.GetSize () != requested.GetSize ())
        complete = false;
    for (const API_Guid& requestedGuid : requested) {
        if (!seen.Contains (requestedGuid)) {
            complete = false;
        }
    }
    // Keep absence explicit for read-only history effect probes.  Ordinary
    // exact reads still require `complete=true`; the TypeScript gateway only
    // accepts this partition when an internal `allowMissingGuids` probe asks
    // for it.  This is what makes a deleted GUID distinguishable from a
    // transport/coverage failure without weakening mutation preconditions.
    const auto& missing = response.AddList<GS::UniString> ("missingGuids");
    for (const API_Guid& requestedGuid : requested) {
        if (seen.Contains (requestedGuid))
            continue;

        // DetailsOfElements can return an unavailable Object row immediately
        // after DELETE, but a missing row is not proof by itself.  Independently
        // query the element header for every requested GUID that lacks a valid
        // snapshot.  Only BADID proves absence; NoError and every other error
        // keep the read incomplete and must never be emitted as missing.
        API_Elem_Head header = {};
        header.guid = requestedGuid;
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        if (!invalidRow && headerError == APIERR_BADID)
            missing (APIGuidToString (requestedGuid));
    }
    response.Add ("contractVersion", ContractVersion);
    response.Add ("complete", complete);
    return response;
}

} // namespace

namespace NativeCrudInternal {

GS::ObjectState ExactSnapshots (
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::UniString& scope,
    GS::ProcessControl& processControl,
    bool& complete)
{
    // Archicad can finish an undoable create/update command just before its
    // element index becomes visible to the next DetailsOfElements request.
    // Make the factual read-back resilient to that short host propagation
    // window without ever dispatching the mutation again.  Invalid/stale
    // GUIDs still return the final incomplete response after this bounded
    // read-only retry budget.
    GS::ObjectState lastResponse;
    complete = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        GS::ObjectState parameters;
        const auto& ids = parameters.AddList<GS::ObjectState> ("elements");
        for (const GS::ObjectState& id : requestedIds) {
            // CREATE read-back collects the nested `{ guid }` object from a
            // returned element row, whereas UPDATE/DELETE callers already
            // hold `{ elementId: { guid } }`.  DetailsOfElements consumes the
            // latter shape; normalize both here before dispatching the
            // read-only detail request.
            if (id.Get ("elementId") != nullptr)
                ids (id);
            else
                ids (GS::ObjectState ("elementId", id));
        }
        parameters.Add ("detailProfile", "full");
        const GS::ObjectState details = GetDetailsOfElementsCommand ().Execute (parameters, processControl);
        lastResponse = BuildSnapshotsFromDetails (details, scope, requestedIds, complete);
        if (complete || attempt == 2)
            return lastResponse;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    return lastResponse;
}

} // namespace NativeCrudInternal

namespace {

GS::Array<GS::ObjectState> RequestedIdsFromGuids (const GS::Array<GS::UniString>& guids)
{
    GS::Array<GS::ObjectState> ids;
    for (const GS::UniString& value : guids)
        // GetDetailsOfElementsCommand consumes the public element-id shape
        // (`elementId: { guid }`).  The native snapshot endpoint accepts a
        // flat `guids` array, so wrap each value before forwarding it to the
        // shared detail reader.  Passing the flat object through used to
        // produce only unavailable Object rows, making every CREATE/UPDATE
        // read-back look like incomplete evidence even though Archicad had
        // already committed the element.
        ids.Push (GS::ObjectState ("elementId", GS::ObjectState ("guid", value)));
    return ids;
}

GS::Optional<GS::UniString> ReadTypeName (const GS::ObjectState& parameters, API_ElemTypeID& typeID)
{
    GS::UniString typeName;
    if (!parameters.Get ("elementType", typeName))
        return GS::UniString ("elementType is required.");
    typeID = GetElementTypeFromNonLocalizedName (typeName);
    if (!IsSupportedNativeType (typeID))
        return GS::UniString ("elementType is not one of Zone, Wall, Column, Text, Line, Arc, Hatch, Door or Window.");
    return {};
}

GS::Optional<GS::UniString> ReadRequestedGuids (
    const GS::ObjectState& parameters,
    GS::Array<GS::UniString>& guids)
{
    if (!parameters.Get ("guids", guids) || guids.IsEmpty ())
        return GS::UniString ("guids must contain at least one exact GUID.");
    if (guids.GetSize () > MaxMutationItems)
        // Exact snapshot READS keep the generated read cap (MaxMutationItems);
        // D-357 write operations use MaxNativeWriteItems instead.
        return GS::UniString::Printf ("guids may contain at most %d exact GUIDs.", static_cast<int> (MaxMutationItems));
    for (const GS::UniString& value : guids) {
        const API_Guid guid = APIGuidFromString (value.ToCStr ());
        if (guid == APINULLGuid)
            return GS::UniString ("guids contains a malformed GUID.");
    }
    return {};
}

} // namespace

GetNativeElementSnapshotsCommand::GetNativeElementSnapshotsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeElementSnapshotsCommand::GetName () const { return "GetNativeElementSnapshots"; }
GS::Optional<GS::UniString> GetNativeElementSnapshotsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"fileScopeKey":{"type":"string"},"elementType":{"type":"string"},"guids":{"type":"array","minItems":1,"maxItems":25,"items":{"type":"string"}},"allowMissingGuids":{"type":"boolean"}},"additionalProperties":false,"required":["contractVersion","fileScopeKey","guids"]})";
}
GS::Optional<GS::UniString> GetNativeElementSnapshotsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeElementSnapshotsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    const GS::Optional<GS::UniString> scopeError = ValidateScope (parameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    GS::UniString requestedType;
    if (parameters.Get ("elementType", requestedType) && !requestedType.IsEmpty () && !IsSupportedNativeTypeName (requestedType)
        && requestedType != "Dimension" && requestedType != "RadialDimension"
        && requestedType != "AngleDimension" && requestedType != "LevelDimension")
        return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    GS::Array<GS::UniString> guids;
    const GS::Optional<GS::UniString> guidError = ReadRequestedGuids (parameters, guids);
    if (guidError.HasValue ()) return ScopeError (guidError);
    // The dimension detail reader is internal to this already scope-validated
    // exact snapshot route.  Classify mixed requests from native headers,
    // capture each subfamily through its owning reader, then reassemble by
    // exact GUID in caller order.  This keeps the canonical response envelope
    // and prevents the unscoped detail command from becoming a public route.
    GS::Array<GS::UniString> dimensionGuids;
    GS::Array<GS::UniString> ordinaryGuids;
    for (const GS::UniString& guidText : guids) {
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        API_Elem_Head header = {};
        header.guid = guid;
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        const bool requestedDimensionType = requestedType == "Dimension" || requestedType == "RadialDimension"
            || requestedType == "AngleDimension" || requestedType == "LevelDimension";
        if (requestedDimensionType || (headerError == NoError && (GetElemTypeId (header) == API_DimensionID
            || GetElemTypeId (header) == API_RadialDimensionID || GetElemTypeId (header) == API_AngleDimensionID
            || GetElemTypeId (header) == API_LevelDimensionID)))
            dimensionGuids.Push (guidText);
        else
            ordinaryGuids.Push (guidText);
    }
    bool complete = false;
    GS::ObjectState response;
    if (dimensionGuids.IsEmpty ()) {
        response = ExactSnapshots (RequestedIdsFromGuids (guids), CurrentSoloFileScope ().Get (), processControl, complete);
    } else {
        GS::Array<GS::ObjectState> dimensionRows;
        bool dimensionsComplete = true;
        if (!dimensionGuids.IsEmpty ()) {
            GS::ObjectState exactParameters;
            exactParameters.Add ("guids", dimensionGuids);
            const GS::ObjectState dimensionResponse = GetNativeDimensionDetailsCommand ().Execute (exactParameters, processControl);
            if (dimensionResponse.Get ("error") != nullptr) return dimensionResponse;
            if (!dimensionResponse.Get ("elements", dimensionRows) || dimensionRows.GetSize () != dimensionGuids.GetSize ())
                return CreateErrorResponse (APIERR_BADID, "Canonical dimension exact read did not preserve every requested GUID.");
            for (const GS::ObjectState& row : dimensionRows) {
                GS::UniString rowStatus;
                row.Get ("status", rowStatus);
                if (rowStatus != "success") dimensionsComplete = false;
            }
        }
        GS::Array<GS::ObjectState> ordinaryRows;
        bool ordinaryComplete = true;
        GS::Array<GS::UniString> missingGuids;
        if (!ordinaryGuids.IsEmpty ()) {
            GS::ObjectState ordinaryResponse = ExactSnapshots (RequestedIdsFromGuids (ordinaryGuids), CurrentSoloFileScope ().Get (), processControl, ordinaryComplete);
            ordinaryResponse.Get ("elements", ordinaryRows);
            ordinaryResponse.Get ("missingGuids", missingGuids);
        }
        std::map<std::string, GS::ObjectState> rowsByGuid;
        for (const GS::ObjectState& row : dimensionRows) {
            const GS::ObjectState* id = row.Get ("elementId");
            if (id != nullptr) {
                GS::ObjectState tagged = row;
                tagged.Add ("canonicalSnapshotKind", "dimension");
                rowsByGuid[std::string (APIGuidToString (GetGuidFromObjectState (*id)).ToCStr (CC_UTF8).Get ())] = tagged;
            }
        }
        for (const GS::ObjectState& row : ordinaryRows) {
            const GS::ObjectState* id = row.Get ("elementId");
            if (id != nullptr) rowsByGuid[std::string (APIGuidToString (GetGuidFromObjectState (*id)).ToCStr (CC_UTF8).Get ())] = row;
        }
        response.Add ("status", "success");
        response.Add ("contractVersion", ContractVersion);
        response.Add ("fileScopeKey", CurrentSoloFileScope ().Get ());
        const auto& outputRows = response.AddList<GS::ObjectState> ("elements");
        for (const GS::UniString& guidText : guids) {
            const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
            const std::string key (APIGuidToString (guid).ToCStr (CC_UTF8).Get ());
            const auto found = rowsByGuid.find (key);
            if (found != rowsByGuid.end ()) {
                GS::UniString actualType;
                found->second.Get ("elementType", actualType);
                if (!requestedType.IsEmpty () && actualType != requestedType)
                    return CreateErrorResponse (APIERR_BADID, "An exact snapshot GUID does not match elementType.");
                outputRows (found->second);
            }
        }
        complete = dimensionsComplete && ordinaryComplete && rowsByGuid.size () == guids.GetSize ();
        response.Add ("complete", complete);
        const auto& missing = response.AddList<GS::UniString> ("missingGuids");
        for (const GS::UniString& guid : missingGuids) missing (guid);
    }
    if (!complete) response.Add ("code", "SNAPSHOT_COVERAGE_INCOMPLETE");
    if (!requestedType.IsEmpty ()) {
        GS::Array<GS::ObjectState> snapshots;
        response.Get ("elements", snapshots);
        for (const GS::ObjectState& snapshot : snapshots) {
            GS::UniString actualType;
            if (snapshot.Get ("elementType", actualType) && actualType != requestedType)
                return CreateErrorResponse (APIERR_BADID, "An exact snapshot GUID does not match elementType.");
        }
    }
    return response;
}

static GSErrCode GetNativeConnectedElements (
    const API_Guid& ownerGuid,
    API_ElemTypeID connectedTypeId,
    GS::Array<API_Guid>& connectedGuids)
{
#if TAPIR_AC_VERSION == 25
    // MigrationHelper maps the Grouping API name to ACAPI_Element on AC25;
    // its variation argument has a default, so the typed ID and output list suffice.
    return ACAPI_Grouping_GetConnectedElements (ownerGuid, connectedTypeId, &connectedGuids);
#else
    API_ElemType connectedType = {};
    connectedType.typeID = connectedTypeId;
    connectedType.variationID = APIVarId_Generic;
    return ACAPI_Grouping_GetConnectedElements (ownerGuid, connectedType, &connectedGuids);
#endif
}

GetNativeHostedOpeningsForWallsCommand::GetNativeHostedOpeningsForWallsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeHostedOpeningsForWallsCommand::GetName () const { return "GetNativeHostedOpeningsForWalls"; }
GS::Optional<GS::UniString> GetNativeHostedOpeningsForWallsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"},"fileScopeKey":{"type":"string"},"guids":{"type":"array","minItems":1,"maxItems":25,"items":{"type":"string"}}},"additionalProperties":false,"required":["contractVersion","fileScopeKey","guids"]})";
}
GS::Optional<GS::UniString> GetNativeHostedOpeningsForWallsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeHostedOpeningsForWallsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::Optional<GS::UniString> scopeError = ValidateScope (parameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    GS::Array<GS::UniString> guidTexts;
    const GS::Optional<GS::UniString> guidError = ReadRequestedGuids (parameters, guidTexts);
    if (guidError.HasValue ()) return ScopeError (guidError);

    GS::Array<API_Guid> requestedWalls;
    for (const GS::UniString& text : guidTexts) {
        const API_Guid guid = APIGuidFromString (text.ToCStr ());
        if (requestedWalls.Contains (guid))
            return CreateErrorResponse (APIERR_BADPARS, "Wall GUIDs must be unique.");
        requestedWalls.Push (guid);
    }

    GS::UniString fileScopeKey;
    if (!parameters.Get ("fileScopeKey", fileScopeKey) || fileScopeKey.IsEmpty ())
        return CreateErrorResponse (APIERR_BADPARS, "fileScopeKey is required.");

    GS::ObjectState response;
    response.Add ("status", "success");
    response.Add ("contractVersion", ContractVersion);
    response.Add ("fileScopeKey", fileScopeKey);
    bool complete = true;
    GS::HashSet<API_Guid> seenOpenings;
    const auto& wallOpenings = response.AddList<GS::ObjectState> ("wallOpenings");
    for (const API_Guid& wallGuid : requestedWalls) {
        GS::ObjectState row;
        row.Add ("wallElementId", CreateGuidObjectState (wallGuid));
        API_Element wall = {};
        wall.header.guid = wallGuid;
        GSErrCode error = ACAPI_Element_Get (&wall);
        if (error != NoError || GetElemTypeId (wall.header) != API_WallID) {
            row.Add ("status", "error");
            row.Add ("error", GS::UniString::Printf ("The requested owner GUID is not a readable Wall (error %d).", error != NoError ? error : APIERR_BADID));
            complete = false;
            wallOpenings (row);
            continue;
        }

        GS::Array<API_Guid> doors;
        GS::Array<API_Guid> windows;
        error = GetNativeConnectedElements (wallGuid, API_DoorID, doors);
        if (error == NoError) error = GetNativeConnectedElements (wallGuid, API_WindowID, windows);
        if (error != NoError) {
            row.Add ("status", "error");
            row.Add ("error", GS::UniString::Printf ("Hosted Door/Window ownership could not be read completely (error %d).", error));
            complete = false;
            wallOpenings (row);
            continue;
        }

        bool openingGuidsValid = true;
        for (const API_Guid& guid : doors) {
            if (guid == APINULLGuid || seenOpenings.Contains (guid)) openingGuidsValid = false;
            else seenOpenings.Add (guid);
        }
        for (const API_Guid& guid : windows) {
            if (guid == APINULLGuid || seenOpenings.Contains (guid)) openingGuidsValid = false;
            else seenOpenings.Add (guid);
        }
        if (!openingGuidsValid) {
            row.Add ("status", "error");
            row.Add ("error", "Hosted Door/Window ownership returned an invalid or duplicate GUID.");
            complete = false;
            wallOpenings (row);
            continue;
        }

        const auto addGuids = [&row] (const char* field, const GS::Array<API_Guid>& guids) {
            const auto& values = row.AddList<GS::ObjectState> (field);
            for (const API_Guid& guid : guids) values (CreateGuidObjectState (guid));
        };
        addGuids ("doorGuids", doors);
        addGuids ("windowGuids", windows);
        row.Add ("status", "success");
        wallOpenings (row);
    }
    response.Add ("complete", complete);
    return response;
}
