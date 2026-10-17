#pragma once

#include <cmath>
#include <string>

#include "ObjectState.hpp"

namespace NativeDimensionCreateWire {

inline bool CandidateString (const GS::ObjectState& object, const char* key, const char* expected)
{
	GS::UniString value;
	return object.Get (key, value) && value == expected;
}

inline bool CandidateInt (const GS::ObjectState& object, const char* key, const Int32 expected)
{
	Int32 value = 0;
	return object.Get (key, value) && value == expected;
}

inline bool CandidateBool (const GS::ObjectState& object, const char* key, const bool expected)
{
	bool value = false;
	return object.Get (key, value) && value == expected;
}

inline bool CandidateExact (const double value, const double expected)
{
	return std::isfinite (value) && value == expected;
}

inline bool CandidateNumber (const GS::ObjectState& object, const char* key)
{
	double value = 0.0;
	return object.Get (key, value) && std::isfinite (value);
}

inline bool CandidateCoordinateShape (const GS::ObjectState& coordinate)
{
	double x = 0.0, y = 0.0;
	GS::UniString unit;
	return coordinate.Get ("x", x) && std::isfinite (x)
		&& coordinate.Get ("y", y) && std::isfinite (y)
		&& coordinate.Get ("unit", unit);
}

inline bool CandidateFingerprint (const GS::ObjectState& row)
{
	GS::UniString fingerprint;
	if (!row.Get ("fingerprint", fingerprint) || fingerprint.GetLength () != 64
		|| !CandidateString (row, "fingerprintAlgorithm", "native-dimension-fnv1a-64-typed-facts-v2")
		|| !CandidateString (row, "fingerprintCoverage", "partial")) return false;
	for (UIndex index = 0; index < fingerprint.GetLength (); ++index) {
		const GS::UniChar::Layout character = fingerprint.GetChar (index).operator GS::UniChar::Layout ();
		if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F'))) return false;
	}
	return true;
}

inline bool CandidateNoteRequiredShape (const GS::ObjectState& note)
{
	const char* integerFields[] = { "pen", "font", "layer", "contentType", "styleBits", "backgroundPen", "framePen", "leaderPen",
		"leaderLineType", "leaderShape", "leaderArrowType", "leaderArrowPen", "anchorPoint", "justification", "rowAnchor", "effectBits" };
	for (const char* field : integerFields) { Int32 value = 0; if (!note.Get (field, value)) return false; }
	const char* numberFields[] = { "textHeight", "angle", "leaderArrowSize", "widthFactor", "spacingFactor" };
	for (const char* field : numberFields) if (!CandidateNumber (note, field)) return false;
	const char* stringFields[] = { "textHeightUnit", "angleUnit", "content" };
	for (const char* field : stringFields) { GS::UniString value; if (!note.Get (field, value)) return false; }
	const char* booleanFields[] = { "fixPos", "opaque", "frame", "useLeaderLine", "anchorAtTextEnd", "leaderArrowAtBegin", "leaderArrowAtEnd" };
	for (const char* field : booleanFields) { bool value = false; if (!note.Get (field, value)) return false; }
	const char* coordinateFields[] = { "position", "leaderBegin", "leaderMiddle", "leaderEnd" };
	for (const char* field : coordinateFields) {
		const GS::ObjectState* coordinate = note.Get (field);
		if (coordinate == nullptr || !CandidateCoordinateShape (*coordinate)) return false;
	}
	return true;
}

inline bool CandidatePoint2D (const GS::ObjectState& point, const double x, const double y)
{
	double actualX = 0.0, actualY = 0.0;
	return CandidateString (point, "unit", "project-meter")
		&& point.Get ("x", actualX) && point.Get ("y", actualY)
		&& CandidateExact (actualX, x) && CandidateExact (actualY, y);
}

inline bool CandidatePoint3D (const GS::ObjectState& point, const double x, const double y, const double z)
{
	double actualX = 0.0, actualY = 0.0, actualZ = 0.0;
	return CandidateString (point, "unit", "project-meter")
		&& point.Get ("x", actualX) && point.Get ("y", actualY) && point.Get ("z", actualZ)
		&& CandidateExact (actualX, x) && CandidateExact (actualY, y) && CandidateExact (actualZ, z);
}

inline bool CandidateVector3 (const GS::ObjectState& point, const double x, const double y, const double z)
{
	double actualX = 0.0, actualY = 0.0, actualZ = 0.0;
	return point.Get ("x", actualX) && point.Get ("y", actualY) && point.Get ("z", actualZ)
		&& CandidateExact (actualX, x) && CandidateExact (actualY, y) && CandidateExact (actualZ, z);
}

inline bool CandidateGuidEqual (const GS::UniString& left, const GS::UniString& right)
{
	if (left.GetLength () != right.GetLength ()) return false;
	for (UIndex index = 0; index < left.GetLength (); ++index) {
		GS::UniChar::Layout a = left.GetChar (index).operator GS::UniChar::Layout ();
		GS::UniChar::Layout b = right.GetChar (index).operator GS::UniChar::Layout ();
		if (a >= 'A' && a <= 'F') a = static_cast<GS::UniChar::Layout> (a + ('a' - 'A'));
		if (b >= 'A' && b <= 'F') b = static_cast<GS::UniChar::Layout> (b + ('a' - 'A'));
		if (a != b) return false;
	}
	return true;
}

inline bool CandidateGuidObject (const GS::ObjectState& object, const char* key, const GS::UniString& expected)
{
	const GS::ObjectState* nested = object.Get (key);
	GS::UniString guid;
	return nested != nullptr && nested->Get ("guid", guid) && CandidateGuidEqual (guid, expected);
}

inline bool CandidateLengthFormat (const GS::ObjectState& format)
{
	Int32 unit = 0, roundInch = 0, decimals = 0, showZeroInch = 0, showSmall5 = 0;
	bool showZeroWhole = false, hideZeroDecimals = false;
	return format.Get ("unit", unit) && format.Get ("roundInch", roundInch) && format.Get ("decimals", decimals)
		&& format.Get ("showZeroWhole", showZeroWhole) && format.Get ("showZeroInch", showZeroInch)
		&& format.Get ("showSmall5", showSmall5) && format.Get ("hideZeroDecimals", hideZeroDecimals)
		&& decimals >= 0;
}

inline bool CandidatePreferencesComplete (const GS::ObjectState& preferences)
{
	GS::ObjectState linear, radial, level, elevation, doorWindow, parapet, area, angle;
	Int32 index = 0, areaUnit = 0, areaDecimals = 0, angleUnit = 0, angleDecimals = 0, angleAccuracy = 0;
	bool areaHideZero = false, angleHideZero = false;
	return CandidateString (preferences, "status", "available")
		&& CandidateString (preferences, "scope", "project-dimension-preferences")
		&& CandidateString (preferences, "formatCoverage", "complete-project-context")
		&& preferences.Get ("linear", linear) && CandidateLengthFormat (linear)
		&& preferences.Get ("radial", radial) && CandidateLengthFormat (radial)
		&& preferences.Get ("level", level) && CandidateLengthFormat (level)
		&& preferences.Get ("elevation", elevation) && CandidateLengthFormat (elevation)
		&& preferences.Get ("doorWindow", doorWindow) && CandidateLengthFormat (doorWindow)
		&& preferences.Get ("parapet", parapet) && CandidateLengthFormat (parapet)
		&& preferences.Get ("area", area) && area.Get ("unit", areaUnit) && area.Get ("decimals", areaDecimals)
		&& area.Get ("hideZeroDecimals", areaHideZero) && areaDecimals >= 0
		&& preferences.Get ("angle", angle) && angle.Get ("unit", angleUnit) && angle.Get ("decimals", angleDecimals)
		&& angle.Get ("accuracy", angleAccuracy) && angle.Get ("hideZeroDecimals", angleHideZero)
		&& angleDecimals >= 0 && preferences.Get ("dimensionStyleAttributeIndex", index);
}

inline bool CandidateNoteFixedMeasured (const GS::ObjectState& note, const bool requireEmptyContent)
{
	GS::UniString content;
	GS::ObjectState nativeContent, sourceExpression, observedDisplay;
	if (!CandidateNoteRequiredShape (note)
		|| !CandidateInt (note, "contentType", 0) || !CandidateString (note, "contentMode", "measured")
		|| !CandidateBool (note, "fixPos", false) || !CandidateBool (note, "useLeaderLine", false)
		|| !note.Get ("content", content) || !note.Get ("nativeContent", nativeContent)
		|| !CandidateString (nativeContent, "meaningStatus", "returned-native-content-expression-meaning-unproven")
		|| !note.Get ("sourceExpression", sourceExpression) || !CandidateString (sourceExpression, "status", "unproven")
		|| !note.Get ("observedDisplay", observedDisplay) || !CandidateString (observedDisplay, "status", "unavailable")) return false;
	GS::UniString nativeValue;
	if (!nativeContent.Get ("value", nativeValue) || nativeValue != content) return false;
	if (requireEmptyContent && !content.IsEmpty ()) return false;
	return true;
}

// Complete fixed-recipe postcondition over the exact raw reader row. This is
// deliberately separate from the older masked-position study predicate.
inline bool FixedCandidateReaderRowMatches (
	const GS::ObjectState& row,
	const GS::UniString& expectedGuid,
	const GS::UniString& expectedDatabaseGuid,
	const Int32 expectedStory)
{
	GS::UniString requestedGuid, elementType, rowStatus, coverage, consistencyStatus, family, contextStatus;
	Int32 nativeType = 0, variation = -1, story = -1;
	Int64 stamp = 0;
	GS::ObjectState database, databaseId, memo, identity, consistency, bounds, units, preferences, geometry, sourceCoverage;
	GS::UniString databaseGuid, boundsStatus, boundsUnit;
	Int32 databaseType = 0, databaseIndex = 0;
	bool hasMemo = false;
	if (!CandidateString (row, "status", "success") || !row.Get ("requestedGuid", requestedGuid)
		|| !CandidateGuidEqual (requestedGuid, expectedGuid) || !row.Get ("elementType", elementType) || elementType != "Dimension"
		|| !row.Get ("nativeElementTypeId", nativeType) || nativeType != 11
		|| !row.Get ("elementVariationId", variation) || variation != 0
		|| !row.Get ("storyIndex", story) || story != expectedStory
		|| !row.Get ("modificationStamp", stamp) || stamp <= 0
		|| !CandidateGuidObject (row, "elementId", expectedGuid)
		|| !row.Get ("hasMemo", hasMemo) || !hasMemo) return false;
	if (!row.Get ("database", database) || !CandidateString (database, "status", "available")
		|| !database.Get ("databaseType", databaseType) || databaseType != 1
		|| !database.Get ("databaseIndex", databaseIndex)
		|| !database.Get ("databaseId", databaseId) || !databaseId.Get ("guid", databaseGuid)
		|| !CandidateGuidEqual (databaseGuid, expectedDatabaseGuid)
		|| !row.Get ("memo", memo) || !CandidateString (memo, "status", "available")
		|| !row.Get ("identity", identity) || !identity.Get ("family", family) || family != "linear-chain"
		|| !identity.Get ("contextStatus", contextStatus) || contextStatus != "database-story-layer-from-native-element-header"
		|| !row.Get ("coverage", coverage) || coverage != "partial-unicode-source-unproven"
		|| !row.Get ("sourceFidelityCoverage", sourceCoverage) || !CandidateString (sourceCoverage, "status", "unproven")
		|| !row.Get ("readConsistency", consistency)
		|| !consistency.Get ("status", consistencyStatus) || consistencyStatus != "same-guid-type-and-modification-stamp-through-memo-read" ) return false;
	if (!CandidateFingerprint (row)) return false;
	Int64 initialStamp = 0, probeStamp = 0, finalStamp = 0;
	Int32 headerError = -1;
	if (!consistency.Get ("initialModificationStamp", initialStamp) || !consistency.Get ("probeModificationStamp", probeStamp)
		|| !consistency.Get ("finalModificationStamp", finalStamp) || !consistency.Get ("finalHeaderErrorCode", headerError)
		|| initialStamp != stamp || probeStamp != stamp || finalStamp != stamp || headerError != 0
		|| !row.Get ("bounds", bounds) || !CandidateString (bounds, "status", "available")
		|| !bounds.Get ("unit", boundsUnit) || boundsUnit != "project-meter") return false;
	double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
	if (!bounds.Get ("xMin", xMin) || !bounds.Get ("yMin", yMin) || !bounds.Get ("xMax", xMax) || !bounds.Get ("yMax", yMax)
		|| !std::isfinite (xMin) || !std::isfinite (yMin) || !std::isfinite (xMax) || !std::isfinite (yMax)
		|| xMin > xMax || yMin > yMax || !row.Get ("units", units)
		|| !CandidateString (units, "coordinates", "project-meter")
		|| !CandidateString (units, "linearMeasurements", "project-length-values-with-project-preferences-attached")
		|| !CandidateString (units, "angles", "radian")
		|| !CandidateString (units, "noteHeights", "millimeter-paper-size")
		|| !CandidateString (units, "markerSizes", "millimeter-paper-size")
		|| !row.Get ("dimensionPreferences", preferences) || !CandidatePreferencesComplete (preferences)
		|| !row.Get ("geometryAndSettings", geometry)) return false;
	GS::ObjectState direction, linePosition, linePosition3D, direction3D, normal3D, defaultNote, defaultPosition, arcAnchor, anchorLocation, anchorLocation3D, measurement;
	GS::UniString measurementSource, measurementDisplay;
	double dx = 0.0, dy = 0.0, witnessValue = 0.0;
	Int32 segmentCount = 0, pointCount = 0;
	if (!CandidateBool (geometry, "defaultStaticSetting", true) || !geometry.Get ("pointCount", pointCount) || pointCount != 2
		|| !geometry.Get ("segmentCount", segmentCount) || segmentCount != 2
		|| !geometry.Get ("direction", direction) || !direction.Get ("x", dx) || !direction.Get ("y", dy)
		|| !CandidateExact (dx, 1.0) || !CandidateExact (dy, 0.0)
		|| !CandidateString (geometry, "directionUnit", "unitless-vector")
		|| !geometry.Get ("linePosition", linePosition) || !CandidatePoint2D (linePosition, 20.0, 20.0)
		|| !geometry.Get ("threeDimensionalPosition", linePosition3D) || !CandidatePoint3D (linePosition3D, 20.0, 20.0, 0.0)
		|| !geometry.Get ("threeDimensionalDirection", direction3D) || !CandidateVector3 (direction3D, 0.0, 0.0, 0.0)
		|| !geometry.Get ("threeDimensionalNormal", normal3D) || !CandidateVector3 (normal3D, 0.0, 0.0, 0.0)
		|| !CandidateInt (geometry, "linePen", 85) || !CandidateInt (geometry, "textPosition", 0)
		|| !CandidateInt (geometry, "textDirection", 1) || !CandidateInt (geometry, "appearance", 0)
		|| !geometry.Get ("measurement", measurement) || !measurement.Get ("source", measurementSource) || !measurement.Get ("display", measurementDisplay)
		|| !CandidateBool (geometry, "detailedOpeningDimension", false) || !CandidateInt (geometry, "nominalOpeningDimension", 0)
		|| !CandidateInt (geometry, "compositeThicknessMode", 1) || !CandidateInt (geometry, "defaultWitnessForm", 2)
		|| !geometry.Get ("defaultWitnessValue", witnessValue) || !CandidateExact (witnessValue, 0.3)
		|| !CandidateBool (geometry, "usedIn3D", false) || !CandidateBool (geometry, "pushNeighborEnabled", false)
		|| !CandidateBool (geometry, "placeUnderEnabled", false) || !CandidateBool (geometry, "advancedLayout", false)
		|| !CandidateBool (geometry, "clipOtherSide", false) || !CandidateBool (geometry, "horizontalText", false)
		|| !CandidateBool (geometry, "onlyDimensionText", false) || !CandidateInt (geometry, "elevationMarkerForm", 0)
		|| !CandidateInt (geometry, "elevationArrowAngleCode", 0) || !CandidateBool (geometry, "elevationArrowFilled", false)
		|| !CandidateBool (geometry, "elevationArrowUp", false) || !CandidateBool (geometry, "elevationNeedsPlus", false)
		|| !CandidateInt (geometry, "elevationOrigin", 0) || !CandidateInt (geometry, "circleLevelFill", 0)
		|| !CandidateInt (geometry, "layout", 1)
		|| !geometry.Get ("defaultNote", defaultNote) || !CandidateNoteFixedMeasured (defaultNote, true)
		|| !defaultNote.Get ("position", defaultPosition) || !CandidatePoint2D (defaultPosition, 0.0, 0.0)
		|| !geometry.Get ("arcAnchor", arcAnchor)
		|| !CandidateInt (arcAnchor, "elementTypeId", 0) || !CandidateBool (arcAnchor, "lineReference", false)
		|| !CandidateInt (arcAnchor, "specialReference", 0) || !CandidateInt (arcAnchor, "nodeIndex", 0)
		|| !CandidateInt (arcAnchor, "nativeNodeId", 0) || !CandidateString (arcAnchor, "associationStatus", "no-element-guid")
		|| !CandidateGuidObject (arcAnchor, "elementId", GS::UniString ("00000000-0000-0000-0000-000000000000"))
		|| !arcAnchor.Get ("location", anchorLocation) || !CandidatePoint2D (anchorLocation, 0.0, 0.0)
		|| !arcAnchor.Get ("location3D", anchorLocation3D) || !CandidatePoint3D (anchorLocation3D, 0.0, 0.0, 0.0)) return false;
	GS::Array<GS::ObjectState> points;
	if (!row.Get ("orderedPoints", points) || points.GetSize () != 2) return false;
	for (UIndex index = 0; index < 2; ++index) {
		const GS::ObjectState& point = points[index];
		const double x = 20.0 + 4.0 * index;
		const double measured = index == 0 ? 0.0 : 4.0;
		Int32 storedIndex = -1, witnessForm = -1;
		UInt64 nativeNodeId = 1;
		bool fixedPosition = false;
		double witness = -1.0, measuredValue = -1.0;
		GS::ObjectState anchor, anchorLocation, anchorLocation3D, pointPosition, pointPosition3D, note;
		GS::UniString associationStatus;
		if (!point.Get ("index", storedIndex) || storedIndex != static_cast<Int32> (index)
			|| !point.Get ("anchor", anchor) || !CandidateInt (anchor, "elementTypeId", 0)
			|| !CandidateBool (anchor, "lineReference", false) || !CandidateInt (anchor, "specialReference", 0)
			|| !CandidateInt (anchor, "nodeIndex", 0) || !anchor.Get ("nativeNodeId", nativeNodeId) || nativeNodeId != 0
			|| !CandidateGuidObject (anchor, "elementId", GS::UniString ("00000000-0000-0000-0000-000000000000"))
			|| !anchor.Get ("associationStatus", associationStatus) || associationStatus != "no-element-guid"
			|| !anchor.Get ("location", anchorLocation) || !CandidatePoint2D (anchorLocation, x, 15.0)
			|| !anchor.Get ("location3D", anchorLocation3D) || !CandidatePoint3D (anchorLocation3D, x, 15.0, 0.0)
			|| !point.Get ("dimensionPosition", pointPosition) || !CandidatePoint2D (pointPosition, x, 20.0)
			|| !point.Get ("dimensionPosition3D", pointPosition3D) || !CandidatePoint3D (pointPosition3D, 0.0, 0.0, 0.0)
			|| !point.Get ("measuredValueFromPreviousPoint", measuredValue) || !CandidateExact (measuredValue, measured)
			|| !CandidateString (point, "measurementSource", "native-API_DimElem.dimVal")
			|| !point.Get ("witnessForm", witnessForm) || witnessForm != 2
			|| !point.Get ("witnessValue", witness) || !CandidateExact (witness, 0.3)
			|| !point.Get ("fixedPosition", fixedPosition) || !fixedPosition
			|| !point.Get ("note", note) || !CandidateString (note, "contentStatus", "unicode-content-available")
			|| !CandidateString (note, "contentSource", "memo-owned-unicode-contentUStr")
			|| !CandidateNoteFixedMeasured (note, false)) return false;
		GS::ObjectState pointNativeContent;
		if (!note.Get ("nativeContent", pointNativeContent)
			|| !CandidateString (pointNativeContent, "accessStatus", "unicode-content-available")
			|| !CandidateString (pointNativeContent, "accessSource", "memo-owned-unicode-contentUStr")) return false;
	}
	return true;
}

} // namespace NativeDimensionCreateWire
