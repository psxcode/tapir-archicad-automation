#include "ElementMutationInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementGDLParameterCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "NotificationCommands.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>

using ElementMutationInternal::IsOneOf;

namespace ElementMutationInternal {

const ElementTypeSpec* GetElementTypeSpec (const API_ElemTypeID typeID)
{
    static const ElementTypeSpec wall   { API_WallID,   "wallsData",   "wallsWithDetails" };
    static const ElementTypeSpec slab   { API_SlabID,   "slabsData",   "slabsWithDetails" };
    static const ElementTypeSpec column { API_ColumnID, "columnsData", "columnsWithDetails" };
    static const ElementTypeSpec beam   { API_BeamID,   "beamsData",   "beamsWithDetails" };
    static const ElementTypeSpec line   { API_LineID,   "linesData",   "linesWithDetails" };
    static const ElementTypeSpec arc    { API_ArcID,    "arcsData",    "arcsWithDetails" };
    static const ElementTypeSpec hatch  { API_HatchID,  "hatchesData", "hatchesWithDetails" };

    switch (typeID) {
        case API_WallID:   return &wall;
        case API_SlabID:   return &slab;
        case API_ColumnID: return &column;
        case API_BeamID:   return &beam;
        case API_LineID:   return &line;
        case API_ArcID:    return &arc;
        case API_HatchID:  return &hatch;
        default:           return nullptr;
    }
}

} // namespace ElementMutationInternal

namespace {

GS::Optional<GS::UniString> MissingFieldError (const char* fieldName)
{
    return GS::UniString::Printf ("Missing required '%s' field.", fieldName);
}

GS::Optional<GS::UniString> ValidateFiniteNumberField (
    const GS::ObjectState& payload,
    const char* fieldName,
    const bool requirePositive = false)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    double value = 0.0;
    if (!payload.Get (fieldName, value) || !std::isfinite (value) || (requirePositive && value <= 0.0)) {
        return GS::UniString::Printf (
            "'%s' must be a finite %s number.",
            fieldName,
            requirePositive ? "positive" : "real");
    }

    return {};
}

GS::Optional<GS::UniString> ValidateIntegerField (
    const GS::ObjectState& payload,
    const char* fieldName)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    Int32 value = 0;
    if (!payload.Get (fieldName, value)) {
        return GS::UniString::Printf ("'%s' must be an integer.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ValidateFloorIndexField (
    const GS::ObjectState& payload,
    const char* fieldName)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    Int32 value = 0;
    if (!payload.Get (fieldName, value)) {
        return GS::UniString::Printf ("'%s' must be an integer.", fieldName);
    }
    if (value < static_cast<Int32> (std::numeric_limits<short>::min ()) ||
        value > static_cast<Int32> (std::numeric_limits<short>::max ())) {
        return GS::UniString::Printf ("'%s' is outside the Archicad story-index range.", fieldName);
    }

    const Stories stories = GetStories ();
    if (stories.empty ()) {
        return "Story settings are unavailable; an explicit floorIndex cannot be validated.";
    }
    if (stories.find (static_cast<short> (value)) == stories.end ()) {
        return GS::UniString::Printf ("Unknown Archicad story index in '%s'.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ValidateBooleanField (
    const GS::ObjectState& payload,
    const char* fieldName)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    bool value = false;
    if (!payload.Get (fieldName, value)) {
        return GS::UniString::Printf ("'%s' must be a boolean.", fieldName);
    }

    return {};
}

} // namespace

namespace ElementMutationInternal {

bool IsOneOf (const GS::UniString& value, const std::initializer_list<const char*>& allowedValues)
{
    for (const char* allowedValue : allowedValues) {
        if (value == allowedValue) {
            return true;
        }
    }
    return false;
}

} // namespace ElementMutationInternal

namespace {

GS::Optional<GS::UniString> ValidateEnumField (
    const GS::ObjectState& payload,
    const char* fieldName,
    const std::initializer_list<const char*>& allowedValues)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    GS::UniString value;
    if (!payload.Get (fieldName, value) || !IsOneOf (value, allowedValues)) {
        return GS::UniString::Printf ("Unknown value for '%s'.", fieldName);
    }

    return {};
}

bool ResolveAttributeReference (
    const GS::ObjectState& payload,
    const char* fieldName,
    const API_AttrTypeID attributeType)
{
    const GS::ObjectState* attributeID = payload.Get (fieldName);
    if (attributeID == nullptr) {
        return false;
    }

    API_Attribute attribute = {};
    attribute.header.typeID = attributeType;
    attribute.header.guid = GetGuidFromObjectState (*attributeID);
    return attribute.header.guid != APINULLGuid && ACAPI_Attribute_Get (&attribute) == NoError;
}

