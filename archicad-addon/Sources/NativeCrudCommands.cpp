#include "NativeCrudCommands.hpp"

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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

#ifndef TAPIR_AC_VERSION
#define TAPIR_AC_VERSION 0
#endif
#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE 0
#endif

namespace {

constexpr const char* ContractVersion = "native-crud-v2";
constexpr const char* SnapshotVersion = "native-element-snapshot-v1";
constexpr const char* ReceiptVersion = "native-mutation-receipt-v1";
constexpr const char* HistoryVersion = "native-mutation-history-v1";
constexpr const char* CapabilitySchemaVersion = "native-crud-capabilities-v2";
constexpr const char* CapabilitySchemaHash = "62e57e16dad3389a7852d70552af41eaf9ef7c08eb71687d0016329002db228b";
constexpr const char* CapabilitySchemaV3Version = "native-crud-capabilities-v3";
constexpr const char* CapabilitySchemaV3Hash = "0b02675a0d1fb224469ee82d55c9eb0f6a7f030c2f3211d3203e717d0b7627b4";
constexpr const char* ImpactEnvelopeVersion = "native-impact-envelope-v1";
constexpr const char* ImpactEvidenceVersion = "native-impact-evidence-v1";
constexpr const char* ImpactMetadataKey = "nativeImpact";
constexpr const char* ImpactScope = "wall-zone-pair-v1";
constexpr const char* ImpactEvidenceSource = "native-impact-coordinator-v1";
constexpr GSSize MaxMutationItems = 25;
constexpr GSSize MaxDiscoveryReferences = 100;
constexpr GSSize MaxDiscoveryCandidates = 4096;
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
    if (cache.size () >= static_cast<std::size_t> (MaxCachedReceipts))
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

void CopyColumnCreateCoordinates (const GS::ObjectState& payload, GS::ObjectState& target)
{
    // CreateColumns consumes one native 3D `coordinates` value.  Origin and
    // elevation are update vocabulary and must never be synthesized into it.
    CopyObjectField (payload, "coordinates", target, "coordinates");
}

void CopyArrayField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName)
{
    GS::Array<GS::ObjectState> values;
    if (!source.Get (sourceName, values)) return;
    const auto& output = target.AddList<GS::ObjectState> (targetName);
    for (const GS::ObjectState& value : values) output (value);
}

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
        CopyDoubleField (details, "zCoordinate", state, "elevation");
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
        CopyDoubleField (details, "zCoordinate", state, "elevation");
        CopyDoubleField (details, "height", state, "height");
        CopyDoubleField (details, "bottomOffset", state, "offset");
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
    const char* createFields[16] = {};
    GSSize createCount = 0;
    GSSize count = 0;
    if (typeName == "Zone") {
        const char* values[] = { "name", "number", "category", "polygon", "stampPlacement" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Wall") {
        const char* values[] = { "begCoordinate", "endCoordinate", "elevation", "height", "thickness", "offset", "structure", "composite", "referenceLine", "flipped" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Column") {
        // The typed Column commands use operation-specific vocabularies.
        const char* values[] = { "origin", "offset", "height", "rotation", "dimensions" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
        const char* createValues[] = { "coordinates", "height", "rotation", "dimensions" };
        createCount = sizeof (createValues) / sizeof (createValues[0]);
        for (GSSize i = 0; i < createCount; ++i) createFields[i] = createValues[i];
    } else if (typeName == "Text") {
        const char* values[] = { "richText", "coordinate", "anchor", "angle", "width", "height", "pen", "fixedSize", "nonBreaking" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Line") {
        const char* values[] = { "begin", "end", "lineType", "pen", "roomSeparator" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
        const char* createValues[] = { "begin", "end", "lineType", "pen", "roomSeparator" };
        createCount = sizeof (createValues) / sizeof (createValues[0]);
        for (GSSize i = 0; i < createCount; ++i) createFields[i] = createValues[i];
    } else if (typeName == "Arc") {
        const char* values[] = { "origin", "radius", "ratio", "axisAngle", "beginAngle", "endAngle", "reflected", "lineType", "pen", "roomSeparator" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Hatch") {
        const char* values[] = { "polygon", "fill", "contourPen", "foregroundPen", "backgroundPen" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
    } else if (typeName == "Door") {
        const char* values[] = { "offset", "sill", "width", "height", "orientation" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
        const char* createValues[] = { "ownerWallId", "offset", "sill", "width", "height", "orientation" };
        createCount = sizeof (createValues) / sizeof (createValues[0]);
        for (GSSize i = 0; i < createCount; ++i) createFields[i] = createValues[i];
    } else if (typeName == "Window") {
        const char* values[] = { "offset", "sill", "width", "height", "orientation" };
        count = sizeof (values) / sizeof (values[0]);
        for (GSSize i = 0; i < count; ++i) fields[i] = values[i];
        const char* createValues[] = { "ownerWallId", "offset", "sill", "width", "height", "orientation" };
        createCount = sizeof (createValues) / sizeof (createValues[0]);
        for (GSSize i = 0; i < createCount; ++i) createFields[i] = createValues[i];
    }
    const auto& writableFields = target.AddList<GS::UniString> ("writableFields");
    const auto& createFieldList = target.AddList<GS::UniString> ("createFields");
    const auto& createPlacementFields = target.AddList<GS::UniString> ("createPlacementFields");
    const auto& updateFields = target.AddList<GS::UniString> ("updateFields");
    createPlacementFields ("storyIndex");
    for (GSSize i = 0; i < count; ++i) {
        writableFields (fields[i]);
        if (typeName != "Column" && typeName != "Line" && typeName != "Door" && typeName != "Window") createFieldList (fields[i]);
        updateFields (fields[i]);
    }
    if (typeName == "Column" || typeName == "Line" || typeName == "Door" || typeName == "Window")
        for (GSSize i = 0; i < createCount; ++i) createFieldList (createFields[i]);
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

bool ReceiptImpactEnvelopeMatches (
    const GS::ObjectState& receipt,
    const GS::ObjectState* requestedSemanticMetadata)
{
    const GS::ObjectState* requestedImpact = requestedSemanticMetadata == nullptr
        ? nullptr
        : requestedSemanticMetadata->Get (ImpactMetadataKey);
    const GS::ObjectState* receiptSemanticMetadata = receipt.Get ("semanticMetadata");
    const GS::ObjectState* receiptImpact = receiptSemanticMetadata == nullptr
        ? nullptr
        : receiptSemanticMetadata->Get (ImpactMetadataKey);
    if ((requestedImpact == nullptr) != (receiptImpact == nullptr)) return false;
    if (requestedImpact == nullptr) return true;
    GS::UniString requestedJson;
    GS::UniString receiptJson;
    if (JSON::CreateFromObjectState (*requestedImpact, requestedJson) != NoError
        || JSON::CreateFromObjectState (*receiptImpact, receiptJson) != NoError)
        return false;
    // The TypeScript coordinator canonicalizes the complete carrier before
    // requestHash and dispatch.  Comparing the complete persisted carrier,
    // not only primary IDs or a hash-shaped string, binds cache replay to the
    // exact closure/evidence contract.
    return requestedJson == receiptJson;
}

bool ReceiptRequestIdentityMatches (
    const GS::ObjectState& receipt,
    const GS::UniString& expectedScope,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::UniString& requestHash,
    const GS::UniString& description,
    const GS::UniString& operatorNote,
    const GS::UniString& origin,
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::ObjectState* requestedSemanticMetadata)
{
    GS::UniString recordedScope;
    GS::UniString recordedOperation;
    GS::UniString recordedType;
    GS::UniString recordedHash;
    GS::UniString recordedDescription;
    GS::UniString recordedOrigin;
    GS::UniString recordedOperatorNote;
    if (!receipt.Get ("fileScopeKey", recordedScope)
        || !receipt.Get ("operation", recordedOperation)
        || !receipt.Get ("elementType", recordedType)
        || !receipt.Get ("requestHash", recordedHash)
        || !receipt.Get ("description", recordedDescription)
        || !receipt.Get ("origin", recordedOrigin))
        return false;

    const GS::UniString expectedOrigin = origin.IsEmpty () ? GS::UniString ("NATIVE") : origin;
    const bool hasRecordedOperatorNote = receipt.Get ("operatorNote", recordedOperatorNote);
    if (recordedScope != expectedScope
        || recordedOperation != operation
        || recordedType != typeName
        || LowerAscii (recordedHash) != LowerAscii (requestHash)
        || recordedDescription != description
        || recordedOrigin != expectedOrigin
        || hasRecordedOperatorNote != !operatorNote.IsEmpty ()
        || (hasRecordedOperatorNote && recordedOperatorNote != operatorNote))
        return false;
    if (!ReceiptImpactEnvelopeMatches (receipt, requestedSemanticMetadata)) return false;

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

struct NativeImpactEnvelope {
    GS::ObjectState envelope;
    GS::Array<GS::ObjectState> collateralIds;
    GS::Array<GS::ObjectState> impactIds;
    GS::Array<GS::UniString> collateralGuids;
    GS::Array<GS::UniString> collateralElementTypes;
};

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
    const GS::Array<GS::ObjectState>& impactIds,
    const bool complete,
    const bool dispatchComplete,
    const GS::ObjectState* semanticMetadata,
    const GS::UniString& operatorNote,
    const GS::ObjectState* mutationDiagnostics = nullptr,
    const char* statusOverride = nullptr,
    const API_Guid* normalizeSurvivor = nullptr,
    const NativeImpactEnvelope* impact = nullptr)
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
    const auto& collateral = receipt.AddList<GS::UniString> ("collateralGuids");
    if (impact != nullptr)
        for (const GS::UniString& guid : impact->collateralGuids) collateral (guid);
    const auto& elements = receipt.AddList<GS::ObjectState> ("elements");
    GS::Array<GS::ObjectState> before;
    GS::Array<GS::ObjectState> after;
    beforeResponse.Get ("elements", before);
    afterResponse.Get ("elements", after);
    for (const GS::ObjectState& id : impactIds) {
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
        bool primaryElement = false;
        for (const GS::ObjectState& primaryId : requestedIds) {
            if (GetRequestedElementGuid (primaryId) == guid) {
                primaryElement = true;
                break;
            }
        }
        GS::UniString snapshotType;
        if (beforeSnapshot != nullptr && beforeSnapshot->Get ("elementType", snapshotType))
            element.Add ("elementType", snapshotType);
        else if (afterSnapshot != nullptr && afterSnapshot->Get ("elementType", snapshotType))
            element.Add ("elementType", snapshotType);
        else if (impact != nullptr && !primaryElement) {
            GS::UniString collateralType;
            const GSSize collateralIdCount = static_cast<GSSize> (impact->collateralIds.GetSize ());
            const GSSize collateralTypeCount = static_cast<GSSize> (impact->collateralElementTypes.GetSize ());
            for (GSSize index = 0; index < collateralIdCount; ++index) {
                if (GetRequestedElementGuid (impact->collateralIds[index]) == guid
                    && index < collateralTypeCount) {
                    collateralType = impact->collateralElementTypes[index];
                    break;
                }
            }
            element.Add ("elementType", collateralType.IsEmpty () ? typeName : collateralType);
        } else
            element.Add ("elementType", typeName);
        element.Add ("role", primaryElement ? "PRIMARY" : "COLLATERAL");
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
    if (impact != nullptr) {
        bool collateralBeforeComplete = true;
        bool collateralAfterComplete = true;
        for (const GS::ObjectState& collateralId : impact->collateralIds) {
            const API_Guid guid = GetRequestedElementGuid (collateralId);
            bool hasBefore = false;
            for (const GS::ObjectState& row : before) {
                const GS::ObjectState* id = row.Get ("elementId");
                if (id != nullptr && GetGuidFromObjectState (*id) == guid) {
                    hasBefore = true;
                    break;
                }
            }
            bool hasAfter = false;
            for (const GS::ObjectState& row : after) {
                const GS::ObjectState* id = row.Get ("elementId");
                if (id != nullptr && GetGuidFromObjectState (*id) == guid) {
                    hasAfter = true;
                    break;
                }
            }
            collateralBeforeComplete = collateralBeforeComplete && hasBefore;
            collateralAfterComplete = collateralAfterComplete && (operation == "delete" ? true : hasAfter);
        }
        const bool impactCoverageComplete = complete && collateralBeforeComplete && collateralAfterComplete;
        GS::ObjectState evidence;
        evidence.Add ("schemaVersion", ImpactEvidenceVersion);
        evidence.Add ("source", ImpactEvidenceSource);
        evidence.Add ("envelope", impact->envelope);
        evidence.Add ("impactComplete", impactCoverageComplete);
        evidence.Add ("predictedImpactComplete", true);
        evidence.Add ("collateralBeforeComplete", collateralBeforeComplete);
        evidence.Add ("collateralAfterComplete", collateralAfterComplete);
        evidence.Add ("coverage", impactCoverageComplete ? "complete" : "incomplete");
        receipt.Add ("impactEvidence", evidence);
        GS::ObjectState dependency = evidence;
        dependency.Add ("status", status);
        receipt.Add ("dependencyEvidence", dependency);
    }
    GS::ObjectState transaction (
        "atomic", true,
        "undoable", true,
        "dispatchAttempted", true,
        "dispatchComplete", dispatchComplete,
        "evidenceComplete", complete,
        "outcome", status);
    if (mutationDiagnostics != nullptr) {
        bool transactionRolledBack = false;
        if (mutationDiagnostics->Get ("transactionRolledBack", transactionRolledBack))
            transaction.Add ("transactionRolledBack", transactionRolledBack);
        Int32 transactionError = 0;
        if (mutationDiagnostics->Get ("transactionError", transactionError))
            transaction.Add ("transactionError", transactionError);
        GS::UniString failureStage;
        if (mutationDiagnostics->Get ("failureStage", failureStage))
            transaction.Add ("failureStage", failureStage);
        bool nativeApiErrorCodeRecorded = false;
        bool nativeFailureMessageRecorded = false;
        bool nativeCommandFailureObserved = false;
        const GS::ObjectState* nativeCommandError = mutationDiagnostics->Get ("error");
        if (nativeCommandError != nullptr) {
            Int32 nativeApiErrorCode = 0;
            if (nativeCommandError->Get ("code", nativeApiErrorCode)) {
                transaction.Add ("nativeApiErrorCode", nativeApiErrorCode);
                nativeApiErrorCodeRecorded = true;
                nativeCommandFailureObserved = true;
            }
            GS::UniString nativeFailureMessage;
            if (nativeCommandError->Get ("message", nativeFailureMessage)) {
                transaction.Add ("nativeFailureMessage", nativeFailureMessage);
                nativeFailureMessageRecorded = true;
                nativeCommandFailureObserved = true;
            }
        }
        // Typed writers keep the API failure beside the failed execution row.
        // Preserve that exact diagnostic in the terminal receipt even when
        // the host did not return an undo-command error; equal snapshots alone
        // do not prove that Archicad rolled back the attempted change.
        GS::Array<GS::ObjectState> executionResults;
        if (mutationDiagnostics->Get ("executionResults", executionResults)) {
            for (const GS::ObjectState& executionResult : executionResults) {
                bool success = false;
                if (!executionResult.Get ("success", success) || success)
                    continue;
                if (failureStage.IsEmpty () && executionResult.Get ("failureStage", failureStage))
                    transaction.Add ("failureStage", failureStage);
                const GS::ObjectState* nativeError = executionResult.Get ("error");
                Int32 nativeApiErrorCode = 0;
                if (!nativeApiErrorCodeRecorded && nativeError != nullptr && nativeError->Get ("code", nativeApiErrorCode))
                    transaction.Add ("nativeApiErrorCode", nativeApiErrorCode);
                GS::UniString nativeFailureMessage;
                if (!nativeFailureMessageRecorded && nativeError != nullptr && nativeError->Get ("message", nativeFailureMessage))
                    transaction.Add ("nativeFailureMessage", nativeFailureMessage);
                const GS::ObjectState* nativeFailureDetails = executionResult.Get ("nativeFailureDetails");
                if (nativeFailureDetails != nullptr)
                    transaction.Add ("nativeFailureDetails", *nativeFailureDetails);
                break;
            }
        }
        if (failureStage.IsEmpty () && nativeCommandFailureObserved)
            transaction.Add ("failureStage", "native-mutation-command");
        bool groupModeRestored = false;
        if (mutationDiagnostics->Get ("groupModeRestored", groupModeRestored))
            transaction.Add ("groupModeRestored", groupModeRestored);
        Int32 groupModeRestoreError = 0;
        if (mutationDiagnostics->Get ("groupModeRestoreError", groupModeRestoreError))
            transaction.Add ("groupModeRestoreError", groupModeRestoreError);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        const GS::ObjectState* testFaultEvidence = mutationDiagnostics->Get ("testFaultEvidence");
        if (testFaultEvidence != nullptr)
            transaction.Add ("testFaultEvidence", *testFaultEvidence);
#endif
        GS::UniString candidateWriter;
        const bool isCandidateColumnCreate = operation == "create"
            && typeName == "Column"
            && semanticMetadata != nullptr
            && semanticMetadata->Get ("nativeCandidateWriter", candidateWriter)
            && candidateWriter == "column-native-v1";
        const GS::ObjectState* candidateFailure = mutationDiagnostics->Get ("nativeCandidateFailure");
        if (isCandidateColumnCreate && GS::UniString (status) == "failed" && candidateFailure != nullptr)
            transaction.Add ("candidateColumnCreateFailure", *candidateFailure);
        const bool isCandidateHatchCreate = operation == "create"
            && typeName == "Hatch"
            && semanticMetadata != nullptr
            && semanticMetadata->Get ("nativeCandidateWriter", candidateWriter)
            && candidateWriter == "hatch-native-v1";
        if (isCandidateHatchCreate && GS::UniString (status) == "failed" && candidateFailure != nullptr)
            transaction.Add ("candidateHatchCreateFailure", *candidateFailure);
    }
    receipt.Add ("transaction", transaction);
    return receipt;
}

double NormalizeNativeArcCreateAngle (double angle)
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    double normalized = std::fmod (angle, twoPi);
    if (normalized < 0.0)
        normalized += twoPi;
    // Adding 2pi to a tiny negative remainder can round to 2pi; keep the
    // adapter's output in the promised half-open interval [0, 2pi).
    if (normalized >= twoPi || normalized == 0.0)
        return 0.0;
    return normalized;
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
        const GS::ObjectState* cloneSource = item.Get ("cloneSourceElementId");
        if (cloneSource != nullptr) copy.Add ("cloneSourceElementId", *cloneSource);
        const GS::ObjectState* payload = item.Get ("payload");
        if (payload == nullptr) payload = item.Get ("patch");
        if (payload != nullptr) {
            GS::ObjectState mapped;
            if (operation == "create") {
                const GS::ObjectState* placement = item.Get ("placement");
                Int32 storyIndex = 0;
                if (placement != nullptr && placement->Get ("storyIndex", storyIndex))
                    mapped.Add (typeName == "Hatch" ? "floorInd" : "floorIndex", storyIndex);
            }
            // Coordinates and scalar fields retain their native units.  The
            // Project-owned typed executors use their own field names; this
            // adapter is the one and only place where that vocabulary is
            // translated from native-crud-v2.
            if (typeName == "Wall") {
                CopyObjectField (*payload, "begCoordinate", mapped, "begCoordinate");
                CopyObjectField (*payload, "endCoordinate", mapped, "endCoordinate");
                if (operation == "create") {
                    // `elevation` is the absolute bottom level in READ and
                    // UPDATE.  CreateWalls reads zCoordinate as a bottom
                    // offset once floorIndex pins the story, so convert it
                    // against the placement story (V1-022).
                    double elevation = 0.0;
                    if (payload->Get ("elevation", elevation)) {
                        const GS::ObjectState* placement = item.Get ("placement");
                        Int32 storyIndex = 0;
                        if (placement != nullptr && placement->Get ("storyIndex", storyIndex))
                            elevation -= GetZPos (static_cast<short> (storyIndex), 0.0, GetStories ());
                        mapped.Add ("zCoordinate", elevation);
                    }
                } else {
                    // Wall UPDATE has a different native mask contract from
                    // CREATE.  Keep the canonical placement names internal
                    // to the narrow adapter; the update helper converts
                    // them to floorInd/bottomOffset without ever forwarding
                    // rejected floorIndex/zCoordinate fields.
                    CopyDoubleField (*payload, "elevation", mapped, "elevation");
                }
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyDoubleField (*payload, "thickness", mapped, "thickness");
                CopyDoubleField (*payload, "offset", mapped, "offset");
                CopyStringField (*payload, "structure", mapped, "structureType");
                CopyObjectField (*payload, "material", mapped, "buildingMaterialId");
                CopyObjectField (*payload, "profile", mapped, "profileId");
                CopyObjectField (*payload, "composite", mapped, "compositeId");
                CopyStringField (*payload, "referenceLine", mapped, "referenceLineLocation");
                CopyBoolField (*payload, "flipped", mapped, "flipped");
            } else if (typeName == "Column") {
                if (operation == "create") {
                    CopyColumnCreateCoordinates (*payload, mapped);
                } else {
                    // ModifyColumns consumes the 2D update placement field
                    // `origin`; CREATE must never receive this field and
                    // UPDATE must never receive the incompatible `coordinates`
                    // field or a rejected zCoordinate translation.
                    CopyObjectField (*payload, "origin", mapped, "origin");
                }
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
                const GS::ObjectState* stamp = payload->Get ("stampPlacement");
                if (stamp != nullptr) {
                CopyObjectField (*stamp, "position", mapped, "stampPosition");
                    CopyDoubleField (*stamp, "angle", mapped, "stampAngle");
                    CopyBoolField (*stamp, "fixedAngle", mapped, "fixedStampAngle");
                }
                const GS::ObjectState* polygon = payload->Get ("polygon");
                GS::ObjectState geometry;
                if (polygon != nullptr) {
                    CopyArrayField (*polygon, "coordinates", geometry, "polygonCoordinates");
                    CopyArrayField (*polygon, "arcs", geometry, "polygonArcs");
                    CopyArrayField (*polygon, "holes", geometry, "holes");
                }
                if (operation == "create") {
                    if (!geometry.IsEmpty ()) mapped.Add ("geometry", geometry);
                } else {
                    if (!geometry.IsEmpty ()) mapped.Add ("geometry", geometry);
                }
            } else if (typeName == "Door" || typeName == "Window") {
                CopyObjectField (*payload, "ownerWallId", mapped, "ownerWallId");
                if (operation == "create") CopyIntField (*payload, "floorIndex", mapped, "floorIndex");
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
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else if (typeName == "Arc") {
                CopyObjectField (*payload, "origin", mapped, "origin");
                CopyDoubleField (*payload, "radius", mapped, "radius");
                CopyDoubleField (*payload, "axisAngle", mapped, "axisAngle");
                CopyDoubleField (*payload, "ratio", mapped, "ratio");
                // This shared Native CRUD adapter is used by candidate and
                // future ordinary requests, and is separate from legacy Arc
                // commands. CREATE input angles are local to the ellipse
                // axis; the factual API_ArcType readback includes axisAngle.
                // Keep sparse UPDATE inputs direct until independently proven.
                double axisAngle = 0.0;
                payload->Get ("axisAngle", axisAngle);
                double beginAngle = 0.0;
                if (payload->Get ("beginAngle", beginAngle)) {
                    mapped.Add ("begAngle", operation == "create" && std::isfinite (axisAngle) && std::isfinite (beginAngle)
                        ? NormalizeNativeArcCreateAngle (beginAngle - axisAngle)
                        : beginAngle);
                }
                double endAngle = 0.0;
                if (payload->Get ("endAngle", endAngle)) {
                    mapped.Add ("endAngle", operation == "create" && std::isfinite (axisAngle) && std::isfinite (endAngle)
                        ? NormalizeNativeArcCreateAngle (endAngle - axisAngle)
                        : endAngle);
                }
                CopyBoolField (*payload, "reflected", mapped, "reflected");
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else if (typeName == "Hatch") {
                const GS::ObjectState* polygon = payload->Get ("polygon");
                if (polygon != nullptr)
                    CopyArrayField (*polygon, "coordinates", mapped, operation == "create" ? "hatchCoordinates" : "polygonOutline");
                CopyIntField (*payload, "fill", mapped, "fillAttributeIndex");
                CopyIntField (*payload, "contourPen", mapped, "contourPenIndex");
                CopyIntField (*payload, "foregroundPen", mapped, "fillPenIndex");
                CopyIntField (*payload, "backgroundPen", mapped, "fillBackgroundPenIndex");
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

constexpr const char* ZoneMemoWriterGate = "disposable-ac25-manual-polygon-v1";
constexpr const char* AutomaticZonePolygonProbeGate = "disposable-automatic-zone-polygon-v1";
constexpr const char* ZoneCandidateWriter = "zone-native-v1";
constexpr const char* ArcCandidateWriter = "arc-native-v1";
constexpr const char* HatchCandidateWriter = "hatch-native-v1";
constexpr const char* DoorCandidateWriter = "door-native-v1";
constexpr const char* WindowCandidateWriter = "window-native-v1";

bool HasHatchCandidateOnlyMetadata (const GS::ObjectState* semanticMetadata)
{
    if (semanticMetadata == nullptr) return false;
    for (const char* field : { "nativeCandidateWriter", "nativeCandidateCaseId", "nativeCandidateOperationId",
                               "nativeCandidateScopeKey", "nativeCandidateFields", "nativeCandidateGuids",
                               "nativeCrudTestFaultAfterFirstApplied" }) {
        if (semanticMetadata->Contains (field)) return true;
    }
    return false;
}

bool HasDoorCandidateMetadata (const GS::ObjectState* semanticMetadata)
{
    if (semanticMetadata == nullptr) return false;
    for (const char* field : { "nativeCandidateWriter", "nativeCandidateCaseId", "nativeCandidateOperationId",
                               "nativeCandidateScopeKey", "nativeCandidateFields", "nativeCandidateGuids",
                               "nativeCrudTestFaultAfterFirstApplied" })
        if (semanticMetadata->Contains (field)) return true;
    return false;
}

bool HasWindowCandidateMetadata (const GS::ObjectState* semanticMetadata)
{
    return HasDoorCandidateMetadata (semanticMetadata);
}

bool HasOnlyObjectStateFields (const GS::ObjectState& value, const std::initializer_list<const char*>& allowedFields)
{
    const GS::HashSet<GS::String> fieldNames = value.GetFieldNames ();
    for (const GS::String& fieldName : fieldNames) {
        bool allowed = false;
        for (const char* allowedField : allowedFields) {
            if (fieldName == GS::String (allowedField)) {
                allowed = true;
                break;
            }
        }
        if (!allowed)
            return false;
    }
    return true;
}

bool IsExactFinitePoint (const GS::ObjectState& point)
{
    const GS::HashSet<GS::String> fieldNames = point.GetFieldNames ();
    double x = 0.0;
    double y = 0.0;
    return fieldNames.GetSize () == 2 && point.Contains ("x") && point.Contains ("y")
        && point.Get ("x", x) && point.Get ("y", y)
        && std::isfinite (x) && std::isfinite (y);
}

GS::Optional<GS::UniString> ValidateZoneCandidatePlacement (const GS::ObjectState& shape)
{
    if (shape.Contains ("referencePosition"))
        return "Zone referencePosition is read-only in Native CRUD.";

    const GS::ObjectState* stampPlacement = shape.Get ("stampPlacement");
    if (stampPlacement == nullptr)
        return {};
    const GS::HashSet<GS::String> stampFields = stampPlacement->GetFieldNames ();
    if (!HasOnlyObjectStateFields (*stampPlacement, { "position", "angle", "fixedAngle" }) || stampFields.IsEmpty ())
        return "Zone stampPlacement accepts only position, angle and fixedAngle fields.";
    const GS::ObjectState* position = stampPlacement->Get ("position");
    if (position != nullptr && !IsExactFinitePoint (*position))
        return "Zone stampPlacement.position must contain exactly finite x and y coordinates.";
    if (stampPlacement->Contains ("angle")) {
        double angle = 0.0;
        if (!stampPlacement->Get ("angle", angle) || !std::isfinite (angle))
            return "Zone stampPlacement.angle must be a finite number.";
    }
    if (stampPlacement->Contains ("fixedAngle")) {
        bool fixedAngle = false;
        if (!stampPlacement->Get ("fixedAngle", fixedAngle))
            return "Zone stampPlacement.fixedAngle must be a boolean.";
    }
    return {};
}

GS::Optional<GS::UniString> ValidateZoneTypedWriter (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    if (operation != "create" && operation != "update")
        return "The validated Zone typed writer only supports CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return "The validated Zone typed writer requires a bounded non-empty item list.";

    const std::initializer_list<const char*> fields = {
        "name", "number", "category", "polygon", "stampPlacement"
    };
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* shape = nullptr;
        if (operation == "create") {
            shape = item.Get ("payload");
            if (shape == nullptr || !HasOnlyObjectStateFields (*shape, fields))
                return "Zone CREATE accepts only the declared typed Zone fields.";
            if (!shape->Contains ("polygon"))
                return "Zone CREATE requires a manual polygon.";
        } else {
            shape = item.Get ("patch");
            if (shape == nullptr || !HasOnlyObjectStateFields (*shape, fields))
                return "Zone UPDATE accepts only the declared typed Zone fields.";
            if (shape->GetFieldNames ().IsEmpty ())
                return "Zone UPDATE requires a non-empty typed patch.";
        }
        const GS::Optional<GS::UniString> placementError = ValidateZoneCandidatePlacement (*shape);
        if (placementError.HasValue ())
            return placementError.Get ();
    }
    return {};
}

bool HasZoneMemoWriterGate (const GS::ObjectState* semanticMetadata)
{
    // This candidate writer is deliberately compiled behind the disposable
    // AC25 identity.  A matching marker in an AC28 (or unknown) APX must not
    // turn a read-only Zone capability into a write path.
#if TAPIR_AC_VERSION != 25
    (void) semanticMetadata;
    return false;
#else
    GS::UniString gate;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("zoneMemoWriterGate", gate)
        && gate == ZoneMemoWriterGate;
#endif
}

bool HasAutomaticZonePolygonProbeGate (const GS::ObjectState* semanticMetadata)
{
#if TAPIR_AC_VERSION == 25 || TAPIR_AC_VERSION == 28
    GS::UniString gate;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("zoneMemoWriterGate", gate)
        && gate == AutomaticZonePolygonProbeGate;
#else
    (void) semanticMetadata;
    return false;
#endif
}

bool HasAnyZoneMemoWriterGate (const GS::ObjectState* semanticMetadata)
{
    return HasZoneMemoWriterGate (semanticMetadata) || HasAutomaticZonePolygonProbeGate (semanticMetadata);
}

bool HasZoneCandidateWriterGate (const GS::ObjectState* semanticMetadata)
{
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    GS::UniString writer;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("nativeCandidateWriter", writer)
        && writer == ZoneCandidateWriter;
#else
    (void) semanticMetadata;
    return false;
#endif
}

bool HasColumnCandidateWriterGate (const GS::ObjectState* semanticMetadata)
{
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    GS::UniString writer;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("nativeCandidateWriter", writer)
        && writer == "column-native-v1";
#else
    (void) semanticMetadata;
    return false;
#endif
}

bool HasZoneCandidateWriterMetadataMarker (const GS::ObjectState* semanticMetadata)
{
    GS::UniString writer;
    return semanticMetadata != nullptr && semanticMetadata->Get ("nativeCandidateWriter", writer);
}

GS::Optional<GS::UniString> ValidateZoneCandidateWriter (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& expectedScope,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
    if (!HasZoneCandidateWriterGate (semanticMetadata))
        return "Zone candidate writer is unavailable in this APX build.";
    if (operation != "create" && operation != "update")
        return "Zone candidate writer only supports CREATE and UPDATE; DELETE uses the ordinary exact delete route.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return "Zone candidate writer requires a bounded non-empty item list.";
    const GS::Optional<GS::UniString> typedError = ValidateZoneTypedWriter (operation, items);
    if (typedError.HasValue ())
        return typedError.Get ();
    GS::UniString candidateOperationId;
    GS::UniString candidateScope;
    GS::UniString candidateCaseId;
    GS::Array<GS::UniString> candidateFields;
    GS::Array<GS::UniString> candidateGuids;
    if (semanticMetadata == nullptr
        || !semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId)
        || candidateOperationId != operationId
        || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope)
        || candidateScope != expectedScope
        || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId)
        || candidateCaseId.IsEmpty ()
        || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
        || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
        return "Zone candidate writer request is not bound to its exact case, operation, disposable file, field set and target list.";
    GS::HashSet<GS::UniString> uniqueCandidateFields;
    for (const GS::UniString& field : candidateFields)
        uniqueCandidateFields.Add (field);
    GS::HashSet<GS::UniString> uniqueCandidateGuids;
    for (const GS::UniString& guid : candidateGuids)
        uniqueCandidateGuids.Add (guid.ToLowerCase ());
    if (uniqueCandidateFields.GetSize () != candidateFields.GetSize ()
        || uniqueCandidateGuids.GetSize () != candidateGuids.GetSize ())
        return "Zone candidate field and GUID bindings must contain unique exact values.";
    bool faultAfterFirstApplied = false;
    if (semanticMetadata != nullptr)
        semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied && (operation != "update" || items.GetSize () != 2))
        return "Zone first-applied fault test requires exactly two UPDATE items.";
#else
    if (faultAfterFirstApplied)
        return "Zone first-applied fault seam is unavailable in this APX build.";
#endif
    const std::initializer_list<const char*> fields = {
        "name", "number", "category", "polygon", "stampPlacement"
    };
    if (operation == "update" && candidateFields.GetSize () != 1)
        return "Zone candidate UPDATE requires exactly one declared field per operation.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* candidateShape = nullptr;
        if (operation == "create") {
            const GS::ObjectState* payload = item.Get ("payload");
            if (payload != nullptr && payload->Contains ("referencePosition"))
                return "Zone referencePosition is read-only in Native CRUD.";
            if (payload == nullptr || !HasOnlyObjectStateFields (*payload, fields))
                return "Zone candidate CREATE accepts only the declared typed Zone fields.";
            const bool hasPolygon = payload->Contains ("polygon");
            if (!hasPolygon)
                return "Zone candidate CREATE requires a manual polygon.";
            candidateShape = payload;
        } else {
            const GS::ObjectState* patch = item.Get ("patch");
            if (patch != nullptr && patch->Contains ("referencePosition"))
                return "Zone referencePosition is read-only in Native CRUD.";
            if (patch == nullptr || !HasOnlyObjectStateFields (*patch, fields))
                return "Zone candidate UPDATE accepts only the declared typed Zone fields.";
            if (patch->GetFieldNames ().IsEmpty ())
                return "Zone candidate UPDATE requires a non-empty typed patch.";
            candidateShape = patch;
        }
        if (candidateShape == nullptr || candidateShape->GetFieldNames ().GetSize () != candidateFields.GetSize ())
            return "Zone candidate metadata fields do not exactly match the typed mutation fields.";
        const GS::Optional<GS::UniString> placementError = ValidateZoneCandidatePlacement (*candidateShape);
        if (placementError.HasValue ())
            return placementError.Get ();
        for (const GS::String& field : candidateShape->GetFieldNames ()) {
            bool declared = false;
            for (const GS::UniString& candidateField : candidateFields)
                declared = declared || field == candidateField.ToCStr (CC_UTF8).Get ();
            if (!declared) return "Zone candidate metadata omits a changed field or declares a different field.";
        }
    }
    if (operation == "update") {
        if (candidateGuids.GetSize () != items.GetSize ())
            return "Zone candidate GUID authorization does not match the exact update target count.";
        GS::HashSet<GS::UniString> authorized;
        for (const GS::UniString& guid : candidateGuids) authorized.Add (guid.ToLowerCase ());
        GS::HashSet<GS::UniString> requested;
        for (const GS::ObjectState& item : items) {
            const GS::ObjectState* id = item.Get ("elementId");
            if (id == nullptr) return "Zone candidate UPDATE is missing an exact elementId.";
            const API_Guid guid = GetGuidFromObjectState (*id);
            if (guid == APINULLGuid) return "Zone candidate UPDATE contains an invalid elementId.";
            GS::UniString guidText;
            id->Get ("guid", guidText);
            requested.Add (guidText.ToLowerCase ());
        }
        if (requested.GetSize () != authorized.GetSize ())
            return "Zone candidate UPDATE GUIDs do not match its exact authorization set.";
        for (const GS::UniString& guid : requested)
            if (!authorized.Contains (guid)) return "Zone candidate UPDATE GUIDs do not match its exact authorization set.";
    } else if (candidateGuids.IsEmpty ()) {
        return "Zone candidate CREATE requires exact prequalified template GUIDs in its manifest binding.";
    }
    return {};
}

GS::Optional<GS::UniString> ValidateColumnCandidateWriter (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& expectedScope,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    (void) operation; (void) operationId; (void) expectedScope; (void) items; (void) semanticMetadata;
    return "Column candidate writer is unavailable in this APX build.";
#else
    GS::UniString writer;
    if (semanticMetadata == nullptr || !semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != "column-native-v1")
        return "Column candidate writer marker is invalid.";
    if (operation != "create" && operation != "update")
        return "Column candidate writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return "Column candidate writer requires a bounded non-empty item list.";

    GS::UniString candidateOperationId, candidateScope, candidateCaseId;
    GS::Array<GS::UniString> candidateFields, candidateGuids;
    if (!semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId)
        || candidateOperationId != operationId
        || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope)
        || candidateScope != expectedScope
        || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId)
        || candidateCaseId.IsEmpty ()
        || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
        || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids)
        || candidateFields.IsEmpty () || candidateGuids.IsEmpty ())
        return "Column candidate request is not bound to its exact case, operation, disposable file, fields and GUIDs.";

    const std::initializer_list<const char*> createFields = { "coordinates", "height", "rotation", "dimensions" };
    const std::initializer_list<const char*> updateFields = { "origin", "offset", "height", "rotation", "dimensions" };
    const std::initializer_list<const char*> expectedFields = operation == "create" ? createFields : updateFields;
    const std::initializer_list<const char*> forbiddenCreate = { "origin", "offset", "bottomOffset", "elevation", "slant", "taper", "profile", "material", "section" };
    const std::initializer_list<const char*> forbiddenUpdate = { "coordinates", "storyIndex", "bottomOffset", "elevation", "slant", "taper", "profile", "material", "section" };
    const auto& forbidden = operation == "create" ? forbiddenCreate : forbiddenUpdate;
    GS::HashSet<GS::UniString> uniqueFields;
    for (const GS::UniString& field : candidateFields) uniqueFields.Add (field);
    if (uniqueFields.GetSize () != candidateFields.GetSize ()) return "Column candidate fields must be unique.";
    if (operation == "create" && candidateFields.GetSize () != 4)
        return "Column candidate CREATE requires coordinates, height, rotation and dimensions; story placement is separate.";
    for (const GS::UniString& field : candidateFields) {
        bool allowed = false;
        for (const char* expected : expectedFields) allowed = allowed || field == expected;
        for (const char* rejected : forbidden) if (field == rejected) return "Column candidate contains an explicitly unsupported field.";
        if (!allowed) return "Column candidate field list exceeds the exact operation vocabulary.";
    }
    if (operation == "create") {
        if (items.GetSize () != 1 || candidateGuids.GetSize () != 1)
            return "Column candidate CREATE requires one payload and one exact same-file template GUID.";
        GS::ObjectState templateSection;
        if (!semanticMetadata->Get ("nativeCandidateTemplateSection", templateSection))
            return "Column candidate CREATE requires factual section readback for its exact qualified template.";
        const API_Guid templateGuid = APIGuidFromString (candidateGuids[0].ToCStr ());
        API_Element templateElement = {};
        templateElement.header.guid = templateGuid;
        if (templateGuid == APINULLGuid || ACAPI_Element_Get (&templateElement) != NoError || GetElemTypeId (templateElement.header) != API_ColumnID)
            return "Column candidate CREATE template must be an existing exact Column in this project.";
    }
    bool faultAfterFirstApplied = false;
    semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
    if (faultAfterFirstApplied && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
        return "Column first-applied rollback test requires exactly two UPDATE targets.";
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied) return "Column first-applied rollback fault seam is unavailable in this APX build.";
#endif
    GS::HashSet<GS::UniString> uniqueGuids;
    for (const GS::UniString& guidText : candidateGuids) {
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        if (guid == APINULLGuid || uniqueGuids.Contains (guidText.ToLowerCase ())) return "Column candidate GUIDs must be valid and unique.";
        uniqueGuids.Add (guidText.ToLowerCase ());
    }
    if (operation == "update") {
        if (candidateGuids.GetSize () != items.GetSize ()) return "Column candidate UPDATE GUID binding does not match its exact item count.";
        const GSSize candidateGuidCount = static_cast<GSSize> (candidateGuids.GetSize ());
        for (GSSize index = 0; index < candidateGuidCount; ++index) {
            const GS::ObjectState* id = items[index].Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) != APIGuidFromString (candidateGuids[index].ToCStr ()))
                return "Column candidate UPDATE GUIDs differ from their exact authorization order.";
        }
    }
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* shape = item.Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr || !HasOnlyObjectStateFields (*shape, expectedFields)) return "Column candidate payload contains unsupported or missing operation fields.";
        if (shape->GetFieldNames ().GetSize () != candidateFields.GetSize ()) return "Column candidate payload fields differ from the exact authorization list.";
        for (const GS::String& fieldName : shape->GetFieldNames ()) {
            bool declared = false;
            const GS::UniString fieldNameUniString (fieldName);
            for (const GS::UniString& field : candidateFields) declared = declared || field == fieldNameUniString;
            if (!declared) return "Column candidate payload changes a field outside the exact authorization list.";
        }
    }
    return {};
