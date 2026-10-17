#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ObjectState.hpp"
#include "UniString.hpp"

// A closed, typed projection of the native dimension reader DTO. This helper
// is shared by the APX and the standalone DevKit-linked regression executable.
// It deliberately does not recursively hash arbitrary ObjectState fields.
namespace NativeDimensionFacts {

inline const std::set<std::string>& FieldsFor (const std::string& path)
{
    static const std::set<std::string> root = {
        "elementType", "elementId", "nativeElementTypeId", "elementVariationId", "storyIndex", "layerIndex",
        "groupElementId", "hotlinkElementId", "drawingOrderIndex", "referenceGroupLayerIndex", "hasMemo",
        "renovationStatus", "renovationFilterId", "database", "bounds", "units", "dimensionPreferences",
        "identity", "geometryAndSettings", "orderedPoints", "standaloneNotes", "status", "coverage",
        "unicodeAccessCoverage", "sourceFidelityCoverage", "readConsistency"
    };
    static const std::set<std::string> guid = { "guid" };
    static const std::set<std::string> database = { "status", "databaseId", "name", "title", "reference", "databaseType", "databaseIndex", "linkedElementId", "linkedDatabaseId", "linkedSegmentMemoIndex", "errorCode" };
    static const std::set<std::string> bounds = { "status", "xMin", "yMin", "xMax", "yMax", "zMin", "zMax", "unit", "errorCode" };
    static const std::set<std::string> units = { "coordinates", "linearMeasurements", "angles", "noteHeights", "markerSizes" };
    static const std::set<std::string> preferences = { "status", "source", "scope", "savedViewEffectiveFormat", "formatCoverage", "errorCode", "linear", "radial", "level", "elevation", "doorWindow", "parapet", "area", "angle", "dimensionStyleAttributeIndex" };
    static const std::set<std::string> lengthPrefs = { "unit", "roundInch", "decimals", "showZeroWhole", "showZeroInch", "showSmall5", "hideZeroDecimals" };
    static const std::set<std::string> anglePrefs = { "unit", "decimals", "accuracy", "hideZeroDecimals" };
    static const std::set<std::string> areaPrefs = { "unit", "decimals", "hideZeroDecimals" };
    static const std::set<std::string> identity = { "family", "contextStatus" };
    static const std::set<std::string> availability = { "status" };
    static const std::set<std::string> readConsistency = { "status" };
    static const std::set<std::string> linearGeometry = {
        "linePosition", "direction", "directionUnit", "threeDimensionalPosition", "threeDimensionalDirection",
        "threeDimensionalNormal", "arcAnchor", "segmentCount", "linePen", "textPosition", "textDirection",
        "appearance", "detailedOpeningDimension", "nominalOpeningDimension", "compositeThicknessMode",
        "defaultStaticSetting", "defaultWitnessForm", "defaultWitnessValue", "defaultNote", "defaultNoteUnicodeStatus",
        "marker", "elevationMarkerForm", "elevationArrowAngleCode", "elevationArrowFilled", "elevationArrowUp",
        "elevationNeedsPlus", "usedIn3D", "horizontalText", "onlyDimensionText", "elevationOrigin", "circleLevelFill",
        "layout", "pushNeighborEnabled", "placeUnderEnabled", "advancedLayout", "clipOtherSide", "textFillOrder",
        "measurement", "staticAnchorEvidence", "pointCount"
    };
    static const std::set<std::string> radialGeometry = {
        "base", "endPosition", "measuredRadius", "measurementSource", "display", "linePen", "textPosition",
        "textDirection", "showOrigin", "onlyDimensionText", "prefix", "prefixSource", "prefixUnicodeStatus", "marker", "staticAnchorEvidence"
    };
    static const std::set<std::string> angleGeometry = {
        "arcPosition", "arcOrigin", "measuredAngle", "measurementUnit", "measurementSource", "display", "arcRadius",
        "arcRadiusUnit", "linePen", "smallArc", "onlyDimensionText", "textPosition", "textDirection", "witnessForm", "witnessValue", "marker", "anchors"
    };
    static const std::set<std::string> levelGeometry = {
        "markerPosition", "levelValue", "levelMeasurementSource", "display", "pen", "format", "needsPlus", "staticLevel",
        "markerSize", "markerSizeUnit", "markerAngle", "markerAngleUnit", "parentTypeVariationId", "parentTypeId",
        "parentElementId", "parentAssociationStatus", "parentFloorSemantics", "origin"
    };
    static const std::set<std::string> coord2 = { "x", "y", "unit" };
    static const std::set<std::string> coord3 = { "x", "y", "z", "unit" };
    static const std::set<std::string> vector2 = { "x", "y" };
    static const std::set<std::string> vector3 = { "x", "y", "z" };
    static const std::set<std::string> anchor = { "elementVariationId", "elementTypeId", "lineReference", "specialReference", "nodeIndex", "nativeNodeId", "sectionNodeId", "elementId", "location", "location3D", "associationStatus" };
    static const std::set<std::string> point = { "index", "anchor", "dimensionPosition", "dimensionPosition3D", "measuredValueFromPreviousPoint", "measurementSource", "witnessForm", "witnessValue", "fixedPosition", "note" };
    static const std::set<std::string> slot = { "slot", "applicability", "details" };
    static const std::set<std::string> marker = { "markerType", "markerPen", "markerSize", "markerSizeUnit" };
    static const std::set<std::string> measurement = { "source", "display" };
    static const std::set<std::string> textFillOrder = { "status" };
    static const std::set<std::string> note = {
        "pen", "font", "layer", "textHeight", "textHeightUnit", "angle", "angleUnit", "position", "content",
        "contentStatus", "contentSource", "nativeContent", "fixedContent", "sourceExpression", "contentType", "contentMode",
        "fixPos", "styleBits", "opaque", "frame", "backgroundPen", "framePen", "useLeaderLine", "anchorAtTextEnd",
        "leaderBegin", "leaderMiddle", "leaderEnd", "leaderPen", "leaderLineType", "leaderShape", "leaderArrowType",
        "leaderArrowAtBegin", "leaderArrowAtEnd", "leaderArrowPen", "leaderArrowSize", "anchorPoint", "justification",
        "widthFactor", "spacingFactor", "rowAnchor", "effectBits", "observedDisplay"
    };
    static const std::set<std::string> nativeContent = { "value", "accessStatus", "accessSource", "meaningStatus" };
    static const std::set<std::string> fixedContent = { "status", "nulTerminated", "value" };
    static const std::set<std::string> sourceExpression = { "status", "reason" };
    static const std::set<std::string> observedDisplay = { "status", "reason", "value", "contentSource" };

    if (path == "root") return root;
    if (path == "guid") return guid;
    if (path == "database") return database;
    if (path == "bounds") return bounds;
    if (path == "units") return units;
    if (path == "preferences") return preferences;
    if (path == "lengthPrefs") return lengthPrefs;
    if (path == "anglePrefs") return anglePrefs;
    if (path == "areaPrefs") return areaPrefs;
    if (path == "identity") return identity;
    if (path == "availability" || path == "readConsistency") return path == "availability" ? availability : readConsistency;
    if (path == "linearGeometry") return linearGeometry;
    if (path == "radialGeometry") return radialGeometry;
    if (path == "angleGeometry") return angleGeometry;
    if (path == "levelGeometry") return levelGeometry;
    if (path == "coord2") return coord2;
    if (path == "coord3") return coord3;
    if (path == "vector2") return vector2;
    if (path == "vector3") return vector3;
    if (path == "anchor") return anchor;
    if (path == "point") return point;
    if (path == "slot") return slot;
    if (path == "marker") return marker;
    if (path == "measurement" || path == "display" || path == "textFillOrder") return path == "measurement" ? measurement : path == "textFillOrder" ? textFillOrder : availability;
    if (path == "note") return note;
    if (path == "nativeContent") return nativeContent;
    if (path == "fixedContent") return fixedContent;
    if (path == "sourceExpression") return sourceExpression;
    if (path == "observedDisplay") return observedDisplay;
    static const std::set<std::string> empty;
    return empty;
}

inline bool IsObjectField (const std::string& path, const std::string& field)
{
    if (path == "root") return field == "elementId" || field == "groupElementId" || field == "hotlinkElementId" || field == "renovationFilterId" || field == "database" || field == "bounds" || field == "units" || field == "dimensionPreferences" || field == "identity" || field == "geometryAndSettings" || field == "readConsistency" || field == "unicodeAccessCoverage" || field == "sourceFidelityCoverage";
    if (path == "database") return field == "databaseId" || field == "linkedElementId" || field == "linkedDatabaseId";
    if (path == "preferences") return field == "linear" || field == "radial" || field == "level" || field == "elevation" || field == "doorWindow" || field == "parapet" || field == "area" || field == "angle";
    if (path == "identity") return false;
    if (path == "linearGeometry" || path == "radialGeometry" || path == "angleGeometry" || path == "levelGeometry") return field == "linePosition" || field == "direction" || field == "threeDimensionalPosition" || field == "threeDimensionalDirection" || field == "threeDimensionalNormal" || field == "arcAnchor" || field == "defaultNote" || field == "marker" || field == "measurement" || field == "base" || field == "endPosition" || field == "arcPosition" || field == "arcOrigin" || field == "markerPosition" || field == "parentElementId";
    if (path == "anchor") return field == "elementId" || field == "location" || field == "location3D";
    if (path == "point") return field == "anchor" || field == "dimensionPosition" || field == "dimensionPosition3D" || field == "note";
    if (path == "slot") return field == "details";
    if (path == "note") return field == "position" || field == "nativeContent" || field == "fixedContent" || field == "sourceExpression" || field == "leaderBegin" || field == "leaderMiddle" || field == "leaderEnd" || field == "observedDisplay";
    return false;
}

inline const char* ChildPath (const std::string& path, const std::string& field, const std::string& family)
{
    if (path == "root") {
        if (field == "elementId" || field == "groupElementId" || field == "hotlinkElementId" || field == "renovationFilterId") return "guid";
        if (field == "database") return "database";
        if (field == "bounds") return "bounds";
        if (field == "units") return "units";
        if (field == "dimensionPreferences") return "preferences";
        if (field == "identity") return "identity";
        if (field == "readConsistency") return "readConsistency";
        if (field == "unicodeAccessCoverage" || field == "sourceFidelityCoverage") return "availability";
        if (field == "geometryAndSettings") return family == "linear-chain" ? "linearGeometry" : family == "radial" ? "radialGeometry" : family == "angle" ? "angleGeometry" : family == "level" ? "levelGeometry" : "invalid";
    }
    if (path == "database" && (field == "databaseId" || field == "linkedElementId" || field == "linkedDatabaseId")) return "guid";
    if (path == "preferences") {
        if (field == "angle") return "anglePrefs";
        if (field == "area") return "areaPrefs";
        if (field != "status" && field != "source" && field != "scope" && field != "savedViewEffectiveFormat" && field != "errorCode" && field != "dimensionStyleAttributeIndex") return "lengthPrefs";
    }
    if (path == "linearGeometry" || path == "radialGeometry" || path == "angleGeometry" || path == "levelGeometry") {
        if (field == "linePosition" || field == "threeDimensionalPosition" || field == "endPosition" || field == "arcPosition" || field == "arcOrigin" || field == "markerPosition" || field == "origin") return (field == "threeDimensionalPosition" ? "coord3" : "coord2");
        if (field == "direction") return "vector2";
        if (field == "threeDimensionalDirection" || field == "threeDimensionalNormal") return "vector3";
        if (field == "arcAnchor" || field == "base" || field == "anchors") return "anchor";
        if (field == "defaultNote") return "note";
        if (field == "marker") return "marker";
        if (field == "measurement") return "measurement";
        if (field == "display") return "display";
        if (field == "textFillOrder") return "textFillOrder";
        if (field == "parentElementId") return "guid";
        if (field == "parentTypeVariationId") return "availability";
    }
    if (path == "anchor") {
        if (field == "elementId") return "guid";
        if (field == "elementVariationId") return "availability";
        if (field == "location") return "coord2";
        if (field == "location3D") return "coord3";
    }
    if (path == "point") {
        if (field == "anchor") return "anchor";
        if (field == "dimensionPosition") return "coord2";
        if (field == "dimensionPosition3D") return "coord3";
        if (field == "note") return "note";
    }
    if (path == "slot" && field == "details") return "note";
    if (path == "note") {
        if (field == "position" || field == "leaderBegin" || field == "leaderMiddle" || field == "leaderEnd") return "coord2";
        if (field == "nativeContent") return "nativeContent";
        if (field == "fixedContent") return "fixedContent";
        if (field == "sourceExpression") return "sourceExpression";
        if (field == "observedDisplay") return "observedDisplay";
    }
    return "invalid";
}

inline bool IsListField (const std::string& path, const std::string& field)
{
    return (path == "root" && (field == "orderedPoints" || field == "standaloneNotes")) ||
        ((path == "angleGeometry") && field == "anchors");
}

inline bool IsBoolField (const std::string& field)
{
    static const std::set<std::string> fields = { "hasMemo", "lineReference", "detailedOpeningDimension", "defaultStaticSetting", "elevationArrowFilled", "elevationArrowUp", "elevationNeedsPlus", "usedIn3D", "horizontalText", "onlyDimensionText", "pushNeighborEnabled", "placeUnderEnabled", "advancedLayout", "clipOtherSide", "showOrigin", "smallArc", "needsPlus", "staticLevel", "fixedPosition", "fixPos", "opaque", "frame", "useLeaderLine", "anchorAtTextEnd", "leaderArrowAtBegin", "leaderArrowAtEnd", "showZeroWhole", "hideZeroDecimals", "nulTerminated" };
    return fields.find (field) != fields.end ();
}

inline bool IsIntegerField (const std::string& field)
{
    static const std::set<std::string> fields = { "nativeElementTypeId", "elementVariationId", "storyIndex", "layerIndex", "drawingOrderIndex", "referenceGroupLayerIndex", "renovationStatus", "databaseType", "databaseIndex", "linkedSegmentMemoIndex", "errorCode", "linear", "radial", "level", "elevation", "doorWindow", "parapet", "roundInch", "decimals", "showZeroInch", "showSmall5", "accuracy", "dimensionStyleAttributeIndex", "segmentCount", "linePen", "textPosition", "textDirection", "appearance", "nominalOpeningDimension", "compositeThicknessMode", "defaultWitnessForm", "elevationMarkerForm", "elevationArrowAngleCode", "elevationOrigin", "circleLevelFill", "layout", "origin", "textFillOrder", "pointCount", "elementTypeId", "specialReference", "nodeIndex", "nativeNodeId", "sectionNodeId", "index", "witnessForm", "contentType", "pen", "font", "layer", "backgroundPen", "framePen", "leaderPen", "leaderLineType", "leaderShape", "leaderArrowType", "leaderArrowPen", "anchorPoint", "justification", "rowAnchor", "styleBits", "effectBits", "markerType", "markerPen", "format", "parentTypeId", "parentTypeVariationId", "byteLength" };
    return fields.find (field) != fields.end ();
}

inline bool IsRealField (const std::string& field)
{
    static const std::set<std::string> fields = { "x", "y", "z", "xMin", "yMin", "xMax", "yMax", "zMin", "zMax", "defaultWitnessValue", "measuredValueFromPreviousPoint", "witnessValue", "measuredRadius", "measuredAngle", "arcRadius", "markerSize", "markerAngle", "levelValue", "textHeight", "angle", "widthFactor", "spacingFactor", "leaderArrowSize" };
    return fields.find (field) != fields.end ();
}

inline bool IsStringField (const std::string& field)
{
    static const std::set<std::string> fields = { "elementType", "guid", "status", "name", "title", "reference", "unit", "coordinates", "linearMeasurements", "angles", "noteHeights", "markerSizes", "source", "scope", "savedViewEffectiveFormat", "formatCoverage", "family", "contextStatus", "coverage", "directionUnit", "textHeightUnit", "angleUnit", "defaultNoteUnicodeStatus", "measurementSource", "display", "staticAnchorEvidence", "markerSizeUnit", "markerAngleUnit", "measurementUnit", "arcRadiusUnit", "levelMeasurementSource", "parentAssociationStatus", "parentFloorSemantics", "associationStatus", "applicability", "slot", "content", "contentStatus", "contentSource", "contentMode", "textHeightUnit", "contentType", "value", "accessStatus", "accessSource", "meaningStatus", "prefix", "prefixSource", "prefixUnicodeStatus", "reason" };
    return fields.find (field) != fields.end ();
}

inline bool AppendDimensionFacts (const GS::ObjectState& state, std::string& output, const std::string& path, const std::string& family);

inline bool AppendValue (const GS::ObjectState& state, const GS::String& field, std::string& output, const std::string& path, const std::string& family)
{
    const std::string name (field.ToCStr ());
    if (IsObjectField (path, name) || (name == "elementVariationId" && path == "anchor" && state.IsObject (field)) || (name == "parentTypeVariationId" && path == "levelGeometry" && state.IsObject (field)) || (name == "textFillOrder" && path == "linearGeometry" && state.IsObject (field))) {
        GS::ObjectState nested;
        if (!state.Get (field, nested)) return false;
        output += "O";
        const bool ok = AppendDimensionFacts (nested, output, ChildPath (path, name, family), family);
        output += "E";
        return ok;
    }
    if (IsListField (path, name)) {
        GS::Array<GS::ObjectState> list;
        if (!state.Get (field, list)) return false;
        output += "L" + std::to_string (list.GetSize ()) + ":";
        for (const GS::ObjectState& item : list) {
            const std::string childPath = name == "orderedPoints" ? "point" : name == "standaloneNotes" ? "slot" : "anchor";
            output += "O";
            if (!AppendDimensionFacts (item, output, childPath, family)) return false;
            output += "E";
        }
        return true;
    }
    if (IsBoolField (name)) { bool value = false; if (!state.Get (field, value) || !state.IsBool (field)) return false; output += value ? "B1" : "B0"; return true; }
    if (name == "unit" && path != "lengthPrefs" && path != "anglePrefs" && path != "areaPrefs") {
        if (!state.IsString (field)) return false;
        GS::UniString value; if (!state.Get (field, value)) return false;
        const std::string text (value.ToCStr (CC_UTF8).Get ()); output += "S" + std::to_string (text.size ()) + ":" + text; return true;
    }
    if (IsIntegerField (name) || (name == "unit" && (path == "lengthPrefs" || path == "anglePrefs" || path == "areaPrefs"))) {
        if (state.IsUInt (field)) { UInt64 value = 0; if (!state.Get (field, value)) return false; output += "U" + std::to_string (value) + ";"; return true; }
        if (state.IsInt (field)) { Int64 value = 0; if (!state.Get (field, value)) return false; output += "I" + std::to_string (value) + ";"; return true; }
        return false;
    }
    if (IsRealField (name)) {
        double value = 0.0;
        if (state.IsReal (field)) { if (!state.Get (field, value)) return false; }
        else if (state.IsInt (field)) { Int64 integer = 0; if (!state.Get (field, integer)) return false; value = static_cast<double> (integer); }
        else if (state.IsUInt (field)) { UInt64 integer = 0; if (!state.Get (field, integer)) return false; value = static_cast<double> (integer); }
        else return false;
        if (!std::isfinite (value)) return false;
        if (value == 0.0) value = 0.0;
        std::uint64_t bits = 0;
        static_assert (sizeof (bits) == sizeof (value), "IEEE-754 binary64 is required");
        std::memcpy (&bits, &value, sizeof (bits));
        std::ostringstream encoded; encoded << "D" << std::hex << std::setfill ('0') << std::setw (16) << bits << ";";
        output += encoded.str (); return true;
    }
    if (IsStringField (name)) {
        if (!state.IsString (field)) return false;
        GS::UniString value;
        if (!state.Get (field, value)) return false;
        std::string text (value.ToCStr (CC_UTF8).Get ());
        if (name == "guid") { std::transform (text.begin (), text.end (), text.begin (), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); }); }
        output += "S" + std::to_string (text.size ()) + ":" + text;
        return true;
    }
    return false;
}

