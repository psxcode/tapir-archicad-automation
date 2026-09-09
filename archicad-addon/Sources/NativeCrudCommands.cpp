#include "NativeCrudCommands.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementMutationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

namespace {

constexpr const char* ContractVersion = "native-crud-v1";
constexpr const char* SnapshotVersion = "native-element-snapshot-v1";
constexpr const char* ReceiptVersion = "native-mutation-receipt-v1";
constexpr const char* HistoryVersion = "native-mutation-history-v1";
constexpr GSSize MaxMutationItems = 25;
constexpr GSSize MaxDiscoveryReferences = 100;
constexpr GSSize MaxImpactElements = 256;

std::atomic<Int64>& SequenceCounter ()
{
    static std::atomic<Int64> sequence (0);
    return sequence;
}

std::mutex& ReceiptCacheMutex ()
{
    static std::mutex mutex;
    return mutex;
}

// Factual receipts are owned durably by the MCP SQLite store.  The APX only
// keeps a small process-local cache so a response timeout can be reconciled
// while this Archicad process is still alive.  Keeping the cache bounded is
// important: it must never become a second unbounded history database or
// couple durable history to an Archicad instance lifecycle.
struct CachedReceipt {
    std::string operationId;
    std::string fileScopeKey;
    GS::ObjectState receipt;
};

std::vector<CachedReceipt>& ReceiptCache ()
{
    static std::vector<CachedReceipt> cache;
    return cache;
}

GS::Optional<GS::ObjectState> FindCachedReceipt (const GS::UniString& operationId, const GS::UniString& expectedScope)
{
    const std::string key (operationId.ToCStr (CC_UTF8).Get ());
    const std::string scope (expectedScope.ToCStr (CC_UTF8).Get ());
    std::lock_guard<std::mutex> lock (ReceiptCacheMutex ());
    auto& cache = ReceiptCache ();
    for (auto it = cache.rbegin (); it != cache.rend (); ++it) {
        if (it->operationId == key && it->fileScopeKey == scope) {
            // Touch the entry on lookup so the bounded process-local cache is
            // an actual LRU rather than FIFO.  A frequently reconciled
            // operation must not evict itself while newer receipts arrive.
            CachedReceipt hit = *it;
            cache.erase (std::next (it).base ());
            cache.push_back (hit);
            return hit.receipt;
        }
    }
    return GS::NoValue;
}

void RememberCachedReceipt (const GS::ObjectState& receipt)
{
    GS::UniString operationId;
    GS::UniString fileScopeKey;
    if (!receipt.Get ("operationId", operationId) || operationId.IsEmpty ()
        || !receipt.Get ("fileScopeKey", fileScopeKey) || fileScopeKey.IsEmpty ())
        return;
    const std::string key (operationId.ToCStr (CC_UTF8).Get ());
    const std::string scope (fileScopeKey.ToCStr (CC_UTF8).Get ());
    std::lock_guard<std::mutex> lock (ReceiptCacheMutex ());
    auto& cache = ReceiptCache ();
    for (CachedReceipt& entry : cache) {
        if (entry.operationId == key && entry.fileScopeKey == scope) {
            entry.receipt = receipt;
            return;
        }
    }
    constexpr GSSize MaxCachedReceipts = 256;
    if (cache.size () >= MaxCachedReceipts)
        cache.erase (cache.begin ());
    cache.push_back ({ key, scope, receipt });
}

Int64 NextCachedReceiptSequence ()
{
    // This sequence is APX-process diagnostic metadata only.  The durable
    // per-file sequence is allocated by MCP SQLite when the prepared row is
    // admitted; the TypeScript store normalizes terminal rows to that
    // sequence.  It is intentionally not persisted by the APX.
    return SequenceCounter ().fetch_add (1) + 1;
}

bool IsSupportedNativeType (const API_ElemTypeID typeID)
{
    switch (typeID) {
        case API_ZoneID:
        case API_WallID:
        case API_ColumnID:
        case API_TextID:
        case API_LineID:
        case API_ArcID:
        // Archicad exposes a full circle as a distinct Circle element in
        // some detail/discovery responses.  The versioned wire contract
        // intentionally normalizes that primitive to Arc, but it must still
        // pass the native type gate before CanonicalElementTypeName can do
        // the normalization.
        case API_CircleID:
        case API_HatchID:
        case API_DoorID:
        case API_WindowID:
            return true;
        default:
            return false;
    }
}

bool IsSupportedNativeTypeName (const GS::UniString& typeName)
{
    return IsSupportedNativeType (GetElementTypeFromNonLocalizedName (typeName));
}

GS::UniString CanonicalElementTypeName (const GS::UniString& typeName)
{
    // Archicad reports a full circle as Circle while the versioned native
    // contract deliberately exposes Arc as the single circular primitive.
    return GetElementTypeFromNonLocalizedName (typeName) == API_CircleID
        ? GS::UniString ("Arc")
        : typeName;
}

GS::Optional<GS::UniString> CurrentSoloFileScope ()
{
    API_ProjectInfo projectInfo = {};
    if (ACAPI_ProjectOperation_Project (&projectInfo) != NoError)
        return {};
    if (projectInfo.untitled || projectInfo.teamwork || projectInfo.projectPath == nullptr)
        return {};

    GS::UniString path (*projectInfo.projectPath);
    path.ReplaceAll ("\\", "/");
    path = path.ToLowerCase ();
    const std::string pathUtf8 (path.ToCStr (CC_UTF8).Get ());
    if (pathUtf8.size () < 4 || pathUtf8.compare (pathUtf8.size () - 4, 4, ".pln") != 0)
        return {};
    GS::UniString scope ("saved:");
    scope.Append (path);
    return scope;
}

GS::Optional<GS::UniString> ValidateScope (const GS::ObjectState& parameters)
{
    GS::UniString expected;
    if (!parameters.Get ("fileScopeKey", expected) || expected.IsEmpty ())
        return GS::UniString ("fileScopeKey is required.");
    const GS::Optional<GS::UniString> current = CurrentSoloFileScope ();
    if (current.IsEmpty ())
        return GS::UniString ("Native CRUD is limited to a saved Solo PLN project.");
    if (expected != current.Get ())
        return GS::UniString::Printf ("fileScopeKey does not match the opened Solo PLN (expected %T).", current.Get ().ToPrintf ());
    return {};
}

GS::ObjectState ScopeError (const GS::Optional<GS::UniString>& error)
{
    return CreateErrorResponse (APIERR_BADPARS, error.Get ());
}

// Some native contract failures need a stable, machine-readable string code
// rather than an Archicad GSErrCode.  Keep the normal error envelope so the
// TypeScript gateway can classify the failure without guessing from a human
// message (notably for an operation-id replay with a different request).
GS::ObjectState NativeContractError (const char* code, const GS::UniString& message)
{
    GS::ObjectState error;
    error.Add ("code", code);
    error.Add ("message", message);
    return GS::ObjectState ("error", error);
}

GS::Optional<GS::UniString> ValidateContractVersion (const GS::ObjectState& parameters)
{
    GS::UniString requested;
    if (!parameters.Get ("contractVersion", requested) || requested.IsEmpty ())
        return GS::UniString::Printf ("contractVersion is required (expected %s).", ContractVersion);
    if (requested != ContractVersion)
        return GS::UniString::Printf ("Unsupported native CRUD contractVersion (expected %s).", ContractVersion);
    return {};
}

GS::ObjectState CoordinateBounds (const API_Box3D& bounds)
{
    return GS::ObjectState (
        "minX", bounds.xMin,
        "minY", bounds.yMin,
        "maxX", bounds.xMax,
        "maxY", bounds.yMax);
}

// Internal snapshot/readback collections carry the public element-id shape
// (`elementId: { guid }`), while a few lower-level helpers historically
// received a bare `{ guid }` object. Accept both forms at this boundary so
// exact GUID comparisons cannot silently turn into APINULLGuid.
API_Guid GetRequestedElementGuid (const GS::ObjectState& value)
{
    const GS::ObjectState* nested = value.Get ("elementId");
    return nested != nullptr ? GetGuidFromObjectState (*nested) : GetGuidFromObjectState (value);
}

// DetailsOfElements is a stable native read DTO, but its historical field
// names are not the public native-crud-v1 writable schema.  Keep the wire
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
        GS::ObjectState stamp;
        CopyObjectField (details, "stampPosition", stamp, "position");
        CopyDoubleField (details, "stampAngle", stamp, "angle");
        CopyBoolField (details, "fixedStampAngle", stamp, "fixedAngle");
        if (!stamp.IsEmpty ()) state.Add ("stampPlacement", stamp);
        CopyPolygonField (details, state);
    } else if (typeName == "Wall") {
        CopyObjectField (details, "begCoordinate", state, "begCoordinate");
        CopyObjectField (details, "endCoordinate", state, "endCoordinate");
        CopyDoubleField (details, "zCoordinate", state, "elevation");
        CopyDoubleField (details, "height", state, "height");
        CopyDoubleField (details, "begThickness", state, "thickness");
        CopyDoubleField (details, "offset", state, "offset");
        CopyStringField (details, "structureType", state, "structure");
        CopyObjectField (details, "compositeId", state, "composite");
        CopyStringField (details, "referenceLineLocation", state, "referenceLine");
    } else if (typeName == "Column") {
        CopyObjectField (details, "origin", state, "origin");
        CopyDoubleField (details, "zCoordinate", state, "elevation");
        CopyDoubleField (details, "height", state, "height");
        CopyDoubleField (details, "bottomOffset", state, "offset");
        CopyDoubleField (details, "slantAngle", state, "slant");
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
        GS::ObjectState hatch;
        CopyStringField (details, "hatchType", hatch, "type");
        CopyStringField (details, "determination", hatch, "determination");
        if (!hatch.IsEmpty ()) state.Add ("hatch", hatch);
    } else if (typeName == "Door" || typeName == "Window") {
        CopyObjectField (details, "ownerElementId", state, "ownerWallId");
        CopyDoubleField (details, "centerOffset", state, "offset");
        CopyDoubleField (details, "sillHeight", state, "sill");
        CopyDoubleField (details, "width", state, "width");
        CopyDoubleField (details, "height", state, "height");
        CopyObjectField (details, "libPart", state, "libraryPart");
        GS::ObjectState orientation;
        CopyBoolField (details, "reflected", orientation, "reflected");
        CopyBoolField (details, "refSide", orientation, "refSide");
        CopyBoolField (details, "oSide", orientation, "oSide");
        if (!orientation.IsEmpty ()) state.Add ("orientation", orientation);
    }
    return state;
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
    snapshot.Add ("recreateState", writableState);
    snapshot.Add ("fingerprintAlgorithm", "native-json-fnv1a-64-v1");
    // The TypeScript adapter treats this as an opaque precondition.  The
    // deterministic FNV value is intentionally derived from the serialized
    // canonical detail object; a future APX can upcast it to SHA-256 without
    // changing the surrounding snapshot shape.
    GS::UniString serialized;
    JSON::CreateFromObjectState (writableState, serialized);
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