#endif
}

GS::Optional<GS::UniString> ValidateLineCandidateWriter (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& expectedScope,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    (void) operation; (void) operationId; (void) expectedScope; (void) items; (void) semanticMetadata;
    return "Line candidate writer is unavailable in this APX build.";
#else
    GS::UniString writer;
    if (semanticMetadata == nullptr || !semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != "line-native-v1")
        return "Line candidate writer marker is invalid.";
    if (operation != "create" && operation != "update") return "Line candidate writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) return "Line candidate writer requires a bounded non-empty item list.";

    GS::UniString candidateOperationId, candidateScope, candidateCaseId;
    GS::Array<GS::UniString> candidateFields, candidateGuids;
    if (!semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId) || candidateOperationId != operationId
        || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope) || candidateScope != expectedScope
        || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId) || candidateCaseId.IsEmpty ()
        || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
        || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
        return "Line candidate request is not bound to its exact case, operation, disposable file, fields and GUIDs.";

    const std::initializer_list<const char*> createFields = { "begin", "end", "lineType", "pen", "roomSeparator" };
    const std::initializer_list<const char*> updateFields = { "begin", "end", "lineType", "pen", "roomSeparator" };
    const auto& allowedFields = operation == "create" ? createFields : updateFields;
    GS::HashSet<GS::UniString> uniqueFields;
    for (const GS::UniString& field : candidateFields) {
        uniqueFields.Add (field);
        bool allowed = false;
        for (const char* candidate : allowedFields) allowed = allowed || field == candidate;
        if (!allowed) return "Line candidate fields exceed its exact typed writer vocabulary.";
    }
    if (candidateFields.IsEmpty () || uniqueFields.GetSize () != candidateFields.GetSize ())
        return "Line candidate fields must be non-empty and unique.";
    if (operation == "create" && (items.GetSize () != 1 || candidateGuids.GetSize () != 1))
        return "Line candidate CREATE requires one exact payload and one qualified same-file Line authorization anchor.";
    if (operation == "update" && (candidateGuids.GetSize () != items.GetSize () || candidateGuids.IsEmpty ()))
        return "Line candidate UPDATE GUID binding must match its exact target count.";
    bool faultAfterFirstApplied = false;
    semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
    if (faultAfterFirstApplied && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
        return "Line first-applied rollback test requires exactly two UPDATE targets.";
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied) return "Line first-applied rollback fault seam is unavailable in this APX build.";
#endif

    GS::HashSet<GS::UniString> uniqueGuids;
    for (const GS::UniString& guidText : candidateGuids) {
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        if (guid == APINULLGuid || uniqueGuids.Contains (guidText.ToLowerCase ())) return "Line candidate GUIDs must be valid and unique.";
        uniqueGuids.Add (guidText.ToLowerCase ());
    }
    if (operation == "create") {
        const API_Guid templateGuid = APIGuidFromString (candidateGuids[0].ToCStr ());
        API_Element templateElement = {};
        templateElement.header.guid = templateGuid;
        if (ACAPI_Element_Get (&templateElement) != NoError || GetElemTypeId (templateElement.header) != API_LineID)
            return "Line candidate CREATE authorization anchor must be an existing exact Line in this project.";
    }
    const GSSize itemCount = static_cast<GSSize> (items.GetSize ());
    for (GSSize index = 0; index < itemCount; ++index) {
        const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr || !HasOnlyObjectStateFields (*shape, allowedFields) || shape->GetFieldNames ().GetSize () != candidateFields.GetSize ())
            return "Line candidate payload contains unsupported, missing or unauthorized fields.";
        if (operation == "create" && (!shape->Contains ("begin") || !shape->Contains ("end")))
            return "Line candidate CREATE requires both endpoints.";
        for (const GS::String& fieldName : shape->GetFieldNames ()) {
            bool declared = false;
            for (const GS::UniString& field : candidateFields) declared = declared || field == GS::UniString (fieldName);
            if (!declared) return "Line candidate payload differs from its exact field authorization.";
        }
        for (const char* endpoint : { "begin", "end" }) {
            const GS::ObjectState* point = shape->Get (endpoint);
            if (point == nullptr) continue;
            double x = 0.0, y = 0.0;
            if (!HasOnlyObjectStateFields (*point, { "x", "y" }) || !point->Get ("x", x) || !point->Get ("y", y) || !std::isfinite (x) || !std::isfinite (y))
                return "Line candidate endpoints must contain finite x and y coordinates only.";
        }
        const GS::ObjectState* begin = shape->Get ("begin");
        const GS::ObjectState* end = shape->Get ("end");
        if (begin != nullptr && end != nullptr) {
            double beginX = 0.0, beginY = 0.0, endX = 0.0, endY = 0.0;
            begin->Get ("x", beginX); begin->Get ("y", beginY); end->Get ("x", endX); end->Get ("y", endY);
            if (beginX == endX && beginY == endY) return "Line candidate endpoints must not be identical.";
        }
        Int32 lineType = 0, pen = 0;
        if (shape->Contains ("lineType") && (!shape->Get ("lineType", lineType) || lineType < 0)) return "Line candidate lineType must be a non-negative integer.";
        if (shape->Contains ("pen") && (!shape->Get ("pen", pen) || pen < 0)) return "Line candidate pen must be a non-negative integer.";
        bool roomSeparator = false;
        if (shape->Contains ("roomSeparator") && !shape->Get ("roomSeparator", roomSeparator)) return "Line candidate roomSeparator must be boolean.";
        if (operation == "update") {
            const GS::ObjectState* id = items[index].Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) != APIGuidFromString (candidateGuids[index].ToCStr ()))
                return "Line candidate UPDATE targets differ from their exact authorization order.";
        }
    }
    return {};
