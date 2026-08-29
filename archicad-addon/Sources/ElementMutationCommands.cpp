#include "ElementMutationCommands.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace {

constexpr GSSize MaxMutationItems = 256;

struct ElementTypeSpec {
    API_ElemTypeID typeID;
    const char* createArrayName;
    const char* updateArrayName;
};

const ElementTypeSpec* GetElementTypeSpec (const API_ElemTypeID typeID)
{
    static const ElementTypeSpec wall   { API_WallID,   "wallsData",   "wallsWithDetails" };
    static const ElementTypeSpec slab   { API_SlabID,   "slabsData",   "slabsWithDetails" };
    static const ElementTypeSpec column { API_ColumnID, "columnsData", "columnsWithDetails" };
    static const ElementTypeSpec beam   { API_BeamID,   "beamsData",   "beamsWithDetails" };

    switch (typeID) {
        case API_WallID:   return &wall;
        case API_SlabID:   return &slab;
        case API_ColumnID: return &column;
        case API_BeamID:   return &beam;
        default:           return nullptr;
    }
}

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

bool IsOneOf (const GS::UniString& value, const std::initializer_list<const char*>& allowedValues)
{
    for (const char* allowedValue : allowedValues) {
        if (value == allowedValue) {
            return true;
        }
    }
    return false;
}

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
        "begCoordinate", "endCoordinate", "coordinates", "origin", "floorIndex", "zCoordinate", "level",
        "height", "width", "depth", "thickness", "bottomOffset", "offset", "slantAngle", "arcAngle",
        "verticalCurveHeight", "axisRotationAngle", "profileAngle", "referenceLineLocation", "referencePlaneLocation",
        "structureType", "buildingMaterialId", "compositeId", "profileId", "anchorPoint", "coreAnchor",
        "circleBased", "isWidthAndHeightLinked", "isSlanted", "polygonCoordinates", "polygonOutline", "polygonArcs", "holes"
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

    if (typeID == API_WallID) {
        if (isCreate) {
            return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "floorIndex", "zCoordinate", "height", "thickness", "offset", "arcAngle",
                "referenceLineLocation", "structureType", "buildingMaterialId", "compositeId", "profileId"
            });
        }
        return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "arcAngle", "height", "thickness", "bottomOffset", "offset",
                "structureType", "buildingMaterialId", "compositeId", "profileId"
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
    } else {
        return "Unsupported element type.";
    }
}