GS::Optional<GS::UniString> ValidateAttributeField (
    const GS::ObjectState& payload,
    const char* fieldName,
    const API_AttrTypeID attributeType)
{
    if (!payload.Contains (fieldName)) {
        return {};
    }

    if (!ResolveAttributeReference (payload, fieldName, attributeType)) {
        return GS::UniString::Printf ("Invalid attribute reference in '%s'.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ValidateCoordinateField (
    const GS::ObjectState& payload,
    const char* fieldName,
    const bool required,
    const bool requireZ)
{
    const GS::ObjectState* coordinate = payload.Get (fieldName);
    if (coordinate == nullptr) {
        return required ? MissingFieldError (fieldName) : GS::Optional<GS::UniString> {};
    }

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (!coordinate->Get ("x", x) || !coordinate->Get ("y", y) ||
        (requireZ && !coordinate->Get ("z", z)) ||
        !std::isfinite (x) || !std::isfinite (y) || (requireZ && !std::isfinite (z))) {
        return GS::UniString::Printf ("'%s' must contain finite coordinate values.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ValidateCoordinateArrayField (
    const GS::ObjectState& payload,
    const char* fieldName,
    const bool required,
    const bool requireZ,
    const GSSize minimumSize)
{
    if (!payload.Contains (fieldName)) {
        return required ? MissingFieldError (fieldName) : GS::Optional<GS::UniString> {};
    }

    GS::Array<GS::ObjectState> coordinates;
    if (!payload.Get (fieldName, coordinates) || coordinates.GetSize () < static_cast<GS::USize> (minimumSize)) {
        return GS::UniString::Printf ("'%s' must contain at least %d coordinates.", fieldName, static_cast<int> (minimumSize));
    }

    for (const GS::ObjectState& coordinate : coordinates) {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (!coordinate.Get ("x", x) || !coordinate.Get ("y", y) ||
            (requireZ && !coordinate.Get ("z", z)) ||
            !std::isfinite (x) || !std::isfinite (y) || (requireZ && !std::isfinite (z))) {
            return GS::UniString::Printf ("Every coordinate in '%s' must contain finite values.", fieldName);
        }
    }

    GSSize effectiveSize = static_cast<GSSize> (coordinates.GetSize ());
    if (effectiveSize > 1 && IsSame2DCoordinate (coordinates.GetFirst (), coordinates.GetLast ())) {
        --effectiveSize;
    }
    if (effectiveSize < minimumSize) {
        return GS::UniString::Printf (
            "'%s' must contain at least %d distinct coordinates after removing a closing duplicate.",
            fieldName,
            static_cast<int> (minimumSize));
    }

    for (GSSize i = 0; i < effectiveSize; ++i) {
        for (GSSize j = i + 1; j < effectiveSize; ++j) {
            if (IsSame2DCoordinate (coordinates[i], coordinates[j])) {
                return GS::UniString::Printf ("'%s' contains duplicate coordinates and is not a valid polygon.", fieldName);
            }
        }
    }

    double twiceArea = 0.0;
    for (GSSize i = 0; i < effectiveSize; ++i) {
        const GSSize next = (i + 1 < effectiveSize) ? i + 1 : 0;
        const API_Coord current = Get2DCoordinateFromObjectState (coordinates[i]);
        const API_Coord following = Get2DCoordinateFromObjectState (coordinates[next]);
        twiceArea += current.x * following.y - following.x * current.y;
    }
    if (std::abs (twiceArea) <= 1.0e-12) {
        return GS::UniString::Printf ("'%s' must enclose a non-zero area.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ValidateArcArray (
    const GS::Array<GS::ObjectState>& arcs,
    const GSSize coordinateCount,
    const char* fieldName)
{
    for (const GS::ObjectState& arc : arcs) {
        Int32 beginIndex = 0;
        Int32 endIndex = 0;
        double angle = 0.0;
        if (!arc.Get ("begIndex", beginIndex) || !arc.Get ("endIndex", endIndex) ||
            !arc.Get ("arcAngle", angle) || beginIndex < 0 || endIndex < beginIndex ||
            static_cast<GSSize> (beginIndex) >= coordinateCount ||
            static_cast<GSSize> (endIndex) >= coordinateCount || !std::isfinite (angle)) {
            return GS::UniString::Printf (
                "Every polygon arc in '%s' must contain ordered indices within the coordinate array and a finite 'arcAngle'.",
                fieldName);
        }
    }

    return {};
}

GS::Optional<GS::UniString> ValidateArcArrayField (
    const GS::ObjectState& payload,
    const char* coordinateFieldName,
    const char* fieldName)
{
    if (!payload.Contains ("polygonArcs")) {
        return {};
    }

    GS::Array<GS::ObjectState> coordinates;
    if (!payload.Get (coordinateFieldName, coordinates)) {
        return GS::UniString::Printf ("'polygonArcs' requires '%s'.", coordinateFieldName);
    }
    GS::Array<GS::ObjectState> arcs;
    if (!payload.Get ("polygonArcs", arcs)) {
        return "'polygonArcs' must be an array.";
    }

    GSSize coordinateCount = static_cast<GSSize> (coordinates.GetSize ());
    if (coordinateCount > 1 && IsSame2DCoordinate (coordinates.GetFirst (), coordinates.GetLast ())) {
        --coordinateCount;
    }
    return ValidateArcArray (arcs, coordinateCount, fieldName);
}

GS::Optional<GS::UniString> ValidateHolesField (const GS::ObjectState& payload)
{
    if (!payload.Contains ("holes")) {
        return {};
    }

    GS::Array<GS::ObjectState> holes;
    if (!payload.Get ("holes", holes)) {
        return "'holes' must be an array.";
    }

    for (const GS::ObjectState& hole : holes) {
        const bool hasPolygonCoordinates = hole.Contains ("polygonCoordinates");
        const bool hasPolygonOutline = hole.Contains ("polygonOutline");
        if (hasPolygonCoordinates && hasPolygonOutline) {
            return "A hole must use either 'polygonCoordinates' or 'polygonOutline', not both.";
        }
        if (!hasPolygonCoordinates && !hasPolygonOutline) {
            return "Every hole must contain 'polygonCoordinates' or 'polygonOutline'.";
        }

        const char* coordinateField = hasPolygonCoordinates ? "polygonCoordinates" : "polygonOutline";
        auto error = ValidateCoordinateArrayField (hole, coordinateField, true, false, 3);
        if (error.HasValue ()) {
            return error;
        }

        if (hole.Contains ("polygonArcs")) {
            GS::Array<GS::ObjectState> coordinates;
            GS::Array<GS::ObjectState> arcs;
            if (!hole.Get (coordinateField, coordinates) || !hole.Get ("polygonArcs", arcs)) {
                return "A hole's 'polygonArcs' must be an array associated with its coordinates.";
            }
            GSSize coordinateCount = static_cast<GSSize> (coordinates.GetSize ());
            if (coordinateCount > 1 && IsSame2DCoordinate (coordinates.GetFirst (), coordinates.GetLast ())) {
                --coordinateCount;
            }
            error = ValidateArcArray (arcs, coordinateCount, "hole polygonArcs");
            if (error.HasValue ()) {
                return error;
            }
        }
    }

    return {};
}

GS::Optional<GS::UniString> ValidateStructureFields (
    const GS::ObjectState& payload,
    const API_ElemTypeID typeID,
    const bool isCreate)
{
    const bool hasBuildingMaterial = payload.Contains ("buildingMaterialId");
    const bool hasComposite = payload.Contains ("compositeId");
    const bool hasProfile = payload.Contains ("profileId");
    const int referenceCount = static_cast<int> (hasBuildingMaterial) + static_cast<int> (hasComposite) + static_cast<int> (hasProfile);
    if (referenceCount > 1) {
        return "Only one of 'buildingMaterialId', 'compositeId' or 'profileId' may be provided at a time.";
    }

    if ((typeID == API_ColumnID || typeID == API_BeamID) && hasComposite) {
        return "'compositeId' is not supported for Column or Beam sections.";
    }
    if (typeID == API_SlabID && hasProfile) {
        return "'profileId' is not supported for Slab elements.";
    }
    if (typeID == API_ColumnID && hasProfile && payload.Contains ("circleBased")) {
        bool circleBased = false;
        if (payload.Get ("circleBased", circleBased) && circleBased) {
            return "'circleBased=true' cannot be combined with 'profileId' for a column section.";
        }
    }

    auto error = ValidateAttributeField (payload, "buildingMaterialId", API_BuildingMaterialID);
    if (error.HasValue ()) return error;
    error = ValidateAttributeField (payload, "compositeId", API_CompWallID);
    if (error.HasValue ()) return error;
    error = ValidateAttributeField (payload, "profileId", API_ProfileID);
    if (error.HasValue ()) return error;

    if (!payload.Contains ("structureType")) {
        return {};
    }

    GS::UniString structureType;
    if (!payload.Get ("structureType", structureType)) {
        return "'structureType' must be a string enum.";
    }

    if (typeID == API_ColumnID || typeID == API_BeamID) {
        return "'structureType' is not supported for Column or Beam sections.";
    }

    const bool allowProfile = typeID == API_WallID;
    const bool allowComposite = typeID == API_WallID || typeID == API_SlabID;
    if (!IsOneOf (structureType, allowProfile ? std::initializer_list<const char*> { "Basic", "Composite", "Profile" }
                                           : std::initializer_list<const char*> { "Basic", "Composite" })) {
        return "Invalid 'structureType'.";
    }
    if (structureType == "Composite" && !allowComposite) {
        return "'structureType=Composite' is not supported for this element type.";
    }
    if (structureType == "Profile" && !allowProfile) {
        return "'structureType=Profile' is not supported for this element type.";
    }
    if (structureType == "Composite" && !hasComposite) {
        return "'structureType=Composite' requires 'compositeId'.";
    }
    if (structureType == "Profile" && !hasProfile) {
        return "'structureType=Profile' requires 'profileId'.";
    }
    if (structureType == "Basic" && (hasComposite || hasProfile)) {
        return "'structureType=Basic' cannot be combined with 'compositeId' or 'profileId'.";
    }
    if (structureType == "Composite" && (hasBuildingMaterial || hasProfile)) {
        return "'structureType=Composite' cannot be combined with 'buildingMaterialId' or 'profileId'.";
    }
    if (structureType == "Profile" && (hasBuildingMaterial || hasComposite)) {
        return "'structureType=Profile' cannot be combined with 'buildingMaterialId' or 'compositeId'.";
    }

    (void) isCreate;
    return {};
}

bool IsAllowedPayloadField (
    const char* fieldName,
    const std::initializer_list<const char*>& allowedFields)
{
    for (const char* allowedField : allowedFields) {
        if (GS::UniString (fieldName) == allowedField) {
            return true;
        }
    }
    return false;
}

GS::Optional<GS::UniString> ValidateAllowedPayloadFields (
    const GS::ObjectState& payload,
    const API_ElemTypeID typeID,
    const bool isCreate)
{
    const GS::UniString typeName = GetElementTypeNonLocalizedName (typeID);
    const char* operationName = isCreate ? "create" : "update";

    static const char* knownFields[] = {
        "begCoordinate", "endCoordinate", "coordinates", "origin", "floorIndex", "storyIndex", "zCoordinate", "elevation", "level",
        "height", "width", "depth", "thickness", "bottomOffset", "offset", "slantAngle", "arcAngle",
        "verticalCurveHeight", "axisRotationAngle", "profileAngle", "referenceLineLocation", "flipped", "referencePlaneLocation",
        "structureType", "buildingMaterialId", "compositeId", "profileId", "anchorPoint", "coreAnchor",
        "circleBased", "isWidthAndHeightLinked", "isSlanted", "polygonCoordinates", "polygonOutline", "polygonArcs", "holes",
        "richText", "coordinate", "angle", "anchor", "pen", "nonBreaking", "linePen", "linePenIndex", "lineTypeIndex",
        "roomSeparator", "axisAngle", "ratio", "begAngle", "beginAngle", "endAngle", "reflected", "floorInd", "hatchCoordinates",
        "polygonOutline", "contourPenIndex", "fillPenIndex", "fillBackgroundPenIndex", "fillAttributeIndex",
        "ownerWallId", "centerOffset", "sillHeight", "refSide", "oSide"
    };

    const auto rejectUnsupportedFields = [&] (const std::initializer_list<const char*>& allowedFields) -> GS::Optional<GS::UniString> {
        for (const char* fieldName : knownFields) {
            if (payload.Contains (fieldName) && !IsAllowedPayloadField (fieldName, allowedFields)) {
                return GS::UniString::Printf (
                    "Field '%s' is not supported for %s %s mutations.",
                    fieldName,
                    typeName.ToPrintf (),
                    operationName);
            }
        }
        return {};
    };

    if (typeID == API_TextID) {
        return rejectUnsupportedFields ({
            "richText", "coordinate", "floorIndex", "angle", "anchor", "width", "height", "pen", "fixedSize", "nonBreaking"
        });
    } else if (typeID == API_WallID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "floorIndex", "zCoordinate", "height", "thickness", "offset", "arcAngle",
                "referenceLineLocation", "flipped", "structureType", "buildingMaterialId", "compositeId", "profileId"
            });
        }
        return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "storyIndex", "elevation", "arcAngle", "height", "thickness", "bottomOffset", "offset",
                "referenceLineLocation", "flipped", "structureType", "buildingMaterialId", "compositeId", "profileId"
        });
    } else if (typeID == API_SlabID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "level", "thickness", "referencePlaneLocation", "polygonCoordinates", "polygonArcs", "holes", "floorIndex"
            });
        }
        return rejectUnsupportedFields ({
                "zCoordinate", "thickness", "referencePlaneLocation", "structureType", "buildingMaterialId", "compositeId",
                "polygonOutline", "polygonArcs", "holes"
        });
    } else if (typeID == API_ColumnID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "coordinates", "height", "axisRotationAngle", "width", "depth", "coreAnchor", "floorIndex"
            });
        }
        return rejectUnsupportedFields ({
                "origin", "zCoordinate", "height", "bottomOffset", "axisRotationAngle", "slantAngle", "width", "depth",
                "circleBased", "isWidthAndHeightLinked", "isSlanted", "buildingMaterialId", "profileId"
        });
    } else if (typeID == API_BeamID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "floorIndex", "zCoordinate", "offset", "slantAngle", "isSlanted", "profileAngle", "arcAngle",
                "verticalCurveHeight", "width", "height", "anchorPoint"
            });
        }
        return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "level", "offset", "slantAngle", "isSlanted", "profileAngle", "arcAngle", "verticalCurveHeight",
                "width", "height", "isWidthAndHeightLinked", "buildingMaterialId", "profileId"
        });
    } else if (typeID == API_LineID) {
        return rejectUnsupportedFields ({
            "begCoordinate", "endCoordinate", "floorIndex", "linePen", "linePenIndex", "lineTypeIndex", "roomSeparator"
        });
    } else if (typeID == API_ArcID) {
        return rejectUnsupportedFields ({
            "origin", "radius", "axisAngle", "ratio", "begAngle", "beginAngle", "endAngle", "reflected", "floorIndex",
            "linePen", "linePenIndex", "lineTypeIndex", "roomSeparator"
        });
    } else if (typeID == API_HatchID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "floorInd", "hatchCoordinates", "contourPenIndex", "fillPenIndex", "fillBackgroundPenIndex", "fillAttributeIndex"
            });
        }
        return rejectUnsupportedFields ({
                "polygonOutline", "contourPenIndex", "fillPenIndex", "fillBackgroundPenIndex", "fillAttributeIndex"
        });
    } else if (typeID == API_DoorID) {
        if (isCreate) return rejectUnsupportedFields ({ "ownerWallId", "floorIndex", "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide" });
        return rejectUnsupportedFields ({ "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide" });
    } else if (typeID == API_WindowID) {
        if (isCreate) return rejectUnsupportedFields ({ "ownerWallId", "floorIndex", "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide" });
        return rejectUnsupportedFields ({ "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide" });
    } else {
        return "Unsupported element type.";
    }
}