#endif
}

GS::Optional<GS::UniString> ValidateArcTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items,
    const GS::Array<GS::UniString>* authorizedFields,
    const GS::Array<GS::UniString>* authorizedGuids)
{
    const std::initializer_list<const char*> allowedFields = {
        "origin", "radius", "ratio", "axisAngle", "beginAngle", "endAngle", "reflected", "lineType", "pen", "roomSeparator"
    };
    for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
        const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr || shape->GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (*shape, allowedFields))
            return "Arc typed payload must contain a non-empty patch with only declared Arc fields.";
        if (operation == "create" && shape->GetFieldNames ().GetSize () != 10)
            return "Arc CREATE must provide all ten typed element fields; placement is separate.";
        if (authorizedFields != nullptr) {
            if (shape->GetFieldNames ().GetSize () != authorizedFields->GetSize ())
                return "Arc candidate payload must contain exactly its authorized typed fields.";
            for (const GS::String& name : shape->GetFieldNames ()) {
                bool declared = false;
                for (const GS::UniString& field : *authorizedFields) declared = declared || field == GS::UniString (name);
                if (!declared) return "Arc candidate payload changes a field outside its exact authorization.";
            }
        }
        const GS::ObjectState* origin = shape->Get ("origin");
        if (origin != nullptr) {
            double x = 0.0, y = 0.0;
            if (!HasOnlyObjectStateFields (*origin, { "x", "y" }) || !origin->Get ("x", x) || !origin->Get ("y", y) || !std::isfinite (x) || !std::isfinite (y))
                return "Arc origin must contain finite x and y coordinates only.";
        }
        double radius = 0.0, ratio = 0.0, axisAngle = 0.0, beginAngle = 0.0, endAngle = 0.0;
        if (shape->Contains ("radius") && (!shape->Get ("radius", radius) || !std::isfinite (radius) || radius <= 0.0)) return "Arc radius must be finite and positive.";
        if (shape->Contains ("ratio") && (!shape->Get ("ratio", ratio) || !std::isfinite (ratio) || ratio <= 0.0)) return "Arc ratio must be finite and positive.";
        if (shape->Contains ("axisAngle") && (!shape->Get ("axisAngle", axisAngle) || !std::isfinite (axisAngle))) return "Arc axisAngle must be finite.";
        if (shape->Contains ("beginAngle") && (!shape->Get ("beginAngle", beginAngle) || !std::isfinite (beginAngle))) return "Arc beginAngle must be finite.";
        if (shape->Contains ("endAngle") && (!shape->Get ("endAngle", endAngle) || !std::isfinite (endAngle))) return "Arc endAngle must be finite.";
        if (operation == "create" && beginAngle == endAngle) return "Arc CREATE requires a non-full open arc.";
        Int32 lineType = 0, pen = 0;
        if (shape->Contains ("lineType") && (!shape->Get ("lineType", lineType) || lineType <= 0)) return "Arc lineType must be positive.";
        if (shape->Contains ("pen") && (!shape->Get ("pen", pen) || pen <= 0)) return "Arc pen must be positive.";
        bool booleanValue = false;
        if (shape->Contains ("reflected") && !shape->Get ("reflected", booleanValue)) return "Arc reflected must be boolean.";
        if (shape->Contains ("roomSeparator") && !shape->Get ("roomSeparator", booleanValue)) return "Arc roomSeparator must be boolean.";
        if (authorizedGuids != nullptr && operation == "update") {
            const GS::ObjectState* id = items[index].Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) != APIGuidFromString ((*authorizedGuids)[index].ToCStr ()))
                return "Arc candidate UPDATE targets differ from their exact authorization order.";
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateArcCandidateWriter (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& expectedScope,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    (void) operation; (void) operationId; (void) expectedScope; (void) items; (void) semanticMetadata;
    return "Arc candidate writer is unavailable in this APX build.";
#else
    GS::UniString writer;
    if (semanticMetadata == nullptr || !semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != ArcCandidateWriter)
        return "Arc candidate writer marker is invalid.";
    if (operation != "create" && operation != "update") return "Arc candidate writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > 2) return "Arc candidate writer is bounded to one or two items.";

    GS::UniString candidateOperationId, candidateScope, candidateCaseId;
    GS::Array<GS::UniString> candidateFields, candidateGuids;
    if (!semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId) || candidateOperationId != operationId
        || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope) || candidateScope != expectedScope
        || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId) || candidateCaseId.IsEmpty ()
        || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
        || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
        return "Arc candidate request is not bound to its exact case, operation, disposable file, fields and GUIDs.";

    const std::initializer_list<const char*> allowedFields = {
        "origin", "radius", "ratio", "axisAngle", "beginAngle", "endAngle", "reflected", "lineType", "pen", "roomSeparator"
    };
    GS::HashSet<GS::UniString> uniqueFields;
    for (const GS::UniString& field : candidateFields) {
        bool allowed = false;
        for (const char* candidate : allowedFields) allowed = allowed || field == candidate;
        if (!allowed || uniqueFields.Contains (field)) return "Arc candidate fields must be unique members of the exact typed writer vocabulary.";
        uniqueFields.Add (field);
    }
    if (candidateFields.IsEmpty ()) return "Arc candidate field authorization must not be empty.";
    if (operation == "create" && (items.GetSize () != 1 || candidateGuids.GetSize () != 1 || candidateFields.GetSize () != 10))
        return "Arc candidate CREATE requires one payload, one same-file Arc authorization anchor and ten element fields; story placement is separate.";
    if (operation == "update" && (candidateGuids.GetSize () != items.GetSize () || candidateGuids.IsEmpty ()))
        return "Arc candidate UPDATE GUID binding must match its exact target count.";

    bool faultAfterFirstApplied = false;
    semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
    if (faultAfterFirstApplied && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
        return "Arc first-applied rollback test requires exactly two UPDATE targets.";
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied) return "Arc first-applied rollback fault seam is unavailable in this APX build.";
#endif

    GS::HashSet<GS::UniString> uniqueGuids;
    for (const GS::UniString& guidText : candidateGuids) {
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        if (guid == APINULLGuid || uniqueGuids.Contains (guidText.ToLowerCase ())) return "Arc candidate GUIDs must be valid and unique.";
        uniqueGuids.Add (guidText.ToLowerCase ());
    }
    if (operation == "create") {
        API_Element templateElement = {};
        templateElement.header.guid = APIGuidFromString (candidateGuids[0].ToCStr ());
        if (ACAPI_Element_Get (&templateElement) != NoError || GetElemTypeId (templateElement.header) != API_ArcID)
            return "Arc candidate CREATE authorization anchor must be an existing exact Arc in this project.";
    }
    return ValidateArcTypedPayloads (operation, items, &candidateFields, &candidateGuids);