inline bool AppendDimensionFacts (const GS::ObjectState& state, std::string& output, const std::string& path, const std::string& family)
{
    GS::HashSet<GS::String> fieldSet = state.GetFieldNames ();
    std::vector<std::pair<std::string, GS::String>> fields;
    const std::set<std::string>& allowed = FieldsFor (path);
    bool hasNativeNodeId = false;
    bool hasLegacyNodeId = false;
    for (const GS::String& field : fieldSet) {
        const std::string name (field.ToCStr ());
        if (path == "anchor" && name == "nativeNodeId") hasNativeNodeId = true;
        if (path == "anchor" && name == "sectionNodeId") hasLegacyNodeId = true;
        const bool explanatoryProse = (path == "sourceExpression" || path == "observedDisplay") && name == "reason";
        if (allowed.find (name) != allowed.end () && !explanatoryProse) fields.emplace_back (name == "sectionNodeId" ? "nativeNodeId" : name, field);
    }
    if (hasNativeNodeId && hasLegacyNodeId) return false;
    std::sort (fields.begin (), fields.end (), [] (const auto& left, const auto& right) { return left.first < right.first; });
    output += "{";
    for (const auto& entry : fields) {
        const std::string& key = entry.first;
        const GS::String& field = entry.second;
        output += std::to_string (key.size ()) + ":" + key + "=";
        if (!AppendValue (state, field, output, path, family)) return false;
        output += ";";
    }
    output += "}";
    return true;
}

inline std::uint64_t NativeDimensionFnv1a64 (const std::string& input)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : input) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}

inline GS::Optional<GS::UniString> NativeDimensionFactualFingerprint (const GS::ObjectState& row)
{
    GS::UniString elementType;
    if (!row.Get ("elementType", elementType)) return {};
    const std::string type (elementType.ToCStr ());
    const std::string family = type == "Dimension" ? "linear-chain" : type == "RadialDimension" ? "radial" : type == "AngleDimension" ? "angle" : type == "LevelDimension" ? "level" : "";
    if (family != "linear-chain" && family != "radial" && family != "angle" && family != "level") return {};
    std::string facts = "native-dimension-facts-v2:";
    if (!AppendDimensionFacts (row, facts, "root", family)) return {};
    const std::uint64_t hash = NativeDimensionFnv1a64 (facts);
    std::ostringstream fingerprint; fingerprint << std::hex << std::setfill ('0') << std::setw (64) << hash;
    return GS::UniString (fingerprint.str ().c_str (), CC_UTF8);
}

} // namespace NativeDimensionFacts