GS::Optional<GS::UniString> ValidateTextPayload (
    const GS::ObjectState& payload,
    const bool isCreate)
{
    if (isCreate && !payload.Contains ("richText")) {
        return MissingFieldError ("richText");
    }

    const GS::ObjectState* richText = payload.Get ("richText");
    if (payload.Contains ("richText") && richText == nullptr) {
        return "'richText' must be an object conforming to native-rich-text-v1.";
    }

    if (richText != nullptr) {
        API_ElementMemo memo = {};
        API_TextType textData = {};
        GS::UniString richTextError;
        const GSErrCode memoError = NativeRichTextMemo::BuildMemo (*richText, memo, textData, richTextError);
        NativeMemoGuard memoGuard (memo);
        if (memoError != NoError) {
            return richTextError.IsEmpty ()
                ? GS::UniString::Printf ("The 'richText' payload is invalid (error %d).", memoError)
                : richTextError;
        }
    }

    if (isCreate && !payload.Contains ("coordinate")) {
        return MissingFieldError ("coordinate");
    }
    if (payload.Contains ("coordinate")) {
        const GS::ObjectState* coordinate = payload.Get ("coordinate");
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (coordinate == nullptr || !coordinate->Get ("x", x) || !coordinate->Get ("y", y) ||
            !std::isfinite (x) || !std::isfinite (y) ||
            (coordinate->Contains ("z") && (!coordinate->Get ("z", z) || !std::isfinite (z)))) {
            return "'coordinate' must contain finite x/y and optional z values.";
        }
    }

    for (const char* fieldName : { "angle", "width", "height" }) {
        auto error = ValidateFiniteNumberField (payload, fieldName);
        if (error.HasValue ()) return error;
    }
    if (payload.Contains ("width")) {
        double width = 0.0;
        payload.Get ("width", width);
        if (width < 0.0) return "'width' must not be negative.";
    }
    if (payload.Contains ("height")) {
        double height = 0.0;
        payload.Get ("height", height);
        if (height < 0.0) return "'height' must not be negative.";
    }
    auto error = ValidateIntegerField (payload, "floorIndex");
    if (error.HasValue ()) return error;
    error = ValidateFloorIndexField (payload, "floorIndex");
    if (error.HasValue ()) return error;
    error = ValidateIntegerField (payload, "anchor");
    if (error.HasValue ()) return error;
    if (payload.Contains ("anchor")) {
        Int32 anchor = 0;
        payload.Get ("anchor", anchor);
        if (anchor < static_cast<Int32> (APIAnc_LT) || anchor > static_cast<Int32> (APIAnc_RB)) {
            return "'anchor' is outside the Archicad text anchor range.";
        }
    }
    error = ValidateIntegerField (payload, "pen");
    if (error.HasValue ()) return error;
    if (payload.Contains ("pen")) {
        Int32 pen = 0;
        payload.Get ("pen", pen);
        if (pen < static_cast<Int32> (std::numeric_limits<short>::min ()) ||
            pen > static_cast<Int32> (std::numeric_limits<short>::max ())) {
            return "'pen' is outside the Archicad pen-index range.";
        }
    }
    error = ValidateBooleanField (payload, "nonBreaking");
    if (error.HasValue ()) return error;
    return {};
}