#endif
}

bool HatchPointOnSegment (const API_Coord& point, const API_Coord& begin, const API_Coord& end)
{
    return point.x >= std::min (begin.x, end.x) && point.x <= std::max (begin.x, end.x)
        && point.y >= std::min (begin.y, end.y) && point.y <= std::max (begin.y, end.y);
}

double HatchOrientation (const API_Coord& first, const API_Coord& second, const API_Coord& third)
{
    return (second.x - first.x) * (third.y - first.y) - (second.y - first.y) * (third.x - first.x);
}

bool HatchSegmentsIntersect (const API_Coord& a, const API_Coord& b, const API_Coord& c, const API_Coord& d)
{
    const double o1 = HatchOrientation (a, b, c);
    const double o2 = HatchOrientation (a, b, d);
    const double o3 = HatchOrientation (c, d, a);
    const double o4 = HatchOrientation (c, d, b);
    if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0))
        && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0))) return true;
    return (o1 == 0.0 && HatchPointOnSegment (c, a, b)) || (o2 == 0.0 && HatchPointOnSegment (d, a, b))
        || (o3 == 0.0 && HatchPointOnSegment (a, c, d)) || (o4 == 0.0 && HatchPointOnSegment (b, c, d));
}

GS::Optional<GS::UniString> ValidateHatchTypedShape (const GS::ObjectState& shape, const GS::UniString& operation)
{
    const std::initializer_list<const char*> allowedFields = { "polygon", "fill", "contourPen", "foregroundPen", "backgroundPen" };
    if (shape.GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (shape, allowedFields))
        return "Hatch payload must contain only its typed simple-polygon and style fields.";
    if (operation == "create" && shape.GetFieldNames ().GetSize () != 5)
        return "Hatch CREATE requires polygon and all four style fields; story placement is separate.";
    const GS::ObjectState* polygon = shape.Get ("polygon");
    if (polygon != nullptr) {
        if (!HasOnlyObjectStateFields (*polygon, { "coordinates" })) return "Hatch polygon accepts only one external coordinates ring; holes and arcs are unsupported.";
        GS::Array<GS::ObjectState> points;
        if (!polygon->Get ("coordinates", points) || points.GetSize () < 3 || points.GetSize () > 64)
            return "Hatch polygon requires between 3 and 64 unclosed vertices.";
        std::vector<API_Coord> ring;
        ring.reserve (static_cast<GSSize> (points.GetSize ()));
        for (const GS::ObjectState& point : points) {
            if (!IsExactFinitePoint (point)) return "Hatch vertices must contain finite x and y only.";
            ring.push_back (Get2DCoordinateFromObjectState (point));
        }
        double twiceArea = 0.0;
        for (GSSize index = 0; index < ring.size (); ++index) {
            const API_Coord& point = ring[index];
            const API_Coord& next = ring[(index + 1) % ring.size ()];
            if (IsSame2DCoordinate (point, next)) return "Hatch polygon cannot contain a zero-length edge or duplicated closing vertex.";
            for (GSSize prior = 0; prior < index; ++prior)
                if (IsSame2DCoordinate (point, ring[prior])) return "Hatch polygon vertices must be unique.";
            twiceArea += point.x * next.y - next.x * point.y;
        }
        if (!std::isfinite (twiceArea) || twiceArea == 0.0) return "Hatch polygon must have finite nonzero area.";
        for (GSSize first = 0; first < ring.size (); ++first) {
            const GSSize firstNext = (first + 1) % ring.size ();
            for (GSSize second = first + 1; second < ring.size (); ++second) {
                const GSSize secondNext = (second + 1) % ring.size ();
                if (first == second || firstNext == second || secondNext == first) continue;
                if (HatchSegmentsIntersect (ring[first], ring[firstNext], ring[second], ring[secondNext]))
                    return "Hatch polygon must be a simple non-self-intersecting ring.";
            }
        }
    }
    for (const char* fieldName : { "fill", "contourPen", "foregroundPen", "backgroundPen" }) {
        if (!shape.Contains (fieldName)) {
            if (operation == "create") return GS::UniString::Printf ("Hatch CREATE is missing '%s'.", fieldName);
            continue;
        }
        Int32 value = 0;
        if (!shape.Get (fieldName, value) || value <= 0) return GS::UniString::Printf ("Hatch '%s' must be a positive integer index.", fieldName);
    }
    if (operation == "create" && polygon == nullptr) return "Hatch CREATE requires a polygon.";
    return {};
}