GS::Optional<GS::UniString> ValidatePayload (
    const GS::ObjectState& payload,
    const API_ElemTypeID typeID,
    const bool isCreate)
{
    auto error = ValidateAllowedPayloadFields (payload, typeID, isCreate);
    if (error.HasValue ()) return error;

    const char* finiteFields[] = {
        "zCoordinate", "level", "bottomOffset", "offset", "slantAngle", "arcAngle",
        "verticalCurveHeight", "axisRotationAngle", "profileAngle"
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
            return "Wall update uses 'bottomOffset'; floorIndex/zCoordinate are creation fields.";
        }
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

GS::ObjectState ExecuteCreate (
    const API_ElemTypeID typeID,
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl)
{
    switch (typeID) {
        case API_WallID:   return CreateWallsCommand ().Execute (parameters, processControl);
        case API_SlabID:   return CreateSlabsCommand ().Execute (parameters, processControl);
        case API_ColumnID: return CreateColumnsCommand ().Execute (parameters, processControl);
        case API_BeamID:   return CreateBeamsCommand ().Execute (parameters, processControl);
        default:           return CreateErrorResponse (APIERR_BADID, "Unsupported element type for create.");
    }
}

GS::ObjectState ExecuteUpdate (
    const API_ElemTypeID typeID,
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl)
{
    switch (typeID) {
        case API_WallID:   return ModifyWallsCommand ().Execute (parameters, processControl);
        case API_SlabID:   return ModifySlabsCommand ().Execute (parameters, processControl);
        case API_ColumnID: return ModifyColumnsCommand ().Execute (parameters, processControl);
        case API_BeamID:   return ModifyBeamsCommand ().Execute (parameters, processControl);
        default:           return CreateErrorResponse (APIERR_BADID, "Unsupported element type for update.");
    }
}

GS::ObjectState ExecuteDelete (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl)
{
    return DeleteElementsCommand ().Execute (BuildArrayParameters ("elements", items), processControl);
}

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
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

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
        for (const GS::ObjectState& detailRow : detailRows) {
            const GS::ObjectState* detail = detailRow.Get ("details");
            if (detail == nullptr || detail->Get ("error") != nullptr) {
                verified = false;
                break;
            }
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
        const bool stillExists = ACAPI_Element_GetHeader (&header) == NoError;
        const bool absent = !stillExists;
        GS::ObjectState status;
        status.Add ("elementId", CreateGuidObjectState (guid));
        status.Add ("absent", absent);
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

GS::Array<API_Guid> CollectCreatedGuids (const GS::ObjectState& mutationResult)
{
    GS::Array<API_Guid> guids;
    GS::Array<GS::ObjectState> elements;
    if (!mutationResult.Get ("elements", elements)) {
        return guids;
    }
    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementID = element.Get ("elementId");
        if (elementID != nullptr) {
            const API_Guid guid = GetGuidFromObjectState (*elementID);
            if (guid != APINULLGuid) {
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

bool MutationResultIsComplete (
    const GS::UniString& operation,
    const GS::ObjectState& mutationResult,
    const GSSize requestedCount)
{
    if (mutationResult.Get ("error") != nullptr) {
        return false;
    }
    if (operation == "create") {
        GS::Array<GS::ObjectState> elements;
        if (!mutationResult.Get ("elements", elements) || static_cast<GSSize> (elements.GetSize ()) != requestedCount) {
            return false;
        }
        for (const GS::ObjectState& element : elements) {
            if (element.Get ("elementId") == nullptr) {
                return false;
            }
        }
        return true;
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

} // namespace

MutateElementsCommand::MutateElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String MutateElementsCommand::GetName () const
{
    return "MutateElements";
}

GS::Optional<GS::UniString> MutateElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "description": "Typed native CRUD envelope. One operation and one element type per request.",
        "properties": {
            "operation": {
                "type": "string",
                "enum": ["create", "update", "delete"]
            },
            "elementType": {
                "type": "string",
                "enum": ["Wall", "Slab", "Column", "Beam"]
            },
            "items": {
                "type": "array",
                "minItems": 1,
                "maxItems": 256,
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "payload": {
                            "type": "object",
                            "properties": {
                                "begCoordinate": { "$ref": "#/Coordinate2D" },
                                "endCoordinate": { "$ref": "#/Coordinate2D" },
                                "coordinates": { "$ref": "#/Coordinate3D" },
                                "origin": { "$ref": "#/Coordinate2D" },
                                "floorIndex": { "type": "integer" },
                                "zCoordinate": { "type": "number" },
                                "level": { "type": "number" },
                                "height": { "type": "number" },
                                "width": { "type": "number" },
                                "depth": { "type": "number" },
                                "thickness": { "type": "number" },
                                "bottomOffset": { "type": "number" },
                                "offset": { "type": "number" },
                                "slantAngle": { "type": "number" },
                                "isSlanted": { "type": "boolean" },
                                "profileAngle": { "type": "number" },
                                "arcAngle": { "type": "number" },
                                "verticalCurveHeight": { "type": "number" },
                                "axisRotationAngle": { "type": "number" },
                                "referenceLineLocation": { "type": "string" },
                                "referencePlaneLocation": { "type": "string" },
                                "structureType": { "type": "string" },
                                "buildingMaterialId": { "$ref": "#/AttributeId" },
                                "compositeId": { "$ref": "#/AttributeId" },
                                "profileId": { "$ref": "#/AttributeId" },
                                "anchorPoint": { "type": "string" },
                                "coreAnchor": { "type": "string" },
                                "circleBased": { "type": "boolean" },
                                "isWidthAndHeightLinked": { "type": "boolean" },
                                "polygonCoordinates": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3
                                },
                                "polygonOutline": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3
                                },
                                "polygonArcs": {
                                    "type": "array",
                                    "items": { "$ref": "#/PolyArc" }
                                },
                                "holes": { "$ref": "#/Holes2D" }
                            },
                            "additionalProperties": false
                        }
                    },
                    "additionalProperties": false
                }
            }
        },
        "additionalProperties": false,
        "required": ["operation", "elementType", "items"]
    })";
}

GS::Optional<GS::UniString> MutateElementsCommand::GetResponseSchema () const
{
    // The command returns either the typed success envelope below or the
    // framework-wide {"error": {code, message}} response from preflight.
    // Handing only the success schema to Archicad would make it discard the
    // useful error response as a schema-validation failure.
    return {};
}