GS::Optional<GS::UniString> ValidatePayload (
    const GS::ObjectState& payload,
    const API_ElemTypeID typeID,
    const bool isCreate)
{
    auto error = ValidateAllowedPayloadFields (payload, typeID, isCreate);
    if (error.HasValue ()) return error;

    if (typeID == API_TextID) {
        return ValidateTextPayload (payload, isCreate);
    }

    const char* finiteFields[] = {
        "zCoordinate", "level", "bottomOffset", "offset", "slantAngle", "arcAngle",
        "verticalCurveHeight", "axisRotationAngle", "profileAngle", "centerOffset", "sillHeight"
    };
    for (const char* fieldName : finiteFields) {
        auto error = ValidateFiniteNumberField (payload, fieldName);
        if (error.HasValue ()) return error;
    }

    const char* positiveFields[] = { "height", "width", "depth", "thickness" };
    for (const char* fieldName : positiveFields) {
        auto error = ValidateFiniteNumberField (payload, fieldName, true);
        if (error.HasValue ()) return error;
    }

    error = ValidateIntegerField (payload, "floorIndex");
    if (error.HasValue ()) return error;
    error = ValidateFloorIndexField (payload, "floorIndex");
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (payload, "circleBased");
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (payload, "isWidthAndHeightLinked");
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (payload, "isSlanted");
    if (error.HasValue ()) return error;

    error = ValidateStructureFields (payload, typeID, isCreate);
    if (error.HasValue ()) return error;

    if (typeID == API_WallID) {
        if (!isCreate && (payload.Contains ("floorIndex") || payload.Contains ("zCoordinate"))) {
            return "Wall update uses canonical storyIndex/elevation; floorIndex/zCoordinate are creation fields.";
        }
        error = ValidateIntegerField (payload, "storyIndex");
        if (error.HasValue ()) return error;
        error = ValidateFloorIndexField (payload, "storyIndex");
        if (error.HasValue ()) return error;
        error = ValidateFiniteNumberField (payload, "elevation");
        if (error.HasValue ()) return error;
        error = ValidateBooleanField (payload, "flipped");
        if (error.HasValue ()) return error;
        error = ValidateEnumField (payload, "referenceLineLocation", {
            "Outside", "Center", "Inside", "CoreOutside", "CoreCenter", "CoreInside"
        });
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "begCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "endCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        if (payload.Contains ("begCoordinate") && payload.Contains ("endCoordinate")) {
            API_Coord begin = Get2DCoordinateFromObjectState (*payload.Get ("begCoordinate"));
            API_Coord end = Get2DCoordinateFromObjectState (*payload.Get ("endCoordinate"));
            if (IsSame2DCoordinate (begin, end)) {
                return "Wall begin and end coordinates must not be identical.";
            }
        }
        if (payload.Contains ("coordinates") || payload.Contains ("origin") || payload.Contains ("polygonCoordinates")) {
            return "The Wall payload contains geometry fields for another element type.";
        }
        if (isCreate) {
            if (!payload.Contains ("height") || !payload.Contains ("thickness")) {
                return "Wall creation requires 'height' and 'thickness'.";
            }
        }
    } else if (typeID == API_SlabID) {
        if (isCreate && (payload.Contains ("structureType") || payload.Contains ("buildingMaterialId") ||
                         payload.Contains ("compositeId"))) {
            return "Slab creation currently accepts geometry and placement fields only; structure updates are a separate typed operation.";
        }
        if (!isCreate && (payload.Contains ("level") || payload.Contains ("floorIndex") || payload.Contains ("polygonCoordinates"))) {
            return "Slab update uses 'zCoordinate' and 'polygonOutline'; level/floorIndex/polygonCoordinates are creation fields.";
        }
        error = ValidateEnumField (payload, "referencePlaneLocation", { "Top", "CoreTop", "CoreBottom", "Bottom" });
        if (error.HasValue ()) return error;
        error = ValidateCoordinateArrayField (payload, "polygonCoordinates", isCreate, false, 3);
        if (error.HasValue ()) return error;
        error = ValidateCoordinateArrayField (payload, "polygonOutline", false, false, 3);
        if (error.HasValue ()) return error;
        if (payload.Contains ("polygonArcs")) {
            const char* coordinateFieldName = isCreate ? "polygonCoordinates" : "polygonOutline";
            error = ValidateArcArrayField (payload, coordinateFieldName, "polygonArcs");
            if (error.HasValue ()) return error;
        }
        error = ValidateHolesField (payload);
        if (error.HasValue ()) return error;
        if (isCreate && !payload.Contains ("level")) {
            return "Slab creation requires 'level'.";
        }
        if (payload.Contains ("begCoordinate") || payload.Contains ("endCoordinate") || payload.Contains ("origin") ||
            (isCreate && payload.Contains ("polygonOutline")) || (!isCreate && payload.Contains ("polygonCoordinates"))) {
            return "The Slab payload contains geometry fields for the wrong operation or element type.";
        }
    } else if (typeID == API_ColumnID) {
        if (!isCreate && (payload.Contains ("floorIndex") || payload.Contains ("coreAnchor"))) {
            return "Column update does not accept floorIndex or coreAnchor; use zCoordinate/origin and section fields.";
        }
        error = ValidateEnumField (payload, "coreAnchor", {
            "TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center", "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"
        });
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "coordinates", isCreate, true);
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "origin", false, false);
        if (error.HasValue ()) return error;
        if (isCreate && payload.Contains ("origin")) {
            return "Column creation uses 'coordinates'; 'origin' is an update field.";
        }
        if (!isCreate && payload.Contains ("coordinates")) {
            return "Column update uses 'origin'; 'coordinates' is a creation field.";
        }
        if (payload.Contains ("begCoordinate") || payload.Contains ("endCoordinate") || payload.Contains ("polygonCoordinates")) {
            return "The Column payload contains geometry fields for another element type.";
        }
    } else if (typeID == API_BeamID) {
        if (!isCreate && (payload.Contains ("floorIndex") || payload.Contains ("anchorPoint"))) {
            return "Beam update does not accept floorIndex or anchorPoint; use level and section fields.";
        }
        error = ValidateEnumField (payload, "anchorPoint", {
            "TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center", "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"
        });
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "begCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "endCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        if (payload.Contains ("begCoordinate") && payload.Contains ("endCoordinate")) {
            API_Coord begin = Get2DCoordinateFromObjectState (*payload.Get ("begCoordinate"));
            API_Coord end = Get2DCoordinateFromObjectState (*payload.Get ("endCoordinate"));
            if (IsSame2DCoordinate (begin, end)) {
                return "Beam begin and end coordinates must not be identical.";
            }
        }
        if (isCreate && !payload.Contains ("zCoordinate")) {
            return "Beam creation requires 'zCoordinate'.";
        }
        if (!isCreate && payload.Contains ("zCoordinate")) {
            return "Beam update uses 'level'; 'zCoordinate' is a creation field.";
        }
        if (payload.Contains ("coordinates") || payload.Contains ("origin") || payload.Contains ("polygonCoordinates")) {
            return "The Beam payload contains geometry fields for another element type.";
        }
    } else if (typeID == API_LineID) {
        error = ValidateCoordinateField (payload, "begCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        error = ValidateCoordinateField (payload, "endCoordinate", isCreate, false);
        if (error.HasValue ()) return error;
        if (payload.Contains ("begCoordinate") && payload.Contains ("endCoordinate")) {
            const API_Coord begin = Get2DCoordinateFromObjectState (*payload.Get ("begCoordinate"));
            const API_Coord end = Get2DCoordinateFromObjectState (*payload.Get ("endCoordinate"));
            if (IsSame2DCoordinate (begin, end))
                return "Line begin and end coordinates must not be identical.";
        }
        for (const char* fieldName : { "linePen", "linePenIndex", "lineTypeIndex" }) {
            error = ValidateIntegerField (payload, fieldName);
            if (error.HasValue ()) return error;
        }
        error = ValidateBooleanField (payload, "roomSeparator");
        if (error.HasValue ()) return error;
    } else if (typeID == API_ArcID) {
        error = ValidateCoordinateField (payload, "origin", isCreate, false);
        if (error.HasValue ()) return error;
        for (const char* fieldName : { "radius", "axisAngle", "ratio", "begAngle", "beginAngle", "endAngle" }) {
            error = ValidateFiniteNumberField (payload, fieldName);
            if (error.HasValue ()) return error;
        }
        if (payload.Contains ("radius")) {
            double radius = 0.0;
            payload.Get ("radius", radius);
            if (radius <= 0.0) return "'radius' must be positive.";
        }
        if (payload.Contains ("ratio")) {
            double ratio = 0.0;
            payload.Get ("ratio", ratio);
            if (ratio <= 0.0) return "'ratio' must be positive.";
        }
        if (isCreate && (!payload.Contains ("radius") ||
                         (!payload.Contains ("begAngle") && !payload.Contains ("beginAngle")) ||
                         !payload.Contains ("endAngle")))
            return "Arc creation requires 'radius', 'begAngle' and 'endAngle'.";
        for (const char* fieldName : { "linePen", "linePenIndex", "lineTypeIndex" }) {
            error = ValidateIntegerField (payload, fieldName);
            if (error.HasValue ()) return error;
        }
        error = ValidateBooleanField (payload, "reflected");
        if (error.HasValue ()) return error;
        error = ValidateBooleanField (payload, "roomSeparator");
        if (error.HasValue ()) return error;
    } else if (typeID == API_HatchID) {
        const char* polygonField = isCreate ? "hatchCoordinates" : "polygonOutline";
        const auto geometryError = ValidateCoordinateArrayField (payload, polygonField, isCreate, false, 3);
        if (geometryError.HasValue ()) return geometryError;
        GS::Array<GS::ObjectState> coordinates;
        if (payload.Contains (polygonField)) {
            if (!payload.Get (polygonField, coordinates) || coordinates.GetSize () > 64)
                return "Hatch simple polygon must contain at most 64 unclosed vertices.";
            for (GSSize i = 0; i < static_cast<GSSize> (coordinates.GetSize ()); ++i) {
                const API_Coord point = Get2DCoordinateFromObjectState (coordinates[i]);
                const API_Coord next = Get2DCoordinateFromObjectState (coordinates[(i + 1) % coordinates.GetSize ()]);
                if (IsSame2DCoordinate (point, next)) return "Hatch polygon cannot contain a zero-length edge or repeated closing vertex.";
                for (GSSize j = 0; j < i; ++j) {
                    if (IsSame2DCoordinate (point, Get2DCoordinateFromObjectState (coordinates[j])))
                        return "Hatch polygon vertices must be unique.";
                }
            }
            double twiceArea = 0.0;
            for (GSSize i = 0; i < static_cast<GSSize> (coordinates.GetSize ()); ++i) {
                const API_Coord point = Get2DCoordinateFromObjectState (coordinates[i]);
                const API_Coord next = Get2DCoordinateFromObjectState (coordinates[(i + 1) % coordinates.GetSize ()]);
                twiceArea += point.x * next.y - next.x * point.y;
            }
            if (!std::isfinite (twiceArea) || twiceArea == 0.0) return "Hatch polygon must have finite nonzero area.";
        }
        for (const char* fieldName : { "contourPenIndex", "fillPenIndex", "fillBackgroundPenIndex", "fillAttributeIndex" }) {
            const auto integerError = ValidateIntegerField (payload, fieldName);
            if (integerError.HasValue ()) return integerError;
            if (payload.Contains (fieldName)) {
                Int32 value = 0;
                payload.Get (fieldName, value);
                const bool isFillAttribute = GS::UniString (fieldName) == "fillAttributeIndex";
                if (value <= 0 || (!isFillAttribute && value > static_cast<Int32> (std::numeric_limits<short>::max ())))
                    return GS::UniString::Printf ("Hatch '%s' must be a positive supported index.", fieldName);
            }
        }
        if (isCreate) {
            error = ValidateIntegerField (payload, "floorInd");
            if (error.HasValue ()) return error;
            error = ValidateFloorIndexField (payload, "floorInd");
            if (error.HasValue ()) return error;
            for (const char* fieldName : { "contourPenIndex", "fillPenIndex", "fillBackgroundPenIndex", "fillAttributeIndex" })
                if (!payload.Contains (fieldName)) return MissingFieldError (fieldName);
        }
    }
    if (typeID == API_DoorID) {
        if (isCreate) {
            const GS::ObjectState* owner = payload.Get ("ownerWallId");
            if (owner == nullptr || GetGuidFromObjectState (*owner) == APINULLGuid)
                return "Door CREATE requires an exact ownerWallId GUID.";
            if (!payload.Contains ("floorIndex")) return MissingFieldError ("floorIndex");
            error = ValidateFloorIndexField (payload, "floorIndex");
            if (error.HasValue ()) return error;
            return {};
        }
    }

    if (typeID == API_ColumnID) {
        error = ValidateAttributeField (payload, "buildingMaterialId", API_BuildingMaterialID);
        if (error.HasValue ()) return error;
        error = ValidateAttributeField (payload, "profileId", API_ProfileID);
        if (error.HasValue ()) return error;
        if (isCreate && (payload.Contains ("buildingMaterialId") || payload.Contains ("profileId"))) {
            return "Column creation does not yet accept section attribute references; use a typed update after creation.";
        }
    }
    if (typeID == API_BeamID) {
        error = ValidateAttributeField (payload, "buildingMaterialId", API_BuildingMaterialID);
        if (error.HasValue ()) return error;
        error = ValidateAttributeField (payload, "profileId", API_ProfileID);
        if (error.HasValue ()) return error;
        if (isCreate && (payload.Contains ("buildingMaterialId") || payload.Contains ("profileId"))) {
            return "Beam creation does not yet accept section attribute references; use a typed update after creation.";
        }
    }

    return {};
}

} // namespace