GS::Optional<GS::UniString> ValidateHatchTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    if (operation != "create" && operation != "update") return "Hatch typed writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) return "Hatch typed writer requires one to twenty-five items.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* shape = item.Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr) return "Hatch item is missing its typed payload or patch.";
        const GS::Optional<GS::UniString> shapeError = ValidateHatchTypedShape (*shape, operation);
        if (shapeError.HasValue ()) return shapeError;
        if (operation == "create") {
            const GS::ObjectState* placement = item.Get ("placement");
            Int32 storyIndex = 0;
            if (placement == nullptr || !HasOnlyObjectStateFields (*placement, { "storyIndex" }) || !placement->Get ("storyIndex", storyIndex))
                return "Hatch CREATE requires exact separate placement.storyIndex.";
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateHatchCandidateWriter (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& expectedScope,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    (void) operation; (void) operationId; (void) expectedScope; (void) items; (void) semanticMetadata;
    return "Hatch candidate writer is unavailable in this ordinary APX build.";
#else
    GS::UniString writer;
    if (semanticMetadata == nullptr || !semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != HatchCandidateWriter)
        return "Hatch candidate writer marker is invalid.";
    if (operation != "create" && operation != "update") return "Hatch candidate writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > 2) return "Hatch candidate writer is bounded to one or two items.";

    GS::UniString candidateOperationId, candidateScope, candidateCaseId;
    GS::Array<GS::UniString> candidateFields, candidateGuids;
    if (!semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId) || candidateOperationId != operationId
        || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope) || candidateScope != expectedScope
        || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId) || candidateCaseId.IsEmpty ()
        || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
        || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
        return "Hatch candidate request is not bound to its exact case, operation, disposable file, fields and GUIDs.";

    const std::initializer_list<const char*> allowedFields = { "polygon", "fill", "contourPen", "foregroundPen", "backgroundPen" };
    GS::HashSet<GS::UniString> uniqueFields;
    for (const GS::UniString& field : candidateFields) {
        bool allowed = false;
        for (const char* candidate : allowedFields) allowed = allowed || field == candidate;
        if (!allowed || uniqueFields.Contains (field)) return "Hatch candidate fields must be unique members of the exact typed writer vocabulary.";
        uniqueFields.Add (field);
    }
    if (candidateFields.IsEmpty ()) return "Hatch candidate field authorization must not be empty.";
    if (operation == "create" && (items.GetSize () != 1 || candidateGuids.GetSize () != 1 || candidateFields.GetSize () != 5))
        return "Hatch candidate CREATE requires one payload, one same-file Hatch authorization anchor and five element fields.";
    if (operation == "update" && (candidateGuids.GetSize () != items.GetSize () || candidateGuids.IsEmpty ()))
        return "Hatch candidate UPDATE GUID binding must match its exact target count.";

    bool faultAfterFirstApplied = false;
    semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
    if (faultAfterFirstApplied && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
        return "Hatch first-applied rollback test requires exactly two UPDATE targets.";
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied) return "Hatch first-applied rollback fault seam is unavailable in this APX build.";
#endif

    GS::HashSet<GS::UniString> uniqueGuids;
    for (const GS::UniString& guidText : candidateGuids) {
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        if (guid == APINULLGuid || uniqueGuids.Contains (guidText.ToLowerCase ())) return "Hatch candidate GUIDs must be valid and unique.";
        uniqueGuids.Add (guidText.ToLowerCase ());
    }
    if (operation == "create") {
        API_Element templateElement = {};
        templateElement.header.guid = APIGuidFromString (candidateGuids[0].ToCStr ());
        if (ACAPI_Element_Get (&templateElement) != NoError || GetElemTypeId (templateElement.header) != API_HatchID
            || templateElement.hatch.hatchType != API_FillHatch || templateElement.hatch.determination != APIHatch_DraftingFills)
            return "Hatch candidate CREATE authorization anchor must be an existing drafting Fill Hatch in this project.";
    }
    for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
        const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr) return "Hatch candidate item is missing its exact payload or patch.";
        const GS::Optional<GS::UniString> shapeError = ValidateHatchTypedShape (*shape, operation);
        if (shapeError.HasValue ()) return shapeError;
        if (shape->GetFieldNames ().GetSize () != candidateFields.GetSize ()) return "Hatch candidate payload fields do not match the exact authorization list.";
        for (const GS::String& field : shape->GetFieldNames ()) {
            bool authorized = false;
            for (const GS::UniString& candidate : candidateFields) authorized = authorized || candidate == GS::UniString (field);
            if (!authorized) return "Hatch candidate payload changes a field outside its exact authorization.";
        }
        if (operation == "create") {
            const GS::ObjectState* placement = items[index].Get ("placement");
            Int32 storyIndex = 0;
            if (placement == nullptr || !HasOnlyObjectStateFields (*placement, { "storyIndex" }) || !placement->Get ("storyIndex", storyIndex))
                return "Hatch candidate CREATE requires exact separate placement.storyIndex.";
        } else {
            const GS::ObjectState* id = items[index].Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) != APIGuidFromString (candidateGuids[index].ToCStr ()))
                return "Hatch candidate UPDATE targets differ from their exact authorization order.";
            API_Element target = {};
            target.header.guid = GetGuidFromObjectState (*id);
            if (ACAPI_Element_Get (&target) != NoError || GetElemTypeId (target.header) != API_HatchID)
                return "Hatch candidate UPDATE target must be an existing exact Hatch GUID.";
        }
    }
    return {};
#endif
}

GS::Optional<GS::UniString> ValidateZoneMemoWriterCandidate (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
    if (!HasAnyZoneMemoWriterGate (semanticMetadata))
        return {};
    if (operation != "update")
        return "The disposable Zone memo writer only supports UPDATE.";
    if (items.GetSize () != 1)
        return "The disposable Zone memo writer accepts exactly one Zone item.";

    if (items[0].Contains ("payload"))
        return "The disposable Zone memo writer rejects payload; use a polygon-only patch.";
    if (!HasOnlyObjectStateFields (items[0], { "elementId", "expectedFingerprint", "patch" }))
        return "The disposable Zone memo writer accepts only elementId, expectedFingerprint and patch fields.";
    const GS::ObjectState* patch = items[0].Get ("patch");
    if (patch == nullptr || !HasOnlyObjectStateFields (*patch, { "polygon" }))
        return "The disposable Zone memo writer accepts a polygon-only patch.";
    const GS::ObjectState* polygon = patch->Get ("polygon");
    if (polygon == nullptr || !HasOnlyObjectStateFields (*polygon, { "coordinates", "arcs", "holes" }))
        return "The disposable Zone memo writer accepts only polygon coordinates, arcs and holes fields.";

    GS::Array<GS::ObjectState> coordinates;
    if (!polygon->Get ("coordinates", coordinates))
        return "The disposable Zone memo writer requires polygon.coordinates.";
    GS::Array<GS::ObjectState> arcs;
    if (polygon->Contains ("arcs") && !polygon->Get ("arcs", arcs))
        return "The disposable Zone memo writer requires polygon.arcs to be an array.";
    GS::Array<GS::ObjectState> holes;
    if (polygon->Contains ("holes") && !polygon->Get ("holes", holes))
        return "The disposable Zone memo writer requires polygon.holes to be an array.";
    if (!arcs.IsEmpty ())
        return "The disposable Zone memo writer rejects polygon arcs.";
    if (!holes.IsEmpty ())
        return "The disposable Zone memo writer rejects polygon holes.";
    return {};
}

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