GS::ObjectState IdFromGuid (const API_Guid& guid)
{
    return CreateElementIdObjectState (guid);
}

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

void AddCanonicalWritableFields (GS::ObjectState& target, const GS::UniString& typeName)
{
    const char* fields[16] = {};
    GSSize count = 0;
    if (typeName == "Zone") {
        const char* values[] = { "name", "number", "category", "storyIndex", "polygon", "referencePosition", "stampPlacement" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Wall") {
        const char* values[] = { "begCoordinate", "endCoordinate", "storyIndex", "elevation", "height", "thickness", "offset", "structure", "composite", "referenceLine" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Column") {
        const char* values[] = { "origin", "storyIndex", "elevation", "height", "offset", "slant" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Text") {
        const char* values[] = { "richText", "coordinate", "storyIndex", "anchor", "angle", "width", "height", "pen", "fixedSize", "nonBreaking" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Line") {
        const char* values[] = { "begin", "end", "storyIndex", "database", "lineType", "pen", "roomSeparator", "arrowSettings" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Arc") {
        const char* values[] = { "origin", "radius", "ratio", "axisAngle", "beginAngle", "endAngle", "whole", "reflected", "lineType", "pen", "roomSeparator", "arrowSettings" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Hatch") {
        const char* values[] = { "polygon", "arcs", "holes", "storyIndex", "database", "hatch", "fill", "contourPen", "backgroundPen", "foregroundPen", "orientation" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Door" || typeName == "Window") {
        const char* values[] = { "ownerWallId", "offset", "sill", "width", "height", "orientation", "libraryPart", "parameters" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    }
    const auto& output = target.AddList<GS::UniString> ("writableFields");
    for (GSSize i = 0; i < count; ++i) output (fields[i]);
}

GS::Optional<GS::UniString> ReadRequestedGuids (
    const GS::ObjectState& parameters,
    GS::Array<GS::UniString>& guids)
{
    if (!parameters.Get ("guids", guids) || guids.IsEmpty ())
        return GS::UniString ("guids must contain at least one exact GUID.");
    if (guids.GetSize () > MaxMutationItems)
        return GS::UniString ("guids may contain at most 25 exact GUIDs.");
    for (const GS::UniString& value : guids) {
        const API_Guid guid = APIGuidFromString (value.ToCStr ());
        if (guid == APINULLGuid)
            return GS::UniString ("guids contains a malformed GUID.");
    }
    return {};
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

bool IsHex64 (const GS::UniString& value)
{
    const std::string text (value.ToCStr (CC_UTF8).Get ());
    if (text.size () != 64) return false;
    for (const unsigned char character : text) {
        if (std::isxdigit (character) == 0) return false;
    }
    return true;
}

std::string UtcTimestamp ()
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now ();
    const std::time_t time = std::chrono::system_clock::to_time_t (now);
    std::tm utc = {};
#if defined (_WIN32)
    gmtime_s (&utc, &time);
#else
    gmtime_r (&time, &utc);
#endif
    char value[32] = {};
    std::strftime (value, sizeof (value), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return value;
}

std::string LowerAscii (const GS::UniString& value)
{
    // UniString performs Unicode case mapping; lowercasing the UTF-16 value
    // before converting to UTF-8 avoids corrupting multibyte characters.
    return std::string (value.ToLowerCase ().ToCStr (CC_UTF8).Get ());
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
                actions (GS::ObjectState ("elementId", APIGuidToString (guid), "action", action));
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


bool ReceiptPrimaryGuidsMatch (
    const GS::ObjectState& receipt,
    const GS::Array<GS::ObjectState>& requestedIds)
{
    GS::Array<GS::UniString> recordedGuids;
    if (!receipt.Get ("primaryGuids", recordedGuids)
        || recordedGuids.GetSize () != requestedIds.GetSize ())
        return false;

    GS::Array<API_Guid> expected;
    for (const GS::ObjectState& requestedId : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        if (guid == APINULLGuid || expected.Contains (guid))
            return false;
        expected.Push (guid);
    }

    GS::Array<API_Guid> recorded;
    for (const GS::UniString& recordedGuid : recordedGuids) {
        const API_Guid guid = APIGuidFromString (recordedGuid.ToCStr ());
        if (guid == APINULLGuid || recorded.Contains (guid))
            return false;
        recorded.Push (guid);
    }

    for (const API_Guid& guid : expected)
        if (!recorded.Contains (guid)) return false;
    return true;
}

bool ReceiptRequestIdentityMatches (
    const GS::ObjectState& receipt,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::UniString& requestHash,
    const GS::Array<GS::ObjectState>& requestedIds)
{
    GS::UniString recordedOperation;
    GS::UniString recordedType;
    GS::UniString recordedHash;
    if (!receipt.Get ("operation", recordedOperation)
        || !receipt.Get ("elementType", recordedType)
        || !receipt.Get ("requestHash", recordedHash))
        return false;

    if (recordedOperation != operation
        || recordedType != typeName
        || LowerAscii (recordedHash) != LowerAscii (requestHash))
        return false;

    // CREATE has no caller-supplied primary GUIDs; its typed payload and
    // clone source are covered by requestHash.  UPDATE/DELETE must also
    // match the exact GUID set so an operation-id typo cannot replay a
    // receipt for a different set of native elements.
    return operation == "create" || ReceiptPrimaryGuidsMatch (receipt, requestedIds);
}


GS::ObjectState ChangedField (const GS::ObjectState* before, const GS::ObjectState* after)
{
    GS::ObjectState field;
    field.Add ("path", "writableState");
    if (before != nullptr) field.Add ("before", *before);
    if (after != nullptr) field.Add ("after", *after);
    return field;
}

// DeleteElementsCommand owns its implementation-local readback helper.  The
// versioned native envelope needs the same per-GUID absence proof without
// depending on that translation unit, so keep a small contract-local helper
// here.  A non-BADID header error is deliberately not treated as proof of
// deletion.
GS::ObjectState BuildDeleteReadback (
    const GS::Array<GS::ObjectState>& requestedIds,
    const API_ElemTypeID typeID,
    bool& verified,
    GSSize& absentCount)
{
    GS::ObjectState readback;
    const auto& deleted = readback.AddList<GS::ObjectState> ("deleted");
    verified = true;
    absentCount = 0;
    for (const GS::ObjectState& requestedId : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        API_Elem_Head header = {};
        header.guid = guid;
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        const bool absent = headerError == APIERR_BADID;
        GS::ObjectState status;
        status.Add ("elementId", requestedId);
        status.Add ("absent", absent);
        if (headerError != NoError && headerError != APIERR_BADID) {
            status.Add ("readbackErrorCode", headerError);
            verified = false;
        }
        if (absent) {
            ++absentCount;
        } else {
            verified = false;
            if (headerError == NoError)
                status.Add ("elementType", GetElementTypeNonLocalizedName (GetElemTypeId (header)));
        }
        deleted (status);
    }
    (void) typeID;
    return readback;
}

GS::ObjectState BuildReceipt (
    const GS::UniString& scope,
    const GS::UniString& operationId,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::UniString& description,
    const GS::UniString& origin,
    const GS::UniString& requestHash,
    const GS::ObjectState& beforeResponse,
    const GS::ObjectState& afterResponse,
    const GS::Array<GS::ObjectState>& requestedIds,
    const bool complete,
    const bool dispatchComplete,
    const GS::ObjectState* semanticMetadata,
    const GS::UniString& operatorNote,
    const char* statusOverride = nullptr,
    const API_Guid* normalizeSurvivor = nullptr)
{
    GS::ObjectState receipt;
    receipt.Add ("schemaVersion", ReceiptVersion);
    receipt.Add ("operationId", operationId);
    receipt.Add ("fileScopeKey", scope);
    receipt.Add ("terminalAt", GS::UniString (UtcTimestamp ().c_str (), CC_UTF8));
    receipt.Add ("operation", operation);
    receipt.Add ("elementType", typeName);
    // A host-side validation/transaction rejection is a known terminal fact;
    // it must not be reported as an evidence gap (which would unnecessarily
    // poison reconciliation).  Only a successfully dispatched operation with
    // missing before/after evidence is `incomplete_evidence`.
    const char* status = statusOverride != nullptr
        ? statusOverride
        : !dispatchComplete ? "failed" : complete ? "applied" : "incomplete_evidence";
    receipt.Add ("status", status);
    receipt.Add ("description", description);
    if (!operatorNote.IsEmpty ()) receipt.Add ("operatorNote", operatorNote);
    receipt.Add ("origin", origin.IsEmpty () ? GS::UniString ("NATIVE") : origin);
    receipt.Add ("requestHash", requestHash);
    if (semanticMetadata != nullptr)
        receipt.Add ("semanticMetadata", *semanticMetadata);
    const auto& primary = receipt.AddList<GS::UniString> ("primaryGuids");
    for (const GS::ObjectState& id : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (id);
        primary (APIGuidToString (guid));
    }
    receipt.AddList<GS::UniString> ("collateralGuids");
    const auto& elements = receipt.AddList<GS::ObjectState> ("elements");
    GS::Array<GS::ObjectState> before;
    GS::Array<GS::ObjectState> after;
    beforeResponse.Get ("elements", before);
    afterResponse.Get ("elements", after);
    for (const GS::ObjectState& id : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (id);
        const GS::ObjectState* beforeSnapshot = nullptr;
        const GS::ObjectState* afterSnapshot = nullptr;
        for (const GS::ObjectState& candidate : before) {
            const GS::ObjectState* candidateID = candidate.Get ("elementId");
            if (candidateID != nullptr && GetGuidFromObjectState (*candidateID) == guid) beforeSnapshot = &candidate;
        }
        for (const GS::ObjectState& candidate : after) {
            const GS::ObjectState* candidateID = candidate.Get ("elementId");
            if (candidateID != nullptr && GetGuidFromObjectState (*candidateID) == guid) afterSnapshot = &candidate;
        }
        GS::ObjectState element;
        element.Add ("elementId", id);
        element.Add ("elementType", typeName);
        element.Add ("role", "PRIMARY");
        const bool isNormalizedDelete = operation == "normalize_multiline"
            && normalizeSurvivor != nullptr
            && guid != *normalizeSurvivor;
        element.Add ("action", isNormalizedDelete ? "delete" : operation == "normalize_multiline" ? "update" : operation);
        if (beforeSnapshot != nullptr) element.Add ("beforeState", *beforeSnapshot);
        if (afterSnapshot != nullptr) element.Add ("afterState", *afterSnapshot);
        if ((operation == "delete" || isNormalizedDelete) && beforeSnapshot != nullptr) {
            const GS::ObjectState* recreate = beforeSnapshot->Get ("recreateState");
            if (recreate != nullptr) element.Add ("recreateState", *recreate);
        }
        const GS::ObjectState* beforeState = beforeSnapshot == nullptr ? nullptr : beforeSnapshot->Get ("writableState");
        const GS::ObjectState* afterState = afterSnapshot == nullptr ? nullptr : afterSnapshot->Get ("writableState");
        const auto& changed = element.AddList<GS::ObjectState> ("changedFields");
        if (beforeState != nullptr || afterState != nullptr) changed (ChangedField (beforeState, afterState));
        elements (element);
    }
    receipt.Add ("transaction", GS::ObjectState (
        "atomic", true,
        "undoable", true,
        "dispatchComplete", dispatchComplete,
        "evidenceComplete", complete,
        "outcome", status));
    return receipt;
}

GS::ObjectState BuildNativeMutationParameters (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    GS::ObjectState parameters;
    const auto& normalized = parameters.AddList<GS::ObjectState> ("items");
    for (const GS::ObjectState& item : items) {
        GS::ObjectState copy;
        const GS::ObjectState* id = item.Get ("elementId");
        if (id != nullptr) copy.Add ("elementId", *id);
        const GS::ObjectState* payload = item.Get ("payload");
        if (payload == nullptr) payload = item.Get ("patch");
        if (payload != nullptr) {
            GS::ObjectState mapped;
            // Coordinates and scalar fields retain their native units.  The
            // Project-owned typed executors use their own field names; this
            // adapter is the one and only place where that vocabulary is
            // translated from native-crud-v1.
            if (typeName == "Wall") {
                CopyObjectField (*payload, "begCoordinate", mapped, "begCoordinate");
                CopyObjectField (*payload, "endCoordinate", mapped, "endCoordinate");
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                CopyDoubleField (*payload, "elevation", mapped, "zCoordinate");
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyDoubleField (*payload, "thickness", mapped, "thickness");
                CopyDoubleField (*payload, "offset", mapped, "offset");
                CopyStringField (*payload, "structure", mapped, "structureType");
                CopyObjectField (*payload, "material", mapped, "buildingMaterialId");
                CopyObjectField (*payload, "profile", mapped, "profileId");
                CopyObjectField (*payload, "composite", mapped, "compositeId");
                CopyStringField (*payload, "referenceLine", mapped, "referenceLineLocation");
            } else if (typeName == "Column") {
                CopyObjectField (*payload, "origin", mapped, "origin");
                // CreateColumns historically calls its 3D placement
                // `coordinates`; updates call it `origin`.  Preserve both
                // forms so the delegated command can apply the operation
                // specific validation.
                if (operation == "create") CopyObjectField (*payload, "origin", mapped, "coordinates");
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                CopyDoubleField (*payload, "elevation", mapped, "zCoordinate");
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyDoubleField (*payload, "offset", mapped, "bottomOffset");
                CopyDoubleField (*payload, "rotation", mapped, "axisRotationAngle");
                CopyDoubleField (*payload, "slant", mapped, "slantAngle");
                const GS::ObjectState* dimensions = payload->Get ("dimensions");
                if (dimensions != nullptr) {
                    CopyDoubleField (*dimensions, "width", mapped, "width");
                    CopyDoubleField (*dimensions, "depth", mapped, "depth");
                }
                CopyObjectField (*payload, "profile", mapped, "profileId");
                CopyObjectField (*payload, "material", mapped, "buildingMaterialId");
            } else if (typeName == "Text") {
                CopyObjectField (*payload, "richText", mapped, "richText");
                CopyObjectField (*payload, "coordinate", mapped, "coordinate");
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                CopyIntField (*payload, "anchor", mapped, "anchor");
                CopyDoubleField (*payload, "angle", mapped, "angle");
                CopyDoubleField (*payload, "width", mapped, "width");
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyIntField (*payload, "pen", mapped, "pen");
                CopyBoolField (*payload, "fixedSize", mapped, "fixedSize");
                CopyBoolField (*payload, "nonBreaking", mapped, "nonBreaking");
            } else if (typeName == "Zone") {
                CopyStringField (*payload, "name", mapped, "name");
                CopyStringField (*payload, "number", mapped, "numberStr");
                CopyObjectField (*payload, "category", mapped, "categoryAttributeId");
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                const GS::ObjectState* stamp = payload->Get ("stampPlacement");
                if (stamp != nullptr) {
                    CopyObjectField (*stamp, "position", mapped, "stampPosition");
                    CopyDoubleField (*stamp, "angle", mapped, "stampAngle");
                    CopyBoolField (*stamp, "fixedAngle", mapped, "fixedStampAngle");
                }
                const GS::ObjectState* polygon = payload->Get ("polygon");
                if (polygon != nullptr) {
                    CopyArrayField (*polygon, "coordinates", mapped, "polygonCoordinates");
                    CopyArrayField (*polygon, "arcs", mapped, "polygonArcs");
                    CopyArrayField (*polygon, "holes", mapped, "holes");
                    GS::ObjectState geometry;
                    CopyArrayField (*polygon, "coordinates", geometry, "polygonCoordinates");
                    CopyArrayField (*polygon, "arcs", geometry, "polygonArcs");
                    CopyArrayField (*polygon, "holes", geometry, "holes");
                    if (!geometry.IsEmpty ()) mapped.Add ("geometry", geometry);
                }
                CopyObjectField (*payload, "referencePosition", mapped, "referencePosition");
            } else if (typeName == "Door" || typeName == "Window") {
                CopyObjectField (*payload, "ownerWallId", mapped, "ownerWallId");
                CopyDoubleField (*payload, "offset", mapped, "centerOffset");
                CopyDoubleField (*payload, "sill", mapped, "sillHeight");
                CopyDoubleField (*payload, "width", mapped, "width");
                CopyDoubleField (*payload, "height", mapped, "height");
                const GS::ObjectState* orientation = payload->Get ("orientation");
                if (orientation != nullptr) {
                    CopyBoolField (*orientation, "reflected", mapped, "reflected");
                    CopyBoolField (*orientation, "refSide", mapped, "refSide");
                    CopyBoolField (*orientation, "oSide", mapped, "oSide");
                }
            } else if (typeName == "Line") {
                CopyObjectField (*payload, "begin", mapped, "begCoordinate");
                CopyObjectField (*payload, "end", mapped, "endCoordinate");
                // MutateElements validates the public story field as
                // `floorIndex`; keep the adapter on that validated name so a
                // native one-step patch cannot silently lose story scope.
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else if (typeName == "Arc") {
                CopyObjectField (*payload, "origin", mapped, "origin");
                CopyDoubleField (*payload, "radius", mapped, "radius");
                CopyDoubleField (*payload, "axisAngle", mapped, "axisAngle");
                CopyDoubleField (*payload, "ratio", mapped, "ratio");
                // The typed Arc command calls the first sweep angle
                // `begAngle`; map the canonical native field explicitly.
                // Without this translation CREATE passes validation but the
                // typed command rejects the required angle.
                CopyDoubleField (*payload, "beginAngle", mapped, "begAngle");
                CopyDoubleField (*payload, "endAngle", mapped, "endAngle");
                CopyBoolField (*payload, "reflected", mapped, "reflected");
                CopyIntField (*payload, "storyIndex", mapped, "floorIndex");
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else {
                mapped = *payload;
            }
            copy.Add ("payload", mapped);
        }
        normalized (copy);
    }
    parameters.Add ("operation", operation);
    return parameters;
}

GS::ObjectState DispatchMutation (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl)
{
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (typeName);
    if (operation == "delete")
        return DeleteElementsCommand ().Execute ([&]() {
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
    // The existing project-owned executor already provides undoable typed
    // operations for these three types.  Keep the adapter narrow until the
    // remaining element-specific memo writers have their own atomic tests;
    // returning a capability error is safer than treating a partial command as
    // factual native CRUD.
    if (typeID == API_WallID || typeID == API_ColumnID || typeID == API_TextID ||
        typeID == API_LineID || typeID == API_ArcID) {
        GS::ObjectState nativeParameters = BuildNativeMutationParameters (operation, typeName, items);
        nativeParameters.Add ("elementType", typeName);
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
        return normalized;
    }
    return CreateErrorResponse (APIERR_BADPARS, "This element type has read snapshots but no validated native memo writer in this APX build.");
}

GS::Optional<GS::UniString> ValidateMutationItems (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return GS::UniString ("items must contain between 1 and 25 entries.");
    GS::Array<API_Guid> seenGuids;
    for (const GS::ObjectState& item : items) {
        if (operation == "create") {
            if (item.Get ("elementId") != nullptr || item.Get ("payload") == nullptr)
                return GS::UniString ("CREATE items require payload and must not contain elementId.");
            if (item.Get ("patch") != nullptr || item.Get ("expectedFingerprint") != nullptr)
                return GS::UniString ("CREATE items accept only a typed payload and an optional source.");
            const bool hasClone = item.Get ("cloneSourceElementId") != nullptr;
            const bool hasHistory = item.Get ("historySnapshotRef") != nullptr;
            if (hasClone && hasHistory)
                return GS::UniString ("CREATE items may use only one clone source.");
            if ((typeName == "Door" || typeName == "Window") && !hasClone && !hasHistory)
                return GS::UniString ("Door/Window CREATE requires a same-file clone source.");
            if (typeName != "Door" && typeName != "Window" && hasClone)
                return GS::UniString ("Only Door/Window CREATE may use a live clone source.");
            if (hasClone) {
                const GS::ObjectState* cloneID = item.Get ("cloneSourceElementId");
                if (cloneID == nullptr || GetGuidFromObjectState (*cloneID) == APINULLGuid)
                    return GS::UniString ("cloneSourceElementId must contain a valid GUID.");
            }
            if (hasHistory) {
                GS::UniString historyRef;
                if (!item.Get ("historySnapshotRef", historyRef) || historyRef.IsEmpty ())
                    return GS::UniString ("historySnapshotRef must be a non-empty same-file receipt reference.");
            }
        } else {
            const GS::ObjectState* id = item.Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) == APINULLGuid)
                return GS::UniString ("UPDATE/DELETE items require a valid elementId.");
            const API_Guid guid = GetGuidFromObjectState (*id);
            if (seenGuids.Contains (guid))
                return GS::UniString ("UPDATE/DELETE items may not repeat a GUID.");
            seenGuids.Push (guid);
            GS::UniString expectedFingerprint;
            if (!item.Get ("expectedFingerprint", expectedFingerprint) || expectedFingerprint.IsEmpty ())
                return GS::UniString ("UPDATE/DELETE items require expectedFingerprint from an exact native read.");
            if (!IsHex64 (expectedFingerprint))
                return GS::UniString ("expectedFingerprint must be a 64-hex native fingerprint.");
            if (operation == "update" && item.Get ("patch") == nullptr)
                return GS::UniString ("UPDATE items require a sparse patch.");
            if (operation == "update" && item.Get ("payload") != nullptr)
                return GS::UniString ("UPDATE items use patch, not payload.");
            if (operation == "delete" && (item.Get ("patch") != nullptr || item.Get ("payload") != nullptr))
                return GS::UniString ("DELETE items must not contain a payload or patch.");
        }
    }
    return {};
}

} // namespace

GetNativeCrudCapabilitiesCommand::GetNativeCrudCapabilitiesCommand () : CommandBase (CommonSchema::NotUsed) {}

GS::String GetNativeCrudCapabilitiesCommand::GetName () const { return "GetNativeCrudCapabilities"; }

GS::Optional<GS::UniString> GetNativeCrudCapabilitiesCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"contractVersion":{"type":"string"}},"additionalProperties":false,"required":["contractVersion"]})";
}

GS::Optional<GS::UniString> GetNativeCrudCapabilitiesCommand::GetResponseSchema () const { return {}; }

GS::ObjectState GetNativeCrudCapabilitiesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::Optional<GS::UniString> contractError = ValidateContractVersion (parameters);
    if (contractError.HasValue ()) return ScopeError (contractError);
    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("snapshotVersion", SnapshotVersion);
    response.Add ("receiptVersion", ReceiptVersion);
    response.Add ("historyVersion", HistoryVersion);
    response.Add ("fileScope", "saved Solo PLN canonical path");
    response.Add ("limits", GS::ObjectState (
        "maxItems", static_cast<Int32> (MaxMutationItems),
        "maxDiscoverReferences", static_cast<Int32> (MaxDiscoveryReferences),
        "maxHistoryEntries", static_cast<Int32> (MaxMutationItems),
        "impactClosure", static_cast<Int32> (MaxImpactElements)));
    const auto& elementTypes = response.AddList<GS::ObjectState> ("elementTypes");
    for (const char* typeName : { "Zone", "Wall", "Column", "Text", "Line", "Arc", "Hatch", "Door", "Window" }) {
        GS::ObjectState type;
        type.Add ("elementType", typeName);
        type.Add ("contracted", GS::ObjectState (
            "create", true,
            "read", true,
            "update", true,
            "delete", true));
        type.Add ("read", true);
        const bool validatedWriter = GS::UniString (typeName) == "Wall" || GS::UniString (typeName) == "Column" || GS::UniString (typeName) == "Text";
        type.Add ("create", validatedWriter);
        type.Add ("update", validatedWriter);
        type.Add ("delete", true);
        type.Add ("writerStatus", validatedWriter ? "validated" : "read_only_until_memo_writer_gate");
        AddCanonicalWritableFields (type, GS::UniString (typeName));
        elementTypes (type);
    }
    response.Add ("safety", GS::ObjectState (
        "previewRequired", false,
        "singleStepAtomicPatch", true,
        "expectedOldValues", true,
        "authorizationRequired", true,
        "authorizationMode", "server-configured",
        "atomicBatch", true,
        "sparseUpdate", true,
        "noRestoreReplayApi", true,
        "unknownOutcomeNoRetry", true));
    return response;
}

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
    if (parameters.Get ("elementType", requestedType) && !requestedType.IsEmpty () && !IsSupportedNativeTypeName (requestedType))
        return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    GS::Array<GS::UniString> guids;
    const GS::Optional<GS::UniString> guidError = ReadRequestedGuids (parameters, guids);
    if (guidError.HasValue ()) return ScopeError (guidError);
    bool complete = false;
    GS::ObjectState response = ExactSnapshots (RequestedIdsFromGuids (guids), CurrentSoloFileScope ().Get (), processControl, complete);
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
    if (parameters.Get ("elementType", typeName)) {
        typeID = GetElementTypeFromNonLocalizedName (typeName);
        if (!IsSupportedNativeType (typeID)) return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    }

    GS::Array<GS::ObjectState> ids;
    if (hasBounds) {
        const GS::ObjectState* bounds = parameters.Get ("bounds");
        if (bounds == nullptr)
            return CreateErrorResponse (APIERR_BADPARS, "bounds is unavailable.");
        double minX = 0.0;
        double minY = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
        if (!bounds->Get ("minX", minX) || !bounds->Get ("minY", minY) ||
            !bounds->Get ("maxX", maxX) || !bounds->Get ("maxY", maxY))
            return CreateErrorResponse (APIERR_BADPARS, "bounds requires numeric minX, minY, maxX and maxY.");
        GS::ObjectState rectParameters;
        rectParameters.Add ("bounds", GS::ObjectState (
            "xMin", minX, "yMin", minY,
            "xMax", maxX, "yMax", maxY));
        rectParameters.Add ("multiStory", true);
        const GS::ObjectState rectResult = GetElementsInRectCommand ().Execute (rectParameters, processControl);
        rectResult.Get ("elements", ids);
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
    bool complete = false;
    const GS::UniString scope = CurrentSoloFileScope ().Get ();
    GS::ObjectState snapshots = ExactSnapshots (ids, scope, processControl, complete);
    GS::Array<GS::ObjectState> snapshotRows;
    snapshots.Get ("elements", snapshotRows);
    GS::ObjectState filtered;
    const auto& elements = filtered.AddList<GS::ObjectState> ("elements");
    for (const GS::ObjectState& snapshot : snapshotRows) {
        Int32 snapshotStory = 0;
        if (hasStory && (!snapshot.Get ("storyIndex", snapshotStory) || snapshotStory != storyIndex)) continue;
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
    GS::ObjectState scopeParameters;
    GS::UniString expectedScope;
    parameters.Get ("expectedFileScopeKey", expectedScope);
    scopeParameters.Add ("fileScopeKey", expectedScope);
    const GS::Optional<GS::UniString> scopeError = ValidateScope (scopeParameters);
    if (scopeError.HasValue ()) return ScopeError (scopeError);
    GS::UniString contract;
    if (!parameters.Get ("contractVersion", contract) || contract != ContractVersion)
        return CreateErrorResponse (APIERR_BADPARS, "contractVersion must be native-crud-v1.");
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
    if (operation != "create" && operation != "update" && operation != "delete")
        return CreateErrorResponse (APIERR_BADPARS, "operation must be create, update or delete.");
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (typeName);
    if (!IsSupportedNativeType (typeID)) return CreateErrorResponse (APIERR_BADPARS, "Unsupported native elementType.");
    // Keep the wire vocabulary canonical at the native boundary as well.  A
    // full Circle is exposed as Arc in native-crud-v1; accepting the alias
    // here would otherwise produce a receipt whose type disagrees with its
    // snapshots and capability row.
    typeName = CanonicalElementTypeName (typeName);

    GS::Array<GS::ObjectState> items;
    if (!parameters.Get ("items", items)) return CreateErrorResponse (APIERR_BADPARS, "items is required.");
    const GS::Optional<GS::UniString> itemError = ValidateMutationItems (operation, typeName, items);
    if (itemError.HasValue ()) return ScopeError (itemError);

    GS::Array<GS::ObjectState> requestedIds;
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* id = item.Get ("elementId");
        if (id != nullptr) requestedIds.Push (*id);
    }

    const GS::UniString scope = CurrentSoloFileScope ().Get ();

    // Idempotency is owned durably by MCP SQLite.  APX keeps only a bounded
    // process-local receipt cache for response-loss reconciliation while this
    // Archicad process is alive.  A cache hit must still match the complete
    // request identity; an operation-id collision is never silently replayed.
    const GS::Optional<GS::ObjectState> cachedReceipt = FindCachedReceipt (operationId, scope);
    if (cachedReceipt.HasValue ()) {
        if (!ReceiptRequestIdentityMatches (cachedReceipt.Get (), operation, typeName, requestHash, requestedIds)) {
            return NativeContractError (
                "DUPLICATE_OPERATION_CONFLICT",
                "operationId already has a different requestHash, operation, elementType or exact GUID set.");
        }
        GS::ObjectState replay;
        replay.Add ("receipt", cachedReceipt.Get ());
        replay.Add ("readback", GS::ObjectState ("elements", GS::ObjectState ()));
        replay.Add ("duplicateOperation", true);
        return replay;
    }
    bool beforeComplete = operation == "create";
    GS::ObjectState before = operation == "create"
        ? GS::ObjectState ("elements", GS::ObjectState ())
        : ExactSnapshots (requestedIds, scope, processControl, beforeComplete);
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
        }
    }

    const GS::ObjectState mutationResult = DispatchMutation (operation, typeName, items, processControl);
    bool dispatchComplete = true;
    mutationResult.Get ("mutationComplete", dispatchComplete);
    if (mutationResult.Get ("error") != nullptr || mutationResult.Get ("transactionRolledBack") != nullptr) dispatchComplete = false;

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
        deleteReadback = BuildDeleteReadback (requestedIds, typeID, deleteVerified, absentCount);
        afterComplete = deleteVerified && absentCount == static_cast<GSSize> (requestedIds.GetSize ());
        after = GS::ObjectState ("elements", GS::ObjectState ());
    } else {
        after = ExactSnapshots (afterIds, scope, processControl, afterComplete);
        afterComplete = afterComplete && createIdsComplete;
    }
    const bool complete = dispatchComplete && afterComplete;
    const GS::ObjectState* semanticMetadata = parameters.Get ("semanticMetadata");
    GS::ObjectState receipt = BuildReceipt (scope, operationId, operation, typeName, description, origin, requestHash, before, after, operation == "create" ? afterIds : requestedIds, complete, dispatchComplete, semanticMetadata, operatorNote);
    // BuildReceipt deliberately has no durable side effect.  Give the APX
    // response a process-local sequence for diagnostics and remember the
    // factual receipt before returning it so MCP can reconcile a lost HTTP
    // response without dispatching the Archicad command a second time.
    receipt.Add ("nativeSequence", NextCachedReceiptSequence ());
    RememberCachedReceipt (receipt);
    GS::ObjectState response;
    response.Add ("contractVersion", ContractVersion);
    response.Add ("status", "success");
    response.Add ("receipt", receipt);
    response.Add ("mutationResult", mutationResult);
    response.Add ("readback", after);
    if (operation == "delete") response.Add ("deleteReadback", deleteReadback);
    return response;
}

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
        const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
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
        before, after, ids, complete, dispatchComplete, parameters.Get ("semanticMetadata"),
        operatorNote, nullptr, &survivorGuid);
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