namespace ElementMutationInternal {

GS::Optional<GS::UniString> ValidateEnvelopeItem (
    const GS::ObjectState& item,
    const GS::UniString& operation,
    const API_ElemTypeID typeID,
    GS::Array<GS::ObjectState>& normalizedItems,
    GS::Array<API_Guid>& elementGuids)
{
    const GS::ObjectState* payload = item.Get ("payload");
    const GS::ObjectState* elementID = item.Get ("elementId");
    const bool isCreate = operation == "create";
    const bool isUpdate = operation == "update";
    const bool isDelete = operation == "delete";

    if (isCreate) {
        if (elementID != nullptr || item.Contains ("elementId")) {
            return "Create items must not provide an elementId; Archicad generates the GUID.";
        }
        if (payload == nullptr) {
            return MissingFieldError ("payload");
        }
        if (payload->Contains ("elementId")) {
            return "The elementId belongs to the envelope item and is not valid inside a create payload.";
        }
        auto error = ValidatePayload (*payload, typeID, true);
        if (error.HasValue ()) return error;
        if (typeID == API_DoorID) {
            const GS::ObjectState* cloneSource = item.Get ("cloneSourceElementId");
            if (cloneSource == nullptr || GetGuidFromObjectState (*cloneSource) == APINULLGuid)
                return "Door CREATE requires an exact cloneSourceElementId from the same file.";
            API_Elem_Head sourceHeader = {};
            sourceHeader.guid = GetGuidFromObjectState (*cloneSource);
            const GSErrCode sourceError = ACAPI_Element_GetHeader (&sourceHeader);
            if (sourceError != NoError || GetElemTypeId (sourceHeader) != API_DoorID)
                return "Door cloneSourceElementId must resolve to an exact same-file Door.";
            GS::ObjectState normalized;
            normalized.Add ("cloneSourceElementId", *cloneSource);
            normalized.Add ("payload", *payload);
            normalizedItems.Push (normalized);
            return {};
        }
        if (typeID == API_WindowID) {
            const GS::ObjectState* cloneSource = item.Get ("cloneSourceElementId");
            if (cloneSource == nullptr || GetGuidFromObjectState (*cloneSource) == APINULLGuid)
                return "Window CREATE requires an exact cloneSourceElementId from the same file.";
            API_Elem_Head sourceHeader = {};
            sourceHeader.guid = GetGuidFromObjectState (*cloneSource);
            const GSErrCode sourceError = ACAPI_Element_GetHeader (&sourceHeader);
            if (sourceError != NoError || GetElemTypeId (sourceHeader) != API_WindowID)
                return "Window cloneSourceElementId must resolve to an exact same-file Window.";
            GS::ObjectState normalized;
            normalized.Add ("cloneSourceElementId", *cloneSource);
            normalized.Add ("payload", *payload);
            normalizedItems.Push (normalized);
            return {};
        }
        normalizedItems.Push (*payload);
        return {};
    }

    if (elementID == nullptr) {
        return MissingFieldError ("elementId");
    }
    if (GetGuidFromObjectState (*elementID) == APINULLGuid) {
        return "'elementId.guid' must be a valid, non-null GUID.";
    }
    if (isDelete) {
        if (payload != nullptr || item.Contains ("payload")) {
            return "Delete items must not provide a payload.";
        }
    } else if (payload == nullptr) {
        return MissingFieldError ("payload");
    }

    const API_Guid elementGuid = GetGuidFromObjectState (*elementID);
    if (ContainsGuid (elementGuids, elementGuid)) {
        return "The same elementId may appear only once in a mutation request.";
    }

    API_Elem_Head header = {};
    header.guid = elementGuid;
    const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
    if (headerError != NoError) {
        return GS::UniString::Printf ("Element '%T' does not exist.", APIGuidToString (elementGuid).ToPrintf ());
    }
    if (GetElemTypeId (header) != typeID) {
        return GS::UniString::Printf (
            "Element '%T' is not a %s.",
            APIGuidToString (elementGuid).ToPrintf (),
            GetElementTypeNonLocalizedName (typeID).ToPrintf ());
    }

    if (isUpdate) {
        if (payload->Contains ("elementId")) {
            return "The elementId belongs to the envelope item, not to its payload.";
        }
        auto error = ValidatePayload (*payload, typeID, false);
        if (error.HasValue ()) return error;
        GS::ObjectState normalized = *payload;
        normalized.Add ("elementId", *elementID);
        normalizedItems.Push (normalized);
        elementGuids.Push (elementGuid);
        return {};
    }

    GS::ObjectState normalized;
    normalized.Add ("elementId", *elementID);
    normalizedItems.Push (normalized);
    elementGuids.Push (elementGuid);
    return {};
}

} // namespace ElementMutationInternal