GS::Optional<GS::UniString> ValidateDoorTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    const bool create = operation == "create";
    if (!create && operation != "update") return "Door typed writer supports only CREATE and UPDATE.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* payload = item.Get (create ? "payload" : "patch");
        if (payload == nullptr) return "Door typed writer requires its exact payload or sparse patch.";
        const GS::HashSet<GS::String> names = payload->GetFieldNames ();
        if (names.IsEmpty ()) return "Door typed writer payload/patch must not be empty.";
        if (create
            ? !HasOnlyObjectStateFields (*payload, { "ownerWallId", "offset", "sill", "width", "height", "orientation" })
            : !HasOnlyObjectStateFields (*payload, { "offset", "sill", "width", "height", "orientation" }))
            return "Door typed writer received a field outside its declared contract.";
        if (create && names.GetSize () != 6)
            return "Door CREATE requires ownerWallId, offset, sill, width, height and orientation.";
        for (const char* field : { "offset", "sill", "width", "height" }) {
            if (!payload->Contains (field)) continue;
            double value = 0.0;
            if (!payload->Get (field, value) || !std::isfinite (value)
                || ((GS::UniString (field) == "width" || GS::UniString (field) == "height") && value <= 0.0))
                return GS::UniString::Printf ("Door '%s' must be a finite%s number.", field,
                    (GS::UniString (field) == "width" || GS::UniString (field) == "height") ? " positive" : "");
        }
        if (create) {
            const GS::ObjectState* owner = payload->Get ("ownerWallId");
            if (owner == nullptr || GetGuidFromObjectState (*owner) == APINULLGuid)
                return "Door CREATE requires an exact ownerWallId GUID.";
        }
        const GS::ObjectState* orientation = payload->Get ("orientation");
        if (orientation != nullptr) {
            const GS::HashSet<GS::String> orientationNames = orientation->GetFieldNames ();
            if (orientationNames.IsEmpty () || !HasOnlyObjectStateFields (*orientation, { "reflected", "refSide", "oSide" }))
                return "Door orientation accepts one or more of reflected, refSide and oSide.";
            for (const GS::String& name : orientationNames) {
                bool value = false;
                if (!orientation->Get (name, value)) return "Door orientation values must be boolean.";
            }
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateWindowTypedPayloads (const GS::UniString& operation, const GS::Array<GS::ObjectState>& items)
{
    const bool create = operation == "create";
    if (!create && operation != "update") return "Window typed writer supports only CREATE and UPDATE.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* payload = item.Get (create ? "payload" : "patch");
        if (payload == nullptr || payload->GetFieldNames ().IsEmpty ()) return "Window typed writer requires a non-empty exact payload or sparse patch.";
        if (create ? !HasOnlyObjectStateFields (*payload, { "ownerWallId", "offset", "sill", "width", "height", "orientation" })
                   : !HasOnlyObjectStateFields (*payload, { "offset", "sill", "width", "height", "orientation" }))
            return "Window typed writer received a field outside its declared contract.";
        if (create && payload->GetFieldNames ().GetSize () != 6) return "Window CREATE requires ownerWallId, offset, sill, width, height and orientation.";
        for (const char* field : { "offset", "sill", "width", "height" }) {
            if (!payload->Contains (field)) continue;
            double value = 0.0;
            if (!payload->Get (field, value) || !std::isfinite (value)
                || ((GS::UniString (field) == "width" || GS::UniString (field) == "height") && value <= 0.0))
                return GS::UniString::Printf ("Window '%s' must be a finite%s number.", field,
                    (GS::UniString (field) == "width" || GS::UniString (field) == "height") ? " positive" : "");
        }
        if (create) {
            const GS::ObjectState* owner = payload->Get ("ownerWallId");
            if (owner == nullptr || GetGuidFromObjectState (*owner) == APINULLGuid) return "Window CREATE requires an exact ownerWallId GUID.";
        }
        const GS::ObjectState* orientation = payload->Get ("orientation");
        if (orientation != nullptr) {
            if (orientation->GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (*orientation, { "reflected", "refSide", "oSide" })) return "Window orientation accepts reflected, refSide and oSide only.";
            for (const GS::String& name : orientation->GetFieldNames ()) { bool value = false; if (!orientation->Get (name, value)) return "Window orientation values must be boolean."; }
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateDoorCloneSource (const API_Guid& sourceGuid)
{
    if (sourceGuid == APINULLGuid) return "Door clone source GUID is missing.";
    API_Element source = {};
    source.header.guid = sourceGuid;
    if (ACAPI_Element_Get (&source) != NoError || GetElemTypeId (source.header) != API_DoorID || source.header.guid != sourceGuid)
        return "Door clone source must resolve to the exact same-file Door element.";
    if (source.door.openingBase.libInd <= 0)
        return "Door clone source has no valid same-file opening library part.";
    API_LibPart doorPart = {};
    LibraryPartLocationGuard doorPartLocationGuard (doorPart);
    doorPart.index = source.door.openingBase.libInd;
    if (ACAPI_LibraryPart_Get (&doorPart) != NoError || doorPart.missingDef || doorPart.version <= 0
        || doorPart.ownUnID[0] == '\0' || doorPart.file_UName[0] == '\0')
        return "Door clone source opening library identity/revision is incomplete.";
    const API_Guid stampGuid = source.door.openingBase.markGuid;
    if (stampGuid == APINULLGuid) return {};
    // Marker-bearing copying is supplemental under D-272. Non-null identities,
    // including identities that might be unresolved, fail closed here.
    return "Door clone source is marker-bearing; this candidate packet accepts only factual markerless templates.";
}

bool WindowCandidateNativeTopologySupported (const API_Element& element)
{
    return GetElemTypeId (element.header) == API_WindowID
        && element.window.openingBase.libInd > 0
        && element.window.fixPoint == APIHoleAnchor_CenterFix
        && element.window.openingBase.verticalLink.linkType == API_LinkSillToWallBottom
        && element.window.directionType == API_WDAssociativeToWall;
}

GS::Optional<GS::UniString> ValidateWindowCloneSource (const API_Guid& sourceGuid)
{
    if (sourceGuid == APINULLGuid) return "Window clone source GUID is missing.";
    API_Element source = {};
    source.header.guid = sourceGuid;
    if (ACAPI_Element_Get (&source) != NoError || GetElemTypeId (source.header) != API_WindowID || source.header.guid != sourceGuid)
        return "Window clone source must resolve to the exact same-file native Window.";
    if (source.window.openingBase.markGuid != APINULLGuid) return "Window clone source is marker-bearing; only factual markerless templates are admitted.";
    if (!WindowCandidateNativeTopologySupported (source))
        return "Window clone source is outside the directly observable centered, sill-linked, wall-associative native subset.";
    API_LibPart part = {};
    LibraryPartLocationGuard partGuard (part);
    part.index = source.window.openingBase.libInd;
    if (ACAPI_LibraryPart_Get (&part) != NoError || part.missingDef || part.version <= 0 || part.ownUnID[0] == '\0' || part.file_UName[0] == '\0')
        return "Window clone source library identity/revision is incomplete.";
    return {};
}

GS::ObjectState DispatchMutation (
    const GS::UniString& operation,
    const GS::UniString& operationId,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl,
    const GS::ObjectState* semanticMetadata)
{
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    (void) operationId;
#endif
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (typeName);
    GS::UniString candidateMarker;
    const bool hasCandidateMarker = semanticMetadata != nullptr && semanticMetadata->Get ("nativeCandidateWriter", candidateMarker);
    if (hasCandidateMarker && candidateMarker != "zone-native-v1" && candidateMarker != "column-native-v1" && candidateMarker != "line-native-v1" && candidateMarker != ArcCandidateWriter && candidateMarker != HatchCandidateWriter && candidateMarker != DoorCandidateWriter && candidateMarker != WindowCandidateWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Unsupported native candidate writer marker.");
    if (hasCandidateMarker && candidateMarker == HatchCandidateWriter
        && (typeID != API_HatchID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Hatch candidate metadata is valid only for Hatch CREATE or UPDATE.");
    if (hasCandidateMarker && candidateMarker == DoorCandidateWriter
        && (typeID != API_DoorID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate metadata is valid only for Door CREATE or UPDATE.");
    if (hasCandidateMarker && candidateMarker == WindowCandidateWriter
        && (typeID != API_WindowID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate metadata is valid only for Window CREATE or UPDATE.");
    if (typeID == API_DoorID && HasDoorCandidateMetadata (semanticMetadata)) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        GS::UniString writer, candidateOperationId, candidateScope, candidateCaseId;
        GS::Array<GS::UniString> candidateFields, candidateGuids;
        if (!semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != DoorCandidateWriter
            || !semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId) || candidateOperationId != operationId
            || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope) || candidateScope != CurrentSoloFileScope ().Get ()
            || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId) || candidateCaseId.IsEmpty ()
            || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
            || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
            return CreateErrorResponse (APIERR_BADPARS, "Door candidate request is not bound to its exact case, operation, file, fields and GUIDs.");
        const std::initializer_list<const char*> allowedFields = { "ownerWallId", "offset", "sill", "width", "height", "orientation" };
        GS::HashSet<GS::UniString> fields;
        for (const GS::UniString& field : candidateFields) {
            bool allowed = false;
            for (const char* name : allowedFields) allowed = allowed || field == name;
            if (!allowed || fields.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate field authorization contains an unknown or repeated field.");
            fields.Add (field);
        }
        const GSSize requiredFields = operation == "create" ? static_cast<GSSize> (6) : candidateFields.GetSize ();
        if (candidateFields.IsEmpty () || static_cast<GSSize> (candidateFields.GetSize ()) != requiredFields || candidateGuids.IsEmpty ()
            || (operation == "create" && (items.GetSize () != 1 || candidateGuids.GetSize () != 1))
            || (operation == "update" && candidateGuids.GetSize () != items.GetSize ()))
            return CreateErrorResponse (APIERR_BADPARS, "Door candidate item/field/GUID binding is incomplete.");
        const GS::Optional<GS::UniString> writerError = ValidateDoorTypedPayloads (operation, items);
        if (writerError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, writerError.Get ());
        GS::HashSet<GS::UniString> uniqueGuids;
        for (const GS::UniString& text : candidateGuids) {
            const API_Guid guid = APIGuidFromString (text.ToCStr ());
            const GS::UniString normalized = text.ToLowerCase ();
            if (guid == APINULLGuid || uniqueGuids.Contains (normalized)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate GUIDs must be valid and unique.");
            uniqueGuids.Add (normalized);
        }
        bool injectFault = false;
        semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", injectFault);
        if (injectFault && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
            return CreateErrorResponse (APIERR_BADPARS, "Door first-applied rollback requires exactly two UPDATE targets.");
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (injectFault) return CreateErrorResponse (APIERR_BADPARS, "Door first-applied fault seam is unavailable in this APX build.");
#endif
        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
            if (shape == nullptr || shape->GetFieldNames ().GetSize () != candidateFields.GetSize ())
                return CreateErrorResponse (APIERR_BADPARS, "Door candidate payload/patch fields do not match the exact declared field set.");
            for (const GS::String& name : shape->GetFieldNames ())
                if (!fields.Contains (GS::UniString (name))) return CreateErrorResponse (APIERR_BADPARS, "Door candidate payload/patch contains an undeclared field.");
            const GS::ObjectState* idValue = operation == "create" ? items[index].Get ("cloneSourceElementId") : items[index].Get ("elementId");
            if (idValue == nullptr || GetGuidFromObjectState (*idValue) != APIGuidFromString (candidateGuids[index].ToCStr ()))
                return CreateErrorResponse (APIERR_BADPARS, "Door candidate target/template GUID differs from its exact authorization order.");
            if (operation == "create") {
                const GS::Optional<GS::UniString> cloneError = ValidateDoorCloneSource (GetGuidFromObjectState (*idValue));
                if (cloneError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, cloneError.Get ());
            }
        }
#else
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate metadata is unavailable in an ordinary APX build.");
#endif
    }
    if (typeID == API_WindowID && HasWindowCandidateMetadata (semanticMetadata)) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        GS::UniString writer, candidateOperationId, candidateScope, candidateCaseId;
        GS::Array<GS::UniString> candidateFields, candidateGuids;
        if (!HasOnlyObjectStateFields (*semanticMetadata, { "nativeCandidateWriter", "nativeCandidateOperationId", "nativeCandidateScopeKey", "nativeCandidateCaseId", "nativeCandidateFields", "nativeCandidateGuids", "nativeCrudTestFaultAfterFirstApplied" }))
            return CreateErrorResponse (APIERR_BADPARS, "Window candidate metadata contains an unrecognized field.");
        if (!semanticMetadata->Get ("nativeCandidateWriter", writer) || writer != WindowCandidateWriter
            || !semanticMetadata->Get ("nativeCandidateOperationId", candidateOperationId) || candidateOperationId != operationId
            || !semanticMetadata->Get ("nativeCandidateScopeKey", candidateScope) || candidateScope != CurrentSoloFileScope ().Get ()
            || !semanticMetadata->Get ("nativeCandidateCaseId", candidateCaseId) || candidateCaseId.IsEmpty ()
            || !semanticMetadata->Get ("nativeCandidateFields", candidateFields)
            || !semanticMetadata->Get ("nativeCandidateGuids", candidateGuids))
            return CreateErrorResponse (APIERR_BADPARS, "Window candidate request is not bound to its exact case, operation, file, fields and GUIDs.");
        const std::initializer_list<const char*> allowedFields = { "ownerWallId", "offset", "sill", "width", "height", "orientation" };
        GS::HashSet<GS::UniString> fields;
        for (const GS::UniString& field : candidateFields) {
            bool allowed = false;
            for (const char* name : allowedFields) allowed = allowed || field == name;
            if (!allowed || fields.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate field authorization contains an unknown or repeated field.");
            fields.Add (field);
        }
        if (candidateFields.IsEmpty () || (operation == "create" && candidateFields.GetSize () != 6) || candidateGuids.IsEmpty ()
            || (operation == "create" && (items.GetSize () != 1 || candidateGuids.GetSize () != 1))
            || (operation == "update" && candidateGuids.GetSize () != items.GetSize ()))
            return CreateErrorResponse (APIERR_BADPARS, "Window candidate item/field/GUID binding is incomplete.");
        const GS::Optional<GS::UniString> writerError = ValidateWindowTypedPayloads (operation, items);
        if (writerError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, writerError.Get ());
        GS::HashSet<GS::UniString> uniqueGuids;
        for (const GS::UniString& text : candidateGuids) {
            const API_Guid guid = APIGuidFromString (text.ToCStr ());
            const GS::UniString normalized = text.ToLowerCase ();
            if (guid == APINULLGuid || uniqueGuids.Contains (normalized)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate GUIDs must be valid and unique.");
            uniqueGuids.Add (normalized);
        }
        bool injectFault = false;
        semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", injectFault);
        if (injectFault && (operation != "update" || items.GetSize () != 2 || candidateGuids.GetSize () != 2))
            return CreateErrorResponse (APIERR_BADPARS, "Window first-applied rollback requires exactly two UPDATE targets.");
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (injectFault) return CreateErrorResponse (APIERR_BADPARS, "Window first-applied fault seam is unavailable in this APX build.");
#endif
        GS::HashSet<GS::UniString> faultHosts;
        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
            if (shape == nullptr || shape->GetFieldNames ().GetSize () != candidateFields.GetSize ())
                return CreateErrorResponse (APIERR_BADPARS, "Window candidate payload/patch fields do not match the exact declared field set.");
            for (const GS::String& name : shape->GetFieldNames ())
                if (!fields.Contains (GS::UniString (name))) return CreateErrorResponse (APIERR_BADPARS, "Window candidate payload/patch contains an undeclared field.");
            const GS::ObjectState* idValue = operation == "create" ? items[index].Get ("cloneSourceElementId") : items[index].Get ("elementId");
            const API_Guid expectedGuid = APIGuidFromString (candidateGuids[index].ToCStr ());
            if (idValue == nullptr || GetGuidFromObjectState (*idValue) != expectedGuid)
                return CreateErrorResponse (APIERR_BADPARS, "Window candidate target/template GUID differs from its exact authorization order.");
            if (operation == "create") {
                const GS::Optional<GS::UniString> cloneError = ValidateWindowCloneSource (expectedGuid);
                if (cloneError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, cloneError.Get ());
            } else if (injectFault) {
                API_Element target = {};
                target.header.guid = expectedGuid;
                if (ACAPI_Element_Get (&target) != NoError || GetElemTypeId (target.header) != API_WindowID
                    || target.header.guid != expectedGuid || target.window.openingBase.markGuid != APINULLGuid
                    || !WindowCandidateNativeTopologySupported (target))
                    return CreateErrorResponse (APIERR_BADID, "Window fault target must freshly resolve as an exact markerless native Window.");
                const GS::UniString owner = APIGuidToString (target.window.owner);
                if (owner.IsEmpty () || faultHosts.Contains (owner))
                    return CreateErrorResponse (APIERR_BADPARS, "Window first-applied fault targets must have distinct freshly read Wall hosts.");
                faultHosts.Add (owner);
            }
        }
#else
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate metadata is unavailable in an ordinary APX build.");
#endif
    }
    if (typeID == API_HatchID && HasHatchCandidateOnlyMetadata (semanticMetadata)) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        if (!hasCandidateMarker || candidateMarker != HatchCandidateWriter
            || (operation != "create" && operation != "update"))
            return CreateErrorResponse (APIERR_BADPARS, "Hatch candidate-only metadata requires the exact candidate CREATE/UPDATE route.");
#else
        return CreateErrorResponse (APIERR_BADPARS, "Hatch candidate-only metadata is unavailable in this ordinary APX build.");
#endif
    }
    if (typeID == API_DoorID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        if (!HasDoorCandidateMetadata (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Door candidate builds require the exact guarded candidate writer route.");
#else
        // Ordinary builds admit only the same markerless typed writer. Any
        // candidate/fault metadata was rejected above; the ordinary request
        // must still use the narrow Door payload contract and exact source.
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
#endif
    }
    if (typeID == API_WindowID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        if (!HasWindowCandidateMetadata (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Window candidate builds require the exact guarded candidate writer route.");
#else
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
#endif
    }
    if (typeID == API_ZoneID && HasAnyZoneMemoWriterGate (semanticMetadata) && HasZoneCandidateWriterMetadataMarker (semanticMetadata))
        return CreateErrorResponse (APIERR_BADPARS, "Zone memo and typed candidate writer gates cannot be combined in one request.");
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
    // markerless Door writer. Candidate builds retain their exact disposable
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
        if (typeID == API_WindowID && semanticMetadata != nullptr) {
            GS::UniString candidateWriter;
            if (semanticMetadata->Get ("nativeCandidateWriter", candidateWriter) && candidateWriter == WindowCandidateWriter) {
                nativeParameters.Add ("nativeCandidateWindowWriter", true);
                GS::Array<GS::UniString> publicFields;
                if (!semanticMetadata->Get ("nativeCandidateFields", publicFields)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate public field authorization is missing.");
                GS::Array<GS::ObjectState> mappedItems;
                if (!nativeParameters.Get ("items", mappedItems) || mappedItems.GetSize () != items.GetSize ()) return CreateErrorResponse (APIERR_BADPARS, "Window candidate field mapping has no exact native items.");
                GS::HashSet<GS::UniString> authorizedNativeFields;
                for (GSSize index = 0; index < static_cast<GSSize> (mappedItems.GetSize ()); ++index) {
                    const GS::ObjectState* shape = mappedItems[index].Get ("payload");
                    const GS::ObjectState* publicShape = items[index].Get (operation == "create" ? "payload" : "patch");
                    if (shape == nullptr || publicShape == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Window candidate public or native payload is missing.");
                    GS::HashSet<GS::UniString> actual;
                    for (const GS::String& name : shape->GetFieldNames ()) {
                        const GS::UniString field (name);
                        if (operation == "create" && field == "floorIndex") continue;
                        actual.Add (field);
                    }
                    GS::HashSet<GS::UniString> expected;
                    for (const GS::UniString& field : publicFields) {
                        if (field == "ownerWallId") expected.Add ("ownerWallId");
                        else if (field == "offset") expected.Add ("centerOffset");
                        else if (field == "sill") expected.Add ("sillHeight");
                        else if (field == "width" || field == "height") expected.Add (field);
                        else if (field == "orientation") {
                            const GS::ObjectState* orientation = publicShape->Get ("orientation");
                            if (orientation == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Window orientation mapping requires exact public subfields.");
                            for (const char* flag : { "reflected", "refSide", "oSide" }) if (orientation->Contains (flag)) expected.Add (flag);
                        } else return CreateErrorResponse (APIERR_BADPARS, "Window candidate public field has no exact native mapping.");
                    }
                    if (actual.GetSize () != expected.GetSize ()) return CreateErrorResponse (APIERR_BADPARS, "Window candidate public-to-native flattened field set differs.");
                    for (const GS::UniString& field : actual) if (!expected.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate contains an unauthorized native field.");
                    if (index == 0) authorizedNativeFields = expected;
                    else if (expected.GetSize () != authorizedNativeFields.GetSize ()) return CreateErrorResponse (APIERR_BADPARS, "Window batch field sets differ.");
                    else for (const GS::UniString& field : expected) if (!authorizedNativeFields.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Window batch field sets differ.");
                }
                const auto& nativeFields = nativeParameters.AddList<GS::UniString> ("nativeCandidateWindowFields");
                for (const GS::UniString& field : authorizedNativeFields) nativeFields (field);
            }
        }
        if (typeID == API_DoorID && (operation == "create" || operation == "update") && semanticMetadata != nullptr) {
            GS::UniString candidateWriter;
            if (semanticMetadata->Get ("nativeCandidateWriter", candidateWriter) && candidateWriter == DoorCandidateWriter) {
                nativeParameters.Add ("nativeCandidateDoorWriter", true);
                GS::Array<GS::ObjectState> normalizedItems;
                if (!nativeParameters.Get ("items", normalizedItems) || normalizedItems.IsEmpty ())
                    return CreateErrorResponse (APIERR_BADPARS, "Door candidate mapping did not produce native items for its second-boundary field guard.");
                GS::Array<GS::UniString> publicFields;
                if (!semanticMetadata->Get ("nativeCandidateFields", publicFields))
                    return CreateErrorResponse (APIERR_BADPARS, "Door candidate public field authorization is missing.");
                GS::HashSet<GS::UniString> authorizedNativeFields;
                for (GSSize index = 0; index < static_cast<GSSize> (normalizedItems.GetSize ()); ++index) {
                    // The public UPDATE envelope is named `patch`, but
                    // BuildNativeMutationParameters translates it into the
                    // typed writer's internal `payload` shape.  Validate
                    // that mapped representation here; the exact public
                    // patch is checked independently below.
                    const GS::ObjectState* shape = normalizedItems[index].Get ("payload");
                    if (shape == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door candidate mapping omitted the native payload/patch.");
                    GS::HashSet<GS::UniString> itemFields;
                    for (const GS::String& name : shape->GetFieldNames ()) {
                        const GS::UniString field (name);
                        // Placement is execution context, never a writable
                        // element field or candidate authorization.
                        if (operation == "create" && field == "floorIndex") continue;
                        itemFields.Add (field);
                    }
                    const GS::ObjectState* publicShape = items[index].Get (operation == "create" ? "payload" : "patch");
                    if (publicShape == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door candidate public payload/patch is missing.");
                    GS::HashSet<GS::UniString> expectedMappedFields;
                    for (const GS::UniString& publicField : publicFields) {
                        const char* nativeField = nullptr;
                        if (publicField == "ownerWallId") nativeField = "ownerWallId";
                        else if (publicField == "offset") nativeField = "centerOffset";
                        else if (publicField == "sill") nativeField = "sillHeight";
                        else if (publicField == "width") nativeField = "width";
                        else if (publicField == "height") nativeField = "height";
                        else if (publicField == "orientation") {
                            const GS::ObjectState* orientation = publicShape->Get ("orientation");
                            if (orientation == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door orientation authorization lacks its exact public subfields.");
                            for (const char* flag : { "reflected", "refSide", "oSide" })
                                if (orientation->Contains (flag)) expectedMappedFields.Add (flag);
                        } else return CreateErrorResponse (APIERR_BADPARS, "Door candidate public field has no allowlisted native mapping.");
                        if (nativeField != nullptr) expectedMappedFields.Add (nativeField);
                    }
                    if (expectedMappedFields.GetSize () != itemFields.GetSize ())
                        return CreateErrorResponse (APIERR_BADPARS, "Door candidate public-to-native field mapping is not one-to-one with its flattened payload.");
                    for (const GS::UniString& field : itemFields)
                        if (!expectedMappedFields.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate public-to-native field mapping differs from its flattened payload.");
                    if (index == 0) authorizedNativeFields = expectedMappedFields;
                    else if (expectedMappedFields.GetSize () != authorizedNativeFields.GetSize ())
                        return CreateErrorResponse (APIERR_BADPARS, "Door candidate batch does not use one exact flattened native field set.");
                    else for (const GS::UniString& field : expectedMappedFields)
                        if (!authorizedNativeFields.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate batch does not use one exact flattened native field set.");
                }
                const std::initializer_list<const char*> allowedNativeFields = {
                    "ownerWallId", "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide"
                };
                for (const GS::UniString& field : authorizedNativeFields) {
                    bool allowed = false;
                    for (const char* name : allowedNativeFields) allowed = allowed || field == name;
                    if (!allowed) return CreateErrorResponse (APIERR_BADPARS, "Door candidate flattened field mapping contains an unauthorized native field.");
                }
                const auto& nativeFields = nativeParameters.AddList<GS::UniString> ("nativeCandidateDoorFields");
                for (const GS::UniString& field : authorizedNativeFields) nativeFields (field);
            }
        }
        if (typeID == API_HatchID && operation == "create" && semanticMetadata != nullptr) {
            GS::UniString candidateWriter;
            if (semanticMetadata->Get ("nativeCandidateWriter", candidateWriter) && candidateWriter == HatchCandidateWriter)
                nativeParameters.Add ("nativeCandidateHatchWriter", true);
        }
        if (typeID == API_ColumnID && operation == "create" && HasColumnCandidateWriterGate (semanticMetadata)) {
            GS::ObjectState templateSection;
            if (semanticMetadata == nullptr || !semanticMetadata->Get ("nativeCandidateTemplateSection", templateSection))
                return CreateErrorResponse (APIERR_BADPARS, "Column candidate CREATE is missing the qualified template section readback.");
            nativeParameters.Add ("nativeCandidateColumnTemplateSection", templateSection);
        }
        bool draftingCandidateFault = false;
        if ((typeID == API_ColumnID || typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID || typeID == API_DoorID || typeID == API_WindowID) && semanticMetadata != nullptr
            && semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", draftingCandidateFault)
            && draftingCandidateFault)
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
        // Candidate and ordinary builds use the same typed executor. Candidate
        // metadata is validated earlier and remains available only behind the
        // developer compile gate; the proven fields are now ordinary internal
        // Native CRUD capability on AC25/AC28.
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
        bool faultAfterFirstApplied = false;
        if (HasZoneCandidateWriterGate (semanticMetadata))
            semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
        return ExecuteZoneTypedUpdate (items, processControl, faultAfterFirstApplied);
    }
    return CreateErrorResponse (APIERR_BADPARS, "This element type has read snapshots but no validated native memo writer in this APX build.");
}

GS::Optional<GS::UniString> ValidateNativeCreatePlacement (
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    const Stories stories = GetStories ();
    if (stories.empty ())
        return "CREATE story placement cannot be checked because Archicad story settings are unavailable.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* placement = item.Get ("placement");
        Int32 storyIndex = 0;
        if (placement == nullptr || !HasOnlyObjectStateFields (*placement, { "storyIndex" })
            || !placement->Get ("storyIndex", storyIndex)
            || storyIndex < static_cast<Int32> (std::numeric_limits<short>::min ())
            || storyIndex > static_cast<Int32> (std::numeric_limits<short>::max ()))
            return "CREATE requires placement.storyIndex as one exact Archicad story index.";
        const short nativeStoryIndex = static_cast<short> (storyIndex);
        if (stories.find (nativeStoryIndex) == stories.end ())
            return "CREATE placement.storyIndex does not identify a current Archicad story.";

        if (typeName == "Door" || typeName == "Window") {
            const GS::ObjectState* payload = item.Get ("payload");
            const GS::ObjectState* ownerWallId = payload == nullptr ? nullptr : payload->Get ("ownerWallId");
            const API_Guid ownerWallGuid = ownerWallId == nullptr ? APINULLGuid : GetGuidFromObjectState (*ownerWallId);
            if (ownerWallGuid == APINULLGuid)
                return "Door/Window CREATE requires the exact ownerWallId to validate host story placement.";
            API_Element hostWall = {};
            hostWall.header.guid = ownerWallGuid;
            if (ACAPI_Element_Get (&hostWall) != NoError || GetElemTypeId (hostWall.header) != API_WallID)
                return "Door/Window CREATE ownerWallId must resolve to an exact current Wall in this file.";
            if (hostWall.header.floorInd != nativeStoryIndex)
                return "Door/Window CREATE placement.storyIndex must match the exact host Wall story.";
        }
    }
    return {};
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
            if (!HasOnlyObjectStateFields (item, { "payload", "placement", "cloneSourceElementId", "historySnapshotRef" }))
                return GS::UniString ("CREATE items accept only payload, placement and one source reference.");
            if (item.Get ("elementId") != nullptr || item.Get ("payload") == nullptr)
                return GS::UniString ("CREATE items require payload and must not contain elementId.");
            const GS::ObjectState* payload = item.Get ("payload");
            if (payload != nullptr && (payload->Contains ("storyIndex") || payload->Contains ("floorIndex") || payload->Contains ("floorInd")))
                return GS::UniString ("storyIndex is read-only evidence and cannot appear in a CREATE payload.");
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
            if (operation == "update" && !HasOnlyObjectStateFields (item, { "elementId", "expectedFingerprint", "patch" }))
                return GS::UniString ("UPDATE items accept only elementId, expectedFingerprint and patch.");
            if (operation == "delete" && !HasOnlyObjectStateFields (item, { "elementId", "expectedFingerprint" }))
                return GS::UniString ("DELETE items accept only elementId and expectedFingerprint.");
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
            const GS::ObjectState* patch = item.Get ("patch");
            if (operation == "update" && patch != nullptr
                && (patch->Contains ("storyIndex") || patch->Contains ("floorIndex") || patch->Contains ("floorInd")))
                return GS::UniString ("storyIndex is read-only evidence and cannot appear in an UPDATE patch.");
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
    response.Add ("capabilityIdentity", GS::ObjectState (
        "schemaVersion", CapabilitySchemaV3Version,
        "schemaHash", CapabilitySchemaV3Hash,
        "source", "apx"));
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
        const bool columnCandidateWriter = GS::UniString (typeName) == "Column" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool lineCandidateWriter = GS::UniString (typeName) == "Line" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool arcCandidateWriter = GS::UniString (typeName) == "Arc" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool hatchCandidateWriter = GS::UniString (typeName) == "Hatch" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool doorCandidateWriter = GS::UniString (typeName) == "Door" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool windowCandidateWriter = GS::UniString (typeName) == "Window" && TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool doorVerifiedWriter = GS::UniString (typeName) == "Door" && !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool windowVerifiedWriter = GS::UniString (typeName) == "Window" && !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool lineVerifiedWriter = GS::UniString (typeName) == "Line" && !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool arcVerifiedWriter = GS::UniString (typeName) == "Arc" && !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool hatchVerifiedWriter = GS::UniString (typeName) == "Hatch" && !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS;
        const bool validatedWriter = GS::UniString (typeName) == "Wall" || GS::UniString (typeName) == "Text" || GS::UniString (typeName) == "Zone" || GS::UniString (typeName) == "Column" || lineVerifiedWriter || arcVerifiedWriter || hatchVerifiedWriter || doorVerifiedWriter || windowVerifiedWriter;
        const bool updateValidatedWriter = validatedWriter || GS::UniString (typeName) == "Column";
        type.Add ("create", validatedWriter || columnCandidateWriter || lineCandidateWriter || arcCandidateWriter || hatchCandidateWriter || doorCandidateWriter || windowCandidateWriter);
        type.Add ("update", updateValidatedWriter || columnCandidateWriter || lineCandidateWriter || arcCandidateWriter || hatchCandidateWriter || doorCandidateWriter || windowCandidateWriter);
        type.Add ("delete", true);
        type.Add ("writerStatus", columnCandidateWriter ? "column_candidate" : lineCandidateWriter ? "line_candidate" : arcCandidateWriter ? "arc_candidate" : hatchCandidateWriter ? "hatch_candidate" : doorCandidateWriter ? "door_candidate" : windowCandidateWriter ? "window_candidate" : validatedWriter ? "validated" : "read_only_until_memo_writer_gate");
        type.Add ("capabilityState", GS::ObjectState (
            "create", (columnCandidateWriter || lineCandidateWriter || arcCandidateWriter || hatchCandidateWriter || doorCandidateWriter || windowCandidateWriter) ? "candidate" : validatedWriter ? "verified" : "unavailable",
            "read", "verified",
            "update", (columnCandidateWriter || lineCandidateWriter || arcCandidateWriter || hatchCandidateWriter || doorCandidateWriter || windowCandidateWriter) ? "candidate" : updateValidatedWriter ? "verified" : "unavailable",
            "delete", "verified"));
        AddCanonicalWritableFields (type, GS::UniString (typeName));
        elementTypes (type);
    }
    for (const char* typeName : { "Dimension", "RadialDimension", "AngleDimension", "LevelDimension" }) {
        GS::ObjectState type;
        type.Add ("elementType", typeName);
        type.Add ("contracted", GS::ObjectState ("create", false, "read", true, "update", false, "delete", false));
        type.Add ("create", false);
        type.Add ("read", true);
        type.Add ("update", false);
        type.Add ("delete", false);
        type.Add ("writerStatus", "read_only_dimension");
        type.Add ("capabilityState", GS::ObjectState (
            "create", "unavailable", "read", "verified", "update", "unavailable", "delete", "unavailable"));
        type.Add ("writableFields", GS::Array<GS::UniString> {});
        type.Add ("createFields", GS::Array<GS::UniString> {});
        type.Add ("createPlacementFields", GS::Array<GS::UniString> {});
        type.Add ("updateFields", GS::Array<GS::UniString> {});
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
    if (operation == "create") {
        const GS::Optional<GS::UniString> placementError = ValidateNativeCreatePlacement (typeName, items);
        if (placementError.HasValue ()) return ScopeError (placementError);
    }
    if (typeID == API_ZoneID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested)
            && faultRequested && !HasZoneCandidateWriterGate (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Zone test fault metadata requires an exact candidate writer request.");
    }
    if (typeID == API_ColumnID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested)
            && faultRequested && !HasZoneCandidateWriterMetadataMarker (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Column test fault metadata requires an exact candidate writer request.");
    }
    if (typeID == API_LineID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested)
            && faultRequested && !HasZoneCandidateWriterMetadataMarker (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Line test fault metadata requires an exact candidate writer request.");
    }
    if (typeID == API_ZoneID && (operation == "create" || operation == "update")) {
        const GS::Optional<GS::UniString> typedError = ValidateZoneTypedWriter (operation, items);
        if (typedError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
    }
    if (typeID == API_ZoneID && HasZoneCandidateWriterMetadataMarker (semanticMetadata)) {
        const GS::Optional<GS::UniString> candidateError = ValidateZoneCandidateWriter (operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
        if (candidateError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
    }
    if (typeID == API_ColumnID && HasZoneCandidateWriterMetadataMarker (semanticMetadata)) {
        const GS::Optional<GS::UniString> candidateError = ValidateColumnCandidateWriter (operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
        if (candidateError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
    }
    if (typeID == API_LineID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        // Candidate APXs must never dispatch an unmarked Line write. Run the
        // exact case/scope/field/GUID validator even when semanticMetadata or
        // its writer marker is absent.
        const GS::Optional<GS::UniString> candidateError = ValidateLineCandidateWriter (operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
        if (candidateError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
#else
        // Preserve the ordinary route, but do not let candidate metadata make
        // a developer-only writer available in an ordinary APX.
        if (HasZoneCandidateWriterMetadataMarker (semanticMetadata)) {
            const GS::Optional<GS::UniString> candidateError = ValidateLineCandidateWriter (operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
            if (candidateError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
        }
#endif
    }
    if (typeID == API_ArcID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        // Candidate APXs must reject every unmarked Arc write before it can
        // enter the shared typed MutateElements executor.
        const GS::Optional<GS::UniString> candidateError = ValidateArcCandidateWriter (operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
        if (candidateError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
#else
        // Candidate metadata never opens the writer in an ordinary APX. The
        // proven Arc fields use the same typed payload validation and native
        // undoable dispatcher as the candidate profile.
        if (HasZoneCandidateWriterMetadataMarker (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Arc candidate metadata is unavailable in this ordinary APX build.");
        const GS::Optional<GS::UniString> typedError = ValidateArcTypedPayloads (operation, items, nullptr, nullptr);
        if (typedError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
#endif
    }
    if (typeID == API_ArcID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested
            && !HasZoneCandidateWriterMetadataMarker (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Arc test fault metadata requires an exact candidate writer request.");
    }
    if (typeID == API_HatchID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        // Candidate builds enforce exact case metadata before the shared
        // typed writer. The same writer is ordinary-enabled below the
        // capability boundary when candidate mode is compiled out.
        const GS::Optional<GS::UniString> candidateError = ValidateHatchCandidateWriter (
            operation, operationId, CurrentSoloFileScope ().Get (), items, semanticMetadata);
        if (candidateError.HasValue ())
            return CreateErrorResponse (APIERR_BADPARS, candidateError.Get ());
#else
        if (semanticMetadata != nullptr
            && (semanticMetadata->Contains ("nativeCandidateWriter")
                || semanticMetadata->Contains ("nativeCandidateCaseId")
                || semanticMetadata->Contains ("nativeCandidateOperationId")
                || semanticMetadata->Contains ("nativeCandidateScopeKey")
                || semanticMetadata->Contains ("nativeCandidateFields")
                || semanticMetadata->Contains ("nativeCandidateGuids")
                || semanticMetadata->Contains ("nativeCrudTestFaultAfterFirstApplied")))
            return CreateErrorResponse (APIERR_BADPARS, "Candidate Hatch metadata is unavailable in this ordinary APX build.");
        const GS::Optional<GS::UniString> typedError = ValidateHatchTypedPayloads (operation, items);
        if (typedError.HasValue ()) return CreateErrorResponse (APIERR_BADPARS, typedError.Get ());
#endif
    }
    if (typeID == API_HatchID && semanticMetadata != nullptr) {
        bool faultRequested = false;
        if (semanticMetadata->Get ("nativeCrudTestFaultAfterFirstApplied", faultRequested) && faultRequested
            && !HasZoneCandidateWriterMetadataMarker (semanticMetadata))
            return CreateErrorResponse (APIERR_BADPARS, "Hatch test fault metadata requires an exact candidate writer request.");
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

    const GS::ObjectState mutationResult = DispatchMutation (operation, operationId, typeName, items, processControl, semanticMetadata);
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
    GS::ObjectState receipt = BuildReceipt (scope, operationId, operation, typeName, description, origin, requestHash, before, after, primaryAfterIds, receiptImpactIds, complete, dispatchComplete, semanticMetadata, operatorNote, &mutationResult, nullptr, nullptr, hasImpactEnvelope ? &impact : nullptr);
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