GS::Optional<GS::UniString> MutateElementsCommand::GetRawResponseSchema () const
{
    // nativeDetails intentionally remains an open object.  It is the existing
    // detail projection and can contain bounded, type-specific fields.  The
    // envelope itself remains schema-checked, including explicit partial and
    // readback status.
    return R"({
        "type": "object",
        "properties": {
            "operation": { "type": "string", "enum": ["create", "update", "delete"] },
            "elementType": { "type": "string", "enum": ["Wall", "Slab", "Column", "Beam"] },
            "requestedCount": { "type": "integer", "minimum": 0 },
            "appliedCount": { "type": "integer", "minimum": 0 },
            "mutationComplete": { "type": "boolean" },
            "readbackVerified": { "type": "boolean" },
            "partial": { "type": "boolean" },
            "mutationResult": { "type": "object" },
            "readback": { "type": "object" }
        },
        "additionalProperties": false,
        "required": [
            "operation", "elementType", "requestedCount", "appliedCount", "mutationComplete",
            "readbackVerified", "partial", "mutationResult", "readback"
        ]
    })";
}

GS::ObjectState MutateElementsCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl) const
{
    GS::UniString operation;
    if (!parameters.Get ("operation", operation) || !IsOneOf (operation, { "create", "update", "delete" })) {
        return CreateErrorResponse (APIERR_BADPARS, "'operation' must be one of 'create', 'update' or 'delete'.");
    }

    GS::UniString elementTypeName;
    if (!parameters.Get ("elementType", elementTypeName)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'elementType' field.");
    }
    const API_ElemTypeID typeID = GetElementTypeFromNonLocalizedName (elementTypeName);
    const ElementTypeSpec* typeSpec = GetElementTypeSpec (typeID);
    if (typeSpec == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "'elementType' must be Wall, Slab, Column or Beam.");
    }

    GS::Array<GS::ObjectState> items;
    if (!parameters.Get ("items", items)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required array field 'items'.");
    }
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) {
        return CreateErrorResponse (APIERR_BADPARS, "'items' must contain between 1 and 256 entries.");
    }

    GS::Array<GS::ObjectState> normalizedItems;
    GS::Array<API_Guid> requestedElementGuids;
    for (const GS::ObjectState& item : items) {
        auto error = ValidateEnvelopeItem (item, operation, typeID, normalizedItems, requestedElementGuids);
        if (error.HasValue ()) {
            // The complete preflight runs before any delegated executor is
            // called.  Invalid enum/range/reference/conflict input therefore
            // cannot result in a native mutation.
            return CreateErrorResponse (APIERR_BADPARS, error.Get ());
        }
    }

    GS::ObjectState mutationParameters;
    if (operation == "create") {
        mutationParameters = BuildArrayParameters (typeSpec->createArrayName, normalizedItems);
    } else if (operation == "update") {
        mutationParameters = BuildArrayParameters (typeSpec->updateArrayName, normalizedItems);
    }

    GS::ObjectState mutationResult;
    if (operation == "create") {
        mutationResult = ExecuteCreate (typeID, mutationParameters, processControl);
    } else if (operation == "update") {
        mutationResult = ExecuteUpdate (typeID, mutationParameters, processControl);
    } else {
        mutationResult = ExecuteDelete (normalizedItems, processControl);
    }

    GS::Array<API_Guid> changedGuids;
    if (operation == "create") {
        changedGuids = CollectCreatedGuids (mutationResult);
    } else if (operation == "update") {
        changedGuids = CollectUpdatedGuids (mutationResult, requestedElementGuids);
    } else {
        changedGuids = requestedElementGuids;
    }

    const bool mutationComplete = MutationResultIsComplete (operation, mutationResult, items.GetSize ());
    // A partial update must still expose the observed state of every requested
    // element, including items whose delegated executor reported failure.  A
    // readback of successful items only can make a failed scalar/memo update
    // look as if the element had never been part of the operation.
    GS::Array<API_Guid> readbackGuids = changedGuids;
    if (operation == "update" && !mutationComplete) {
        readbackGuids = requestedElementGuids;
    }

    bool readbackVerified = true;
    GSSize appliedCount = static_cast<GSSize> (changedGuids.GetSize ());
    GS::ObjectState readback;
    if (operation == "delete") {
        readback = BuildDeleteReadback (changedGuids, typeID, readbackVerified, appliedCount);
    } else {
        readback = BuildElementReadback (readbackGuids, typeID, processControl, readbackVerified);
    }

    GS::ObjectState response;
    response.Add ("operation", operation);
    response.Add ("elementType", elementTypeName);
    response.Add ("requestedCount", static_cast<Int32> (items.GetSize ()));
    response.Add ("appliedCount", static_cast<Int32> (appliedCount));
    response.Add ("mutationComplete", mutationComplete);
    response.Add ("readbackVerified", readbackVerified);
    response.Add ("partial", !mutationComplete || !readbackVerified);
    response.Add ("mutationResult", mutationResult);
    response.Add ("readback", readback);
    return response;
}
