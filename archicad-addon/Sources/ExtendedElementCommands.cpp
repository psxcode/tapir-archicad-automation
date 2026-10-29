#include "ExtendedElementCommands.hpp"
#include "NativeUndoable.hpp"

#include "MigrationHelper.hpp"
#include "NotificationCommands.hpp"
#include "NativeOwnership.hpp"
#include "ObjectStateJSONConversion.hpp"
#include "NativeDimensionFacts.hpp"
#include "NativeDimensionAuditPolicy.hpp"
#include "NativeDimensionLayerControlPolicy.hpp"
#include "NativeDimensionUtf16.hpp"
#include "NativeDimensionCreateWire.hpp"
#include "NativeDimensionCreateWireObjectState.hpp"
#include "NativeDimensionCreateCandidateFacts.hpp"
#include "GSProcessControl.hpp"

#ifdef ServerMainVers_2900
#include	"ACAPI/Element/Opening/OpeningDefault.hpp"
#include	"ACAPI/Element/Opening/Opening.hpp"
#include "Polygon2D.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <numeric>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE 0
#endif
#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT 0
#endif

namespace {

constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;

using NativeDimensionFacts::NativeDimensionFactualFingerprint;

enum class StructureSelectionKind {
    Unspecified,
    Basic,
    Composite,
    Profile
};

struct StructureSelection {
    StructureSelectionKind kind = StructureSelectionKind::Unspecified;
    API_AttributeIndex buildingMaterial = APIInvalidAttributeIndex;
    API_AttributeIndex composite = APIInvalidAttributeIndex;
    API_AttributeIndex profile = APIInvalidAttributeIndex;
};

struct AssociativeDimensionPoint {
    API_Guid elementGuid = APINULLGuid;
    API_ElemTypeID elementType = API_ZombieElemID;
    bool line = false;
    Int32 inIndex = 0;
    char special = 0;
    short nodeType = 0;
    short nodeStatus = 0;
    UInt32 nodeId = 0;
};

enum class SectionAssociativeDimensionPreset {
    WallCompositeFaces,
    WallSkinBorders,
    SlabCompositeFaces,
    SlabSkinBorders,
    BeamOrColumnRefLineEndPoints,
    BeamOrColumnBoundingBoxCorners,
    DoorWindowWallHoleCorners,
    DoorWindowModelHotspots
};

GS::Optional<double> GetOptionalDouble (const GS::ObjectState& parameters, const char* fieldName)
{
    double value = 0.0;
    if (parameters.Get (fieldName, value)) {
        return value;
    }
    return {};
}

GS::Optional<GS::ObjectState> GetOptionalObjectState (const GS::ObjectState& parameters, const char* fieldName)
{
    const GS::ObjectState* value = parameters.Get (fieldName);
    if (value == nullptr) {
        return {};
    }
    return *value;
}

GS::Optional<API_Coord> GetOptionalCoordinate2D (const GS::ObjectState& parameters, const char* fieldName)
{
    const GS::ObjectState* coord = parameters.Get (fieldName);
    if (coord == nullptr) {
        return {};
    }
    return Get2DCoordinateFromObjectState (*coord);
}

GS::Optional<GS::UniString> GetElementArray (const GS::ObjectState& parameters, const char* fieldName, GS::Array<GS::ObjectState>& outArray)
{
    if (!parameters.Get (fieldName, outArray)) {
        return GS::UniString::Printf ("Missing required array field '%s'.", fieldName);
    }
    return {};
}

GS::Optional<API_Coord3D> GetOptionalCoordinate3D (const GS::ObjectState& parameters, const char* fieldName)
{
    const GS::ObjectState* coord = parameters.Get (fieldName);
    if (coord == nullptr) {
        return {};
    }
    return Get3DCoordinateFromObjectState (*coord);
}

bool ResolveAttributeIndex (const GS::ObjectState& attributeId, API_AttrTypeID attributeType, API_AttributeIndex& attributeIndex)
{
    API_Attribute attribute = {};
    attribute.header.typeID = attributeType;
    attribute.header.guid = GetGuidFromObjectState (attributeId);
    if (attribute.header.guid == APINULLGuid) {
        return false;
    }

    if (ACAPI_Attribute_Get (&attribute) != NoError) {
        return false;
    }

    attributeIndex = attribute.header.index;
    return true;
}

GS::Optional<GS::UniString> TryResolveAttributeField (
    const GS::ObjectState& parameters,
    const char* fieldName,
    API_AttrTypeID attributeType,
    bool& hasValue,
    API_AttributeIndex& outIndex)
{
    hasValue = false;

    const GS::ObjectState* attributeId = parameters.Get (fieldName);
    if (attributeId == nullptr) {
        return {};
    }

    hasValue = true;
    if (!ResolveAttributeIndex (*attributeId, attributeType, outIndex)) {
        return GS::UniString::Printf ("Invalid attribute reference in '%s'.", fieldName);
    }

    return {};
}

GS::Optional<GS::UniString> ParseStructureSelection (
    const GS::ObjectState& parameters,
    bool allowComposite,
    bool allowProfile,
    StructureSelection& selection)
{
    GS::UniString structureType;
    const bool hasStructureType = parameters.Get ("structureType", structureType);

    bool hasBuildingMaterial = false;
    bool hasComposite = false;
    bool hasProfile = false;

    {
        auto err = TryResolveAttributeField (parameters, "buildingMaterialId", API_BuildingMaterialID, hasBuildingMaterial, selection.buildingMaterial);
        if (err.HasValue ()) {
            return err;
        }
    }
    {
        auto err = TryResolveAttributeField (parameters, "compositeId", API_CompWallID, hasComposite, selection.composite);
        if (err.HasValue ()) {
            return err;
        }
    }
    {
        auto err = TryResolveAttributeField (parameters, "profileId", API_ProfileID, hasProfile, selection.profile);
        if (err.HasValue ()) {
            return err;
        }
    }

    const int explicitlyProvidedKinds = static_cast<int> (hasBuildingMaterial) + static_cast<int> (hasComposite) + static_cast<int> (hasProfile);
    if (explicitlyProvidedKinds > 1) {
        return "Only one of 'buildingMaterialId', 'compositeId' or 'profileId' may be provided at a time.";
    }

    if (hasComposite && !allowComposite) {
        return "'compositeId' is not supported for this element type.";
    }
    if (hasProfile && !allowProfile) {
        return "'profileId' is not supported for this element type.";
    }

    if (hasStructureType) {
        if (structureType == "Basic") {
            selection.kind = StructureSelectionKind::Basic;
        } else if (structureType == "Composite") {
            if (!allowComposite) {
                return "'structureType=Composite' is not supported for this element type.";
            }
            selection.kind = StructureSelectionKind::Composite;
        } else if (structureType == "Profile") {
            if (!allowProfile) {
                return "'structureType=Profile' is not supported for this element type.";
            }
            selection.kind = StructureSelectionKind::Profile;
        } else {
            return "Invalid 'structureType'. Use 'Basic', 'Composite' or 'Profile'.";
        }
    } else if (hasBuildingMaterial) {
        selection.kind = StructureSelectionKind::Basic;
    } else if (hasComposite) {
        selection.kind = StructureSelectionKind::Composite;
    } else if (hasProfile) {
        selection.kind = StructureSelectionKind::Profile;
    }

    if (selection.kind == StructureSelectionKind::Basic && (hasComposite || hasProfile)) {
        return "'structureType=Basic' cannot be combined with 'compositeId' or 'profileId'.";
    }
    if (selection.kind == StructureSelectionKind::Composite && (hasBuildingMaterial || hasProfile)) {
        return "'structureType=Composite' cannot be combined with 'buildingMaterialId' or 'profileId'.";
    }
    if (selection.kind == StructureSelectionKind::Profile && (hasBuildingMaterial || hasComposite)) {
        return "'structureType=Profile' cannot be combined with 'buildingMaterialId' or 'compositeId'.";
    }

    return {};
}

static GS::Optional<GS::UniString> ValidateFiniteNumberField (
    const GS::ObjectState& parameters,
    const char* fieldName,
    bool requirePositive)
{
    if (!parameters.Contains (fieldName)) {
        return {};
    }

    double value = 0.0;
    if (!parameters.Get (fieldName, value) || !std::isfinite (value) || (requirePositive && value <= 0.0)) {
        return GS::UniString::Printf (
            "'%s' must be a finite %s number.",
            fieldName,
            requirePositive ? "positive" : "real");
    }

    return {};
}

static GS::Optional<GS::UniString> ValidateBooleanField (
    const GS::ObjectState& parameters,
    const char* fieldName)
{
    if (!parameters.Contains (fieldName)) {
        return {};
    }

    bool value = false;
    if (!parameters.Get (fieldName, value)) {
        return GS::UniString::Printf ("'%s' must be a boolean.", fieldName);
    }

    return {};
}

static GS::Optional<GS::UniString> ValidateAttributeField (
    const GS::ObjectState& parameters,
    const char* fieldName,
    API_AttrTypeID attributeType)
{
    if (!parameters.Contains (fieldName)) {
        return {};
    }

    const GS::ObjectState* attributeId = parameters.Get (fieldName);
    API_AttributeIndex attributeIndex = APIInvalidAttributeIndex;
    if (attributeId == nullptr || !ResolveAttributeIndex (*attributeId, attributeType, attributeIndex)) {
        return GS::UniString::Printf ("Invalid attribute reference in '%s'.", fieldName);
    }

    return {};
}

static GS::Optional<GS::UniString> ValidateColumnSectionPayload (const GS::ObjectState& details)
{
    auto error = ValidateFiniteNumberField (details, "width", true);
    if (error.HasValue ()) return error;
    error = ValidateFiniteNumberField (details, "depth", true);
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (details, "circleBased");
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (details, "isWidthAndHeightLinked");
    if (error.HasValue ()) return error;

    const bool hasBuildingMaterial = details.Contains ("buildingMaterialId");
    const bool hasProfile = details.Contains ("profileId");
    if (hasBuildingMaterial && hasProfile) {
        return "Only one of 'buildingMaterialId' or 'profileId' may be provided for a column section.";
    }
    bool circleBased = false;
    if (hasProfile && details.Get ("circleBased", circleBased) && circleBased) {
        return "'circleBased=true' cannot be combined with 'profileId' for a column section.";
    }

    error = ValidateAttributeField (details, "buildingMaterialId", API_BuildingMaterialID);
    if (error.HasValue ()) return error;
    return ValidateAttributeField (details, "profileId", API_ProfileID);
}

static GS::Optional<GS::UniString> ValidateBeamSectionPayload (const GS::ObjectState& details)
{
    auto error = ValidateFiniteNumberField (details, "width", true);
    if (error.HasValue ()) return error;
    error = ValidateFiniteNumberField (details, "height", true);
    if (error.HasValue ()) return error;
    error = ValidateBooleanField (details, "isWidthAndHeightLinked");
    if (error.HasValue ()) return error;

    const bool hasBuildingMaterial = details.Contains ("buildingMaterialId");
    const bool hasProfile = details.Contains ("profileId");
    if (hasBuildingMaterial && hasProfile) {
        return "Only one of 'buildingMaterialId' or 'profileId' may be provided for a beam section.";
    }

    error = ValidateAttributeField (details, "buildingMaterialId", API_BuildingMaterialID);
    if (error.HasValue ()) return error;
    return ValidateAttributeField (details, "profileId", API_ProfileID);
}

static bool HasColumnSectionFields (const GS::ObjectState& details)
{
    return details.Contains ("width") || details.Contains ("depth") || details.Contains ("circleBased") ||
           details.Contains ("isWidthAndHeightLinked") || details.Contains ("buildingMaterialId") ||
           details.Contains ("profileId");
}

static bool HasBeamSectionFields (const GS::ObjectState& details)
{
    return details.Contains ("width") || details.Contains ("height") || details.Contains ("isWidthAndHeightLinked") ||
           details.Contains ("buildingMaterialId") || details.Contains ("profileId");
}

static GSErrCode ValidateColumnSupportedUpdateState (const API_Element& element, const GS::ObjectState& patch)
{
    if (element.column.isSlanted || !std::isfinite (element.column.slantAngle)
        || !std::isfinite (element.column.origoPos.x) || !std::isfinite (element.column.origoPos.y)
        || !std::isfinite (element.column.bottomOffset) || !std::isfinite (element.column.height)
        || element.column.height <= 0.0 || !std::isfinite (element.column.axisRotationAngle))
        return APIERR_NOTSUPPORTED;
    if (patch.Contains ("height") && element.column.relativeTopStory != 0)
        return APIERR_NOTSUPPORTED;

    API_ElementMemo memo = {};
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

    const GSErrCode err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_ColumnSegment);
    if (err != NoError) {
        return err;
    }
    if (memo.columnSegments == nullptr) {
        return APIERR_BADPARS;
    }
    const GSSize bytes = BMGetPtrSize (reinterpret_cast<GSPtr> (memo.columnSegments));
    if (bytes == 0 || bytes % sizeof (API_ColumnSegmentType) != 0
        || bytes / sizeof (API_ColumnSegmentType) != 1)
        return APIERR_NOTSUPPORTED;

    const API_AssemblySegmentData& segment = memo.columnSegments[0].assemblySegmentData;
    if (segment.circleBased || segment.modelElemStructureType != API_BasicStructure
        || segment.profileType != APISect_Normal || !segment.isHomogeneous
        || !std::isfinite (segment.nominalWidth) || segment.nominalWidth <= 0.0
        || !std::isfinite (segment.nominalHeight) || segment.nominalHeight <= 0.0
        || segment.buildingMaterial == APIInvalidAttributeIndex
        || GetAttributeGuidFromIndex (API_BuildingMaterialID, segment.buildingMaterial) == APINULLGuid)
        return APIERR_NOTSUPPORTED;
    if (HasColumnSectionFields (patch) && segment.isWidthAndHeightLinked)
        return APIERR_NOTSUPPORTED;
    return NoError;
}

static GSErrCode ValidateBeamSectionMemo (API_Guid elementGuid)
{
    API_ElementMemo memo = {};
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

    const GSErrCode err = ACAPI_Element_GetMemo (elementGuid, &memo, APIMemoMask_BeamSegment);
    if (err != NoError) {
        return err;
    }
    if (memo.beamSegments == nullptr ||
        BMGetPtrSize (reinterpret_cast<GSPtr> (memo.beamSegments)) < sizeof (API_BeamSegmentType)) {
        return APIERR_BADPARS;
    }
    return NoError;
}

static GSErrCode ApplyColumnSectionToMemo (API_Guid elementGuid, const GS::ObjectState& details)
{
    API_ElementMemo memo = {};
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

    GSErrCode err = ACAPI_Element_GetMemo (elementGuid, &memo, APIMemoMask_ColumnSegment);
    if (err != NoError) {
        return err;
    }
    if (memo.columnSegments == nullptr) {
        return APIERR_BADPARS;
    }

    double width = 0.0;
    double depth = 0.0;
    const bool hasWidth = details.Get ("width", width);
    const bool hasDepth = details.Get ("depth", depth);
    bool circleBased = false;
    const bool hasCircleBased = details.Get ("circleBased", circleBased);
    bool isWidthAndHeightLinked = false;
    const bool hasIsWidthAndHeightLinked = details.Get ("isWidthAndHeightLinked", isWidthAndHeightLinked);

    API_AttributeIndex buildingMaterial = APIInvalidAttributeIndex;
    API_AttributeIndex profile = APIInvalidAttributeIndex;
    const GS::ObjectState* buildingMaterialId = details.Get ("buildingMaterialId");
    const GS::ObjectState* profileId = details.Get ("profileId");
    if (buildingMaterialId != nullptr && !ResolveAttributeIndex (*buildingMaterialId, API_BuildingMaterialID, buildingMaterial)) {
        return APIERR_BADPARS;
    }
    if (profileId != nullptr && !ResolveAttributeIndex (*profileId, API_ProfileID, profile)) {
        return APIERR_BADPARS;
    }

    const GSSize segmentCount = BMGetPtrSize (reinterpret_cast<GSPtr> (memo.columnSegments)) / sizeof (API_ColumnSegmentType);
    if (segmentCount == 0) {
        return APIERR_BADPARS;
    }

    for (GSSize i = 0; i < segmentCount; ++i) {
        API_AssemblySegmentData& segment = memo.columnSegments[i].assemblySegmentData;
        if (hasIsWidthAndHeightLinked) {
            segment.isWidthAndHeightLinked = isWidthAndHeightLinked;
        } else if (hasWidth && hasDepth) {
            // Treat two explicit dimensions as independent unless the caller asks
            // for linked dimensions.  Archicad defaults can otherwise make the
            // second assignment overwrite the first one.
            segment.isWidthAndHeightLinked = false;
        }
        if (hasWidth) {
            segment.nominalWidth = width;
        }
        if (hasDepth) {
            segment.nominalHeight = depth;
        }
        if (hasCircleBased) {
            segment.circleBased = circleBased;
        }
        if (profileId != nullptr) {
            segment.modelElemStructureType = API_ProfileStructure;
            segment.profileAttr = profile;
            segment.buildingMaterial = APIInvalidAttributeIndex;
            segment.circleBased = false;
        } else if (buildingMaterialId != nullptr) {
            segment.modelElemStructureType = API_BasicStructure;
            segment.buildingMaterial = buildingMaterial;
            segment.profileAttr = APIInvalidAttributeIndex;
        }
    }

    return ACAPI_Element_ChangeMemo (elementGuid, APIMemoMask_ColumnSegment, &memo);
}

static GSErrCode ApplyBeamSectionToMemo (API_Guid elementGuid, const GS::ObjectState& details)
{
    API_ElementMemo memo = {};
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

    GSErrCode err = ACAPI_Element_GetMemo (elementGuid, &memo, APIMemoMask_BeamSegment);
    if (err != NoError) {
        return err;
    }
    if (memo.beamSegments == nullptr) {
        return APIERR_BADPARS;
    }

    double width = 0.0;
    double height = 0.0;
    const bool hasWidth = details.Get ("width", width);
    const bool hasHeight = details.Get ("height", height);
    bool isWidthAndHeightLinked = false;
    const bool hasIsWidthAndHeightLinked = details.Get ("isWidthAndHeightLinked", isWidthAndHeightLinked);

    API_AttributeIndex buildingMaterial = APIInvalidAttributeIndex;
    API_AttributeIndex profile = APIInvalidAttributeIndex;
    const GS::ObjectState* buildingMaterialId = details.Get ("buildingMaterialId");
    const GS::ObjectState* profileId = details.Get ("profileId");
    if (buildingMaterialId != nullptr && !ResolveAttributeIndex (*buildingMaterialId, API_BuildingMaterialID, buildingMaterial)) {
        return APIERR_BADPARS;
    }
    if (profileId != nullptr && !ResolveAttributeIndex (*profileId, API_ProfileID, profile)) {
        return APIERR_BADPARS;
    }

    const GSSize segmentCount = BMGetPtrSize (reinterpret_cast<GSPtr> (memo.beamSegments)) / sizeof (API_BeamSegmentType);
    if (segmentCount == 0) {
        return APIERR_BADPARS;
    }

    for (GSSize i = 0; i < segmentCount; ++i) {
        API_AssemblySegmentData& segment = memo.beamSegments[i].assemblySegmentData;
        if (hasIsWidthAndHeightLinked) {
            segment.isWidthAndHeightLinked = isWidthAndHeightLinked;
        } else if (hasWidth && hasHeight) {
            segment.isWidthAndHeightLinked = false;
        }
        if (hasWidth) {
            segment.nominalWidth = width;
        }
        if (hasHeight) {
            segment.nominalHeight = height;
        }
        if (profileId != nullptr) {
            segment.modelElemStructureType = API_ProfileStructure;
            segment.profileAttr = profile;
            segment.buildingMaterial = APIInvalidAttributeIndex;
        } else if (buildingMaterialId != nullptr) {
            segment.modelElemStructureType = API_BasicStructure;
            segment.buildingMaterial = buildingMaterial;
            segment.profileAttr = APIInvalidAttributeIndex;
        }
    }

    return ACAPI_Element_ChangeMemo (elementGuid, APIMemoMask_BeamSegment, &memo);
}

void SetOpeningSizeMask (API_Element& mask)
{
    ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width);
    ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height);
}

bool DoesWallExist (const API_Guid& wallGuid)
{
    return DoesElementExist (wallGuid, API_WallID);
}

// Archicad cannot place a Door or Window in a polygonal wall. Letting the call through
// produces a successful-looking element at a fixed location, ignoring centerOffset.
// Return the read status separately so a failed native read cannot be treated as a
// non-polygonal wall and accidentally fall through to CreateExt.
GSErrCode GetWallPolygonalState (const API_Guid& wallGuid, bool& isPolygonal)
{
    isPolygonal = false;
    API_Element wall = {};
#ifdef ServerMainVers_2600
    wall.header.type = API_WallID;
#else
    wall.header.typeID = API_WallID;
#endif
    wall.header.guid = wallGuid;
    const GSErrCode err = ACAPI_Element_Get (&wall);
    if (err != NoError) {
        return err;
    }
    isPolygonal = wall.wall.type == APIWtyp_Poly;
    return NoError;
}

// CreateOpenings builds its base polygon from width and height. Reject missing or
// non-positive dimensions before an empty/invalid polygon reaches Archicad. Window and
// Door sizes remain optional because their tool defaults/favorites provide them.
GS::Optional<GS::UniString> CheckOpeningSize (const GS::ObjectState& data)
{
    const auto width = GetOptionalDouble (data, "width");
    const auto height = GetOptionalDouble (data, "height");
    if (!width.HasValue () || !height.HasValue ()) {
        return GS::UniString ("Both 'width' and 'height' are required to create an opening.");
    }
    if (!std::isfinite (width.Get ()) || !std::isfinite (height.Get ()) || width.Get () <= 0.0 || height.Get () <= 0.0) {
        return GS::UniString ("'width' and 'height' must be finite and greater than zero.");
    }
    return {};
}

GSErrCode PrepareWindowOrDoorDefaults (API_ElemTypeID elemTypeId, API_Element& element, API_ElementMemo& memo, API_SubElement& marker)
{
    element = {};
    marker = {};
#ifdef ServerMainVers_2600
    element.header.type = elemTypeId;
#else
    element.header.typeID = elemTypeId;
#endif
    marker.subType = APISubElement_MainMarker;

    GSErrCode err = ACAPI_Element_GetDefaultsExt (&element, &memo, 1UL, &marker);
    if (err != NoError) {
        return err;
    }

    API_LibPart libPart = {};
    LibraryPartLocationGuard libPartLocationGuard (libPart);
#ifdef ServerMainVers_2700
    err = ACAPI_LibraryPart_GetMarkerParent (element.header.type, libPart);
#elif ServerMainVers_2600
    err = ACAPI_Goodies_GetMarkerParent (element.header.type, libPart);
#else
    err = ACAPI_Goodies (APIAny_GetMarkerParentID, (void*)&element.header.typeID, (void*)&libPart);
#endif
    if (err != NoError) {
        return NoError;
    }

    err = ACAPI_LibraryPart_Search (&libPart, false, true);
    if (err != NoError) {
        return err;
    }

    double a = 0.0;
    double b = 0.0;
    Int32 addParNum = 0;
    API_AddParType** markAddPars = nullptr;
    err = ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &markAddPars);
    if (err != NoError) {
        return err;
    }

    marker.memo.params = markAddPars;
    marker.subElem.object.pen = 166;
    marker.subElem.object.useObjPens = true;
    return NoError;
}

// Apply a named FAVORITE to the Door/Window tool defaults BEFORE
// the caller clones element/memo/marker via PrepareWindowOrDoorDefaults.
//
// The parameter is a FAVORITE name (as returned by `GetFavoritesByType
// (elementType=Door|Window)`), NOT a libpart `docu_UName`. Real
// libpart-by-name lookup against AC29's Door/Window library is not
// viable: the favorite name is the only stable identifier exposed to
// API callers.
//
// This is the workaround for `-2130313110 Failed to create door` /
// `Failed to create window` on a fresh project where the user has
// never opened the Door / Window tool — without the favorite-apply,
// `PrepareWindowOrDoorDefaults` clones empty/invalid tool defaults
// that AC's `ACAPI_Element_CreateExt` rejects. Calling this helper
// first pushes a known-good defaults snapshot into the tool, then
// the caller's PrepareWindowOrDoorDefaults clones the favorite-applied
// state (libpart, marker memo, all openingBase fields).
//
// Flow (mirrors `ApplyFavoritesToElementDefaultsCommand`):
//   1. `ACAPI_Favorite_Get(&favorite)` by name
//   2. `ACAPI_Element_ChangeDefaultsExt` with mask FULL — also passes
//      the favorite's `elementMarker` / `memoMarker` as a Main Marker
//      sub-element when present, since AC25+ recommends ChangeDefaultsExt
//      for markered element types (Door / Window).
//   3. Replay classifications, category values, user properties via
//      the existing TAPIR_Element_* helpers in `MigrationHelper.hpp`.
//      Required: some Door favorites (e.g. `Porte d'entrée`) still
//      return REFUSEDPAR after ChangeDefaultsExt alone if the
//      classifications are missing.
//
// IMPORTANT: must be invoked BEFORE PrepareWindowOrDoorDefaults so
// the marker built from `ACAPI_LibraryPart_GetMarkerParent` matches
// the favorite-applied libpart. Calling it AFTER leaves the outer
// marker stale and CreateExt fails with REFUSEDPAR.
//
// Returns NoError on success or when `favoriteName` is absent
// (caller falls back to the cloned tool defaults). Returns
// APIERR_REFUSEDPAR when the favorite resolves to a different
// element type than `expectedTypeId` (e.g. passing a Window favorite
// to CreateDoors). Otherwise returns the underlying Favorite_Get /
// ChangeDefaultsExt error code.
static GSErrCode ApplyWindowOrDoorFavoriteToDefaults (const GS::ObjectState& data, API_ElemTypeID expectedTypeId)
{
    GS::UniString favoriteName;
    if (!data.Get ("favoriteName", favoriteName)) {
        return NoError; // field absent — keep tool defaults as-is
    }
    if (favoriteName.IsEmpty ()) {
        return NoError;
    }

    API_Favorite favorite;
    favorite.name = favoriteName;
    favorite.memo.New ();
    favorite.elementMarker.New ();
    favorite.memoMarker.New ();
    favorite.properties.New ();
    favorite.classifications.New ();
    favorite.elemCategoryValues.New ();

    GSErrCode err = ACAPI_Favorite_Get (&favorite);
    const auto disposeFavoriteMemos = [&]() {
        ACAPI_DisposeElemMemoHdls (&favorite.memo.Get ());
        if (favorite.memoMarker.HasValue ()) {
            ACAPI_DisposeElemMemoHdls (&favorite.memoMarker.Get ());
        }
    };
    if (err != NoError) {
        disposeFavoriteMemos ();
        return err;
    }

    // Guard against type mismatch: a Window favorite applied to the
    // Door tool defaults (or vice versa) would silently corrupt the
    // tool state and leave the caller's PrepareWindowOrDoorDefaults
    // operating on the wrong subtype. Reject early.
#ifdef ServerMainVers_2600
    const API_ElemTypeID favoriteTypeId = favorite.element.header.type.typeID;
#else
    const API_ElemTypeID favoriteTypeId = favorite.element.header.typeID;
#endif
    if (favoriteTypeId != expectedTypeId) {
        disposeFavoriteMemos ();
        return APIERR_REFUSEDPAR;
    }

    // Push the favorite's full element state into the Door/Window tool
    // defaults. AC25+ recommends `ACAPI_Element_ChangeDefaultsExt` for
    // markered element types (API_DoorID / API_WindowID) so the marker
    // sub-element is updated in lock-step with the main element. If the
    // favorite carries a marker (`elementMarker` / `memoMarker`), pass
    // it as a Main Marker sub-element; otherwise pass `nSubElems = 0`
    // and the existing tool marker is preserved.
    API_Element mask;
    ACAPI_ELEMENT_MASK_SETFULL (mask);

    API_SubElement markerSubElement = {};
    UInt32 nSubElems = 0;
    API_SubElement* subElemsPtr = nullptr;
    if (favorite.elementMarker.HasValue () && favorite.memoMarker.HasValue ()) {
        markerSubElement.subType = APISubElement_MainMarker;
        markerSubElement.subElem = favorite.elementMarker.Get ();
        markerSubElement.memo = favorite.memoMarker.Get ();
        ACAPI_ELEMENT_MASK_SETFULL (markerSubElement.mask);
        nSubElems = 1;
        subElemsPtr = &markerSubElement;
    }

    err = ACAPI_Element_ChangeDefaultsExt (&favorite.element, favorite.memo.GetPtr (), &mask, nSubElems, subElemsPtr);
    disposeFavoriteMemos ();
    if (err != NoError) {
        return err;
    }

    // Mirror the full ApplyFavoritesToElementDefaultsCommand flow so the
    // tool defaults include the favorite's classifications, category
    // values and user-defined properties. Some Door favorites (e.g.
    // "Porte d'entrée") will not survive a subsequent CreateExt without
    // this extra metadata: ChangeDefaults alone gets accepted, but the
    // create fails with APIERR_REFUSEDPAR because mandatory classification
    // or category fields remain unset on the tool defaults.
    for (const GS::Pair<API_Guid, API_Guid>& pair : *favorite.classifications) {
        TAPIR_Element_AddClassificationItemDefault (favorite.element.header, pair.second);
    }
    for (const API_ElemCategoryValue& categoryValue : *favorite.elemCategoryValues) {
        TAPIR_Element_SetCategoryValueDefault (favorite.element.header, categoryValue);
    }
    TAPIR_Element_SetPropertiesOfDefaultElem (favorite.element.header, *favorite.properties);

    return NoError;
}

void FillDimensionDefaults (API_Element& element, const API_Coord& referencePoint, const API_Vector& direction)
{
    element.dimension.dimAppear = APIApp_Normal;
    element.dimension.textPos = APIPos_Above;
    element.dimension.textWay = APIDir_Parallel;
    element.dimension.defStaticDim = false;
    element.dimension.usedIn3D = false;
    element.dimension.horizontalText = false;
    element.dimension.refC = referencePoint;
    element.dimension.direction = direction;
}

GS::Optional<GS::UniString> ParseAssociativeDimensionPoint (const GS::ObjectState& pointData, AssociativeDimensionPoint& point)
{
    const GS::ObjectState* elementId = pointData.Get ("elementId");
    if (elementId == nullptr) {
        return "Missing required field 'elementId'.";
    }

    point.elementGuid = GetGuidFromObjectState (*elementId);
    if (point.elementGuid == APINULLGuid) {
        return "Invalid element identifier for associative dimension point.";
    }

    API_Elem_Head elementHeader = {};
    if (!LoadElementHeaderByGuid (point.elementGuid, elementHeader)) {
        return "Failed to load referenced element for associative dimension point.";
    }
    point.elementType = GetElemTypeId (elementHeader);

    pointData.Get ("line", point.line);
    pointData.Get ("inIndex", point.inIndex);

    Int32 special = 0;
    if (pointData.Get ("special", special)) {
        point.special = static_cast<char> (special);
    }

    Int32 nodeType = 0;
    if (pointData.Get ("nodeType", nodeType)) {
        point.nodeType = static_cast<short> (nodeType);
    }

    Int32 nodeStatus = 0;
    if (pointData.Get ("nodeStatus", nodeStatus)) {
        point.nodeStatus = static_cast<short> (nodeStatus);
    }

    auto nodeId = GetOptionalDouble (pointData, "nodeId");

    if (nodeId.HasValue ()) {
        if (nodeId.Get () < 0.0 || nodeId.Get () > static_cast<double> (std::numeric_limits<UInt32>::max ())) {
            return "The 'nodeId' field must be between 0 and 4294967295.";
        }
        point.nodeId = static_cast<UInt32> (nodeId.Get ());
    }

    return {};
}

GS::Optional<GS::UniString> PopulateAssociativeDimensionMemo (
    const GS::Array<AssociativeDimensionPoint>& points,
    API_Element& element,
    API_ElementMemo& memo)
{
    if (static_cast<GSSize> (points.GetSize ()) > static_cast<GSSize> (std::numeric_limits<Int32>::max ()))
        return "Too many associative dimension points.";
    element.dimension.nDimElem = static_cast<Int32> (points.GetSize ());
    const GSSize dimElemCount = static_cast<GSSize> (element.dimension.nDimElem);
    if (dimElemCount > std::numeric_limits<GSSize>::max () / sizeof (API_DimElem))
        return "Associative dimension data exceeds the supported range.";
    const GSSize dimElemBytes = dimElemCount * sizeof (API_DimElem);
    memo.dimElems = reinterpret_cast<API_DimElem**> (BMhAllClear (dimElemBytes));
    if (memo.dimElems == nullptr || *memo.dimElems == nullptr) {
        return "Failed to allocate associative dimension witness data.";
    }

    for (UIndex pointIndex = 0; pointIndex < points.GetSize (); ++pointIndex) {
        const AssociativeDimensionPoint& point = points[pointIndex];
        API_DimElem& dimElem = (*memo.dimElems)[pointIndex];
#ifdef ServerMainVers_2600
        dimElem.base.base.type = API_ElemType (point.elementType);
#else
        dimElem.base.base.typeID = point.elementType;
#endif
        dimElem.base.base.guid = point.elementGuid;
        dimElem.base.base.line = point.line;
        dimElem.base.base.inIndex = point.inIndex;
        dimElem.base.base.special = point.special;
        dimElem.base.base.node_typ = point.nodeType;
        dimElem.base.base.node_status = point.nodeStatus;
        dimElem.base.base.node_id = point.nodeId;
        dimElem.note = element.dimension.defNote;
        dimElem.witnessVal = element.dimension.defWitnessVal;
        dimElem.witnessForm = element.dimension.defWitnessForm;
    }

    return {};
}

void TryApplyDimensionFloorIndex (
    const GS::Array<AssociativeDimensionPoint>& points,
    const GS::Optional<double>& floorIndex,
    API_Element& element)
{
    if (floorIndex.HasValue ()) {
        element.header.floorInd = static_cast<short> (floorIndex.Get ());
        return;
    }

    for (const AssociativeDimensionPoint& point : points) {
        API_Elem_Head elementHeader = {};
        if (LoadElementHeaderByGuid (point.elementGuid, elementHeader)) {
            element.header.floorInd = elementHeader.floorInd;
            return;
        }
    }
}

GS::Optional<GS::UniString> ParseSectionAssociativeDimensionPreset (
    const GS::UniString& presetName,
    SectionAssociativeDimensionPreset& preset)
{
    if (presetName == "WallCompositeFaces") {
        preset = SectionAssociativeDimensionPreset::WallCompositeFaces;
    } else if (presetName == "WallSkinBorders") {
        preset = SectionAssociativeDimensionPreset::WallSkinBorders;
    } else if (presetName == "SlabCompositeFaces") {
        preset = SectionAssociativeDimensionPreset::SlabCompositeFaces;
    } else if (presetName == "SlabSkinBorders") {
        preset = SectionAssociativeDimensionPreset::SlabSkinBorders;
    } else if (presetName == "BeamOrColumnRefLineEndPoints") {
        preset = SectionAssociativeDimensionPreset::BeamOrColumnRefLineEndPoints;
    } else if (presetName == "BeamOrColumnBoundingBoxCorners") {
        preset = SectionAssociativeDimensionPreset::BeamOrColumnBoundingBoxCorners;
    } else if (presetName == "DoorWindowWallHoleCorners") {
        preset = SectionAssociativeDimensionPreset::DoorWindowWallHoleCorners;
    } else if (presetName == "DoorWindowModelHotspots") {
        preset = SectionAssociativeDimensionPreset::DoorWindowModelHotspots;
    } else {
        return "Invalid 'preset' value for section associative dimension.";
    }

    return {};
}

GS::Optional<GS::UniString> LoadSectionElementAndParent (
    const API_Guid& sectionElementGuid,
    API_Element& sectionElement,
    API_Element& parentElement)
{
    sectionElement = {};
    sectionElement.header.guid = sectionElementGuid;
    if (ACAPI_Element_Get (&sectionElement) != NoError || GetElemTypeId (sectionElement.header) != API_SectElemID) {
        return "The referenced 'sectionElementId' is not a valid section element.";
    }

    parentElement = {};
#ifdef ServerMainVers_2600
    parentElement.header.type = sectionElement.sectElem.parentType;
#endif
    parentElement.header.guid = sectionElement.sectElem.parentGuid;
    if (parentElement.header.guid == APINULLGuid || ACAPI_Element_Get (&parentElement) != NoError) {
        return "Failed to load the parent element for the referenced section element.";
    }

    return {};
}

void AddSectionAssociativePoint (
    GS::Array<AssociativeDimensionPoint>& points,
    const API_Guid& sectionElementGuid,
    bool line,
    short nodeType,
    short nodeStatus,
    UInt32 nodeId,
    Int32 inIndex = 0,
    char special = 0)
{
    AssociativeDimensionPoint point;
    point.elementGuid = sectionElementGuid;
    point.elementType = API_SectElemID;
    point.line = line;
    point.inIndex = inIndex;
    point.special = special;
    point.nodeType = nodeType;
    point.nodeStatus = nodeStatus;
    point.nodeId = nodeId;
    points.Push (point);
}

GS::Optional<GS::UniString> BuildSectionAssociativeDimensionPoints (
    const GS::ObjectState& data,
    GS::Array<AssociativeDimensionPoint>& points,
    API_Vector& defaultDirection)
{
    const GS::ObjectState* sectionElementId = data.Get ("sectionElementId");
    if (sectionElementId == nullptr) {
        return "Missing required field 'sectionElementId'.";
    }

    API_Element sectionElement = {};
    API_Element parentElement = {};
    {
        auto error = LoadSectionElementAndParent (GetGuidFromObjectState (*sectionElementId), sectionElement, parentElement);
        if (error.HasValue ()) {
            return error;
        }
    }

    GS::UniString presetName;
    if (!data.Get ("preset", presetName)) {
        return "Missing required field 'preset'.";
    }

    SectionAssociativeDimensionPreset preset;
    auto error = ParseSectionAssociativeDimensionPreset (presetName, preset);
    if (error.HasValue ()) {
        return error;
    }

    auto requireParentType = [&] (std::initializer_list<API_ElemTypeID> allowedTypes, const char* message) -> GS::Optional<GS::UniString> {
        const API_ElemTypeID parentTypeId = GetElemTypeId (parentElement.header);
        for (API_ElemTypeID allowedType : allowedTypes) {
            if (parentTypeId == allowedType) {
                return {};
            }
        }
        return message;
    };

    switch (preset) {
        case SectionAssociativeDimensionPreset::WallCompositeFaces: {
            auto error = requireParentType ({API_WallID}, "The 'WallCompositeFaces' preset requires a wall section element.");
            if (error.HasValue ()) {
                return error;
            }
            defaultDirection = {1.0, 0.0};
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 256, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 1024, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 512, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 768, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 131, 0, 0);
            break;
        }

        case SectionAssociativeDimensionPreset::WallSkinBorders: {
            auto error = requireParentType ({API_WallID}, "The 'WallSkinBorders' preset requires a wall section element.");
            if (error.HasValue ()) {
                return error;
            }
            GS::Array<Int32> skinBorderIndices;
            if (!data.Get ("skinBorderIndices", skinBorderIndices) || skinBorderIndices.IsEmpty ()) {
                return "The 'WallSkinBorders' preset requires a non-empty 'skinBorderIndices' array.";
            }
            defaultDirection = {1.0, 0.0};
            for (Int32 skinBorderIndex : skinBorderIndices) {
                AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 1280, static_cast<UInt32> (skinBorderIndex));
            }
            break;
        }

        case SectionAssociativeDimensionPreset::SlabCompositeFaces: {
            auto error = requireParentType ({API_SlabID}, "The 'SlabCompositeFaces' preset requires a slab section element.");
            if (error.HasValue ()) {
                return error;
            }
            defaultDirection = {0.0, 1.0};
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 256, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 1024, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 512, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 768, 0);
            AddSectionAssociativePoint (points, sectionElement.header.guid, true, 131, 0, 0);
            break;
        }

        case SectionAssociativeDimensionPreset::SlabSkinBorders: {
            auto error = requireParentType ({API_SlabID}, "The 'SlabSkinBorders' preset requires a slab section element.");
            if (error.HasValue ()) {
                return error;
            }
            GS::Array<Int32> skinBorderIndices;
            if (!data.Get ("skinBorderIndices", skinBorderIndices) || skinBorderIndices.IsEmpty ()) {
                return "The 'SlabSkinBorders' preset requires a non-empty 'skinBorderIndices' array.";
            }
            defaultDirection = {0.0, 1.0};
            for (Int32 skinBorderIndex : skinBorderIndices) {
                AddSectionAssociativePoint (points, sectionElement.header.guid, true, 130, 1280, static_cast<UInt32> (skinBorderIndex));
            }
            break;
        }

        case SectionAssociativeDimensionPreset::BeamOrColumnRefLineEndPoints: {
            auto error = requireParentType ({API_BeamID, API_ColumnID}, "The 'BeamOrColumnRefLineEndPoints' preset requires a beam or column section element.");
            if (error.HasValue ()) {
                return error;
            }
            defaultDirection = {1.0, 0.0};
            AddSectionAssociativePoint (points, sectionElement.header.guid, false, 0, 0, 1049586);
            AddSectionAssociativePoint (points, sectionElement.header.guid, false, 0, 0, 2099172);
            break;
        }

        case SectionAssociativeDimensionPreset::BeamOrColumnBoundingBoxCorners: {
            auto error = requireParentType ({API_BeamID, API_ColumnID}, "The 'BeamOrColumnBoundingBoxCorners' preset requires a beam or column section element.");
            if (error.HasValue ()) {
                return error;
            }
            bool beginPlane = true;
            data.Get ("beginPlane", beginPlane);
            bool totalSizePlane = false;
            data.Get ("totalSizePlane", totalSizePlane);

            defaultDirection = {1.0, 2.0};

            const UInt32 planePart = beginPlane ? 4128768U : 8257537U;
            const UInt32 offsets[] = {0U, 12U, 4U, 48U, 60U, 52U, 16U, 28U, 20U};
            for (UInt32 offset : offsets) {
                UInt32 nodeId = planePart + offset;
                if (totalSizePlane) {
                    nodeId += 2U;
                }
                AddSectionAssociativePoint (points, sectionElement.header.guid, false, 0, 0, nodeId);
            }
            break;
        }

        case SectionAssociativeDimensionPreset::DoorWindowWallHoleCorners: {
            auto error = requireParentType ({API_WindowID, API_DoorID}, "The 'DoorWindowWallHoleCorners' preset requires a door or window section element.");
            if (error.HasValue ()) {
                return error;
            }
            bool placeOnTop = false;
            data.Get ("placeOnTop", placeOnTop);

            defaultDirection = {1.0, 0.0};
            for (Int32 pointIndex = 0; pointIndex < 4; ++pointIndex) {
                const short nodeStatus = static_cast<short> (2 + 2 * pointIndex + (placeOnTop ? 1 : 0));
                AddSectionAssociativePoint (points, sectionElement.header.guid, false, 2100, nodeStatus, 0);
            }
            break;
        }

        case SectionAssociativeDimensionPreset::DoorWindowModelHotspots: {
            auto error = requireParentType ({API_WindowID, API_DoorID}, "The 'DoorWindowModelHotspots' preset requires a door or window section element.");
            if (error.HasValue ()) {
                return error;
            }
            defaultDirection = {1.0, 0.0};
            AddSectionAssociativePoint (points, sectionElement.header.guid, false, 0, 0, 11111);
            AddSectionAssociativePoint (points, sectionElement.header.guid, false, 0, 0, 11113);
            break;
        }
    }

    return {};
}

GS::ObjectState CreateElementListResponse (const GS::Array<GS::ObjectState>& elementResults)
{
    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    for (const auto& result : elementResults) {
        elements (result);
    }
    return response;
}

std::set<std::string> gUsedNativeDimensionFixtureNonces;

std::string FixtureUtf8 (const GS::UniString& value)
{
    return std::string (value.ToCStr (CC_UTF8).Get ());
}

std::string FixtureByteHex (const std::string& bytes)
{
    static const char hexDigits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve (bytes.size () * 2);
    for (unsigned char byte : bytes) {
        result.push_back (hexDigits[byte >> 4]);
        result.push_back (hexDigits[byte & 0x0F]);
    }
    return result;
}

bool BuildNativeDimensionFixtureInputProof (GS::ObjectState& proof, GS::UniString& longUnicodeNote, GS::UniString& linearUnicodeNote)
{
    // Explicit UTF-8 bytes keep these fixed values independent of the source
    // file/compiler charset.  This proof describes only input construction.
    const char* const longUnicodeNoteUtf8 =
        "\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBA\xD0\xB0 Unicode \xD0\xB7\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x82\xD0\xBA\xD0\xB8 \xE2\x80\x94 \xD0\xB4\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD1\x8B\xD0\xB9 \xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 \xD0\xB4\xD0\xBB\xD1\x8F \xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F \xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F \xD0\xB8\xD1\x81\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE \xD1\x81\xD0\xBE\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB8\xD0\xBC\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xB1\xD0\xB5\xD0\xB7 \xD1\x83\xD1\x81\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F";
    const char* const linearUnicodeNoteUtf8 = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x87\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 \xE2\x80\x94 \xD1\x80\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80";
    longUnicodeNote = GS::UniString (longUnicodeNoteUtf8, CC_UTF8);
    linearUnicodeNote = GS::UniString (linearUnicodeNoteUtf8, CC_UTF8);
    const std::string longUnicodeBytes (longUnicodeNoteUtf8);
    const std::string linearUnicodeBytes (linearUnicodeNoteUtf8);
    const std::string longUnicodeHex = FixtureByteHex (FixtureUtf8 (longUnicodeNote));
    const std::string linearUnicodeHex = FixtureByteHex (FixtureUtf8 (linearUnicodeNote));
    const bool verified = FixtureUtf8 (longUnicodeNote) == longUnicodeBytes
        && FixtureUtf8 (linearUnicodeNote) == linearUnicodeBytes;

    proof.Add ("schemaVersion", "native-dimension-fixture-input-proof/v1");
    proof.Add ("status", verified ? "verified-fixed-input-construction" : "encoding-mismatch-before-create");
    proof.Add ("meaning", "requested-fixture-input-only-not-readback-or-source");
    const auto& inputs = proof.AddList<GS::ObjectState> ("inputs");
    inputs (GS::ObjectState ("field", "longUnicodeNote", "value", longUnicodeNote,
        "encoding", "UTF-8", "utf8ByteHex", GS::UniString (longUnicodeHex.c_str (), CC_UTF8),
        "byteLength", static_cast<Int32> (FixtureUtf8 (longUnicodeNote).size ())));
    inputs (GS::ObjectState ("field", "linearUnicodeNote", "value", linearUnicodeNote,
        "encoding", "UTF-8", "utf8ByteHex", GS::UniString (linearUnicodeHex.c_str (), CC_UTF8),
        "byteLength", static_cast<Int32> (FixtureUtf8 (linearUnicodeNote).size ())));
    return verified;
}

struct PreparedNonradialFixtureDefaults {
	API_Element linear = {};
	API_Element angle = {};
	API_Element levelSingle = {};
	API_Element levelTwo = {};
	API_ElementMemo linearMemo = {};
	API_ElementMemo angleMemo = {};
	API_ElementMemo levelSingleMemo = {};
	API_ElementMemo levelTwoMemo = {};
	bool prepared = false;

	~PreparedNonradialFixtureDefaults ()
	{
		ACAPI_DisposeElemMemoHdls (&linearMemo);
		ACAPI_DisposeElemMemoHdls (&angleMemo);
		ACAPI_DisposeElemMemoHdls (&levelSingleMemo);
		ACAPI_DisposeElemMemoHdls (&levelTwoMemo);
	}
	PreparedNonradialFixtureDefaults () = default;
	PreparedNonradialFixtureDefaults (const PreparedNonradialFixtureDefaults&) = delete;
	PreparedNonradialFixtureDefaults& operator= (const PreparedNonradialFixtureDefaults&) = delete;
};

struct PreparedLinearStaticFixtureDefaults {
	API_Element linear = {};
	API_ElementMemo memo = {};
	bool prepared = false;

	~PreparedLinearStaticFixtureDefaults () { ACAPI_DisposeElemMemoHdls (&memo); }
	PreparedLinearStaticFixtureDefaults () = default;
	PreparedLinearStaticFixtureDefaults (const PreparedLinearStaticFixtureDefaults&) = delete;
	PreparedLinearStaticFixtureDefaults& operator= (const PreparedLinearStaticFixtureDefaults&) = delete;
};

bool ReadNonradialFixtureDefaultNotePointerFacts (GS::ObjectState& facts, PreparedNonradialFixtureDefaults& defaults);
bool ReadLinearStaticFixtureDefaultFacts (GS::ObjectState& facts, PreparedLinearStaticFixtureDefaults& defaults);

bool AddNativeDimensionFixturePresetInputProof (GS::ObjectState& proof, const GS::UniString& preset,
	const GS::UniString& longUnicodeNote, const GS::UniString& linearUnicodeNote,
	const GS::ObjectState& defaultNotePointers)
{
	if (preset == "linear-static-measured-two-point-v1") {
		GS::ObjectState controls (
			"preset", "linear-static-measured-two-point-v1",
			"meaning", "requested-fixed-input-construction-only-not-readback-source-or-display");
		controls.Add ("defaultAvailability", defaultNotePointers);
		GS::ObjectState linear (
			"linePosition", GS::ObjectState ("x", 20.0, "y", 20.0),
			"direction", GS::ObjectState ("x", 1.0, "y", 0.0),
			"appearance", static_cast<Int32> (APIApp_Normal),
			"textPosition", static_cast<Int32> (APIPos_Above),
			"textWay", "APIDir_Horizontal", "textWayValue", static_cast<Int32> (APIDir_Horizontal),
			"defaultStaticSetting", true, "usedIn3D", false, "horizontalText", false, "pushNeighborEnabled", false,
			"defaultNote", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null"));
		const auto& points = linear.AddList<GS::ObjectState> ("points");
		points (GS::ObjectState ("index", 0, "anchor", GS::ObjectState ("x", 20.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 20.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "sentinel", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		points (GS::ObjectState ("index", 1, "anchor", GS::ObjectState ("x", 24.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 24.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "single-real-interval", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		controls.Add ("linear", linear);
		proof.Add ("presetControls", controls);
		GS::UniString status;
		return defaultNotePointers.Get ("status", status) && status == "verified-null";
	}
	if (preset == "linear-static-refc-mask-study-v1") {
		GS::ObjectState controls (
			"preset", "linear-static-refc-mask-study-v1",
			"meaning", "fixed-create-then-refc-only-native-mask-study-not-writer-admission-or-source-proof");
		controls.Add ("defaultAvailability", defaultNotePointers);
		GS::ObjectState linear (
			"linePosition", GS::ObjectState ("x", 20.0, "y", 20.0),
			"direction", GS::ObjectState ("x", 1.0, "y", 0.0),
			"appearance", static_cast<Int32> (APIApp_Normal),
			"textPosition", static_cast<Int32> (APIPos_Above),
			"textWay", "APIDir_Horizontal", "textWayValue", static_cast<Int32> (APIDir_Horizontal),
			"defaultStaticSetting", true, "usedIn3D", false, "horizontalText", false, "pushNeighborEnabled", false,
			"defaultNote", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null"));
		const auto& points = linear.AddList<GS::ObjectState> ("points");
		points (GS::ObjectState ("index", 0, "anchor", GS::ObjectState ("x", 20.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 20.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "sentinel", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		points (GS::ObjectState ("index", 1, "anchor", GS::ObjectState ("x", 24.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 24.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "single-real-interval", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		controls.Add ("linear", linear);
	GS::ObjectState mutation (
		"field", "API_DimensionType.refC", "from", GS::ObjectState ("x", 20.0, "y", 20.0),
		"to", GS::ObjectState ("x", 20.0, "y", 21.0), "memo", "null", "memoMask", 0, "withdel", false);
	const auto& maskFields = mutation.AddList<GS::UniString> ("elementMaskFields");
	maskFields ("refC");
	controls.Add ("mutation", mutation);
		proof.Add ("presetControls", controls);
		GS::UniString status;
		return defaultNotePointers.Get ("status", status) && status == "verified-null";
	}
	if (preset == "linear-static-refc-inplace-study-v1") {
		GS::ObjectState controls (
			"preset", "linear-static-refc-inplace-study-v1",
			"meaning", "fixed-create-then-refc-only-native-inplace-study-not-writer-admission-or-source-proof");
		controls.Add ("defaultAvailability", defaultNotePointers);
		GS::ObjectState linear (
			"linePosition", GS::ObjectState ("x", 20.0, "y", 20.0),
			"direction", GS::ObjectState ("x", 1.0, "y", 0.0),
			"appearance", static_cast<Int32> (APIApp_Normal),
			"textPosition", static_cast<Int32> (APIPos_Above),
			"textWay", "APIDir_Horizontal", "textWayValue", static_cast<Int32> (APIDir_Horizontal),
			"defaultStaticSetting", true, "usedIn3D", false, "horizontalText", false, "pushNeighborEnabled", false,
			"defaultNote", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null"));
		const auto& points = linear.AddList<GS::ObjectState> ("points");
		points (GS::ObjectState ("index", 0, "anchor", GS::ObjectState ("x", 20.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 20.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "sentinel", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		points (GS::ObjectState ("index", 1, "anchor", GS::ObjectState ("x", 24.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 24.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "single-real-interval", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		controls.Add ("linear", linear);
		GS::ObjectState mutation (
			"field", "API_DimensionType.refC", "from", GS::ObjectState ("x", 20.0, "y", 20.0),
			"to", GS::ObjectState ("x", 20.0, "y", 21.0), "memo", "null", "memoMask", 0, "withdel", true);
		const auto& maskFields = mutation.AddList<GS::UniString> ("elementMaskFields");
		maskFields ("refC");
		controls.Add ("mutation", mutation);
		proof.Add ("presetControls", controls);
		GS::UniString status;
		return defaultNotePointers.Get ("status", status) && status == "verified-null";
	}
	if (preset == "linear-static-refc-automatic-measured-relocation-v1") {
		GS::ObjectState controls (
			"preset", "linear-static-refc-automatic-measured-relocation-v1",
			"meaning", "fixed-create-then-automatic-measured-refc-relocation-hypothesis-not-writer-admission-or-source-proof");
		controls.Add ("defaultAvailability", defaultNotePointers);
		GS::ObjectState linear (
			"linePosition", GS::ObjectState ("x", 20.0, "y", 20.0),
			"direction", GS::ObjectState ("x", 1.0, "y", 0.0),
			"appearance", static_cast<Int32> (APIApp_Normal), "textPosition", static_cast<Int32> (APIPos_Above),
			"textWay", "APIDir_Horizontal", "textWayValue", static_cast<Int32> (APIDir_Horizontal),
			"defaultStaticSetting", true, "usedIn3D", false, "horizontalText", false, "pushNeighborEnabled", false,
			"defaultNote", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null"));
		const auto& points = linear.AddList<GS::ObjectState> ("points");
		points (GS::ObjectState ("index", 0, "anchor", GS::ObjectState ("x", 20.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 20.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "sentinel", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		points (GS::ObjectState ("index", 1, "anchor", GS::ObjectState ("x", 24.0, "y", 15.0), "dimensionPosition", GS::ObjectState ("x", 24.0, "y", 20.0), "fixedPosition", true, "measuredIntervalRole", "single-real-interval", "note", GS::ObjectState ("contentMode", "measured", "fixPos", false, "useLeaderLine", false, "fixedBuffer", "empty", "unicodePointer", "null")));
		controls.Add ("linear", linear);
		GS::ObjectState mutation ("field", "API_DimensionType.refC", "from", GS::ObjectState ("x", 20.0, "y", 20.0),
			"to", GS::ObjectState ("x", 20.0, "y", 18.0), "memo", "null", "memoMask", 0, "withdel", true);
		const auto& maskFields = mutation.AddList<GS::UniString> ("elementMaskFields");
		maskFields ("refC");
		controls.Add ("mutation", mutation);
		GS::Array<GS::UniString> allowedPaths = {
			"geometryAndSettings.linePosition.y", "geometryAndSettings.threeDimensionalPosition.y",
			"orderedPoints[0].dimensionPosition.y",
			"orderedPoints[0].note.position.y", "orderedPoints[1].dimensionPosition.y", "orderedPoints[1].note.position.y",
			"bounds.yMin", "bounds.yMax", "fingerprint", "modificationStamp",
			"readConsistency.initialModificationStamp", "readConsistency.probeModificationStamp", "readConsistency.finalModificationStamp" };
		controls.Add ("allowedDeltaPaths", allowedPaths);
		proof.Add ("presetControls", controls);
		GS::UniString status;
		return defaultNotePointers.Get ("status", status) && status == "verified-null";
	}
	if (preset != "nonradial-read-controls-v1") return true;
	GS::ObjectState controls (
		"preset", "nonradial-read-controls-v1",
		"meaning", "requested-fixed-controls-only-not-readback-source-or-display");
	controls.Add ("defaultNotePointerAvailability", defaultNotePointers);
	GS::ObjectState linear (
		"linePosition", GS::ObjectState ("x", 20.0, "y", 20.0),
		"direction", GS::ObjectState ("x", 1.0, "y", 0.0),
		"appearance", static_cast<Int32> (APIApp_Normal),
		"textPosition", static_cast<Int32> (APIPos_Above),
		"textWay", "APIDir_Horizontal",
		"textWayValue", static_cast<Int32> (APIDir_Horizontal),
		"defaultStaticSetting", true,
		"usedIn3D", false,
		"horizontalText", false,
		"pointCount", 7,
		"defaultNote", GS::ObjectState ("contentMode", "measured", "unicodePointer", "null"));
	const auto& points = linear.AddList<GS::ObjectState> ("pointControls");
	const GS::UniString pointTexts[] = { "<MeasuredValue>", "Literal dimension note", "4999", linearUnicodeNote, "<UnrecognizedDimensionToken>" };
	for (Int32 index = 0; index < 7; ++index) {
		const double anchorX = 20.0 + 4.0 * index;
		const double dimensionX = 20.0 + 4.0 * index;
		GS::ObjectState note;
		note.Add ("contentMode", index == 0 || index == 6 ? "measured" : "custom");
		note.Add ("fixPos", index != 0);
		note.Add ("witnessControl", "copied-from-native-default");
		if (index == 0 || index == 6) note.Add ("unicodePointer", "null");
		if (index > 0 && index < 6) note.Add ("content", pointTexts[index - 1]);
		if (index == 6) {
			note.Add ("useLeaderLine", true);
			note.Add ("anchorAtTextEnd", false);
			note.Add ("position", GS::ObjectState ("x", 44.5, "y", 21.0));
			note.Add ("leaderBegin", GS::ObjectState ("x", 44.0, "y", 20.0));
			note.Add ("leaderMiddle", GS::ObjectState ("x", 45.0, "y", 22.0));
			note.Add ("leaderEnd", GS::ObjectState ("x", 49.0, "y", 22.0));
		}
		points (GS::ObjectState (
			"index", index,
			"anchor", GS::ObjectState ("x", anchorX, "y", 15.0),
			"dimensionPosition", GS::ObjectState ("x", dimensionX, "y", 20.0),
			"fixedPosition", true,
			"note", note));
	}
	controls.Add ("linear", linear);
	GS::ObjectState angle (
		"arcOrigin", GS::ObjectState ("x", 20.0, "y", 5.0),
		"arcPosition", GS::ObjectState ("x", 23.5, "y", 8.5),
		"note", GS::ObjectState (
			"contentMode", "measured", "unicodePointer", "null", "fixPos", true, "useLeaderLine", true,
			"position", GS::ObjectState ("x", 23.5, "y", 8.5),
			"leaderBegin", GS::ObjectState ("x", 23.5, "y", 8.5),
			"leaderMiddle", GS::ObjectState ("x", 24.5, "y", 10.5),
			"leaderEnd", GS::ObjectState ("x", 29.5, "y", 10.5)));
	const auto& anchors = angle.AddList<GS::ObjectState> ("anchors");
	anchors (GS::ObjectState ("x", 20.0, "y", 5.0));
	anchors (GS::ObjectState ("x", 25.0, "y", 5.0));
	anchors (GS::ObjectState ("x", 20.0, "y", 5.0));
	anchors (GS::ObjectState ("x", 20.0, "y", 10.0));
	controls.Add ("angle", angle);
	const auto& levels = controls.AddList<GS::ObjectState> ("levels");
	levels (GS::ObjectState (
		"role", "level-single-note", "format", 0, "level", 1.25,
		"markerPosition", GS::ObjectState ("x", 20.0, "y", 30.0), "staticLevel", false,
		"parentType", "API_ZombieElemID", "parentGuid", "APINULLGuid",
		"note1", GS::ObjectState ("contentMode", "custom", "content", longUnicodeNote),
		"note2", GS::ObjectState ("applicability", "stored-but-not-applicable-for-current-format", "contentMode", "custom", "content", longUnicodeNote)));
	levels (GS::ObjectState (
		"role", "level-two-note", "format", 8, "level", 2.5,
		"markerPosition", GS::ObjectState ("x", 30.0, "y", 30.0), "staticLevel", false,
		"parentType", "API_ZombieElemID", "parentGuid", "APINULLGuid", "needPlus", false,
		"note1", GS::ObjectState ("contentMode", "custom", "content", longUnicodeNote),
		"note2", GS::ObjectState ("applicability", "applicable", "contentMode", "custom", "content", longUnicodeNote)));
	proof.Add ("presetControls", controls);
	GS::UniString status;
	return defaultNotePointers.Get ("status", status) && status == "verified-null";
}

std::string FixturePathKey (std::string result)
{
    std::replace (result.begin (), result.end (), '\\', '/');
    std::transform (result.begin (), result.end (), result.begin (), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return result;
}

bool FixtureFullPath (const std::string& path, std::string& result)
{
    std::vector<char> buffer (32768, '\0');
    const DWORD length = GetFullPathNameA (path.c_str (), static_cast<DWORD> (buffer.size ()), buffer.data (), nullptr);
    if (length == 0 || length >= buffer.size ()) return false;
    result.assign (buffer.data (), length);
    return true;
}

bool FixtureCanonicalExistingPath (const std::string& path, std::string& result)
{
    std::string fullPath;
    if (!FixtureFullPath (path, fullPath)) return false;
    HANDLE handle = CreateFileA (fullPath.c_str (), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buffer (32768, '\0');
    const DWORD length = GetFinalPathNameByHandleA (handle, buffer.data (), static_cast<DWORD> (buffer.size ()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle (handle);
    if (length == 0 || length >= buffer.size ()) return false;
    result.assign (buffer.data (), length);
    if (result.rfind ("\\\\?\\UNC\\", 0) == 0) result = "\\\\" + result.substr (8);
    else if (result.rfind ("\\\\?\\", 0) == 0) result.erase (0, 4);
    return true;
}

const char* FixtureElementTypeName (API_ElemTypeID type)
{
    switch (type) {
        case API_DimensionID: return "Dimension";
        case API_AngleDimensionID: return "AngleDimension";
        case API_LevelDimensionID: return "LevelDimension";
        case API_ArcID: return "Arc";
        case API_RadialDimensionID: return "RadialDimension";
        default: return "Unsupported";
    }
}

void SetNativeElementTypeHeader (API_Element& element, API_ElemTypeID type)
{
#if TAPIR_AC_VERSION == 25
    element.header.typeID = type;
#else
    element.header.type.typeID = type;
#endif
}

bool ReadNonradialFixtureDefaultNotePointerFacts (GS::ObjectState& facts, PreparedNonradialFixtureDefaults& defaults)
{
	SetNativeElementTypeHeader (defaults.linear, API_DimensionID);
	const GSErrCode linearError = ACAPI_Element_GetDefaults (&defaults.linear, &defaults.linearMemo);
	if (linearError != NoError) {
		facts.Add ("status", "not-read");
		facts.Add ("linearDefaultNoteErrorCode", static_cast<Int32> (linearError));
		return false;
	}

	SetNativeElementTypeHeader (defaults.angle, API_AngleDimensionID);
	const GSErrCode angleError = ACAPI_Element_GetDefaults (&defaults.angle, &defaults.angleMemo);
	if (angleError != NoError) {
		facts.Add ("status", "not-read");
		facts.Add ("angleNoteErrorCode", static_cast<Int32> (angleError));
		return false;
	}

	SetNativeElementTypeHeader (defaults.levelSingle, API_LevelDimensionID);
	const GSErrCode levelSingleError = ACAPI_Element_GetDefaults (&defaults.levelSingle, &defaults.levelSingleMemo);
	if (levelSingleError != NoError) {
		facts.Add ("status", "not-read");
		facts.Add ("levelSingleNotesErrorCode", static_cast<Int32> (levelSingleError));
		return false;
	}

	SetNativeElementTypeHeader (defaults.levelTwo, API_LevelDimensionID);
	const GSErrCode levelTwoError = ACAPI_Element_GetDefaults (&defaults.levelTwo, &defaults.levelTwoMemo);
	if (levelTwoError != NoError) {
		facts.Add ("status", "not-read");
		facts.Add ("levelTwoNotesErrorCode", static_cast<Int32> (levelTwoError));
		return false;
	}
	defaults.prepared = true;

	const Int32 linearDefaultPointCount = defaults.linear.dimension.nDimElem;
	const bool linearMemoHandleAbsent = defaults.linearMemo.dimElems == nullptr;
	const bool linearMemoEmpty = linearMemoHandleAbsent && linearDefaultPointCount == 0;
	bool linearMemoNotesNull = linearMemoHandleAbsent;
	const UInt64 requiredLinearMemoBytes = linearDefaultPointCount < 0 ? 0 : static_cast<UInt64> (linearDefaultPointCount) * sizeof (API_DimElem);
	const UInt64 actualLinearMemoBytes = defaults.linearMemo.dimElems == nullptr ? 0
		: static_cast<UInt64> (BMGetHandleSize ((GSHandle) defaults.linearMemo.dimElems));
	if (linearDefaultPointCount < 0 || linearDefaultPointCount > 100000
		|| (linearDefaultPointCount > 0 && (defaults.linearMemo.dimElems == nullptr || *defaults.linearMemo.dimElems == nullptr))
		|| actualLinearMemoBytes < requiredLinearMemoBytes) {
		linearMemoNotesNull = false;
	} else {
		for (Int32 index = 0; index < linearDefaultPointCount; ++index)
			linearMemoNotesNull = linearMemoNotesNull && (*defaults.linearMemo.dimElems)[index].note.contentUStr == nullptr;
	}
	const bool allNull = linearMemoEmpty && linearMemoNotesNull
		&& defaults.linear.dimension.defNote.contentUStr == nullptr
		&& defaults.angle.angleDimension.note.contentUStr == nullptr
		&& defaults.levelSingle.levelDimension.note1.contentUStr == nullptr
		&& defaults.levelSingle.levelDimension.note2.contentUStr == nullptr
		&& defaults.levelTwo.levelDimension.note1.contentUStr == nullptr
		&& defaults.levelTwo.levelDimension.note2.contentUStr == nullptr;
	facts.Add ("status", allNull ? "verified-null" : "refused-nonnull-or-unverifiable-optional-access-pointer");
	facts.Add ("linearMemoHandle", linearMemoEmpty ? "absent-empty" : "present-or-unverifiable");
	facts.Add ("linearDefaultPointCount", linearDefaultPointCount);
	facts.Add ("linearMemoNotes", linearMemoNotesNull ? "null" : "present-or-unavailable");
	facts.Add ("linearDefaultNote", defaults.linear.dimension.defNote.contentUStr == nullptr ? "null" : "present");
	facts.Add ("angleNote", defaults.angle.angleDimension.note.contentUStr == nullptr ? "null" : "present");
	facts.Add ("levelSingleNote1", defaults.levelSingle.levelDimension.note1.contentUStr == nullptr ? "null" : "present");
	facts.Add ("levelSingleNote2", defaults.levelSingle.levelDimension.note2.contentUStr == nullptr ? "null" : "present");
	facts.Add ("levelTwoNote1", defaults.levelTwo.levelDimension.note1.contentUStr == nullptr ? "null" : "present");
	facts.Add ("levelTwoNote2", defaults.levelTwo.levelDimension.note2.contentUStr == nullptr ? "null" : "present");
	return allNull;
}

bool ReadLinearStaticFixtureDefaultFacts (GS::ObjectState& facts, PreparedLinearStaticFixtureDefaults& defaults)
{
	SetNativeElementTypeHeader (defaults.linear, API_DimensionID);
	const GSErrCode getDefaultsError = ACAPI_Element_GetDefaults (&defaults.linear, &defaults.memo);
	if (getDefaultsError != NoError) {
		facts.Add ("status", "not-read");
		facts.Add ("getDefaultsErrorCode", static_cast<Int32> (getDefaultsError));
		return false;
	}
	defaults.prepared = true;
	const Int32 pointCount = defaults.linear.dimension.nDimElem;
	const bool memoHandleAbsent = defaults.memo.dimElems == nullptr;
	const bool emptyMemo = memoHandleAbsent && pointCount == 0;
	const bool defaultNoteNull = defaults.linear.dimension.defNote.contentUStr == nullptr;
	const bool verified = emptyMemo && defaultNoteNull;
	facts.Add ("status", verified ? "verified-null" : "refused-nonnull-or-unverifiable-optional-access-pointer");
	facts.Add ("memoHandle", memoHandleAbsent ? "absent-empty" : "present-or-unverifiable");
	facts.Add ("storedPointCount", pointCount);
	facts.Add ("defaultNoteUnicodePointer", defaultNoteNull ? "null" : "present-unverifiable-not-dereferenced");
	return verified;
}

// Shared by the historical E-737 static fixture and the separately compiled
// private producer. Keep this fixed construction in one place so the producer
// cannot silently drift from the independently observed recipe.
GSErrCode BuildFixedStaticMeasuredTwoPointDimension (API_Element& linear, API_ElementMemo& memo, const Int16 story)
{
	if (linear.dimension.defNote.contentUStr != nullptr || linear.dimension.nDimElem != 0 || memo.dimElems != nullptr)
		return APIERR_BADPARS;
	linear.header.floorInd = story;
	linear.dimension.dimAppear = APIApp_Normal;
	linear.dimension.textPos = APIPos_Above;
	linear.dimension.textWay = APIDir_Horizontal;
	linear.dimension.defStaticDim = true;
	linear.dimension.usedIn3D = false;
	linear.dimension.horizontalText = false;
	linear.dimension.pushNeighborEnabled = false;
	linear.dimension.refC = { 20.0, 20.0 };
	linear.dimension.direction = { 1.0, 0.0 };
	linear.dimension.defNote.contentType = API_NoteContent_Measured;
	linear.dimension.defNote.contentUStr = nullptr;
	std::fill (linear.dimension.defNote.content, linear.dimension.defNote.content + sizeof (linear.dimension.defNote.content), '\0');
	linear.dimension.defNote.fixPos = false;
	linear.dimension.defNote.useLeaderLine = false;
	API_DimElem** points = reinterpret_cast<API_DimElem**> (BMhAllClear (2 * sizeof (API_DimElem)));
	if (points == nullptr) return APIERR_MEMFULL;
	if (*points == nullptr) {
		BMKillHandle (reinterpret_cast<GSHandle*> (&points));
		return APIERR_MEMFULL;
	}
	memo.dimElems = points;
	linear.dimension.nDimElem = 2;
	for (Int32 index = 0; index < 2; ++index) {
		API_DimElem& point = (*memo.dimElems)[index];
		point.base.loc = { 20.0 + 4.0 * index, 15.0 };
		point.pos = { 20.0 + 4.0 * index, 20.0 };
		point.note = linear.dimension.defNote;
		point.note.contentType = API_NoteContent_Measured;
		point.note.contentUStr = nullptr;
		std::fill (point.note.content, point.note.content + sizeof (point.note.content), '\0');
		point.note.fixPos = false;
		point.note.useLeaderLine = false;
		point.witnessVal = linear.dimension.defWitnessVal;
		point.witnessForm = linear.dimension.defWitnessForm;
		point.fixedPos = true;
	}
	return NoError;
}

GS::ObjectState ReadNativeDimensionFixtureRow (const API_Guid& guid, GS::ProcessControl& processControl)
{
	GS::ObjectState request;
	const auto& guids = request.AddList<GS::UniString> ("guids");
	guids (APIGuidToString (guid));
	return GetNativeDimensionDetailsCommand ().Execute (request, processControl);
}

bool NativeDimensionLayerControlGuidEqual (const GS::UniString& left, const GS::UniString& right);

bool ExtractExactNativeDimensionFixtureRow (const GS::ObjectState& response, const API_Guid& requestedGuid, GS::ObjectState& row)
{
	GS::UniString schemaVersion, status;
	GS::Array<GS::ObjectState> rows;
	if (!response.Get ("schemaVersion", schemaVersion) || schemaVersion != "native-dimension-read/v1"
		|| !response.Get ("status", status) || status != "success"
		|| !response.Get ("elements", rows) || rows.GetSize () != 1)
		return false;
	GS::UniString rowStatus, requestedGuidText, elementType;
	Int32 nativeType = -1;
	GS::ObjectState elementId;
	GS::UniString returnedGuid;
	row = rows[0];
	return row.Get ("status", rowStatus) && rowStatus == "success"
		&& row.Get ("requestedGuid", requestedGuidText)
		&& NativeDimensionLayerControlGuidEqual (requestedGuidText, APIGuidToString (requestedGuid))
		&& row.Get ("elementType", elementType) && elementType == "Dimension"
		&& row.Get ("nativeElementTypeId", nativeType) && nativeType == static_cast<Int32> (API_DimensionID)
		&& row.Get ("elementId", elementId) && elementId.Get ("guid", returnedGuid)
		&& NativeDimensionLayerControlGuidEqual (returnedGuid, APIGuidToString (requestedGuid));
}

bool ReadFixedStudyCoordinate (const GS::ObjectState& point, const double expectedX, const double expectedY)
{
	GS::UniString unit;
	double x = 0.0, y = 0.0;
	return point.Get ("unit", unit) && unit == "project-meter"
		&& point.Get ("x", x) && point.Get ("y", y)
		&& std::isfinite (x) && std::isfinite (y)
		&& std::abs (x - expectedX) <= 1.0e-8 && std::abs (y - expectedY) <= 1.0e-8;
}

bool ReadFiniteStudyCoordinate (const GS::ObjectState& point, double& x, double& y)
{
	GS::UniString unit;
	return point.Get ("unit", unit) && unit == "project-meter"
		&& point.Get ("x", x) && point.Get ("y", y)
		&& std::isfinite (x) && std::isfinite (y);
}

bool ReadFixedStudyCoordinate3D (const GS::ObjectState& point, const double expectedX, const double expectedY, const double expectedZ)
{
	GS::UniString unit;
	double x = 0.0, y = 0.0, z = 0.0;
	return point.Get ("unit", unit) && unit == "project-meter"
		&& point.Get ("x", x) && point.Get ("y", y) && point.Get ("z", z)
		&& std::isfinite (x) && std::isfinite (y) && std::isfinite (z)
		&& std::abs (x - expectedX) <= 1.0e-8 && std::abs (y - expectedY) <= 1.0e-8
		&& std::abs (z - expectedZ) <= 1.0e-8;
}

// This is a fixed recipe guard, not a general dimension update validator.
// It admits the refC Change only after the created parent's complete two-point
// reader row proves the predeclared T4a2 baseline at the exact requested story.
bool IsNativeDimensionMaskedPositionStage (const GS::ObjectState& response, const API_Guid& expectedGuid,
	const API_Guid& expectedDatabaseGuid, const Int32 expectedStory, const double expectedLineY, UInt64& modificationStamp)
{
	GS::UniString schemaVersion, responseStatus;
	GS::Array<GS::ObjectState> rows;
	if (!response.Get ("schemaVersion", schemaVersion) || schemaVersion != "native-dimension-read/v1"
		|| !response.Get ("status", responseStatus) || responseStatus != "success"
		|| !response.Get ("elements", rows) || rows.GetSize () != 1)
		return false;
	const GS::ObjectState& row = rows[0];
	GS::UniString rowStatus, requestedGuid, elementType, coverage;
	Int32 nativeType = 0, variation = -1, story = -1;
	Int64 stamp = 0;
	GS::ObjectState elementId, database, databaseId, identity, geometry, memo, consistency;
	GS::UniString databaseStatus, family, contextStatus, memoStatus, consistencyStatus, returnedDatabaseGuid;
	Int32 databaseType = -1;
	if (!row.Get ("status", rowStatus) || rowStatus != "success"
		|| !row.Get ("requestedGuid", requestedGuid) || APIGuidFromString (requestedGuid.ToCStr ()) != expectedGuid
		|| !row.Get ("elementType", elementType) || elementType != "Dimension"
		|| !row.Get ("nativeElementTypeId", nativeType) || nativeType != API_DimensionID
		|| !row.Get ("elementVariationId", variation) || variation != 0
		|| !row.Get ("storyIndex", story) || story != expectedStory
		|| !row.Get ("modificationStamp", stamp) || stamp <= 0
		|| !row.Get ("elementId", elementId))
		return false;
	GS::UniString returnedGuid;
	if (!elementId.Get ("guid", returnedGuid) || APIGuidFromString (returnedGuid.ToCStr ()) != expectedGuid)
		return false;
	if (!row.Get ("database", database) || !database.Get ("status", databaseStatus) || databaseStatus != "available"
		|| !database.Get ("databaseType", databaseType) || databaseType != APIWind_FloorPlanID
		|| !database.Get ("databaseId", databaseId) || !databaseId.Get ("guid", returnedDatabaseGuid)
		|| APIGuidFromString (returnedDatabaseGuid.ToCStr ()) != expectedDatabaseGuid
		|| !row.Get ("identity", identity) || !identity.Get ("family", family) || family != "linear-chain"
		|| !identity.Get ("contextStatus", contextStatus) || contextStatus != "database-story-layer-from-native-element-header"
		|| !row.Get ("memo", memo) || !memo.Get ("status", memoStatus) || memoStatus != "available"
		|| !row.Get ("coverage", coverage)
		|| (coverage != "complete-for-versioned-dimension-fields" && coverage != "partial-unicode-source-unproven")
		|| !row.Get ("readConsistency", consistency)
		|| !consistency.Get ("status", consistencyStatus) || consistencyStatus != "same-guid-type-and-modification-stamp-through-memo-read")
		return false;
	Int64 initialStamp = 0, probeStamp = 0, finalStamp = 0;
	Int32 finalHeaderError = -1;
	if (!consistency.Get ("initialModificationStamp", initialStamp)
		|| !consistency.Get ("probeModificationStamp", probeStamp)
		|| !consistency.Get ("finalModificationStamp", finalStamp)
		|| !consistency.Get ("finalHeaderErrorCode", finalHeaderError)
		|| initialStamp != stamp || probeStamp != stamp || finalStamp != stamp || finalHeaderError != NoError)
		return false;
	// Preserve a coherent exact identity/revision even when later geometry or
	// presentation hypotheses differ. The caller reports those differences
	// without discarding the observed stamp.
	modificationStamp = static_cast<UInt64> (stamp);

	if (!row.Get ("geometryAndSettings", geometry)) return false;
	GS::ObjectState line, direction, defaultNote;
	double directionX = 0.0, directionY = 0.0;
	Int32 segmentCount = 0, pointCount = 0, appearance = -1, textPosition = -1, textDirection = -1;
	bool staticSetting = false, usedIn3D = true, horizontalText = true, pushNeighbor = true;
	if (!geometry.Get ("linePosition", line) || !ReadFixedStudyCoordinate (line, 20.0, expectedLineY)
		|| !geometry.Get ("direction", direction) || !direction.Get ("x", directionX) || !direction.Get ("y", directionY)
		|| !std::isfinite (directionX) || !std::isfinite (directionY)
		|| std::abs (directionX - 1.0) > 1.0e-8 || std::abs (directionY) > 1.0e-8
		|| !geometry.Get ("segmentCount", segmentCount) || segmentCount != 2
		|| !geometry.Get ("pointCount", pointCount) || pointCount != 2
		|| !geometry.Get ("appearance", appearance) || appearance != APIApp_Normal
		|| !geometry.Get ("textPosition", textPosition) || textPosition != APIPos_Above
		|| !geometry.Get ("textDirection", textDirection) || textDirection != APIDir_Horizontal
		|| !geometry.Get ("defaultStaticSetting", staticSetting) || !staticSetting
		|| !geometry.Get ("usedIn3D", usedIn3D) || usedIn3D
		|| !geometry.Get ("horizontalText", horizontalText) || horizontalText
		|| !geometry.Get ("pushNeighborEnabled", pushNeighbor) || pushNeighbor
		|| !geometry.Get ("defaultNote", defaultNote))
		return false;
	GS::UniString defaultMode;
	bool defaultFixPos = true, defaultLeader = true;
	if (!defaultNote.Get ("contentMode", defaultMode) || defaultMode != "measured"
		|| !defaultNote.Get ("fixPos", defaultFixPos) || defaultFixPos
		|| !defaultNote.Get ("useLeaderLine", defaultLeader) || defaultLeader)
		return false;

	GS::Array<GS::ObjectState> points;
	if (!row.Get ("orderedPoints", points) || points.GetSize () != 2) return false;
	for (UIndex index = 0; index < 2; ++index) {
		const GS::ObjectState& point = points[index];
		Int32 storedIndex = -1;
		Int32 elementTypeId = -1, specialReference = -1, nodeIndex = -1;
		UInt64 nativeNodeId = 1;
		bool lineReference = true;
		bool fixedPosition = false;
		GS::ObjectState anchor, anchorLocation, anchorLocation3D, anchorElementId, position, position3D, note;
		if (!point.Get ("index", storedIndex) || storedIndex != static_cast<Int32> (index)
			|| !point.Get ("anchor", anchor) || !anchor.Get ("location", anchorLocation)
			|| !anchor.Get ("location3D", anchorLocation3D)
			|| !anchor.Get ("elementTypeId", elementTypeId) || elementTypeId != API_ZombieElemID
			|| !anchor.Get ("lineReference", lineReference) || lineReference
			|| !anchor.Get ("specialReference", specialReference) || specialReference != 0
			|| !anchor.Get ("nodeIndex", nodeIndex) || nodeIndex != 0
			|| !anchor.Get ("nativeNodeId", nativeNodeId) || nativeNodeId != 0
			|| !anchor.Get ("elementId", anchorElementId)
			|| !ReadFixedStudyCoordinate (anchorLocation, 20.0 + 4.0 * index, 15.0)
			|| !ReadFixedStudyCoordinate3D (anchorLocation3D, 20.0 + 4.0 * index, 15.0, 0.0)
			|| !point.Get ("dimensionPosition", position)
			|| !ReadFixedStudyCoordinate (position, 20.0 + 4.0 * index, expectedLineY)
			|| !point.Get ("dimensionPosition3D", position3D) || !ReadFixedStudyCoordinate3D (position3D, 0.0, 0.0, 0.0)
			|| !point.Get ("fixedPosition", fixedPosition) || !fixedPosition
			|| !point.Get ("note", note))
			return false;
	GS::UniString anchorGuid, associationStatus;
	if (!anchorElementId.Get ("guid", anchorGuid) || APIGuidFromString (anchorGuid.ToCStr ()) != APINULLGuid
		|| !anchor.Get ("associationStatus", associationStatus) || associationStatus != "no-element-guid")
		return false;
#if TAPIR_AC_VERSION == 25
	GS::ObjectState anchorVariation;
	GS::UniString unavailableVariation;
	if (!anchor.Get ("elementVariationId", anchorVariation)
		|| !anchorVariation.Get ("status", unavailableVariation) || unavailableVariation != "not-available-in-ac25-API_Base") return false;
#else
	Int32 variationId = -1;
	if (!anchor.Get ("elementVariationId", variationId) || variationId != 0) return false;
#endif
		GS::UniString noteMode;
		bool noteFixPos = true, noteLeader = true;
		double measured = -1.0;
		if (!note.Get ("contentMode", noteMode) || noteMode != "measured"
			|| !note.Get ("fixPos", noteFixPos) || noteFixPos
			|| !note.Get ("useLeaderLine", noteLeader) || noteLeader
			|| !point.Get ("measuredValueFromPreviousPoint", measured) || !std::isfinite (measured)
			|| std::abs (measured - (index == 0 ? 0.0 : 4.0)) > 1.0e-6)
			return false;
	}
	return true;
}

bool IsNativeDimensionAutomaticRelocationPrechangeStage (const GS::ObjectState& response, const API_Guid& expectedGuid,
	const API_Guid& expectedDatabaseGuid, const Int32 expectedStory, UInt64& modificationStamp)
{
	if (!IsNativeDimensionMaskedPositionStage (response, expectedGuid, expectedDatabaseGuid, expectedStory, 20.0, modificationStamp)) return false;
	GS::ObjectState row, geometry, reference3D, defaultNote, defaultNotePosition, bounds;
	GS::Array<GS::ObjectState> points;
	double defaultNoteX = 0.0, defaultNoteY = 0.0;
	if (!ExtractExactNativeDimensionFixtureRow (response, expectedGuid, row)
		|| !row.Get ("geometryAndSettings", geometry)
		|| !geometry.Get ("threeDimensionalPosition", reference3D)
		|| !ReadFixedStudyCoordinate3D (reference3D, 20.0, 20.0, 0.0)
		|| !geometry.Get ("defaultNote", defaultNote)
		|| !defaultNote.Get ("position", defaultNotePosition)
		|| !ReadFiniteStudyCoordinate (defaultNotePosition, defaultNoteX, defaultNoteY)
		|| !row.Get ("orderedPoints", points) || points.GetSize () != 2
		|| !row.Get ("bounds", bounds)) return false;
		GS::UniString boundsStatus, boundsUnit;
		double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0, zMin = 0.0, zMax = 0.0;
		if (!bounds.Get ("status", boundsStatus) || boundsStatus != "available"
			|| !bounds.Get ("unit", boundsUnit) || boundsUnit != "project-meter"
			|| !bounds.Get ("xMin", xMin) || !bounds.Get ("yMin", yMin) || !bounds.Get ("xMax", xMax) || !bounds.Get ("yMax", yMax)
			|| !bounds.Get ("zMin", zMin) || !bounds.Get ("zMax", zMax)
			|| !std::isfinite (xMin) || !std::isfinite (yMin) || !std::isfinite (xMax) || !std::isfinite (yMax)
			|| !std::isfinite (zMin) || !std::isfinite (zMax) || xMin > xMax || yMin > yMax || zMin > zMax) return false;
		for (UInt32 index = 0; index < 2; ++index) {
			GS::ObjectState point, note, notePosition;
		double noteX = 0.0, noteY = 0.0;
			if (!points[index].Get ("dimensionPosition", notePosition)
				|| !ReadFixedStudyCoordinate (notePosition, 20.0 + 4.0 * index, 20.0)
				|| !points[index].Get ("note", note) || !note.Get ("position", notePosition)
				|| !ReadFiniteStudyCoordinate (notePosition, noteX, noteY)) return false;
		}
	return true;
}

bool IsNativeDimensionAutomaticRelocationHypothesis (const GS::ObjectState& beforeResponse, const GS::ObjectState& afterResponse,
	const API_Guid& expectedGuid, const API_Guid& expectedDatabaseGuid, const Int32 expectedStory, UInt64& afterStamp)
{
	UInt64 beforeStamp = 0;
	if (!IsNativeDimensionAutomaticRelocationPrechangeStage (beforeResponse, expectedGuid, expectedDatabaseGuid, expectedStory, beforeStamp)
		|| !IsNativeDimensionMaskedPositionStage (afterResponse, expectedGuid, expectedDatabaseGuid, expectedStory, 18.0, afterStamp)) return false;
	GS::ObjectState beforeRow, afterRow, beforeGeometry, afterGeometry, beforeReference3D, afterReference3D;
	GS::ObjectState beforeDefault, afterDefault, beforeDefaultPosition, afterDefaultPosition, beforeBounds, afterBounds;
	GS::Array<GS::ObjectState> beforePoints, afterPoints;
	double beforeDefaultX = 0.0, beforeDefaultY = 0.0, afterDefaultX = 0.0, afterDefaultY = 0.0;
	if (!ExtractExactNativeDimensionFixtureRow (beforeResponse, expectedGuid, beforeRow)
		|| !ExtractExactNativeDimensionFixtureRow (afterResponse, expectedGuid, afterRow)
		|| !beforeRow.Get ("geometryAndSettings", beforeGeometry) || !afterRow.Get ("geometryAndSettings", afterGeometry)
		|| !beforeGeometry.Get ("threeDimensionalPosition", beforeReference3D) || !afterGeometry.Get ("threeDimensionalPosition", afterReference3D)
		|| !ReadFixedStudyCoordinate3D (beforeReference3D, 20.0, 20.0, 0.0) || !ReadFixedStudyCoordinate3D (afterReference3D, 20.0, 18.0, 0.0)
		|| !beforeGeometry.Get ("defaultNote", beforeDefault) || !afterGeometry.Get ("defaultNote", afterDefault)
		|| !beforeDefault.Get ("position", beforeDefaultPosition) || !afterDefault.Get ("position", afterDefaultPosition)
		|| !ReadFiniteStudyCoordinate (beforeDefaultPosition, beforeDefaultX, beforeDefaultY)
		|| !ReadFiniteStudyCoordinate (afterDefaultPosition, afterDefaultX, afterDefaultY)
		|| std::abs (afterDefaultX - beforeDefaultX) > 1.0e-8 || std::abs (afterDefaultY - beforeDefaultY) > 1.0e-8
		|| !beforeRow.Get ("orderedPoints", beforePoints) || !afterRow.Get ("orderedPoints", afterPoints)
		|| beforePoints.GetSize () != 2 || afterPoints.GetSize () != 2
		|| !beforeRow.Get ("bounds", beforeBounds) || !afterRow.Get ("bounds", afterBounds)) return false;
		GS::UniString beforeBoundsStatus, afterBoundsStatus, beforeBoundsUnit, afterBoundsUnit;
		double bXMin = 0.0, bYMin = 0.0, bXMax = 0.0, bYMax = 0.0, bZMin = 0.0, bZMax = 0.0;
		double aXMin = 0.0, aYMin = 0.0, aXMax = 0.0, aYMax = 0.0, aZMin = 0.0, aZMax = 0.0;
		if (!beforeBounds.Get ("status", beforeBoundsStatus) || beforeBoundsStatus != "available"
			|| !afterBounds.Get ("status", afterBoundsStatus) || afterBoundsStatus != "available"
			|| !beforeBounds.Get ("unit", beforeBoundsUnit) || beforeBoundsUnit != "project-meter"
			|| !afterBounds.Get ("unit", afterBoundsUnit) || afterBoundsUnit != "project-meter"
			|| !beforeBounds.Get ("xMin", bXMin) || !beforeBounds.Get ("yMin", bYMin) || !beforeBounds.Get ("xMax", bXMax) || !beforeBounds.Get ("yMax", bYMax)
			|| !beforeBounds.Get ("zMin", bZMin) || !beforeBounds.Get ("zMax", bZMax)
			|| !afterBounds.Get ("xMin", aXMin) || !afterBounds.Get ("yMin", aYMin) || !afterBounds.Get ("xMax", aXMax) || !afterBounds.Get ("yMax", aYMax)
			|| !afterBounds.Get ("zMin", aZMin) || !afterBounds.Get ("zMax", aZMax)) return false;
		for (UInt32 index = 0; index < 2; ++index) {
			GS::ObjectState beforePoint, afterPoint, beforePosition, afterPosition, beforeNote, afterNote, beforeNotePosition, afterNotePosition;
			double beforeNoteX = 0.0, beforeNoteY = 0.0, afterNoteX = 0.0, afterNoteY = 0.0;
			if (!beforePoints[index].Get ("dimensionPosition", beforePosition) || !afterPoints[index].Get ("dimensionPosition", afterPosition)
				|| !ReadFixedStudyCoordinate (beforePosition, 20.0 + 4.0 * index, 20.0)
				|| !ReadFixedStudyCoordinate (afterPosition, 20.0 + 4.0 * index, 18.0)
				|| !beforePoints[index].Get ("note", beforeNote) || !afterPoints[index].Get ("note", afterNote)
				|| !beforeNote.Get ("position", beforeNotePosition) || !afterNote.Get ("position", afterNotePosition)
				|| !ReadFiniteStudyCoordinate (beforeNotePosition, beforeNoteX, beforeNoteY)
				|| !ReadFiniteStudyCoordinate (afterNotePosition, afterNoteX, afterNoteY)
				|| std::abs (afterNoteX - beforeNoteX) > 1.0e-8 || std::abs (afterNoteY - beforeNoteY + 2.0) > 1.0e-8) return false;
		}
		GS::UniString beforeFingerprint, afterFingerprint;
	return std::abs (aYMin - bYMin + 2.0) <= 1.0e-8 && std::abs (aYMax - bYMax + 2.0) <= 1.0e-8
		&& std::abs ((aYMax - aYMin) - (bYMax - bYMin)) <= 1.0e-8
		&& std::abs (aXMin - bXMin) <= 1.0e-8 && std::abs (aXMax - bXMax) <= 1.0e-8
		&& std::abs (aZMin - bZMin) <= 1.0e-8 && std::abs (aZMax - bZMax) <= 1.0e-8
		&& beforeRow.Get ("fingerprint", beforeFingerprint) && afterRow.Get ("fingerprint", afterFingerprint)
		&& !beforeFingerprint.IsEmpty () && !afterFingerprint.IsEmpty () && beforeFingerprint != afterFingerprint;
}

GS::Optional<GS::UniString> FixtureCurrentScope (const API_ProjectInfo& projectInfo)
{
    if (projectInfo.projectPath == nullptr || projectInfo.untitled || projectInfo.teamwork)
        return {};
    GS::UniString path (*projectInfo.projectPath);
    path.ReplaceAll ("\\", "/");
    path = path.ToLowerCase ();
    const std::string utf8 = FixtureUtf8 (path);
    if (utf8.size () < 4 || utf8.compare (utf8.size () - 4, 4, ".pln") != 0)
        return {};
    GS::UniString scope ("saved:");
    scope.Append (path);
    return scope;
}

bool CaptureFixedMaskedStudyContext (GS::ObjectState& facts, const GS::UniString& expectedScope,
	const API_Guid& expectedDatabaseGuid, const Int32 expectedStory)
{
	API_ProjectInfo projectInfo = {};
	const GSErrCode projectError = ACAPI_ProjectOperation_Project (&projectInfo);
	API_WindowInfo window = {};
	const GSErrCode windowError = ACAPI_Window_GetCurrentWindow (&window);
	API_DatabaseInfo database = {};
	const GSErrCode databaseError = ACAPI_Database_GetCurrentDatabase (&database);
	API_StoryInfo stories = {};
	const GS::OnExit storyGuard ([&stories] () { if (stories.data != nullptr) BMKillHandle (reinterpret_cast<GSHandle*> (&stories.data)); });
	const GSErrCode storyError = ACAPI_ProjectSetting_GetStorySettings (&stories);
	const GS::Optional<GS::UniString> scope = projectError == NoError ? FixtureCurrentScope (projectInfo) : GS::Optional<GS::UniString> ();
	const API_Guid observedDatabaseGuid = database.databaseUnId.elemSetId;
	const bool valid = projectError == NoError && !scope.IsEmpty () && scope.Get () == expectedScope
		&& windowError == NoError && window.typeID == APIWind_FloorPlanID
		&& databaseError == NoError && database.typeID == APIWind_FloorPlanID
		&& observedDatabaseGuid != APINULLGuid && observedDatabaseGuid == expectedDatabaseGuid
		&& storyError == NoError && stories.actStory == expectedStory;
	facts.Add ("status", valid ? "same-saved-solo-floor-plan-story" : "unavailable-or-context-different");
	facts.Add ("fileScopeKey", scope.IsEmpty () ? GS::UniString () : scope.Get ());
	facts.Add ("windowType", windowError == NoError ? static_cast<Int32> (window.typeID) : -1);
	facts.Add ("windowErrorCode", static_cast<Int32> (windowError));
	facts.Add ("databaseType", databaseError == NoError ? static_cast<Int32> (database.typeID) : -1);
	facts.Add ("databaseErrorCode", static_cast<Int32> (databaseError));
	facts.Add ("databaseGuid", databaseError == NoError ? APIGuidToString (observedDatabaseGuid) : GS::UniString ());
	facts.Add ("storyIndex", storyError == NoError ? stories.actStory : -1);
	facts.Add ("storyErrorCode", static_cast<Int32> (storyError));
	facts.Add ("projectErrorCode", static_cast<Int32> (projectError));
	return valid;
}

GS::ObjectState FixtureError (const GS::UniString& message, const char* code = "FIXTURE_PREFLIGHT_FAILED")
{
    GS::ObjectState error;
    error.Add ("code", code);
    error.Add ("message", message);
    return GS::ObjectState ("error", error);
}

GSErrCode InitializeNativeFixtureElement (API_Element& element, API_ElementMemo* memo, Int32 storyIndex)
{
    GSErrCode err = ACAPI_Element_GetDefaults (&element, memo);
    if (err != NoError)
        return err;
    element.header.floorInd = static_cast<short> (storyIndex);
    return NoError;
}

} // namespace

GetNativeDimensionFixtureInputProofCommand::GetNativeDimensionFixtureInputProofCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionFixtureInputProofCommand::GetName () const { return "GetNativeDimensionFixtureInputProof"; }
GS::Optional<GS::UniString> GetNativeDimensionFixtureInputProofCommand::GetInputParametersSchema () const
{
	return R"({"type":"object","properties":{"preset":{"type":"string","enum":["nonradial-read-controls-v1","linear-static-measured-two-point-v1","linear-static-refc-mask-study-v1","linear-static-refc-inplace-study-v1","linear-static-refc-automatic-measured-relocation-v1"]}},"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionFixtureInputProofCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionFixtureInputProofCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
	GS::UniString preset;
	const auto fieldNames = parameters.GetFieldNames ();
	if (!fieldNames.IsEmpty () && (fieldNames.GetSize () != 1 || !parameters.Get ("preset", preset)
		|| (preset != "nonradial-read-controls-v1" && preset != "linear-static-measured-two-point-v1" && preset != "linear-static-refc-mask-study-v1" && preset != "linear-static-refc-inplace-study-v1" && preset != "linear-static-refc-automatic-measured-relocation-v1")))
		return FixtureError ("GetNativeDimensionFixtureInputProof accepts only an empty object or a closed fixture preset.", "INVALID_INPUT_PROOF_REQUEST");
    GS::ObjectState proof;
    GS::UniString longUnicodeNote, linearUnicodeNote;
    (void) longUnicodeNote;
    (void) linearUnicodeNote;
    if (!BuildNativeDimensionFixtureInputProof (proof, longUnicodeNote, linearUnicodeNote)) {
        GS::ObjectState error = FixtureError ("Fixed fixture Unicode construction did not round-trip its explicit UTF-8 bytes.", "FIXTURE_INPUT_ENCODING_MISMATCH");
        error.Add ("requestedFixtureInputProof", proof);
        return error;
    }
	GS::ObjectState defaultNotePointers;
	bool defaultsVerified = true;
	if (preset == "nonradial-read-controls-v1") {
		PreparedNonradialFixtureDefaults defaults;
		defaultsVerified = ReadNonradialFixtureDefaultNotePointerFacts (defaultNotePointers, defaults);
	} else if (preset == "linear-static-measured-two-point-v1" || preset == "linear-static-refc-mask-study-v1" || preset == "linear-static-refc-inplace-study-v1" || preset == "linear-static-refc-automatic-measured-relocation-v1") {
		PreparedLinearStaticFixtureDefaults defaults;
		defaultsVerified = ReadLinearStaticFixtureDefaultFacts (defaultNotePointers, defaults);
	}
	if (!defaultsVerified || !AddNativeDimensionFixturePresetInputProof (proof, preset, longUnicodeNote, linearUnicodeNote, defaultNotePointers)) {
		proof.Add ("presetControls", GS::ObjectState ("defaultAvailability", defaultNotePointers));
		GS::ObjectState error = FixtureError ("Fixture defaults did not prove the required null optional note-access pointers.", "FIXTURE_DEFAULT_NOTE_POINTER_UNAVAILABLE");
		error.Add ("requestedFixtureInputProof", proof);
		return error;
	}
    return proof;
}

CreateNativeDimensionReadFixturesCommand::CreateNativeDimensionReadFixturesCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String CreateNativeDimensionReadFixturesCommand::GetName () const { return "CreateNativeDimensionReadFixtures"; }
GS::Optional<GS::UniString> CreateNativeDimensionReadFixturesCommand::GetInputParametersSchema () const
{
	return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-read-fixture-v1"]},"preset":{"type":"string","enum":["all-family-read-v2","core-dimension-read-v1","radial-arc-on-read-v1","nonradial-read-controls-v1","linear-static-measured-two-point-v1","linear-static-refc-mask-study-v1","linear-static-refc-inplace-study-v1","linear-static-refc-automatic-measured-relocation-v1"]},"runId":{"type":"string","pattern":"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$"},"actionNonce":{"type":"string","minLength":16,"maxLength":96,"pattern":"^[A-Za-z0-9_-]+$"},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer"}},"required":["schemaVersion","preset","runId","actionNonce","expectedProjectPath","fileScopeKey","storyIndex"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> CreateNativeDimensionReadFixturesCommand::GetResponseSchema () const { return {}; }
GS::ObjectState CreateNativeDimensionReadFixturesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    GS::UniString schemaVersion, preset, runId, actionNonce, expectedProjectPath, fileScopeKey;
    Int32 requestedStory = 0;
    if (!parameters.Get ("schemaVersion", schemaVersion) || schemaVersion != "native-dimension-read-fixture-v1"
		|| !parameters.Get ("preset", preset) || (preset != "all-family-read-v2" && preset != "core-dimension-read-v1" && preset != "radial-arc-on-read-v1" && preset != "nonradial-read-controls-v1" && preset != "linear-static-measured-two-point-v1" && preset != "linear-static-refc-mask-study-v1" && preset != "linear-static-refc-inplace-study-v1" && preset != "linear-static-refc-automatic-measured-relocation-v1")
        || !parameters.Get ("runId", runId) || !parameters.Get ("actionNonce", actionNonce)
        || !parameters.Get ("expectedProjectPath", expectedProjectPath) || !parameters.Get ("fileScopeKey", fileScopeKey)
        || !parameters.Get ("storyIndex", requestedStory))
        return FixtureError ("The fixed native dimension fixture contract is incomplete or unsupported.");
    const bool useNonradialReadControls = preset == "nonradial-read-controls-v1";
	const bool useLinearStaticMeasuredTwoPoint = preset == "linear-static-measured-two-point-v1";
	const bool useLinearStaticRefcMaskStudy = preset == "linear-static-refc-mask-study-v1";
	const bool useLinearStaticRefcInplaceStudy = preset == "linear-static-refc-inplace-study-v1";
	const bool useLinearStaticAutomaticMeasuredRelocation = preset == "linear-static-refc-automatic-measured-relocation-v1";
	const bool useFixedStaticLinear = useLinearStaticMeasuredTwoPoint || useLinearStaticRefcMaskStudy || useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation;

    const std::string runIdUtf8 = FixtureUtf8 (runId);
    const std::string nonceUtf8 = FixtureUtf8 (actionNonce);
    if (runIdUtf8.empty () || runIdUtf8.size () > 48 || !std::all_of (runIdUtf8.begin (), runIdUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; })
        || nonceUtf8.size () < 16 || nonceUtf8.size () > 96 || !std::all_of (nonceUtf8.begin (), nonceUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; }))
        return FixtureError ("runId or actionNonce is outside the closed safe character bounds.");
    if (gUsedNativeDimensionFixtureNonces.find (nonceUtf8) != gUsedNativeDimensionFixtureNonces.end ())
        return FixtureError ("actionNonce was already dispatched in this Archicad process.", "ACTION_NONCE_REPLAY");

    API_ProjectInfo projectInfo = {};
    if (ACAPI_ProjectOperation_Project (&projectInfo) != NoError || projectInfo.untitled || projectInfo.teamwork || projectInfo.projectPath == nullptr)
        return FixtureError ("Fixture preparation requires a saved Solo PLN copy.");
    const GS::Optional<GS::UniString> currentScope = FixtureCurrentScope (projectInfo);
    if (currentScope.IsEmpty () || fileScopeKey != currentScope.Get ())
        return FixtureError ("fileScopeKey does not match the current saved Solo PLN.");

    char localAppData[MAX_PATH + 1] = {};
    const DWORD localAppDataLength = GetEnvironmentVariableA ("LOCALAPPDATA", localAppData, MAX_PATH);
    if (localAppDataLength == 0 || localAppDataLength >= MAX_PATH)
        return FixtureError ("LOCALAPPDATA is unavailable or too long.");
#if TAPIR_AC_VERSION == 25
    constexpr const char* ArchicadMajor = "25";
#else
    constexpr const char* ArchicadMajor = "28";
#endif
    const std::string expectedCopy = std::string (localAppData) + "\\ArchicadTapirMcp\\runner\\scratch\\AC" + ArchicadMajor + "\\mutation-" + runIdUtf8 + ".pln";
    const std::string suppliedPath = FixtureUtf8 (expectedProjectPath);
    const std::string openedPath = FixtureUtf8 (*projectInfo.projectPath);
    std::string expectedCanonical, expectedFull, suppliedCanonical, openedCanonical;
    if (!FixtureCanonicalExistingPath (expectedCopy, expectedCanonical) || !FixtureFullPath (expectedCopy, expectedFull)
        || !FixtureCanonicalExistingPath (suppliedPath, suppliedCanonical) || !FixtureCanonicalExistingPath (openedPath, openedCanonical))
        return FixtureError ("The expected disposable copy, supplied path, or current project path is unavailable.");
    if (FixturePathKey (expectedCanonical) != FixturePathKey (expectedFull)
        || FixturePathKey (suppliedPath) != FixturePathKey (expectedCopy)
        || FixturePathKey (openedPath) != FixturePathKey (expectedCopy)
        || FixturePathKey (suppliedCanonical) != FixturePathKey (expectedCanonical)
        || FixturePathKey (openedCanonical) != FixturePathKey (expectedCanonical))
        return FixtureError ("The expected, supplied and opened project must be the exact canonical task-owned mutation copy.");

    API_WindowInfo currentWindow = {};
    if (ACAPI_Window_GetCurrentWindow (&currentWindow) != NoError || currentWindow.typeID != APIWind_FloorPlanID)
        return FixtureError ("Fixture preparation is limited to the active Floor Plan window.");
    API_DatabaseInfo currentDatabase = {};
    if (ACAPI_Database_GetCurrentDatabase (&currentDatabase) != NoError || currentDatabase.typeID != APIWind_FloorPlanID)
        return FixtureError ("Fixture preparation requires the current native database itself to be the Floor Plan.", "CURRENT_DATABASE_NOT_FLOOR_PLAN");
	const API_Guid expectedFixtureDatabaseGuid = currentDatabase.databaseUnId.elemSetId;
	if (expectedFixtureDatabaseGuid == APINULLGuid)
		return FixtureError ("The current Floor Plan database identity is unavailable.", "CURRENT_DATABASE_IDENTITY_UNAVAILABLE");
    API_StoryInfo storyInfo = {};
    const GS::OnExit storyInfoGuard ([&storyInfo] () { if (storyInfo.data != nullptr) BMKillHandle (reinterpret_cast<GSHandle*> (&storyInfo.data)); });
    if (ACAPI_ProjectSetting_GetStorySettings (&storyInfo) != NoError || storyInfo.actStory != requestedStory)
        return FixtureError ("storyIndex must match the currently active native story.");

	GS::ObjectState defaultNotePointerFacts;
	PreparedNonradialFixtureDefaults preparedDefaults;
	PreparedLinearStaticFixtureDefaults preparedStaticLinear;
	if (useNonradialReadControls && !ReadNonradialFixtureDefaultNotePointerFacts (defaultNotePointerFacts, preparedDefaults)) {
		GS::ObjectState error = FixtureError ("Nonradial fixture defaults did not prove null optional note-access pointers before dispatch.", "FIXTURE_DEFAULT_NOTE_POINTER_UNAVAILABLE");
		error.Add ("defaultNotePointerAvailability", defaultNotePointerFacts);
		return error;
	}
	if (useFixedStaticLinear && !ReadLinearStaticFixtureDefaultFacts (defaultNotePointerFacts, preparedStaticLinear)) {
		GS::ObjectState error = FixtureError ("Linear static fixture defaults did not prove an empty memo and null default-note Unicode pointer before dispatch.", "FIXTURE_DEFAULT_NOTE_POINTER_UNAVAILABLE");
		error.Add ("defaultAvailability", defaultNotePointerFacts);
		return error;
	}
	if (useNonradialReadControls) {
		preparedDefaults.linear.header.floorInd = static_cast<short> (requestedStory);
		preparedDefaults.angle.header.floorInd = static_cast<short> (requestedStory);
		preparedDefaults.levelSingle.header.floorInd = static_cast<short> (requestedStory);
		preparedDefaults.levelTwo.header.floorInd = static_cast<short> (requestedStory);
	}
	if (useFixedStaticLinear) preparedStaticLinear.linear.header.floorInd = static_cast<short> (requestedStory);

    const bool includeCoreDimensions = preset != "radial-arc-on-read-v1";
	const bool includeRadialArcOn = preset != "core-dimension-read-v1" && preset != "nonradial-read-controls-v1" && !useFixedStaticLinear;
    GS::ObjectState radialHostBefore;
    GS::ObjectState radialHostAfter;
    API_Guid radialHostGuid = APINULLGuid;
    bool radialHostUnchanged = false;
    struct CreatedFixture { const char* role; API_Guid guid; API_ElemTypeID type; };
    GS::Array<CreatedFixture> created;
    // Reuse the pure fixed-input generator also exposed by the read-only
    // preflight. Keep this independent byte roundtrip guard before creation.
    GS::ObjectState requestedFixtureInputProof;
    GS::UniString longUnicodeNote, linearUnicodeNote;
    const bool fixtureInputEncodingVerified = BuildNativeDimensionFixtureInputProof (requestedFixtureInputProof, longUnicodeNote, linearUnicodeNote);
    if (!fixtureInputEncodingVerified) {
        GS::ObjectState error = FixtureError ("Fixed fixture Unicode construction did not round-trip its explicit UTF-8 bytes before the undoable write.", "FIXTURE_INPUT_ENCODING_MISMATCH");
        error.Add ("requestedFixtureInputProof", requestedFixtureInputProof);
        return error;
    }
	if (!AddNativeDimensionFixturePresetInputProof (requestedFixtureInputProof, preset, longUnicodeNote, linearUnicodeNote, defaultNotePointerFacts)) {
		GS::ObjectState error = FixtureError ("Nonradial fixture defaults changed before dispatch; optional note-access pointers must remain null.", "FIXTURE_DEFAULT_NOTE_POINTER_UNAVAILABLE");
		error.Add ("requestedFixtureInputProof", requestedFixtureInputProof);
		return error;
	}
    // The API memo note pointers use heap ownership.  Keep distinct strings for
    // each note until the undoable command and memo disposal have completed;
    // never alias defNote.contentUStr across point notes.
    std::array<std::unique_ptr<GS::UniString>, 16> fixtureNoteStrings;
    const auto makeFixtureText = [&fixtureNoteStrings, &longUnicodeNote] (const GS::UIndex index) -> bool {
        fixtureNoteStrings[index].reset (new (std::nothrow) GS::UniString (longUnicodeNote));
        return fixtureNoteStrings[index] != nullptr;
    };
    const char* failingStep = "undoable-command";
    const char* failingRole = "transaction";
    GS::ObjectState refcMaskStudy;
    GS::ObjectState refcInplaceStudy;
	GS::ObjectState automaticMeasuredRelocationStudy;
	if (useLinearStaticRefcInplaceStudy) {
		// This invariant exists even when CREATE fails before a later study stage
		// can be observed. Do not synthesize missing stages in that prefix.
		refcInplaceStudy.Add ("meaning", "private-characterization-only-sdk-does-not-guarantee-derived-position-or-note-translation");
	}
	if (useLinearStaticAutomaticMeasuredRelocation) {
		automaticMeasuredRelocationStudy.Add ("meaning", "private-automatic-measured-refc-relocation-hypothesis-not-sdk-guarantee-source-proof-or-writer-admission");
	}
    bool inplaceReturnedIdentityExact = false;
    bool inplaceChangeMayHaveRun = false;
	bool automaticRelocationReturnedIdentityExact = false;
	bool automaticRelocationChangeMayHaveRun = false;
    // Burn the nonce immediately before dispatching the only write transaction.
    // Any timeout after this point is unknown and cannot be retried in-process.
    gUsedNativeDimensionFixtureNonces.insert (nonceUtf8);
    GSErrCode createResult = ACAPI_CallUndoableCommand ("Create Native Dimension Read Fixtures", [&] () -> GSErrCode {
        GSErrCode err = NoError;
        if (includeCoreDimensions) {
        API_Element linearStorage = {};
        API_ElementMemo linearMemoStorage = {};
        API_Element& linear = useNonradialReadControls ? preparedDefaults.linear : useFixedStaticLinear ? preparedStaticLinear.linear : linearStorage;
        API_ElementMemo& linearMemo = useNonradialReadControls ? preparedDefaults.linearMemo : useFixedStaticLinear ? preparedStaticLinear.memo : linearMemoStorage;
        const GS::OnExit linearMemoGuard ([&linearMemoStorage, useNonradialReadControls, useFixedStaticLinear] () { if (!useNonradialReadControls && !useFixedStaticLinear) ACAPI_DisposeElemMemoHdls (&linearMemoStorage); });
        const char* linearRole = useLinearStaticRefcMaskStudy ? "linear-static-refc-mask-study"
            : useLinearStaticRefcInplaceStudy ? "linear-static-refc-inplace-study"
            : useLinearStaticAutomaticMeasuredRelocation ? "linear-static-refc-automatic-measured-relocation"
            : useLinearStaticMeasuredTwoPoint ? "linear-static-measured-two-point" : "linear-chain-notes";
        failingStep = "defaults"; failingRole = linearRole;
        if (!useNonradialReadControls && !useFixedStaticLinear) {
            SetNativeElementTypeHeader (linear, API_DimensionID);
            err = InitializeNativeFixtureElement (linear, &linearMemo, requestedStory);
        }
        if (err != NoError) return err;
		if ((useNonradialReadControls || useFixedStaticLinear) && linear.dimension.defNote.contentUStr != nullptr) return APIERR_BADPARS;
        linear.dimension.dimAppear = APIApp_Normal;
        linear.dimension.textPos = APIPos_Above;
		linear.dimension.textWay = (useNonradialReadControls || useFixedStaticLinear) ? APIDir_Horizontal : APIDir_Parallel;
        linear.dimension.defStaticDim = true;
        linear.dimension.usedIn3D = false;
        linear.dimension.horizontalText = false;
        if (useFixedStaticLinear) linear.dimension.pushNeighborEnabled = false;
        linear.dimension.refC = { 20.0, 20.0 };
        linear.dimension.direction = { 1.0, 0.0 };
		linear.dimension.defNote.contentType = (useNonradialReadControls || useFixedStaticLinear) ? API_NoteContent_Measured : API_NoteContent_Custom;
		linear.dimension.defNote.contentUStr = nullptr;
		if (useFixedStaticLinear) {
			std::fill (linear.dimension.defNote.content, linear.dimension.defNote.content + sizeof (linear.dimension.defNote.content), '\0');
			linear.dimension.defNote.fixPos = false;
			linear.dimension.defNote.useLeaderLine = false;
		}
		if (!useNonradialReadControls) {
		if (!useFixedStaticLinear) {
			failingStep = "text-allocation"; failingRole = "linear-chain-notes.defaultNote";
			if (!makeFixtureText (6)) return APIERR_MEMFULL;
			linear.dimension.defNote.contentUStr = fixtureNoteStrings[6].get ();
		}
		}
        failingStep = "memo-allocation"; failingRole = linearRole;
		// The prepared SDK default memo remains owned by PreparedNonradialFixtureDefaults.
		// Never overwrite its handle: a nonempty/uninspectable default was refused
		// before dispatch, and this immediate guard protects against drift.
		if ((useNonradialReadControls || useFixedStaticLinear) && (linear.dimension.nDimElem != 0 || linearMemo.dimElems != nullptr)) return APIERR_BADPARS;
		if (useFixedStaticLinear) {
			err = BuildFixedStaticMeasuredTwoPointDimension (linear, linearMemo, static_cast<Int16> (requestedStory));
			if (err != NoError) return err;
		} else {
			const Int32 fixturePointCount = 7;
			API_DimElem** allocatedDimElems = reinterpret_cast<API_DimElem**> (BMhAllClear (fixturePointCount * sizeof (API_DimElem)));
			if (allocatedDimElems == nullptr) return APIERR_MEMFULL;
			if (*allocatedDimElems == nullptr) {
				BMKillHandle (reinterpret_cast<GSHandle*> (&allocatedDimElems));
				return APIERR_MEMFULL;
			}
			linearMemo.dimElems = allocatedDimElems;
			linear.dimension.nDimElem = fixturePointCount;

        const GS::UniString linearNoteTexts[] = {
            GS::UniString ("<MeasuredValue>"), GS::UniString ("Literal dimension note"), GS::UniString ("4999"),
            linearUnicodeNote, GS::UniString ("<UnrecognizedDimensionToken>")
        };
        for (Int32 i = 0; i < 7; ++i) {
            API_DimElem& point = (*linearMemo.dimElems)[i];
            point.base.loc = { 20.0 + 4.0 * i, 15.0 };
            point.pos = { 20.0 + 4.0 * i, 20.0 };
            point.note = linear.dimension.defNote;
            point.note.contentUStr = nullptr;
            point.note.contentType = API_NoteContent_Measured;
            point.witnessVal = linear.dimension.defWitnessVal;
            point.witnessForm = linear.dimension.defWitnessForm;
            point.fixedPos = true;
            if (i == 0) { point.note.contentType = API_NoteContent_Measured; point.note.fixPos = false; }
            if (i >= 1 && i <= 5) {
                failingStep = "text-allocation"; failingRole = "linear-chain-notes.point-note";
                point.note.contentType = API_NoteContent_Custom;
                const GS::UIndex noteIndex = static_cast<GS::UIndex> (i - 1);
                fixtureNoteStrings[noteIndex].reset (new (std::nothrow) GS::UniString (linearNoteTexts[noteIndex]));
                if (fixtureNoteStrings[noteIndex] == nullptr) return APIERR_MEMFULL;
                point.note.contentUStr = fixtureNoteStrings[noteIndex].release ();
                point.note.fixPos = true;
                point.note.pos = { point.pos.x + 0.5, point.pos.y + 1.0 };
            }
            if (i == 6) {
                point.note.contentType = API_NoteContent_Measured;
                point.note.fixPos = true;
                point.note.useLeaderLine = true; point.note.anchorAtTextEnd = false;
                point.note.pos = { point.pos.x + 0.5, point.pos.y + 1.0 };
                point.note.begC = { point.pos.x, point.pos.y }; point.note.midC = { point.pos.x + 1.0, point.pos.y + 2.0 }; point.note.endC = { point.pos.x + 5.0, point.pos.y + 2.0 };
            }
            if ((i == 0 || i == 6) && !useNonradialReadControls) {
                const GS::UIndex noteIndex = i == 0 ? 13 : 14;
                failingStep = "text-allocation"; failingRole = "linear-chain-notes.measured-point-note";
                if (!makeFixtureText (noteIndex)) return APIERR_MEMFULL;
                point.note.contentUStr = fixtureNoteStrings[noteIndex].release ();
            }
        }
		}
        failingStep = "element-create"; failingRole = linearRole;
        err = ACAPI_Element_Create (&linear, &linearMemo);
        if (err != NoError || linear.header.guid == APINULLGuid) return err == NoError ? APIERR_BADID : err;
        created.Push ({ linearRole, linear.header.guid, API_DimensionID });

		if (useLinearStaticRefcMaskStudy || useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) {
			GS::ObjectState& study = useLinearStaticAutomaticMeasuredRelocation ? automaticMeasuredRelocationStudy
				: useLinearStaticRefcInplaceStudy ? refcInplaceStudy : refcMaskStudy;
			GS::ObjectState beforeRead;
			UInt64 beforeStamp = 0;
			GS::ObjectState beforeRaw = ReadNativeDimensionFixtureRow (linear.header.guid, processControl);
			const bool beforeFactsMatch = useLinearStaticAutomaticMeasuredRelocation
				? IsNativeDimensionAutomaticRelocationPrechangeStage (beforeRaw, linear.header.guid, expectedFixtureDatabaseGuid, requestedStory, beforeStamp)
				: IsNativeDimensionMaskedPositionStage (beforeRaw, linear.header.guid, expectedFixtureDatabaseGuid, requestedStory, 20.0, beforeStamp);
			beforeRead.Add ("status", beforeFactsMatch ? "exact-created-parent-baseline" : "unavailable-or-baseline-different");
			beforeRead.Add ("modificationStamp", static_cast<Int64> (beforeStamp));
			beforeRead.Add ("rawResponse", beforeRaw);
			study.Add ("beforeRead", beforeRead);
			if (!beforeFactsMatch) { failingStep = "before-read"; failingRole = linearRole; return APIERR_BADID; }

			API_Element changed = {};
			changed.header.guid = linear.header.guid;
			failingStep = "prechange-exact-read"; failingRole = linearRole;
			err = ACAPI_Element_Get (&changed);
			const bool directIdentityExact = err == NoError && changed.header.guid == linear.header.guid
				&& GetElemTypeId (changed.header) == API_DimensionID;
			const bool directRevisionAndRefCMatch = directIdentityExact && changed.header.modiStamp == beforeStamp
				&& std::abs (changed.dimension.refC.x - 20.0) <= 1.0e-8 && std::abs (changed.dimension.refC.y - 20.0) <= 1.0e-8;
			const char* directReadStatus = err != NoError ? "sdk-error" : !directIdentityExact ? "identity-mismatch"
				: !directRevisionAndRefCMatch ? "revision-or-refC-mismatch" : "exact-created-guid-type-stamp-and-refC";
			study.Add ("beforeElement", GS::ObjectState (
				"status", directReadStatus, "errorCode", static_cast<Int32> (err),
				"guid", err == NoError ? APIGuidToString (changed.header.guid) : GS::UniString (),
				"nativeElementTypeId", err == NoError ? static_cast<Int32> (GetElemTypeId (changed.header)) : -1,
				"modificationStamp", err == NoError ? static_cast<Int64> (changed.header.modiStamp) : 0,
				"refC", err == NoError ? GS::ObjectState ("x", changed.dimension.refC.x, "y", changed.dimension.refC.y) : GS::ObjectState ()));
			if (err != NoError || !directIdentityExact || !directRevisionAndRefCMatch) {
				const char* refusalStatus = err != NoError ? "sdk-error" : !directIdentityExact ? "identity-mismatch"
					: "revision-or-refC-mismatch";
				GS::ObjectState changeStage ("status", "not-dispatched", "errorCode", static_cast<Int32> (err), "refusalStatus", refusalStatus, "field", "API_DimensionType.refC", "memo", "null", "memoMask", 0, "withdel", useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation);
				changeStage.AddList<GS::UniString> ("elementMaskFields");
				study.Add ("change", changeStage);
				failingStep = "prechange-exact-read";
				return err != NoError ? err : APIERR_BADID;
			}
			GS::ObjectState beforeContext;
			const bool contextBeforeChangeValid = CaptureFixedMaskedStudyContext (beforeContext, currentScope.Get (), expectedFixtureDatabaseGuid, requestedStory);
			study.Add ("contextBeforeChange", beforeContext);
			if (!contextBeforeChangeValid) {
				GS::ObjectState changeStage ("status", "not-dispatched", "errorCode", static_cast<Int32> (APIERR_BADID), "field", "API_DimensionType.refC", "memo", "null", "memoMask", 0, "withdel", useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation);
				changeStage.AddList<GS::UniString> ("elementMaskFields");
				study.Add ("change", changeStage);
				failingStep = "prechange-context-check"; return APIERR_BADID;
			}
			changed.dimension.refC = { 20.0, useLinearStaticAutomaticMeasuredRelocation ? 18.0 : 21.0 };
			API_Element mask = {};
			ACAPI_ELEMENT_MASK_CLEAR (mask);
			ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, refC);
			failingStep = "element-change"; failingRole = linearRole;
			const API_Guid changeInputGuid = changed.header.guid;
			if (useLinearStaticRefcInplaceStudy) inplaceChangeMayHaveRun = true;
			if (useLinearStaticAutomaticMeasuredRelocation) automaticRelocationChangeMayHaveRun = true;
			err = (useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation)
				? ACAPI_Element_Change (&changed, &mask, nullptr, 0, true)
				: ACAPI_Element_Change (&changed, &mask, nullptr, 0, false);
			const API_Guid returnedGuid = changed.header.guid;
			const bool returnedGuidValid = returnedGuid != APINULLGuid;
			const bool returnedGuidSame = returnedGuidValid && returnedGuid == changeInputGuid;
			const API_ElemTypeID returnedType = GetElemTypeId (changed.header);
			inplaceReturnedIdentityExact = !useLinearStaticRefcInplaceStudy
				|| (err == NoError && returnedGuidSame && returnedType == API_DimensionID);
			automaticRelocationReturnedIdentityExact = !useLinearStaticAutomaticMeasuredRelocation
				|| (err == NoError && returnedGuidSame && returnedType == API_DimensionID);
			GS::ObjectState changeStage ("status", err == NoError ? "applied" : "error", "errorCode", static_cast<Int32> (err),
				"field", "API_DimensionType.refC", "memo", "null", "memoMask", 0,
				"withdel", useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation);
			if (useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) {
				changeStage.Add ("inputGuid", APIGuidToString (changeInputGuid));
				changeStage.Add ("returnedGuid", returnedGuidValid ? APIGuidToString (returnedGuid) : GS::UniString ());
				changeStage.Add ("returnedGuidStatus", !returnedGuidValid ? "null-guid-observed" : returnedGuidSame ? "same-exact-guid" : "different-guid-observed");
				changeStage.Add ("returnedNativeElementTypeId", static_cast<Int32> (returnedType));
				changeStage.Add ("returnedHeaderStatus", err == NoError ? "returned-header-observed" : "sdk-error-buffer-retained");
			}
			const auto& changedFields = changeStage.AddList<GS::UniString> ("elementMaskFields");
			changedFields ("refC");
			study.Add ("change", changeStage);
			if (err != NoError) return err;
			if ((useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) && (!returnedGuidSame || returnedType != API_DimensionID)) {
				failingStep = "change-returned-identity"; failingRole = linearRole;
				// No follow-up getter may reinterpret a different or null returned
				// identity as the original dimension. Preserve this prefix only.
				return APIERR_BADID;
			}
			GS::ObjectState afterContext;
			const bool contextAfterChangeValid = CaptureFixedMaskedStudyContext (afterContext, currentScope.Get (), expectedFixtureDatabaseGuid, requestedStory);
			study.Add ("contextAfterChange", afterContext);
			API_Element afterElement = {};
			const API_Guid expectedAfterGuid = (useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) ? returnedGuid : linear.header.guid;
			GSErrCode afterElementError = NoError;
			if (expectedAfterGuid != APINULLGuid) {
				afterElement.header.guid = expectedAfterGuid;
				afterElementError = ACAPI_Element_Get (&afterElement);
			}
			const bool afterElementIdentityExact = afterElementError == NoError && expectedAfterGuid != APINULLGuid && afterElement.header.guid == expectedAfterGuid
				&& GetElemTypeId (afterElement.header) == API_DimensionID;
			const char* afterElementStatus = expectedAfterGuid == APINULLGuid ? "null-guid-observed" : afterElementError != NoError ? "sdk-error"
				: afterElement.header.guid != expectedAfterGuid ? "guid-mismatch"
				: GetElemTypeId (afterElement.header) != API_DimensionID ? "type-mismatch"
				: (useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) ? "exact-change-returned-guid-type-readback" : "exact-created-guid-type-readback";
			study.Add ("afterElement", GS::ObjectState (
				"status", afterElementStatus,
				"errorCode", static_cast<Int32> (afterElementError),
				"guid", afterElementError == NoError ? APIGuidToString (afterElement.header.guid) : GS::UniString (),
				"nativeElementTypeId", afterElementError == NoError ? static_cast<Int32> (GetElemTypeId (afterElement.header)) : -1,
				"modificationStamp", afterElementError == NoError ? static_cast<Int64> (afterElement.header.modiStamp) : 0,
				"refC", afterElementIdentityExact ? GS::ObjectState ("x", afterElement.dimension.refC.x, "y", afterElement.dimension.refC.y) : GS::ObjectState ()));

			GS::ObjectState afterRaw;
			UInt64 afterStamp = 0;
			const bool afterRawAvailable = expectedAfterGuid != APINULLGuid;
			bool afterHypothesisMatches = false;
			if (afterRawAvailable) {
				afterRaw = ReadNativeDimensionFixtureRow (expectedAfterGuid, processControl);
				afterHypothesisMatches = useLinearStaticAutomaticMeasuredRelocation
					? IsNativeDimensionAutomaticRelocationHypothesis (beforeRaw, afterRaw, expectedAfterGuid, expectedFixtureDatabaseGuid, requestedStory, afterStamp)
					: IsNativeDimensionMaskedPositionStage (afterRaw, expectedAfterGuid, expectedFixtureDatabaseGuid, requestedStory, 21.0, afterStamp);
			}
			const bool afterReaderIdentityRevisionAvailable = afterStamp > 0 && afterElementIdentityExact
				&& afterElement.header.modiStamp == afterStamp;
			const bool afterRefCMatches = afterElementIdentityExact
				&& std::abs (afterElement.dimension.refC.x - 20.0) <= 1.0e-8
				&& std::abs (afterElement.dimension.refC.y - (useLinearStaticAutomaticMeasuredRelocation ? 18.0 : 21.0)) <= 1.0e-8;
			const bool afterGeometryHypothesisMatches = afterHypothesisMatches && afterReaderIdentityRevisionAvailable && afterRefCMatches;
			GS::ObjectState afterRead (
				"status", !afterRawAvailable ? "not-read-null-returned-guid" : afterReaderIdentityRevisionAvailable ? "exact-reader-identity-and-revision-available" : "reader-identity-or-revision-unavailable",
				"readerIdentityStatus", afterReaderIdentityRevisionAvailable ? "exact-parent-story-database-and-revision" : "identity-or-revision-unavailable",
				"geometryHypothesisStatus", !afterReaderIdentityRevisionAvailable ? "incomplete" : afterGeometryHypothesisMatches ? "matches" : "differs",
				"modificationStamp", static_cast<Int64> (afterStamp));
			if (afterRawAvailable) afterRead.Add ("rawResponse", afterRaw);
			study.Add ("afterRead", afterRead);
			const bool allAfterGeometricHypothesesMatch = afterGeometryHypothesisMatches && contextAfterChangeValid;
			study.Add ("hypothesisStatus", allAfterGeometricHypothesesMatch ? "predeclared-geometric-hypotheses-match" : "predeclared-hypothesis-not-confirmed-raw-facts-retained");
			if (!useLinearStaticRefcInplaceStudy && !useLinearStaticAutomaticMeasuredRelocation)
				study.Add ("meaning", "private-characterization-only-sdk-does-not-guarantee-derived-position-or-note-translation");
		}

        if (!useFixedStaticLinear) {
        API_Element angleStorage = {};
        API_ElementMemo angleMemoStorage = {};
        API_Element& angle = useNonradialReadControls ? preparedDefaults.angle : angleStorage;
        API_ElementMemo& angleMemo = useNonradialReadControls ? preparedDefaults.angleMemo : angleMemoStorage;
        const GS::OnExit angleMemoGuard ([&angleMemoStorage, useNonradialReadControls] () { if (!useNonradialReadControls) ACAPI_DisposeElemMemoHdls (&angleMemoStorage); });
        failingStep = "defaults"; failingRole = "static-angle";
        if (!useNonradialReadControls) {
            SetNativeElementTypeHeader (angle, API_AngleDimensionID);
            err = InitializeNativeFixtureElement (angle, &angleMemo, requestedStory);
        }
        if (err != NoError) return err;
		if (useNonradialReadControls && angle.angleDimension.note.contentUStr != nullptr) return APIERR_BADPARS;
        angle.angleDimension.base[0].loc = { 20.0, 5.0 }; angle.angleDimension.base[1].loc = { 25.0, 5.0 };
        angle.angleDimension.base[2].loc = { 20.0, 5.0 }; angle.angleDimension.base[3].loc = { 20.0, 10.0 };
        angle.angleDimension.origo = { 20.0, 5.0 }; angle.angleDimension.pos = { 23.5, 8.5 };
		angle.angleDimension.note.contentType = useNonradialReadControls ? API_NoteContent_Measured : API_NoteContent_Custom;
		angle.angleDimension.note.contentUStr = nullptr;
		if (!useNonradialReadControls) {
			failingStep = "text-allocation"; failingRole = "static-angle.note";
			if (!makeFixtureText (7)) return APIERR_MEMFULL;
			angle.angleDimension.note.contentUStr = fixtureNoteStrings[7].get ();
		}
        angle.angleDimension.note.fixPos = true; angle.angleDimension.note.useLeaderLine = true;
        if (useNonradialReadControls) angle.angleDimension.note.pos = { 23.5, 8.5 };
        angle.angleDimension.note.begC = angle.angleDimension.pos; angle.angleDimension.note.midC = { 24.5, 10.5 }; angle.angleDimension.note.endC = { 29.5, 10.5 };
        failingStep = "element-create"; failingRole = "static-angle";
        err = ACAPI_Element_Create (&angle, &angleMemo);
        if (err != NoError || angle.header.guid == APINULLGuid) return err == NoError ? APIERR_BADID : err;
        created.Push ({ "static-angle", angle.header.guid, API_AngleDimensionID });

        for (Int32 form : { 0, 8 }) {
            API_Element levelStorage = {};
            API_ElementMemo levelMemoStorage = {};
            API_Element& level = useNonradialReadControls
                ? (form == 0 ? preparedDefaults.levelSingle : preparedDefaults.levelTwo) : levelStorage;
            API_ElementMemo& levelMemo = useNonradialReadControls
                ? (form == 0 ? preparedDefaults.levelSingleMemo : preparedDefaults.levelTwoMemo) : levelMemoStorage;
            const GS::OnExit levelMemoGuard ([&levelMemoStorage, useNonradialReadControls] () { if (!useNonradialReadControls) ACAPI_DisposeElemMemoHdls (&levelMemoStorage); });
            failingStep = "defaults"; failingRole = form == 0 ? "level-single-note" : "level-two-note";
            if (!useNonradialReadControls) {
                SetNativeElementTypeHeader (level, API_LevelDimensionID);
                err = InitializeNativeFixtureElement (level, &levelMemo, requestedStory);
            }
            if (err != NoError) return err;
			if (useNonradialReadControls && (level.levelDimension.note1.contentUStr != nullptr || level.levelDimension.note2.contentUStr != nullptr)) return APIERR_BADPARS;
			level.levelDimension.staticLevel = !useNonradialReadControls;
            level.levelDimension.dimForm = static_cast<short> (form);
            level.levelDimension.parentType = API_ZombieElemID;
			if (useNonradialReadControls) {
				level.levelDimension.parentGuid = APINULLGuid;
				if (form == 8) level.levelDimension.needPlus = false;
			}
            level.levelDimension.level = form == 0 ? 1.25 : 2.5;
            level.levelDimension.loc = { form == 0 ? 20.0 : 30.0, 30.0 };
            level.levelDimension.note2.contentUStr = nullptr;
            const GS::UIndex levelNote1Index = form == 0 ? 9 : 10;
            failingStep = "text-allocation"; failingRole = form == 0 ? "level-single-note.note1" : "level-two-note.note1";
            if (!makeFixtureText (levelNote1Index)) return APIERR_MEMFULL;
            level.levelDimension.note1.contentType = API_NoteContent_Custom;
            level.levelDimension.note1.contentUStr = fixtureNoteStrings[levelNote1Index].get ();
            if (form == 8) {
                failingStep = "text-allocation"; failingRole = "level-two-note.note2";
                if (!makeFixtureText (11)) return APIERR_MEMFULL;
                level.levelDimension.note2.contentType = API_NoteContent_Custom;
                level.levelDimension.note2.contentUStr = fixtureNoteStrings[11].get ();
            } else {
                failingStep = "text-allocation"; failingRole = "level-single-note.inactive-note2";
                if (!makeFixtureText (15)) return APIERR_MEMFULL;
                level.levelDimension.note2.contentType = API_NoteContent_Custom;
                level.levelDimension.note2.contentUStr = fixtureNoteStrings[15].get ();
            }
            failingStep = "element-create"; failingRole = form == 0 ? "level-single-note" : "level-two-note";
            err = ACAPI_Element_Create (&level, &levelMemo);
            if (err != NoError || level.header.guid == APINULLGuid) return err == NoError ? APIERR_BADID : err;
            created.Push ({ form == 0 ? "level-single-note" : "level-two-note", level.header.guid, API_LevelDimensionID });
        }

        }
		}

        if (includeRadialArcOn) {
        API_Element arc = {};
        SetNativeElementTypeHeader (arc, API_ArcID);
        failingStep = "defaults"; failingRole = "radial-host-arc";
        err = InitializeNativeFixtureElement (arc, nullptr, requestedStory);
        if (err != NoError) return err;
        arc.arc.origC = { 40.0, 20.0 }; arc.arc.r = 4.0; arc.arc.ratio = 1.0; arc.arc.begAng = 0.0; arc.arc.endAng = 5.0;
        failingStep = "element-create"; failingRole = "radial-host-arc";
        err = ACAPI_Element_Create (&arc, nullptr);
        if (err != NoError || arc.header.guid == APINULLGuid) return err == NoError ? APIERR_BADID : err;
        radialHostGuid = arc.header.guid;
        created.Push ({ "radial-host-arc", arc.header.guid, API_ArcID });

        API_Element freshArcBeforeRadial = {};
        freshArcBeforeRadial.header.guid = arc.header.guid;
        failingStep = "radial-host-readback-before"; failingRole = "radial-host-arc";
        err = ACAPI_Element_Get (&freshArcBeforeRadial);
        if (err != NoError || freshArcBeforeRadial.header.guid != arc.header.guid || GetElemTypeId (freshArcBeforeRadial.header) != API_ArcID
            || std::abs (freshArcBeforeRadial.arc.origC.x - 40.0) > 1.0e-9 || std::abs (freshArcBeforeRadial.arc.origC.y - 20.0) > 1.0e-9
            || std::abs (freshArcBeforeRadial.arc.r - 4.0) > 1.0e-9 || std::abs (freshArcBeforeRadial.arc.ratio - 1.0) > 1.0e-9
            || std::abs (freshArcBeforeRadial.arc.begAng - 0.0) > 1.0e-9 || std::abs (freshArcBeforeRadial.arc.endAng - 5.0) > 1.0e-9)
            return err == NoError ? APIERR_BADID : err;
        const auto arcSnapshot = [] (const API_Element& value) {
            return GS::ObjectState (
                "status", "exact-guid-type-and-fixed-geometry-readback",
                "guid", APIGuidToString (value.header.guid),
                "elementType", "Arc",
                "geometry", GS::ObjectState (
                    "center", GS::ObjectState ("x", value.arc.origC.x, "y", value.arc.origC.y),
                    "radius", value.arc.r, "ratio", value.arc.ratio,
                    "beginAngle", value.arc.begAng, "endAngle", value.arc.endAng));
        };
        radialHostBefore = arcSnapshot (freshArcBeforeRadial);

        API_Element radial = {};
        SetNativeElementTypeHeader (radial, API_RadialDimensionID);
        API_ElementMemo radialMemo = {};
        const GS::OnExit radialMemoGuard ([&radialMemo] () { ACAPI_DisposeElemMemoHdls (&radialMemo); });
        failingStep = "defaults"; failingRole = "radial-associated-to-created-arc";
        err = InitializeNativeFixtureElement (radial, &radialMemo, requestedStory);
        if (err != NoError) return err;
#if TAPIR_AC_VERSION == 25
        radial.radialDimension.base.base.typeID = API_ArcID;
#else
        radial.radialDimension.base.base.type.typeID = API_ArcID;
#endif
        radial.radialDimension.base.base.line = preset == "radial-arc-on-read-v1";
        radial.radialDimension.base.base.guid = arc.header.guid;
        radial.radialDimension.base.loc = { 40.0, 20.0 };
        radial.radialDimension.endC = { 44.0, 20.0 };
        radial.radialDimension.note.contentType = API_NoteContent_Custom;
        failingStep = "text-allocation"; failingRole = "radial-associated-to-created-arc.note";
        if (!makeFixtureText (8)) return APIERR_MEMFULL;
        radial.radialDimension.note.contentUStr = fixtureNoteStrings[8].get ();
        failingStep = "prefix-setup"; failingRole = "radial-associated-to-created-arc.prefix";
        std::fill (radial.radialDimension.prefix, radial.radialDimension.prefix + sizeof (radial.radialDimension.prefix), '\0');
        radial.radialDimension.prefix[0] = 'R';
        radial.radialDimension.prefixUStr = nullptr;
        failingStep = "element-create"; failingRole = "radial-associated-to-created-arc";
        err = ACAPI_Element_Create (&radial, &radialMemo);
        if (err != NoError || radial.header.guid == APINULLGuid) return err == NoError ? APIERR_BADID : err;
        created.Push ({ "radial-associated-to-created-arc", radial.header.guid, API_RadialDimensionID });

        API_Element freshArcAfterRadial = {};
        freshArcAfterRadial.header.guid = arc.header.guid;
        failingStep = "radial-host-readback-after"; failingRole = "radial-host-arc";
        err = ACAPI_Element_Get (&freshArcAfterRadial);
        if (err != NoError || freshArcAfterRadial.header.guid != arc.header.guid || GetElemTypeId (freshArcAfterRadial.header) != API_ArcID
            || std::abs (freshArcAfterRadial.arc.origC.x - 40.0) > 1.0e-9 || std::abs (freshArcAfterRadial.arc.origC.y - 20.0) > 1.0e-9
            || std::abs (freshArcAfterRadial.arc.r - 4.0) > 1.0e-9 || std::abs (freshArcAfterRadial.arc.ratio - 1.0) > 1.0e-9
            || std::abs (freshArcAfterRadial.arc.begAng - 0.0) > 1.0e-9 || std::abs (freshArcAfterRadial.arc.endAng - 5.0) > 1.0e-9)
            return err == NoError ? APIERR_BADID : err;
        radialHostAfter = arcSnapshot (freshArcAfterRadial);
        radialHostUnchanged = true;
        }

        for (const CreatedFixture& item : created) {
            API_Element readback = {};
            readback.header.guid = item.guid;
            failingStep = "element-readback"; failingRole = item.role;
            err = ACAPI_Element_Get (&readback);
            const bool exactReadback = err == NoError && readback.header.guid != APINULLGuid
                && readback.header.guid == item.guid && GetElemTypeId (readback.header) == item.type;
            if (!exactReadback) return err == NoError ? APIERR_BADID : err;
        }
        if ((useLinearStaticRefcInplaceStudy && !inplaceReturnedIdentityExact)
			|| (useLinearStaticAutomaticMeasuredRelocation && !automaticRelocationReturnedIdentityExact)) {
            failingStep = "change-returned-identity";
			failingRole = useLinearStaticAutomaticMeasuredRelocation
				? "linear-static-refc-automatic-measured-relocation" : "linear-static-refc-inplace-study";
        } else {
            failingStep = "complete"; failingRole = "";
        }
        return NoError;
    });
    if (createResult != NoError && failingRole[0] == '\0') {
        failingStep = "undoable-command";
        failingRole = "transaction";
    }
    if (useLinearStaticRefcMaskStudy || useLinearStaticRefcInplaceStudy || useLinearStaticAutomaticMeasuredRelocation) {
		GS::ObjectState& study = useLinearStaticAutomaticMeasuredRelocation ? automaticMeasuredRelocationStudy
			: useLinearStaticRefcInplaceStudy ? refcInplaceStudy : refcMaskStudy;
        study.Add ("outerUndoableCommand", GS::ObjectState (
            "status", createResult == NoError ? "completed" : "error",
            "errorCode", static_cast<Int32> (createResult)));
    }

    bool rollbackProven = false;
    GS::Array<GS::ObjectState> rollbackReadback;
    if (createResult != NoError) {
        // The in-place study may return a GUID distinct from the CREATE GUID.
        // Absence of only the original cannot prove rollback for that operation.
        rollbackProven = !inplaceChangeMayHaveRun && !automaticRelocationChangeMayHaveRun && !created.IsEmpty ();
        for (const CreatedFixture& item : created) {
            API_Elem_Head remaining = {};
            remaining.guid = item.guid;
            const GSErrCode readbackError = ACAPI_Element_GetHeader (&remaining);
            const bool absent = readbackError == APIERR_DELETED;
            if (!absent) rollbackProven = false;
            rollbackReadback.Push (GS::ObjectState (
                "guid", APIGuidToString (item.guid),
                "absenceStatus", absent ? "apierr-deleted-confirmed-absent" : readbackError == NoError ? "still-present" : "indeterminate-error",
                "readbackErrorCode", static_cast<Int32> (readbackError)));
        }
        if (rollbackReadback.GetSize () != created.GetSize ()) rollbackProven = false;
    }

    GS::Array<GS::ObjectState> finalCreatedReadbacks;
    bool finalCreatedReadbacksExact = true;
    for (const CreatedFixture& item : created) {
        API_Element readback = {};
        readback.header.guid = item.guid;
        const GSErrCode readErr = ACAPI_Element_Get (&readback);
        const bool exactReadback = readErr == NoError && readback.header.guid != APINULLGuid
            && readback.header.guid == item.guid && GetElemTypeId (readback.header) == item.type;
        const GSErrCode reportedReadError = readErr == NoError && !exactReadback ? APIERR_BADID : readErr;
        if (!exactReadback) {
            finalCreatedReadbacksExact = false;
            if (createResult == NoError) {
                failingStep = "element-readback-response";
                failingRole = item.role;
            }
        }
        GS::ObjectState row;
        row.Add ("role", item.role);
        row.Add ("guid", APIGuidToString (item.guid));
        row.Add ("elementType", FixtureElementTypeName (item.type));
        row.Add ("nativeReadback", GS::ObjectState (
            "status", exactReadback ? "exact-guid-type-confirmed" : "unavailable",
            "errorCode", static_cast<Int32> (reportedReadError)));
        finalCreatedReadbacks.Push (row);
    }
    const bool fixtureProvisioned = createResult == NoError && finalCreatedReadbacksExact
		&& (!useLinearStaticRefcInplaceStudy || inplaceReturnedIdentityExact)
		&& (!useLinearStaticAutomaticMeasuredRelocation || automaticRelocationReturnedIdentityExact);

    GS::ObjectState response;
    response.Add ("schemaVersion", "native-dimension-read-fixture-v1");
    response.Add ("preset", preset);
    response.Add ("actionNonce", actionNonce);
    response.Add ("fileScopeKey", currentScope.Get ());
    response.Add ("storyIndex", requestedStory);
    response.Add ("currentDatabaseType", static_cast<Int32> (currentDatabase.typeID));
    response.Add ("currentWindowType", static_cast<Int32> (currentWindow.typeID));
    response.Add ("status", fixtureProvisioned ? "provisioned" : createResult != NoError && rollbackProven ? "failed_rolled_back" : "unknown");
    response.Add ("transactionErrorCode", static_cast<Int32> (createResult));
    response.Add ("failingStep", fixtureProvisioned ? "complete" : failingStep);
    response.Add ("failingRole", fixtureProvisioned ? "" : failingRole);
    response.Add ("preservationStatus", "independent-protected-set-readback-required");
    response.Add ("requestedFixtureInputProof", requestedFixtureInputProof);
    if (useLinearStaticRefcMaskStudy) response.Add ("maskedPositionStudy", refcMaskStudy);
    if (useLinearStaticRefcInplaceStudy) response.Add ("inplacePositionStudy", refcInplaceStudy);
	if (useLinearStaticAutomaticMeasuredRelocation) response.Add ("automaticMeasuredRelocationStudy", automaticMeasuredRelocationStudy);
    if (includeRadialArcOn) {
        response.Add ("radialHostReadback", GS::ObjectState (
            "status", radialHostUnchanged ? "verified-unchanged-exact-host-geometry" : "unverified",
            "guid", radialHostGuid == APINULLGuid ? GS::UniString () : APIGuidToString (radialHostGuid),
            "elementType", "Arc", "before", radialHostBefore, "after", radialHostAfter));
        response.Add ("radialAssociationDescriptorStatus", preset == "radial-arc-on-read-v1"
            ? "fixed-ArcOn-line-kind; node-index-and-location-semantics-unproven"
            : "all-family-read-v2-fixed-base-line-false");
    }
    response.Add ("rollbackVerification", GS::ObjectState (
        "status", rollbackProven ? "all-created-guids-absent" : fixtureProvisioned ? "not-needed" : "indeterminate",
        "created", rollbackReadback));
    const auto& rows = response.AddList<GS::ObjectState> ("created");
    for (const GS::ObjectState& row : finalCreatedReadbacks) rows (row);
    return response;
}

namespace {

GS::ObjectState CreateExecutionResultResponse (const GS::Array<GS::ObjectState>& results)
{
    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");
    for (const auto& result : results) {
        executionResults (result);
    }
    return response;
}

template<typename Func>
GS::ObjectState ExecuteCreateWithElements (const GS::String& commandName, Func&& createFunc)
{
    GS::Array<GS::ObjectState> results;

    // D-357: inside a Native transaction the outer command owns the bracket.
    const bool nestedInNativeTransaction = NativeUndoableScopeIsOpen ();
    API_NotifyElementType notification = {};
    notification.notifID = APINotifyElement_BeginEvents;
    if (!nestedInNativeTransaction)
        AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    const GSErrCode undoErr = NativeUndoableCommand (commandName, [&]() -> GSErrCode {
        createFunc (results);
        return NoError;
    });

    notification = {};
    notification.notifID = APINotifyElement_EndEvents;
    if (!nestedInNativeTransaction)
        AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    GS::ObjectState response = CreateElementListResponse (results);
    if (undoErr != NoError) {
        // A host-level undo/commit failure can occur after the callback has
        // collected generated GUIDs.  Expose that fact so the mutation
        // envelope cannot report a complete create based on stale GUIDs.
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoErr);
    }
    return response;
}

template<typename Func>
GS::ObjectState ExecuteModifyWithResults (const GS::String& commandName, Func&& modifyFunc, const bool atomic = false)
{
    GS::Array<GS::ObjectState> results;

    const GSErrCode undoErr = NativeUndoableCommand (commandName, [&]() -> GSErrCode {
        modifyFunc (results);
        if (atomic) {
            for (const GS::ObjectState& result : results) {
                bool success = false;
                if (!result.Get ("success", success) || !success) {
                    // Returning an error from the undoable callback asks
                    // Archicad to discard the complete command scope.  This
                    // is deliberately opt-in: legacy Modify* commands keep
                    // their historical per-item result behavior, while the
                    // project-owned MutateElements envelope gets all-or-none
                    // batch semantics.
                    return APIERR_COMMANDFAILED;
                }
            }
        }
        return NoError;
    });

    if (atomic && undoErr != NoError) {
        // Keep the per-item diagnostics, but make the all-or-none outcome
        // explicit for the envelope.  The caller must not count a successful
        // item from a rolled-back command as applied.
        if (results.IsEmpty ()) {
            results.Push (CreateFailedExecutionResult (undoErr, "Atomic mutation was rolled back by Archicad."));
        }
    }

    GS::ObjectState response = CreateExecutionResultResponse (results);
    if (atomic && undoErr != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoErr);
    }

    return response;
}

}

GS::Optional<GS::UniString> BuildSlabMemoFromGeometry (
    API_Element& element,
    API_ElementMemo& memo,
    GS::Array<GS::ObjectState>& polygonOutline,
    const GS::Array<GS::ObjectState>& polygonArcs,
    const GS::Array<GS::ObjectState>& holes)
{
    if (polygonOutline.GetSize () < 3) {
        return "'polygonOutline' must contain at least 3 coordinates.";
    }

    if (IsSame2DCoordinate (polygonOutline.GetFirst (), polygonOutline.GetLast ())) {
        polygonOutline.Pop ();
    }
    const GSSize polygonCoordinateCount = static_cast<GSSize> (polygonOutline.GetSize ());
    const GSSize polygonArcCount = static_cast<GSSize> (polygonArcs.GetSize ());
    if (polygonCoordinateCount < 3 || polygonCoordinateCount > static_cast<GSSize> (std::numeric_limits<Int32>::max () - 1) ||
        polygonArcCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()))
        return "Invalid slab polygon dimensions.";

    const API_Polygon oldPoly = element.slab.poly;
    element.slab.poly.nCoords = static_cast<Int32> (polygonCoordinateCount) + 1;
    element.slab.poly.nSubPolys = 1;
    element.slab.poly.nArcs = static_cast<Int32> (polygonArcCount);

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (!GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs) || holePolygonOutline.GetSize () < 3)
            return "Invalid slab hole geometry.";
        const GSSize holeCoordinateCount = static_cast<GSSize> (holePolygonOutline.GetSize ());
        const GSSize holeArcCount = static_cast<GSSize> (holePolygonArcs.GetSize ());
        const GSSize maxInt32 = static_cast<GSSize> (std::numeric_limits<Int32>::max ());
        const GSSize currentCoordinateCount = static_cast<GSSize> (element.slab.poly.nCoords);
        const GSSize currentArcCount = static_cast<GSSize> (element.slab.poly.nArcs);
        if (currentCoordinateCount > maxInt32 - 1 || currentArcCount > maxInt32 ||
            static_cast<GSSize> (element.slab.poly.nSubPolys) >= maxInt32 ||
            holeCoordinateCount > maxInt32 - currentCoordinateCount - 1 ||
            holeArcCount > maxInt32 - currentArcCount)
            return "Slab polygon dimensions exceed the supported range.";
        element.slab.poly.nCoords += static_cast<Int32> (holeCoordinateCount) + 1;
        ++element.slab.poly.nSubPolys;
        element.slab.poly.nArcs += static_cast<Int32> (holeArcCount);
    }

    // ACAPI_Element_GetDefaults does not always allocate the polygon memo handles for
    // slabs (e.g. the default slab reports nCoords but leaves memo.coords == nullptr).
    // The original size-change-only guards skipped allocation whenever the requested
    // polygon matched the default size, leaving memo.coords null and crashing
    // AddPolyToMemo with a null dereference.
    //
    // Each handle below is checked independently rather than bundled behind a single
    // coords-based guard: ACAPI_Element_GetMemo can legitimately return edgeTrims/
    // sideMaterials/vertexIDs as null even when coords itself is populated (e.g. a slab
    // that has never had a custom edge trim never materializes memo.edgeTrims), and
    // AddPolyToMemo below unconditionally writes through edgeTrims/sideMaterials whenever
    // a non-null override pointer is passed in (as it is here).
    //
    // On a real size change, existing handles are freed with BMKillHandle/BMKillPtr and
    // reallocated fresh, not resized in place with BMReallocHandle/BMReallocPtr - confirmed
    // via a real crash report (issue #452, "Fatal memory error in BMReallocPtr... Requested
    // memory size is 48 bytes" / BNValidWritePtr failure) that a handle coming back from
    // ACAPI_Element_GetMemo is not safe to hand to BMRealloc*, even though it reads back
    // non-null and its contents are valid. This is the same free-then-allocate pattern
    // already used for the Stair baseline memo rebuild elsewhere in this file.
    const Int32 nCoords    = element.slab.poly.nCoords;
    const Int32 nSubPolys  = element.slab.poly.nSubPolys;
    const Int32 nArcs      = element.slab.poly.nArcs;
    if (nCoords < 4 || nSubPolys < 1 || nArcs < 0)
        return "Invalid slab polygon memo dimensions.";
    const GSSize coordCount = static_cast<GSSize> (nCoords) + 1;
    const GSSize subPolyCount = static_cast<GSSize> (nSubPolys) + 1;
    if (coordCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord) ||
        coordCount > std::numeric_limits<GSSize>::max () / sizeof (UInt32) ||
        coordCount > std::numeric_limits<GSSize>::max () / sizeof (API_EdgeTrim) ||
        coordCount > std::numeric_limits<GSSize>::max () / sizeof (API_OverriddenAttribute) ||
        subPolyCount > std::numeric_limits<GSSize>::max () / sizeof (Int32) ||
        (nArcs > 0 && static_cast<GSSize> (nArcs) > std::numeric_limits<GSSize>::max () / sizeof (API_PolyArc)))
        return "Slab polygon memo dimensions exceed the supported range.";
    const bool  sizeChanged = (oldPoly.nCoords != nCoords);

    if (memo.coords != nullptr && sizeChanged) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.coords));
    }
    if (memo.coords == nullptr) {
        memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    }
    if (memo.vertexIDs != nullptr && sizeChanged) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.vertexIDs));
    }
    if (memo.vertexIDs == nullptr) {
        memo.vertexIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((nCoords + 1) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
    }
    if (memo.edgeTrims != nullptr && sizeChanged) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.edgeTrims));
    }
    if (memo.edgeTrims == nullptr) {
        memo.edgeTrims = reinterpret_cast<API_EdgeTrim**> (BMAllocateHandle ((nCoords + 1) * sizeof (API_EdgeTrim), ALLOCATE_CLEAR, 0));
    }
    if (memo.sideMaterials != nullptr && sizeChanged) {
        BMKillPtr (reinterpret_cast<GSPtr*> (&memo.sideMaterials));
    }
    if (memo.sideMaterials == nullptr) {
        memo.sideMaterials = reinterpret_cast<API_OverriddenAttribute*> (BMAllocatePtr ((nCoords + 1) * sizeof (API_OverriddenAttribute), ALLOCATE_CLEAR, 0));
    }
    const bool subPolysChanged = (oldPoly.nSubPolys != nSubPolys);
    if (memo.pends != nullptr && subPolysChanged) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.pends));
    }
    if (memo.pends == nullptr) {
        memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle ((nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    }
    if (memo.coords == nullptr || *memo.coords == nullptr) {
        return "Slab coordinate memo handle is invalid.";
    }
    if (memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr) {
        return "Slab vertex memo handle is invalid.";
    }
    if (memo.edgeTrims == nullptr || *memo.edgeTrims == nullptr) {
        return "Slab edge-trim memo handle is invalid.";
    }
    if (memo.sideMaterials == nullptr) {
        return "Slab side-material memo handle is invalid.";
    }
    if (memo.pends == nullptr || *memo.pends == nullptr) {
        return "Slab polygon-end memo handle is invalid.";
    }
    if (nArcs > 0) {
        const bool arcsChanged = (oldPoly.nArcs != nArcs);
        if (memo.parcs != nullptr && arcsChanged) {
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.parcs));
        }
        if (memo.parcs == nullptr) {
            memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (nArcs * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
        }
    }
    // Always rewrite vertex IDs: with the free-then-allocate pattern above, any size change
    // yields freshly zeroed (and therefore always-stale) vertex ID slots, and a same-size
    // in-place reuse still needs correct IDs recomputed for the new geometry.
    const bool needToProcessVertexIDs = true;
    if (nArcs > 0 && (memo.parcs == nullptr || *memo.parcs == nullptr)) {
        return "Slab arc memo handle is invalid.";
    }

    const API_EdgeTrimID edgeTrimSideType = APIEdgeTrim_Vertical;
    Int32 iCoord = 1;
    Int32 iArc = 0;
    Int32 iPends = 1;
    GSErrCode memoErr = AddPolyToMemo (polygonOutline, polygonArcs, iCoord, iArc, iPends, memo, &edgeTrimSideType, &element.slab.sideMat, needToProcessVertexIDs);
    if (memoErr != NoError)
        return "Failed to populate slab polygon memo data.";

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (!GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs) || holePolygonOutline.GetSize () < 3)
            return "Invalid slab hole geometry.";
        memoErr = AddPolyToMemo (holePolygonOutline, holePolygonArcs, iCoord, iArc, iPends, memo, &edgeTrimSideType, &element.slab.sideMat, needToProcessVertexIDs);
        if (memoErr != NoError)
            return "Failed to populate slab hole memo data.";
    }

    // vertexIDs[0] must hold the max vertex ID used across the whole shape - an undocumented
    // ACAPI_Element_Change requirement (confirmed live in this repo's PolyLine/Hatch geometry
    // SET work) without which ACAPI_Element_Change rejects the polygon with APIERR_BADPOLY.
    // AddPolyToMemo never writes index 0 itself, so it is filled in here by scanning the IDs
    // it did write.
    if (memo.vertexIDs != nullptr) {
        UInt32 maxVertexID = 0;
        for (Int32 i = 1; i <= nCoords; ++i) {
            maxVertexID = std::max (maxVertexID, (*memo.vertexIDs)[i]);
        }
        (*memo.vertexIDs)[0] = maxVertexID;
    }

    return {};
}

// The new-style ACAPI_Polygon_InsertPolyNode/DeletePolyNode/InsertSubPoly/DeleteSubPoly functions
// do not exist before Archicad 27. Pre-27, the same operations are reached through the older,
// untyped ACAPI_Goodies dispatcher with APIAny_*PolyNodeID/APIAny_*SubPolyID constants. Keep the
// polygon mutation kernel version-agnostic for the AC25/AC26 builds as well as AC27+.
#ifdef ServerMainVers_2700
static GSErrCode TAPIR_Polygon_InsertPolyNode (API_ElementMemo* memo, Int32* nodeIndex, API_Coord* coord)
{
    return ACAPI_Polygon_InsertPolyNode (memo, nodeIndex, coord);
}
static GSErrCode TAPIR_Polygon_DeletePolyNode (API_ElementMemo* memo, Int32* nodeIndex)
{
    return ACAPI_Polygon_DeletePolyNode (memo, nodeIndex);
}
static GSErrCode TAPIR_Polygon_InsertSubPoly (API_ElementMemo* memo, API_ElementMemo* insMemo)
{
    return ACAPI_Polygon_InsertSubPoly (memo, insMemo);
}
static GSErrCode TAPIR_Polygon_DeleteSubPoly (API_ElementMemo* memo, Int32* subPolyIndex)
{
    return ACAPI_Polygon_DeleteSubPoly (memo, subPolyIndex);
}
#else
static GSErrCode TAPIR_Polygon_InsertPolyNode (API_ElementMemo* memo, Int32* nodeIndex, API_Coord* coord)
{
    return ACAPI_Goodies (APIAny_InsertPolyNodeID, memo, nodeIndex, coord);
}
static GSErrCode TAPIR_Polygon_DeletePolyNode (API_ElementMemo* memo, Int32* nodeIndex)
{
    return ACAPI_Goodies (APIAny_DeletePolyNodeID, memo, nodeIndex);
}
static GSErrCode TAPIR_Polygon_InsertSubPoly (API_ElementMemo* memo, API_ElementMemo* insMemo)
{
    return ACAPI_Goodies (APIAny_InsertSubPolyID, memo, insMemo);
}
static GSErrCode TAPIR_Polygon_DeleteSubPoly (API_ElementMemo* memo, Int32* subPolyIndex)
{
    return ACAPI_Goodies (APIAny_DeleteSubPolyID, memo, subPolyIndex);
}
#endif

// Applies a new outline/arcs/holes shape to an EXISTING slab's memo (already populated via
// ACAPI_Element_GetMemo), for use with ACAPI_Element_ChangeMemo. The polygon-editing primitives
// preserve Archicad's internal consistency across point-count and hole changes; a from-scratch
// replacement reliably produced APIERR_BADPOLY in the upstream issue #452 scenario. Existing
// vertex IDs are preserved, while new vertices receive zero IDs for Archicad to assign.
static GS::Optional<GS::UniString> ApplySlabPolygonChange (
    API_ElementMemo& memo,
    const API_OverriddenAttribute& sideMat,
    GS::Array<GS::ObjectState>& polygonOutline,
    const GS::Array<GS::ObjectState>& polygonArcs,
    const GS::Array<GS::ObjectState>& holes)
{
    if (polygonOutline.GetSize () < 3) {
        return "'polygonOutline' must contain at least 3 coordinates.";
    }
    if (IsSame2DCoordinate (polygonOutline.GetFirst (), polygonOutline.GetLast ())) {
        polygonOutline.Pop ();
    }
    if (memo.coords == nullptr || *memo.coords == nullptr || memo.pends == nullptr || *memo.pends == nullptr) {
        return "Slab has no polygon data to modify.";
    }

    // A from-scratch memo rebuild (matching CreateSlabs, and BuildMeshPolyMemoFromGeometry's own
    // proven-working pattern for ModifyMeshes) plus ACAPI_Element_Change reliably fails with
    // APIERR_BADPOLY here whenever nCoords changes at all - confirmed live on the plain point-count
    // case alone (no holes involved), so this is not a vertexIDs-numbering issue as first assumed
    // (tried: global sequential, global with the closing duplicate mirroring the first point, and
    // per-contour restart - all three failed identically). Graphisoft's own polygon-editing
    // primitives - ACAPI_Polygon_InsertPolyNode/DeletePolyNode/InsertSubPoly/DeleteSubPoly, used by
    // the DevKit's own reference example (Element_Test/Element_Modify_Polygon.cpp, targeting the
    // same element types including API_SlabID) together with ACAPI_Element_ChangeMemo - keep the
    // existing GetMemo-provided handles' internal consistency intact across a size change, which a
    // full replacement apparently cannot always reproduce. They document that memo's coords/pends/
    // parcs/vertexIDs handles "must be initialized"; ACAPI_Element_GetMemo can legitimately leave
    // vertexIDs/parcs null (e.g. a slab with no per-edge customization/arcs), so those are seeded
    // here first.
    if (memo.vertexIDs == nullptr) {
        // Per Graphisoft's own API_ElementMemo documentation: "If you retrieve an array of
        // vertices, edges or contours with ACAPI_Element_GetMemo, do not change the IDs in these
        // arrays. New vertices, edges and contours should be inserted with ID = 0." "Initialized"
        // (as InsertPolyNode/DeleteSubPoly/etc. require) means the handle must exist at the right
        // size, not that it must be pre-filled with a manually invented ID scheme - Archicad
        // itself assigns real IDs to zero-ID vertices. Earlier attempts at manually numbering this
        // (global sequential, per-contour restart, with/without mirroring the closing duplicate)
        // were all guesses at an ID scheme Archicad was never asking for.
        const Int32 existingNCoords = (Int32) (BMGetHandleSize ((GSHandle) memo.coords) / sizeof (API_Coord)) - 1;
        memo.vertexIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((existingNCoords + 1) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
        if (memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr) {
            return "Failed to initialize slab vertex IDs.";
        }
    }
    if (memo.parcs == nullptr) {
        memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (0, ALLOCATE_CLEAR, 0));
        if (memo.parcs == nullptr) {
            return "Failed to initialize slab arc memo.";
        }
    }

    // Step 1: remove every existing hole (subpoly index >= 2), highest index first so earlier
    // deletions don't shift the indices of subpolys not yet processed.
    const Int32 nSubPolys = (Int32) (BMGetHandleSize ((GSHandle) memo.pends) / sizeof (Int32)) - 1;
    for (Int32 subPolyIndex = nSubPolys; subPolyIndex >= 2; --subPolyIndex) {
        Int32 idx = subPolyIndex;
        GSErrCode err = TAPIR_Polygon_DeleteSubPoly (&memo, &idx);
        if (err != NoError) {
            return "Failed to remove an existing slab hole.";
        }
    }

    // Step 2: resize the main outline (subpoly 1) to the requested point count by repeatedly
    // inserting/deleting node 1 - the coordinate value used for an inserted placeholder node does
    // not matter, every point is overwritten with the real final coordinates right after.
    Int32 outlineCount = (*memo.pends)[1] - 1; // real points, excluding the closing duplicate
    const Int32 desiredOutlineCount = (Int32) polygonOutline.GetSize ();
    while (outlineCount > desiredOutlineCount) {
        Int32 idx = 1;
        GSErrCode err = TAPIR_Polygon_DeletePolyNode (&memo, &idx);
        if (err != NoError) {
            return "Failed to adjust the slab outline's point count.";
        }
        --outlineCount;
    }
    while (outlineCount < desiredOutlineCount) {
        // Per Graphisoft's own DevKit reference example (Do_Poly_InsertNode), nodeIndex is set to
        // the clicked edge's begin-node index + 1 - i.e. the position the NEW node will occupy
        // (shifting what was there up by one), not "insert after this existing node" as the header
        // doc's wording alone suggests. To insert between existing nodes 1 and 2, that means
        // nodeIndex = 2, not 1.
        Int32 idx = 2;
        const API_Coord& p1 = (*memo.coords)[1];
        const API_Coord& p2 = (*memo.coords)[2];
        API_Coord placeholder = {(p1.x + p2.x) / 2.0, (p1.y + p2.y) / 2.0};
        GSErrCode err = TAPIR_Polygon_InsertPolyNode (&memo, &idx, &placeholder);
        if (err != NoError) {
            return "Failed to adjust the slab outline's point count.";
        }
        ++outlineCount;
    }

    for (Int32 i = 0; i < desiredOutlineCount; ++i) {
        (*memo.coords)[i + 1] = Get2DCoordinateFromObjectState (polygonOutline[i]);
    }
    (*memo.coords)[desiredOutlineCount + 1] = (*memo.coords)[1];

    // Resized to the EXACT new arc count, not just overwritten in place: reusing a stale-sized
    // handle left old arc entries beyond the new count untouched (e.g. going from 2 arcs to 1, or
    // to 0, silently kept the old 2nd arc around) - confirmed live, arcs "stuck" across modify
    // calls that were supposed to remove/replace them.
    {
        const GS::Array<API_PolyArc> arcs = GetPolyArcs (polygonArcs, 1);
        if (memo.parcs != nullptr) {
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.parcs));
        }
        if (!arcs.IsEmpty ()) {
            memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (arcs.GetSize () * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
            for (UIndex i = 0; i < arcs.GetSize (); ++i) {
                (*memo.parcs)[i] = arcs[i];
            }
        } else {
            memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (0, ALLOCATE_CLEAR, 0));
        }
    }

    // Step 3: add the requested holes back, each as a fresh subpoly.
    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (!GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs) || holePolygonOutline.GetSize () < 3)
            return "Invalid slab hole geometry.";
        const Int32 holeNCoords = (Int32) holePolygonOutline.GetSize ();
        API_ElementMemo insMemo = {};
        const GS::OnExit insCleanup ([&insMemo] () { ACAPI_DisposeElemMemoHdls (&insMemo); });
        // +2, not +1: index 0 is the unused dummy slot and index holeNCoords+1 holds the closing
        // duplicate point written below - allocating only holeNCoords+1 slots left that last write
        // one element past the end of the handle, corrupting adjacent heap memory (a delayed,
        // hard-to-diagnose C0000374 crash reported by Archicad much later during an unrelated
        // commit - confirmed live by reproducing it on a plain hole-add).
        insMemo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((holeNCoords + 2) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        if (insMemo.coords == nullptr || *insMemo.coords == nullptr) {
            return "Failed to allocate slab hole coordinates.";
        }
        for (Int32 i = 0; i < holeNCoords; ++i) {
            (*insMemo.coords)[i + 1] = Get2DCoordinateFromObjectState (holePolygonOutline[i]);
        }
        (*insMemo.coords)[holeNCoords + 1] = (*insMemo.coords)[1];
        insMemo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
        if (insMemo.pends == nullptr || *insMemo.pends == nullptr) {
            return "Failed to allocate slab hole polygon ends.";
        }
        (*insMemo.pends)[0] = 0;
        (*insMemo.pends)[1] = holeNCoords + 1;
        if (!holePolygonArcs.IsEmpty ()) {
            const GS::Array<API_PolyArc> holeArcs = GetPolyArcs (holePolygonArcs, 1);
            insMemo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (holeArcs.GetSize () * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
            if (insMemo.parcs == nullptr || *insMemo.parcs == nullptr) {
                return "Failed to allocate slab hole arcs.";
            }
            for (UIndex i = 0; i < holeArcs.GetSize (); ++i) {
                (*insMemo.parcs)[i] = holeArcs[i];
            }
        }
        GSErrCode err = TAPIR_Polygon_InsertSubPoly (&memo, &insMemo);
        if (err != NoError) {
            return "Failed to add a slab hole.";
        }
    }

    // edgeTrims/sideMaterials are not touched by the Insert/Delete primitives above (per their own
    // documentation - "other memo handles are not touched"), so they are resized fresh to match the
    // final coordinate count. edgeTrims is a plain Handle (BMKillHandle+BMAllocateHandle is safe);
    // sideMaterials is a raw Ptr, and BMReallocPtr on a GetMemo-provided one reliably corrupted the
    // heap (issue #452's original crash report) - so it is freed and reallocated fresh too, never
    // resized in place. Despite the DevKit's own Do_Poly_InsertNode/DeleteNode reference example
    // treating this as unnecessary for API_SlabID and using APIMemoMask_Polygon alone for
    // ACAPI_Element_ChangeMemo, using that narrower mask here reproducibly failed with
    // APIERR_BADPARS on this exact hole-removal case - live-confirmed; the combined mask plus this
    // rebuild is what actually works for hole add/remove, multi-hole, and arcs.
    const Int32 finalNCoords = (Int32) (BMGetHandleSize ((GSHandle) memo.coords) / sizeof (API_Coord)) - 1;
    if (memo.edgeTrims != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.edgeTrims));
    }
    memo.edgeTrims = reinterpret_cast<API_EdgeTrim**> (BMAllocateHandle ((finalNCoords + 1) * sizeof (API_EdgeTrim), ALLOCATE_CLEAR, 0));
    if (memo.edgeTrims == nullptr || *memo.edgeTrims == nullptr) {
        return "Failed to allocate slab edge trims.";
    }
    if (memo.sideMaterials != nullptr) {
        BMKillPtr (reinterpret_cast<GSPtr*> (&memo.sideMaterials));
    }
    memo.sideMaterials = reinterpret_cast<API_OverriddenAttribute*> (BMAllocatePtr ((finalNCoords + 1) * sizeof (API_OverriddenAttribute), ALLOCATE_CLEAR, 0));
    if (memo.sideMaterials == nullptr) {
        return "Failed to allocate slab side materials.";
    }
    for (Int32 i = 1; i <= finalNCoords; ++i) {
        (*memo.edgeTrims)[i].sideType = APIEdgeTrim_Vertical;
        memo.sideMaterials[i] = sideMat;
    }

    return {};
}

namespace {

GSErrCode AddAdditionalPolyToMemo (
    const GS::Array<GS::ObjectState>& coords,
    const GS::Array<GS::ObjectState>& arcs,
    Int32& iCoord,
    Int32& iArc,
    Int32& iPends,
    API_ElementMemo& memo)
{
    if (coords.GetSize () < 3 || memo.additionalPolyCoords == nullptr || *memo.additionalPolyCoords == nullptr ||
        memo.additionalPolyPends == nullptr || *memo.additionalPolyPends == nullptr || iCoord < 1 || iArc < 0 || iPends < 1)
        return APIERR_BADPARS;

    const GSSize coordCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.additionalPolyCoords)) / sizeof (API_Coord);
    const GSSize pendsCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.additionalPolyPends)) / sizeof (Int32);
    const GSSize coordinateCount = static_cast<GSSize> (coords.GetSize ());
    if (coordinateCount > std::numeric_limits<Int32>::max () ||
        static_cast<GSSize> (iCoord) + coordinateCount + 1 > coordCapacity ||
        static_cast<GSSize> (iPends) >= pendsCapacity)
        return APIERR_BADPARS;

    const GS::Array<API_PolyArc> polyArcs = GetPolyArcs (arcs, iCoord);
    if (!polyArcs.IsEmpty ()) {
        if (memo.additionalPolyParcs == nullptr || *memo.additionalPolyParcs == nullptr)
            return APIERR_BADPARS;
        const GSSize arcCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.additionalPolyParcs)) / sizeof (API_PolyArc);
        if (static_cast<GSSize> (iArc) + static_cast<GSSize> (polyArcs.GetSize ()) > arcCapacity)
            return APIERR_BADPARS;
    }

    const Int32 startIndex = iCoord;
    for (const GS::ObjectState& coord : coords) {
        (*memo.additionalPolyCoords)[iCoord++] = Get2DCoordinateFromObjectState (coord);
    }

    (*memo.additionalPolyCoords)[iCoord] = (*memo.additionalPolyCoords)[startIndex];
    (*memo.additionalPolyPends)[iPends++] = iCoord;
    ++iCoord;

    for (const API_PolyArc& polyArc : polyArcs) {
        (*memo.additionalPolyParcs)[iArc++] = polyArc;
    }
    return NoError;
}

GS::Optional<GS::UniString> BuildRoofMemoFromGeometry (
    API_Element& element,
    API_ElementMemo& memo,
    GS::Array<GS::ObjectState>& polygonOutline,
    const GS::Array<GS::ObjectState>& polygonArcs,
    const GS::Array<GS::ObjectState>& holes)
{
    if (polygonOutline.GetSize () < 3) {
        return "'polygonOutline' must contain at least 3 coordinates.";
    }

    if (IsSame2DCoordinate (polygonOutline.GetFirst (), polygonOutline.GetLast ())) {
        polygonOutline.Pop ();
    }
    const GSSize polygonCoordinateCount = static_cast<GSSize> (polygonOutline.GetSize ());
    if (polygonCoordinateCount < 3 || polygonCoordinateCount > static_cast<GSSize> (std::numeric_limits<Int32>::max () - 1))
        return "'polygonOutline' must contain between 3 and INT32_MAX-1 distinct coordinates.";
    const GSSize polygonArcCount = static_cast<GSSize> (polygonArcs.GetSize ());
    if (polygonArcCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()))
        return "'polygonArcs' exceeds the supported range.";

    element.roof.u.polyRoof.pivotPolygon.nCoords = static_cast<Int32> (polygonCoordinateCount) + 1;
    element.roof.u.polyRoof.pivotPolygon.nSubPolys = 1;
    element.roof.u.polyRoof.pivotPolygon.nArcs = static_cast<Int32> (polygonArcCount);

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs) && holePolygonOutline.GetSize () >= 3) {
            const GSSize holeCoordinateCount = static_cast<GSSize> (holePolygonOutline.GetSize ());
            const GSSize holeArcCount = static_cast<GSSize> (holePolygonArcs.GetSize ());
            const GSSize maxInt32 = static_cast<GSSize> (std::numeric_limits<Int32>::max ());
            const GSSize currentCoordinateCount = static_cast<GSSize> (element.roof.u.polyRoof.pivotPolygon.nCoords);
            const GSSize currentArcCount = static_cast<GSSize> (element.roof.u.polyRoof.pivotPolygon.nArcs);
            if (currentCoordinateCount > maxInt32 - 1 || currentArcCount > maxInt32 ||
                static_cast<GSSize> (element.roof.u.polyRoof.pivotPolygon.nSubPolys) >= maxInt32 ||
                holeCoordinateCount > maxInt32 - currentCoordinateCount - 1 ||
                holeArcCount > maxInt32 - currentArcCount)
                return "Roof polygon dimensions exceed the supported range.";
            element.roof.u.polyRoof.pivotPolygon.nCoords += static_cast<Int32> (holeCoordinateCount) + 1;
            ++element.roof.u.polyRoof.pivotPolygon.nSubPolys;
            element.roof.u.polyRoof.pivotPolygon.nArcs += static_cast<Int32> (holeArcCount);
        }
    }

    // GetDefaults typically leaves the roof pivot-polygon memo handles null, and
    // BMReallocHandle does not allocate from a null handle (it returns null), which would
    // crash AddAdditionalPolyToMemo with a null dereference. Allocate fresh when null
    // (same fix as the slab path); only resize an existing handle with BMReallocHandle.
    const Int32 roofNCoords   = element.roof.u.polyRoof.pivotPolygon.nCoords;
    const Int32 roofNSubPolys = element.roof.u.polyRoof.pivotPolygon.nSubPolys;
    const Int32 roofNArcs     = element.roof.u.polyRoof.pivotPolygon.nArcs;
    if (roofNCoords < 4 || roofNSubPolys < 1 || roofNArcs < 0)
        return "Invalid roof pivot polygon memo dimensions.";

    const GSSize roofCoordCount = static_cast<GSSize> (roofNCoords) + 1;
    const GSSize roofSubPolyCount = static_cast<GSSize> (roofNSubPolys) + 1;
    if (roofCoordCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord) ||
        roofSubPolyCount > std::numeric_limits<GSSize>::max () / sizeof (Int32) ||
        (roofNArcs > 0 && static_cast<GSSize> (roofNArcs) > std::numeric_limits<GSSize>::max () / sizeof (API_PolyArc)))
        return "Roof pivot polygon memo dimensions exceed the supported range.";
    const GSSize coordBytes = roofCoordCount * sizeof (API_Coord);
    const GSSize pendsBytes = roofSubPolyCount * sizeof (Int32);
    if (memo.additionalPolyCoords == nullptr) {
        memo.additionalPolyCoords = reinterpret_cast<API_Coord**> (BMAllocateHandle (coordBytes, ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.additionalPolyCoords == nullptr)
            return "Roof pivot coordinate memo handle is invalid.";
        GSHandle resizedCoords = BMReallocHandle (reinterpret_cast<GSHandle> (memo.additionalPolyCoords), coordBytes, REALLOC_CLEAR, 0);
        if (resizedCoords == nullptr)
            return "Failed to resize roof pivot coordinate memo.";
        memo.additionalPolyCoords = reinterpret_cast<API_Coord**> (resizedCoords);
    }
    if (memo.additionalPolyPends == nullptr) {
        memo.additionalPolyPends = reinterpret_cast<Int32**> (BMAllocateHandle (pendsBytes, ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.additionalPolyPends == nullptr)
            return "Roof pivot endpoint memo handle is invalid.";
        GSHandle resizedPends = BMReallocHandle (reinterpret_cast<GSHandle> (memo.additionalPolyPends), pendsBytes, REALLOC_CLEAR, 0);
        if (resizedPends == nullptr)
            return "Failed to resize roof pivot endpoint memo.";
        memo.additionalPolyPends = reinterpret_cast<Int32**> (resizedPends);
    }
    if (roofNArcs > 0) {
        const GSSize arcBytes = static_cast<GSSize> (roofNArcs) * sizeof (API_PolyArc);
        if (memo.additionalPolyParcs == nullptr) {
            memo.additionalPolyParcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (arcBytes, ALLOCATE_CLEAR, 0));
        } else {
            if (*memo.additionalPolyParcs == nullptr)
                return "Roof pivot arc memo handle is invalid.";
            GSHandle resizedArcs = BMReallocHandle (reinterpret_cast<GSHandle> (memo.additionalPolyParcs), arcBytes, REALLOC_CLEAR, 0);
            if (resizedArcs == nullptr)
                return "Failed to resize roof pivot arc memo.";
            memo.additionalPolyParcs = reinterpret_cast<API_PolyArc**> (resizedArcs);
        }
    } else if (memo.additionalPolyParcs != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.additionalPolyParcs));
        memo.additionalPolyParcs = nullptr;
    }
    if (memo.additionalPolyCoords == nullptr || *memo.additionalPolyCoords == nullptr ||
        memo.additionalPolyPends == nullptr || *memo.additionalPolyPends == nullptr ||
        (roofNArcs > 0 && (memo.additionalPolyParcs == nullptr || *memo.additionalPolyParcs == nullptr)))
        return "Failed to allocate roof pivot polygon memo data.";

    Int32 iCoord = 1;
    Int32 iArc = 0;
    Int32 iPends = 1;
    GSErrCode memoErr = AddAdditionalPolyToMemo (polygonOutline, polygonArcs, iCoord, iArc, iPends, memo);
    if (memoErr != NoError)
        return "Failed to populate roof pivot polygon memo data.";

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holePolygonOutline;
        GS::Array<GS::ObjectState> holePolygonArcs;
        if (GetHoleGeometry (hole, holePolygonOutline, holePolygonArcs) && holePolygonOutline.GetSize () >= 3) {
            memoErr = AddAdditionalPolyToMemo (holePolygonOutline, holePolygonArcs, iCoord, iArc, iPends, memo);
            if (memoErr != NoError)
                return "Failed to populate roof pivot hole memo data.";
        }
    }

    // A multi-plane roof (API_PolyRoofData) needs BOTH a pivot polygon (set above via the
    // additionalPoly* memo) AND a contour polygon (memo.coords/pends/parcs). The contour was
    // never populated, leaving the roof under-specified so ACAPI_Element_Create throws a
    // GSException. Use the same outline for the contour as for the pivot polygon by
    // duplicating the handles.
    element.roof.u.polyRoof.contourPolygon = element.roof.u.polyRoof.pivotPolygon;
    if (*memo.additionalPolyCoords != nullptr) {
        if (memo.coords != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.coords));
        memo.coords = nullptr;
        BMHandleToHandle (reinterpret_cast<GSConstHandle> (memo.additionalPolyCoords), reinterpret_cast<GSHandle*> (&memo.coords));
        if (memo.coords == nullptr || *memo.coords == nullptr)
            return "Failed to duplicate the roof contour coordinate memo.";
    }
    if (*memo.additionalPolyPends != nullptr) {
        if (memo.pends != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.pends));
        memo.pends = nullptr;
        BMHandleToHandle (reinterpret_cast<GSConstHandle> (memo.additionalPolyPends), reinterpret_cast<GSHandle*> (&memo.pends));
        if (memo.pends == nullptr || *memo.pends == nullptr)
            return "Failed to duplicate the roof contour endpoint memo.";
    }
    if (memo.additionalPolyParcs != nullptr && *memo.additionalPolyParcs != nullptr) {
        if (memo.parcs != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.parcs));
        memo.parcs = nullptr;
        BMHandleToHandle (reinterpret_cast<GSConstHandle> (memo.additionalPolyParcs), reinterpret_cast<GSHandle*> (&memo.parcs));
        if (memo.parcs == nullptr || *memo.parcs == nullptr)
            return "Failed to duplicate the roof contour arc memo.";
    }

    return {};
}

GS::Optional<GS::UniString> ApplyWallStructure (
    API_Element& element,
    API_Element* mask,
    const GS::ObjectState& details,
    bool& changed)
{
    StructureSelection selection;
    auto error = ParseStructureSelection (details, true, true, selection);
    if (error.HasValue ()) {
        return error;
    }

    if (selection.kind == StructureSelectionKind::Unspecified) {
        return {};
    }

    switch (selection.kind) {
        case StructureSelectionKind::Basic:
            element.wall.modelElemStructureType = API_BasicStructure;
            if (selection.buildingMaterial != APIInvalidAttributeIndex) {
                element.wall.buildingMaterial = selection.buildingMaterial;
            }
            element.wall.composite = APIInvalidAttributeIndex;
            element.wall.profileAttr = APIInvalidAttributeIndex;
            if (element.wall.type == APIWtyp_Poly) {
                element.wall.type = APIWtyp_Normal;
            }
            break;
        case StructureSelectionKind::Composite:
            element.wall.modelElemStructureType = API_CompositeStructure;
            if (selection.composite != APIInvalidAttributeIndex) {
                element.wall.composite = selection.composite;
            }
            element.wall.profileAttr = APIInvalidAttributeIndex;
            if (element.wall.type == APIWtyp_Poly) {
                element.wall.type = APIWtyp_Normal;
            }
            break;
        case StructureSelectionKind::Profile:
            element.wall.modelElemStructureType = API_ProfileStructure;
            if (selection.profile != APIInvalidAttributeIndex) {
                element.wall.profileAttr = selection.profile;
            }
            break;
        case StructureSelectionKind::Unspecified:
            break;
    }

    if (mask != nullptr) {
        ACAPI_ELEMENT_MASK_SET ((*mask), API_WallType, modelElemStructureType);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_WallType, buildingMaterial);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_WallType, composite);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_WallType, profileAttr);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_WallType, type);
    }
    changed = true;
    return {};
}

GS::Optional<GS::UniString> ApplyRoofStructure (
    API_Element& element,
    API_Element* mask,
    const GS::ObjectState& details,
    bool& changed)
{
    StructureSelection selection;
    auto error = ParseStructureSelection (details, true, false, selection);
    if (error.HasValue ()) {
        return error;
    }

    if (selection.kind == StructureSelectionKind::Unspecified) {
        return {};
    }

    switch (selection.kind) {
        case StructureSelectionKind::Basic:
            element.roof.shellBase.modelElemStructureType = API_BasicStructure;
            if (selection.buildingMaterial != APIInvalidAttributeIndex) {
                element.roof.shellBase.buildingMaterial = selection.buildingMaterial;
            }
            element.roof.shellBase.composite = APIInvalidAttributeIndex;
            break;
        case StructureSelectionKind::Composite:
            element.roof.shellBase.modelElemStructureType = API_CompositeStructure;
            if (selection.composite != APIInvalidAttributeIndex) {
                element.roof.shellBase.composite = selection.composite;
            }
            break;
        case StructureSelectionKind::Profile:
        case StructureSelectionKind::Unspecified:
            break;
    }

    if (mask != nullptr) {
        ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, shellBase.modelElemStructureType);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, shellBase.buildingMaterial);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, shellBase.composite);
    }
    changed = true;
    return {};
}

GS::Optional<GS::UniString> ApplySlabStructure (
    API_Element& element,
    API_Element* mask,
    const GS::ObjectState& details,
    bool& changed)
{
    StructureSelection selection;
    auto error = ParseStructureSelection (details, true, false, selection);
    if (error.HasValue ()) {
        return error;
    }

    if (selection.kind == StructureSelectionKind::Unspecified) {
        return {};
    }

    switch (selection.kind) {
        case StructureSelectionKind::Basic:
            element.slab.modelElemStructureType = API_BasicStructure;
            if (selection.buildingMaterial != APIInvalidAttributeIndex) {
                element.slab.buildingMaterial = selection.buildingMaterial;
            }
            element.slab.composite = APIInvalidAttributeIndex;
            break;
        case StructureSelectionKind::Composite:
            element.slab.modelElemStructureType = API_CompositeStructure;
            if (selection.composite != APIInvalidAttributeIndex) {
                element.slab.composite = selection.composite;
            }
            break;
        case StructureSelectionKind::Profile:
        case StructureSelectionKind::Unspecified:
            break;
    }

    if (mask != nullptr) {
        ACAPI_ELEMENT_MASK_SET ((*mask), API_SlabType, modelElemStructureType);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_SlabType, buildingMaterial);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_SlabType, composite);
    }
    changed = true;
    return {};
}

bool ApplyWallDetails (API_Element& element, API_Element& mask, const GS::ObjectState& details)
{
    bool changed = false;
    auto begCoordinate = GetOptionalCoordinate2D (details, "begCoordinate");
    if (begCoordinate.HasValue ()) {
        element.wall.begC = begCoordinate.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, begC);
        changed = true;
    }
    auto endCoordinate = GetOptionalCoordinate2D (details, "endCoordinate");
    if (endCoordinate.HasValue ()) {
        element.wall.endC = endCoordinate.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, endC);
        changed = true;
    }
    auto arcAngle = GetOptionalDouble (details, "arcAngle");
    if (arcAngle.HasValue ()) {
        element.wall.angle = arcAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, angle);
        changed = true;
    }
    auto height = GetOptionalDouble (details, "height");
    if (height.HasValue ()) {
        element.wall.height = height.Get ();
        element.wall.relativeTopStory = 0;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, height);
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, relativeTopStory);
        changed = true;
    }
    auto offset = GetOptionalDouble (details, "offset");
    if (offset.HasValue ()) {
        element.wall.offset = offset.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, offset);
        changed = true;
    }
    auto thickness = GetOptionalDouble (details, "thickness");
    if (thickness.HasValue ()) {
        element.wall.thickness = thickness.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness);
        changed = true;
    }
    auto bottomOffset = GetOptionalDouble (details, "bottomOffset");
    if (bottomOffset.HasValue ()) {
        element.wall.bottomOffset = bottomOffset.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, bottomOffset);
        changed = true;
    }
    return changed;
}

GS::Optional<GS::UniString> ApplyRoofLevels (API_Element& element, API_Element* mask, const GS::ObjectState& details, bool& changed)
{
    GS::Array<GS::ObjectState> roofLevels;
    if (!details.Get ("levels", roofLevels)) {
        return {};
    }

    if (roofLevels.IsEmpty () || roofLevels.GetSize () > 16) {
        return "'levels' must contain between 1 and 16 level definitions.";
    }

    double previousHeight = -1.0e18;
    element.roof.u.polyRoof.levelNum = static_cast<short> (roofLevels.GetSize ());
    for (UIndex i = 0; i < roofLevels.GetSize (); ++i) {
        double levelHeight = 0.0;
        double levelAngle = 0.0;
        if (!roofLevels[i].Get ("levelHeight", levelHeight) || !roofLevels[i].Get ("levelAngle", levelAngle)) {
            return "Each roof level must contain 'levelHeight' and 'levelAngle'.";
        }
        if (levelAngle <= 0.0) {
            return "'levelAngle' must be greater than zero.";
        }
        if (levelHeight < previousHeight) {
            return "'levels' must be ordered by non-decreasing 'levelHeight'.";
        }

        element.roof.u.polyRoof.levelData[i].levelHeight = levelHeight;
        element.roof.u.polyRoof.levelData[i].levelAngle = levelAngle;
        previousHeight = levelHeight;
    }

    if (mask != nullptr) {
        ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, u.polyRoof.levelNum);
        ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, u.polyRoof.levelData);
    }
    changed = true;
    return {};
}

bool ApplySlabDetails (API_Element& element, API_Element& mask, const GS::ObjectState& details, const Stories& stories)
{
    bool changed = false;
    auto thickness = GetOptionalDouble (details, "thickness");
    if (thickness.HasValue ()) {
        element.slab.thickness = thickness.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, thickness);
        changed = true;
    }

    auto zCoordinate = GetOptionalDouble (details, "zCoordinate");

    if (zCoordinate.HasValue ()) {
        const auto floorIndexAndOffset = GetFloorIndexAndOffset (zCoordinate.Get (), stories);
        element.header.floorInd = floorIndexAndOffset.first;
        element.slab.level = floorIndexAndOffset.second;
        ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
        ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, level);
        changed = true;
    }

    GS::UniString referencePlaneLocation;
    if (details.Get ("referencePlaneLocation", referencePlaneLocation)) {
        bool recognized = true;
        if (referencePlaneLocation == "Top") {
            element.slab.referencePlaneLocation = APISlabRefPlane_Top;
        } else if (referencePlaneLocation == "CoreTop") {
            element.slab.referencePlaneLocation = APISlabRefPlane_CoreTop;
        } else if (referencePlaneLocation == "CoreBottom") {
            element.slab.referencePlaneLocation = APISlabRefPlane_CoreBottom;
        } else if (referencePlaneLocation == "Bottom") {
            element.slab.referencePlaneLocation = APISlabRefPlane_Bottom;
        } else {
            recognized = false;
        }
        if (recognized) {
            ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, referencePlaneLocation);
            changed = true;
        }
    }

    return changed;
}

GS::Optional<GS::UniString> ApplyRoofDetails (
    API_Element& element,
    API_Element* mask,
    const GS::ObjectState& details,
    const Stories& stories,
    bool& changed)
{
    auto thickness = GetOptionalDouble (details, "thickness");
    if (thickness.HasValue ()) {
        element.roof.shellBase.thickness = thickness.Get ();
        if (mask != nullptr) {
            ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, shellBase.thickness);
        }
        changed = true;
    }

    auto level = GetOptionalDouble (details, "level");

    if (level.HasValue ()) {
        const auto floorIndexAndOffset = ResolveFloorIndexAndOffset (details, "floorIndex", level.Get (), stories);
        element.header.floorInd = floorIndexAndOffset.first;
        element.roof.shellBase.level = floorIndexAndOffset.second;
        if (mask != nullptr) {
            ACAPI_ELEMENT_MASK_SET ((*mask), API_Elem_Head, floorInd);
            ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, shellBase.level);
        }
        changed = true;
    }

    auto eavesOverhang = GetOptionalDouble (details, "eavesOverhang");

    if (eavesOverhang.HasValue ()) {
        element.roof.u.polyRoof.overHangType = API_OffsetOverhang;
        element.roof.u.polyRoof.eavesOverHang = eavesOverhang.Get ();
        if (mask != nullptr) {
            ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, u.polyRoof.overHangType);
            ACAPI_ELEMENT_MASK_SET ((*mask), API_RoofType, u.polyRoof.eavesOverHang);
        }
        changed = true;
    }

    return ApplyRoofLevels (element, mask, details, changed);
}

bool ApplyColumnDetails (API_Element& element, API_Element& mask, const GS::ObjectState& details, const Stories& stories)
{
    bool changed = false;
    auto origin = GetOptionalCoordinate2D (details, "origin");
    if (origin.HasValue ()) {
        element.column.origoPos = origin.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, origoPos);
        changed = true;
    }
    auto zCoordinate = GetOptionalDouble (details, "zCoordinate");
    if (zCoordinate.HasValue ()) {
        const auto floorIndexAndOffset = GetFloorIndexAndOffset (zCoordinate.Get (), stories);
        element.header.floorInd = floorIndexAndOffset.first;
        element.column.bottomOffset = floorIndexAndOffset.second;
        ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, bottomOffset);
        changed = true;
    }
    auto height = GetOptionalDouble (details, "height");
    if (height.HasValue ()) {
        element.column.height = height.Get ();
        element.column.relativeTopStory = 0;
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, height);
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, relativeTopStory);
        changed = true;
    }
    auto bottomOffset = GetOptionalDouble (details, "bottomOffset");
    if (bottomOffset.HasValue ()) {
        element.column.bottomOffset = bottomOffset.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, bottomOffset);
        changed = true;
    }
    auto axisRotationAngle = GetOptionalDouble (details, "axisRotationAngle");
    if (axisRotationAngle.HasValue ()) {
        element.column.axisRotationAngle = axisRotationAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, axisRotationAngle);
        changed = true;
    }
    bool isSlanted = false;
    if (details.Get ("isSlanted", isSlanted)) {
        element.column.isSlanted = isSlanted;
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, isSlanted);
        changed = true;
    }
    auto slantAngle = GetOptionalDouble (details, "slantAngle");
    if (slantAngle.HasValue ()) {
        element.column.slantAngle = slantAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, slantAngle);
        // Archicad discards a column slant angle while the column remains vertical.
        // Derive the state only when the caller did not provide it explicitly.
        bool explicitIsSlanted = false;
        if (!details.Get ("isSlanted", explicitIsSlanted)) {
            element.column.isSlanted = (slantAngle.Get () != 0.0);
            ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, isSlanted);
        }
        changed = true;
    }
    return changed;
}

bool ApplyBeamDetails (API_Element& element, API_Element& mask, const GS::ObjectState& details)
{
    bool changed = false;
    auto begCoordinate = GetOptionalCoordinate2D (details, "begCoordinate");
    if (begCoordinate.HasValue ()) {
        element.beam.begC = begCoordinate.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, begC);
        changed = true;
    }
    auto endCoordinate = GetOptionalCoordinate2D (details, "endCoordinate");
    if (endCoordinate.HasValue ()) {
        element.beam.endC = endCoordinate.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, endC);
        changed = true;
    }
    auto level = GetOptionalDouble (details, "level");
    if (level.HasValue ()) {
        element.beam.level = level.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, level);
        changed = true;
    }
    auto offset = GetOptionalDouble (details, "offset");
    if (offset.HasValue ()) {
        element.beam.offset = offset.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, offset);
        changed = true;
    }
    auto slantAngle = GetOptionalDouble (details, "slantAngle");
    if (slantAngle.HasValue ()) {
        element.beam.slantAngle = slantAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, slantAngle);
        // Archicad discards a beam slant angle while the beam remains horizontal.
        // Derive the state only when the caller did not provide it explicitly.
        bool explicitIsSlanted = false;
        if (!details.Get ("isSlanted", explicitIsSlanted)) {
            element.beam.isSlanted = (slantAngle.Get () != 0.0);
            ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, isSlanted);
        }
        changed = true;
    }
    auto arcAngle = GetOptionalDouble (details, "arcAngle");
    if (arcAngle.HasValue ()) {
        element.beam.curveAngle = arcAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, curveAngle);
        changed = true;
    }
    auto curveHeight = GetOptionalDouble (details, "verticalCurveHeight");
    if (curveHeight.HasValue ()) {
        element.beam.verticalCurveHeight = curveHeight.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, verticalCurveHeight);
        changed = true;
    }
    bool isSlanted = false;
    if (details.Get ("isSlanted", isSlanted)) {
        element.beam.isSlanted = isSlanted;
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, isSlanted);
        changed = true;
    }
    auto profileAngle = GetOptionalDouble (details, "profileAngle");
    if (profileAngle.HasValue ()) {
        element.beam.profileAngle = profileAngle.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_BeamType, profileAngle);
        changed = true;
    }
    return changed;
}

bool BuildCuboidMorphMemo (double sizeX, double sizeY, double sizeZ, API_AttributeIndex buildingMaterial, API_ElementMemo& memo)
{
    void* bodyData = nullptr;
    if (ACAPI_Body_Create (nullptr, nullptr, &bodyData) != NoError || bodyData == nullptr) {
        return false;
    }

    const GS::OnExit disposeBody ([&]() {
        if (bodyData != nullptr) {
            ACAPI_Body_Dispose (&bodyData);
        }
    });

    API_Coord3D coords[] = {
        {0.0,   0.0,   0.0},
        {sizeX, 0.0,   0.0},
        {sizeX, sizeY, 0.0},
        {0.0,   sizeY, 0.0},
        {0.0,   0.0,   sizeZ},
        {sizeX, 0.0,   sizeZ},
        {sizeX, sizeY, sizeZ},
        {0.0,   sizeY, sizeZ}
    };

    UInt32 vertices[8];
    for (UIndex i = 0; i < 8; ++i) {
        ACAPI_Body_AddVertex (bodyData, coords[i], vertices[i]);
    }

    Int32 edges[12];
    ACAPI_Body_AddEdge (bodyData, vertices[0], vertices[1], edges[0]);
    ACAPI_Body_AddEdge (bodyData, vertices[1], vertices[2], edges[1]);
    ACAPI_Body_AddEdge (bodyData, vertices[2], vertices[3], edges[2]);
    ACAPI_Body_AddEdge (bodyData, vertices[3], vertices[0], edges[3]);
    ACAPI_Body_AddEdge (bodyData, vertices[4], vertices[5], edges[4]);
    ACAPI_Body_AddEdge (bodyData, vertices[5], vertices[6], edges[5]);
    ACAPI_Body_AddEdge (bodyData, vertices[6], vertices[7], edges[6]);
    ACAPI_Body_AddEdge (bodyData, vertices[7], vertices[4], edges[7]);
    ACAPI_Body_AddEdge (bodyData, vertices[0], vertices[4], edges[8]);
    ACAPI_Body_AddEdge (bodyData, vertices[1], vertices[5], edges[9]);
    ACAPI_Body_AddEdge (bodyData, vertices[2], vertices[6], edges[10]);
    ACAPI_Body_AddEdge (bodyData, vertices[3], vertices[7], edges[11]);

#ifdef ServerMainVers_2700
    API_OverriddenAttribute material;
    material = buildingMaterial;
#else
    (void) buildingMaterial;
    API_OverriddenAttribute material = {};
#endif
    UInt32 polygon = 0;
    ACAPI_Body_AddPolygon (bodyData, {edges[0], edges[1], edges[2], edges[3]}, 0, material, polygon);
    ACAPI_Body_AddPolygon (bodyData, {edges[4], edges[5], edges[6], edges[7]}, 0, material, polygon);
    ACAPI_Body_AddPolygon (bodyData, {edges[0], edges[9], -edges[4], -edges[8]}, 0, material, polygon);
    ACAPI_Body_AddPolygon (bodyData, {edges[1], edges[10], -edges[5], -edges[9]}, 0, material, polygon);
    ACAPI_Body_AddPolygon (bodyData, {edges[2], edges[11], -edges[6], -edges[10]}, 0, material, polygon);
    ACAPI_Body_AddPolygon (bodyData, {edges[3], edges[8], -edges[7], -edges[11]}, 0, material, polygon);

    if (ACAPI_Body_Finish (bodyData, &memo.morphBody, &memo.morphMaterialMapTable) != NoError) {
        return false;
    }

    return true;
}

bool ApplyWindowOrDoorDetails (API_Element& element, API_Element& mask, const GS::ObjectState& details)
{
    bool changed = false;
    auto width = GetOptionalDouble (details, "width");
    if (width.HasValue ()) {
        element.window.openingBase.width = width.Get ();
        SetOpeningSizeMask (mask);
        changed = true;
    }
    auto height = GetOptionalDouble (details, "height");
    if (height.HasValue ()) {
        element.window.openingBase.height = height.Get ();
        SetOpeningSizeMask (mask);
        changed = true;
    }
    auto sillHeight = GetOptionalDouble (details, "sillHeight");
    if (sillHeight.HasValue ()) {
        element.window.lower = sillHeight.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, lower);
        changed = true;
    }
    auto centerOffset = GetOptionalDouble (details, "centerOffset");
    if (centerOffset.HasValue ()) {
        element.window.objLoc = centerOffset.Get ();
        ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, objLoc);
        changed = true;
    }
    bool reflected = false;
    if (details.Get ("reflected", reflected)) {
        element.window.openingBase.reflected = reflected;
        ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.reflected);
        changed = true;
    }
    bool refSide = false;
    if (details.Get ("refSide", refSide)) {
        element.window.openingBase.refSide = refSide;
        ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.refSide);
        changed = true;
    }
    bool oSide = false;
    if (details.Get ("oSide", oSide)) {
        element.window.openingBase.oSide = oSide;
        ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.oSide);
        changed = true;
    }
    return changed;
}

}

CreateWallsCommand::CreateWallsCommand () :
    CreateElementsCommandBase ("CreateWalls", API_WallID, "wallsData")
{
}

GS::Optional<GS::UniString> CreateWallsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "wallsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "begCoordinate": { "$ref": "#/Coordinate2D" },
                        "endCoordinate": { "$ref": "#/Coordinate2D" },
                        "floorIndex": { "type": "integer", "description": "Story index (as returned by GetStories). When provided, zCoordinate is interpreted as bottomOffset relative to the floor. Takes priority over zCoordinate for floor assignment." },
                        "zCoordinate": { "type": "number", "description": "Absolute Z when floorIndex is absent; bottomOffset relative to the floor when floorIndex is provided." },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "thickness": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "offset": { "type": "number" },
                        "arcAngle": { "type": "number", "description": "Arc angle in radians; non-zero creates a curved wall (begCoordinate/endCoordinate are the chord endpoints)." },
                        "referenceLineLocation": {
                            "type": "string",
                            "enum": ["Outside", "Center", "Inside", "CoreOutside", "CoreCenter", "CoreInside"]
                        },
                        "flipped": { "type": "boolean", "description": "The Wall body lies on the mirrored side of its reference line." },
                        "structureType": {
                            "type": "string",
                            "enum": ["Basic", "Composite", "Profile"]
                        },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "compositeId": { "$ref": "#/AttributeId" },
                        "profileId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["begCoordinate", "endCoordinate", "height", "thickness"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["wallsData"]
    })";
}

GS::Optional<GS::ObjectState> CreateWallsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo&, const Stories& stories, const GS::ObjectState& parameters) const
{
    if (parameters.Get ("begCoordinate") == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'begCoordinate' parameter.");
    }
    if (parameters.Get ("endCoordinate") == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'endCoordinate' parameter.");
    }
    API_Coord begCoordinate = Get2DCoordinateFromObjectState (*parameters.Get ("begCoordinate"));
    API_Coord endCoordinate = Get2DCoordinateFromObjectState (*parameters.Get ("endCoordinate"));

    if (IsSame2DCoordinate (begCoordinate, endCoordinate)) {
        return CreateErrorResponse (APIERR_BADPARS, "Zero-length wall: 'begCoordinate' and 'endCoordinate' are identical.");
    }

    double zCoordinate = 0.0;
    double height = 0.0;
    double thickness = 0.0;
    parameters.Get ("zCoordinate", zCoordinate);
    parameters.Get ("height", height);
    parameters.Get ("thickness", thickness);

    element.wall.type = APIWtyp_Normal;
    element.wall.begC = begCoordinate;
    element.wall.endC = endCoordinate;
    auto arcAngle = GetOptionalDouble (parameters, "arcAngle");
    if (arcAngle.HasValue ()) {
        element.wall.angle = arcAngle.Get ();
    }
    element.wall.height = height;
    element.wall.relativeTopStory = 0;
    element.wall.thickness = thickness;
    element.wall.referenceLineLocation = APIWallRefLine_Center;
    GS::UniString referenceLineLocation;
    if (parameters.Get ("referenceLineLocation", referenceLineLocation)) {
        if (referenceLineLocation == "Outside") {
            element.wall.referenceLineLocation = APIWallRefLine_Outside;
        } else if (referenceLineLocation == "Center") {
            element.wall.referenceLineLocation = APIWallRefLine_Center;
        } else if (referenceLineLocation == "Inside") {
            element.wall.referenceLineLocation = APIWallRefLine_Inside;
        } else if (referenceLineLocation == "CoreOutside") {
            element.wall.referenceLineLocation = APIWallRefLine_CoreOutside;
        } else if (referenceLineLocation == "CoreCenter") {
            element.wall.referenceLineLocation = APIWallRefLine_CoreCenter;
        } else if (referenceLineLocation == "CoreInside") {
            element.wall.referenceLineLocation = APIWallRefLine_CoreInside;
        }
    }
    element.wall.modelElemStructureType = API_BasicStructure;
    element.wall.offset = 0.0;
    // Without an explicit value the Wall tool default decides the side.
    bool flipped = false;
    if (parameters.Get ("flipped", flipped)) {
        element.wall.flipped = flipped;
    }

    auto offset = GetOptionalDouble (parameters, "offset");

    if (offset.HasValue ()) {
        element.wall.offset = offset.Get ();
    }

    Int32 explicitFloorIndex = -1;
    if (parameters.Get ("floorIndex", explicitFloorIndex)) {
        element.header.floorInd   = static_cast<short> (explicitFloorIndex);
        element.wall.bottomOffset = zCoordinate;
    } else {
        const auto floorIndexAndOffset = GetFloorIndexAndOffset (zCoordinate, stories);
        element.header.floorInd   = floorIndexAndOffset.first;
        element.wall.bottomOffset = floorIndexAndOffset.second;
    }

    bool structureChanged = false;
    auto error = ApplyWallStructure (element, nullptr, parameters, structureChanged);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return {};
}

CreateBeamsCommand::CreateBeamsCommand () :
    CreateElementsCommandBase ("CreateBeams", API_BeamID, "beamsData")
{
}

GS::Optional<GS::UniString> CreateBeamsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "beamsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "begCoordinate": { "$ref": "#/Coordinate2D" },
                        "endCoordinate": { "$ref": "#/Coordinate2D" },
                        "floorIndex": { "type": "integer", "description": "Optional floor index. If omitted, derived from zCoordinate." },
                        "zCoordinate": { "type": "number" },
                        "offset": { "type": "number" },
                        "slantAngle": {
                            "type": "number",
                            "description": "Slant angle in radians. A non-zero value also switches the beam to slanted, unless isSlanted is given explicitly."
                        },
                        "isSlanted": {
                            "type": "boolean",
                            "description": "Optional explicit slanted state. By default it is derived from slantAngle."
                        },
                        "profileAngle": {
                            "type": "number",
                            "description": "Rotation angle of the profile around the beam's center line, in radians."
                        },
                        "arcAngle": { "type": "number" },
                        "verticalCurveHeight": { "type": "number" },
                        "width": {
                            "type": "number",
                            "description": "Cross section width of the beam. Applied to all segments.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        },
                        "height": {
                            "type": "number",
                            "description": "Cross section height of the beam. Applied to all segments.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        },
                        "anchorPoint": {
                            "type": "string",
                            "description": "Optional anchor point of the beam cross section on a 3x3 grid.",
                            "enum": ["TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center", "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"]
                        }
                    },
                    "additionalProperties": false,
                    "required": ["begCoordinate", "endCoordinate", "zCoordinate"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["beamsData"]
    })";
}

GS::Optional<GS::ObjectState> CreateBeamsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const
{
    if (parameters.Get ("begCoordinate") == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'begCoordinate' parameter.");
    }
    if (parameters.Get ("endCoordinate") == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required 'endCoordinate' parameter.");
    }
    element.beam.begC = Get2DCoordinateFromObjectState (*parameters.Get ("begCoordinate"));
    element.beam.endC = Get2DCoordinateFromObjectState (*parameters.Get ("endCoordinate"));

    double zCoordinate = 0.0;
    parameters.Get ("zCoordinate", zCoordinate);
    const auto floorIndexAndOffset = ResolveFloorIndexAndOffset (parameters, "floorIndex", zCoordinate, stories);
    element.header.floorInd = floorIndexAndOffset.first;
    element.beam.level = floorIndexAndOffset.second;

    auto offset = GetOptionalDouble (parameters, "offset");

    if (offset.HasValue ()) {
        element.beam.offset = offset.Get ();
    }
    auto slantAngle = GetOptionalDouble (parameters, "slantAngle");
    if (slantAngle.HasValue ()) {
        element.beam.slantAngle = slantAngle.Get ();
        // Without isSlanted the new beam remains horizontal and Archicad discards
        // the angle. An explicit value below intentionally wins over this derivation.
        element.beam.isSlanted = (slantAngle.Get () != 0.0);
    }
    bool isSlanted = false;
    if (parameters.Get ("isSlanted", isSlanted)) {
        element.beam.isSlanted = isSlanted;
    }
    auto profileAngle = GetOptionalDouble (parameters, "profileAngle");
    if (profileAngle.HasValue ()) {
        element.beam.profileAngle = profileAngle.Get ();
    }
    auto arcAngle = GetOptionalDouble (parameters, "arcAngle");
    if (arcAngle.HasValue ()) {
        element.beam.curveAngle = arcAngle.Get ();
    }
    auto curveHeight = GetOptionalDouble (parameters, "verticalCurveHeight");
    if (curveHeight.HasValue ()) {
        element.beam.verticalCurveHeight = curveHeight.Get ();
    }

    GS::UniString anchorPoint;
    if (parameters.Get ("anchorPoint", anchorPoint)) {
        element.beam.anchorPoint = ParseAnchorPointString (anchorPoint);
    }

    auto width = GetOptionalDouble (parameters, "width");
    auto height = GetOptionalDouble (parameters, "height");

    if ((width.HasValue () || height.HasValue ()) && memo.beamSegments != nullptr) {
        GSSize nSegments = BMGetPtrSize (reinterpret_cast<GSPtr>(memo.beamSegments)) / sizeof (API_BeamSegmentType);
        for (GSSize i = 0; i < nSegments; ++i) {
            if (width.HasValue () && height.HasValue ()) {
                memo.beamSegments[i].assemblySegmentData.isWidthAndHeightLinked = false;
            }
            if (width.HasValue ()) {
                memo.beamSegments[i].assemblySegmentData.nominalWidth = width.Get ();
            }
            if (height.HasValue ()) {
                memo.beamSegments[i].assemblySegmentData.nominalHeight = height.Get ();
            }
        }
    }

    return {};
}

CreateStairsCommand::CreateStairsCommand () :
    CreateElementsCommandBase ("CreateStairs", API_StairID, "stairsData")
{
}

GS::Optional<GS::UniString> CreateStairsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "stairsData": {
                "type": "array",
                "description": "Array of data to create Stair elements.",
                "items": {
                    "type": "object",
                    "description": "The parameters of the new Stair.",
                    "properties": {
                        "baseLinePoints": {
                            "type": "array",
                            "description": "2D coordinates defining the stair baseline polyline. Minimum 2 points for a straight stair, 3+ for L-shaped or U-shaped stairs.",
                            "items": { "$ref": "#/Coordinate2D" },
                            "minItems": 2
                        },
                        "zCoordinate": {
                            "type": "number",
                            "description": "The Z coordinate (absolute elevation) of the stair base."
                        },
                        "floorIndex": {
                            "type": "integer",
                            "description": "Optional floor index. If omitted, derived from zCoordinate."
                        },
                        "totalHeight": {
                            "type": "number",
                            "description": "Total height of the stair.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        },
                        "flightWidth": {
                            "type": "number",
                            "description": "Width of the stair flight.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        },
                        "stepNum": {
                            "type": "integer",
                            "description": "Number of risers (steps).",
                            "minimum": 1
                        },
                        "riserHeight": {
                            "type": "number",
                            "description": "Height of each riser.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        },
                        "treadDepth": {
                            "type": "number",
                            "description": "Depth (going) of each tread.",
                            "minimum": 0.0, "exclusiveMinimum": true
                        }
                    },
                    "additionalProperties": false,
                    "required": ["baseLinePoints", "zCoordinate"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["stairsData"]
    })";
}

GS::Optional<GS::ObjectState> CreateStairsCommand::SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const
{
    GS::Array<GS::ObjectState> baseLinePoints;
    parameters.Get ("baseLinePoints", baseLinePoints);
    if (baseLinePoints.GetSize () < 2) {
        return CreateErrorResponse (APIERR_BADPARS, "baseLinePoints must have at least 2 points.");
    }

    double zCoordinate = 0.0;
    parameters.Get ("zCoordinate", zCoordinate);
    const auto floorIndexAndOffset = ResolveFloorIndexAndOffset (parameters, "floorIndex", zCoordinate, stories);
    element.header.floorInd = floorIndexAndOffset.first;

    auto totalHeight = GetOptionalDouble (parameters, "totalHeight");
    if (totalHeight.HasValue ()) {
        element.stair.totalHeight = totalHeight.Get ();
    }
    auto flightWidth = GetOptionalDouble (parameters, "flightWidth");
    if (flightWidth.HasValue ()) {
        element.stair.flightWidth = flightWidth.Get ();
    }

    Int32 stepNum = 0;
    if (parameters.Get ("stepNum", stepNum)) {
        element.stair.stepNum = static_cast<UInt32> (stepNum);
    }

    auto riserHeight = GetOptionalDouble (parameters, "riserHeight");
    if (riserHeight.HasValue ()) {
        element.stair.riserHeight = riserHeight.Get ();
    }
    auto treadDepth = GetOptionalDouble (parameters, "treadDepth");
    if (treadDepth.HasValue ()) {
        element.stair.treadDepth = treadDepth.Get ();
    }

    // Build the baseline polyline in the memo.
    const GSSize baselinePointCount = static_cast<GSSize> (baseLinePoints.GetSize ());
    if (baselinePointCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()))
        return CreateErrorResponse (APIERR_BADPARS, "baseLinePoints exceeds the supported range.");
    const Int32 nCoords = static_cast<Int32> (baselinePointCount);
    const GSSize baselineCoordCount = baselinePointCount + 1;
    if (baselineCoordCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord))
        return CreateErrorResponse (APIERR_BADPARS, "Stair baseline dimensions exceed the supported range.");

    // Allocate replacements before releasing the defaults. A failed allocation must not
    // leave a partially initialized memo that the code below could dereference.
    API_Coord** newCoords = reinterpret_cast<API_Coord**> (BMAllocateHandle (baselineCoordCount * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    Int32** newPends = reinterpret_cast<Int32**> (BMAllocateHandle (static_cast<GSSize> (2) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    if (newCoords == nullptr || *newCoords == nullptr || newPends == nullptr || *newPends == nullptr) {
        if (newCoords != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&newCoords));
        if (newPends != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&newPends));
        return CreateErrorResponse (APIERR_MEMFULL, "Failed to allocate stair baseline memo data.");
    }

    // Free existing baseline handles from defaults only after all replacements exist.
    if (memo.stairBaseLine.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*>(&memo.stairBaseLine.coords));
    if (memo.stairBaseLine.pends != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*>(&memo.stairBaseLine.pends));
    if (memo.stairBaseLine.parcs != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*>(&memo.stairBaseLine.parcs));
    if (memo.stairBaseLine.edgeData != nullptr)
        BMKillPtr (reinterpret_cast<GSPtr*>(&memo.stairBaseLine.edgeData));
    if (memo.stairBaseLine.vertexData != nullptr)
        BMKillPtr (reinterpret_cast<GSPtr*>(&memo.stairBaseLine.vertexData));

    // Allocate new baseline: polyline (not polygon), so no closing vertex needed
    // Coords: index 1..nCoords (1-based), index 0 unused
    // edgeData/vertexData are intentionally left null: ACAPI_Element_Create derives them
    // from the baseline geometry (see the Element_Test example in the DevKit). Pre-filling
    // them marks every edge as a steps segment, which makes multi-segment (L/U-shaped)
    // baselines fail with -2130313215 (#444).
    memo.stairBaseLine.coords = newCoords;
    memo.stairBaseLine.pends = newPends;
    memo.stairBaseLine.parcs = nullptr;

    for (Int32 i = 0; i < nCoords; ++i) {
        (*memo.stairBaseLine.coords)[i + 1] = Get2DCoordinateFromObjectState (baseLinePoints[i]);
    }

    (*memo.stairBaseLine.pends)[1] = nCoords;

    memo.stairBaseLine.polygon.nCoords = nCoords;
    memo.stairBaseLine.polygon.nSubPolys = 1;
    memo.stairBaseLine.polygon.nArcs = 0;

    return {};
}

CreateWindowsCommand::CreateWindowsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateWindowsCommand::GetName () const
{
    return "CreateWindows";
}

GS::Optional<GS::UniString> CreateWindowsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "windowsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "ownerWallId": { "$ref": "#/ElementId" },
                        "centerOffset": { "type": "number", "minimum": 0.0 },
                        "sillHeight": { "type": "number" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "reflected": { "type": "boolean" },
                        "refSide": { "type": "boolean" },
                        "oSide": { "type": "boolean" },
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional. Name of an existing Window favorite (as returned by `GetFavoritesByType`). Applied to the Window tool defaults before the create."
                        }
                    },
                    "additionalProperties": false,
                    "required": ["ownerWallId", "centerOffset"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["windowsData"]
    })";
}

GS::Optional<GS::UniString> CreateWindowsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateWindowsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> windowsData;
    auto error = GetElementArray (parameters, "windowsData", windowsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Windows", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : windowsData) {
            if (data.Get ("ownerWallId") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'ownerWallId' field."));
                continue;
            }
            const API_Guid wallGuid = GetGuidFromObjectState (*data.Get ("ownerWallId"));
            if (!DoesWallExist (wallGuid)) {
                elements.Push (CreateErrorResponse (APIERR_BADID, "Failed to load owner wall."));
                continue;
            }
            bool isPolygonalWall = false;
            const GSErrCode wallReadErr = GetWallPolygonalState (wallGuid, isPolygonalWall);
            if (wallReadErr != NoError) {
                elements.Push (CreateErrorResponse (wallReadErr, "Failed to read owner wall before creating window."));
                continue;
            }
            if (isPolygonalWall) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS,
                    "The owner wall is polygonal, and Archicad cannot place a window in one."));
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            API_SubElement marker = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
                ACAPI_DisposeElemMemoHdls (&marker.memo);
            });

            // Apply the favorite to the Window tool defaults FIRST so
            // that PrepareWindowOrDoorDefaults clones the favorite-applied
            // libpart and builds a matching marker. Calling
            // PrepareWindowOrDoorDefaults first and applying the favorite
            // after leaves the marker pointing at the previous libpart,
            // causing CreateExt to fail with -2130313110.
            GSErrCode err = ApplyWindowOrDoorFavoriteToDefaults (data, API_WindowID);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to resolve `favoriteName` for window."));
                continue;
            }

            err = PrepareWindowOrDoorDefaults (API_WindowID, element, memo, marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare window defaults."));
                continue;
            }

            double centerOffset = 0.0;
            data.Get ("centerOffset", centerOffset);
            element.window.owner = wallGuid;
            element.window.objLoc = centerOffset;
            auto sillHeight = GetOptionalDouble (data, "sillHeight");
            if (sillHeight.HasValue ()) {
                element.window.lower = sillHeight.Get ();
            }
            auto width = GetOptionalDouble (data, "width");
            if (width.HasValue ()) {
                element.window.openingBase.width = width.Get ();
            }
            auto height = GetOptionalDouble (data, "height");
            if (height.HasValue ()) {
                element.window.openingBase.height = height.Get ();
            }
            bool reflected = false;
            if (data.Get ("reflected", reflected)) {
                element.window.openingBase.reflected = reflected;
            }
            bool refSide = false;
            if (data.Get ("refSide", refSide)) {
                element.window.openingBase.refSide = refSide;
            }
            bool oSide = false;
            if (data.Get ("oSide", oSide)) {
                element.window.openingBase.oSide = oSide;
            }

            err = ACAPI_Element_CreateExt (&element, &memo, 1UL, &marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create window."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

CreateDoorsCommand::CreateDoorsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateDoorsCommand::GetName () const
{
    return "CreateDoors";
}

GS::Optional<GS::UniString> CreateDoorsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "doorsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "ownerWallId": { "$ref": "#/ElementId" },
                        "centerOffset": { "type": "number", "minimum": 0.0 },
                        "sillHeight": { "type": "number" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "reflected": { "type": "boolean" },
                        "refSide": { "type": "boolean" },
                        "oSide": { "type": "boolean" },
                        "favoriteName": {
                            "type": "string",
                            "description": "Optional. Name of an existing Door favorite (as returned by `GetFavoritesByType`). Applied to the Door tool defaults before the create."
                        }
                    },
                    "additionalProperties": false,
                    "required": ["ownerWallId", "centerOffset"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["doorsData"]
    })";
}

GS::Optional<GS::UniString> CreateDoorsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateDoorsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> doorsData;
    auto error = GetElementArray (parameters, "doorsData", doorsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Doors", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : doorsData) {
            if (data.Get ("ownerWallId") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'ownerWallId' field."));
                continue;
            }
            const API_Guid wallGuid = GetGuidFromObjectState (*data.Get ("ownerWallId"));
            if (!DoesWallExist (wallGuid)) {
                elements.Push (CreateErrorResponse (APIERR_BADID, "Failed to load owner wall."));
                continue;
            }
            bool isPolygonalWall = false;
            const GSErrCode wallReadErr = GetWallPolygonalState (wallGuid, isPolygonalWall);
            if (wallReadErr != NoError) {
                elements.Push (CreateErrorResponse (wallReadErr, "Failed to read owner wall before creating door."));
                continue;
            }
            if (isPolygonalWall) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS,
                    "The owner wall is polygonal, and Archicad cannot place a door in one."));
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            API_SubElement marker = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
                ACAPI_DisposeElemMemoHdls (&marker.memo);
            });

            // Apply the favorite to the Door tool defaults FIRST so
            // that PrepareWindowOrDoorDefaults clones the favorite-applied
            // libpart and builds a matching marker. Calling
            // PrepareWindowOrDoorDefaults first and applying the favorite
            // after leaves the marker pointing at the previous libpart,
            // causing CreateExt to fail with -2130313110.
            GSErrCode err = ApplyWindowOrDoorFavoriteToDefaults (data, API_DoorID);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to resolve `favoriteName` for door."));
                continue;
            }

            err = PrepareWindowOrDoorDefaults (API_DoorID, element, memo, marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare door defaults."));
                continue;
            }

            double centerOffset = 0.0;
            data.Get ("centerOffset", centerOffset);
            element.window.owner = wallGuid;
            element.window.objLoc = centerOffset;
            auto sillHeight = GetOptionalDouble (data, "sillHeight");
            if (sillHeight.HasValue ()) {
                element.window.lower = sillHeight.Get ();
            }
            auto width = GetOptionalDouble (data, "width");
            if (width.HasValue ()) {
                element.window.openingBase.width = width.Get ();
            }
            auto height = GetOptionalDouble (data, "height");
            if (height.HasValue ()) {
                element.window.openingBase.height = height.Get ();
            }
            bool reflected = false;
            if (data.Get ("reflected", reflected)) {
                element.window.openingBase.reflected = reflected;
            }
            bool refSide = false;
            if (data.Get ("refSide", refSide)) {
                element.window.openingBase.refSide = refSide;
            }
            bool oSide = false;
            if (data.Get ("oSide", oSide)) {
                element.window.openingBase.oSide = oSide;
            }

            err = ACAPI_Element_CreateExt (&element, &memo, 1UL, &marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create door."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

CreateOpeningsCommand::CreateOpeningsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateOpeningsCommand::GetName () const
{
    return "CreateOpenings";
}

GS::Optional<GS::UniString> CreateOpeningsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "openingsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "ownerElementId": { "$ref": "#/ElementId" },
                        "basePoint": { "$ref": "#/Coordinate3D" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true }
                    },
                    "additionalProperties": false,
                    "required": ["ownerElementId", "basePoint", "width", "height"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["openingsData"]
    })";
}

GS::Optional<GS::UniString> CreateOpeningsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateOpeningsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> openingsData;
    auto error = GetElementArray (parameters, "openingsData", openingsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Openings", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : openingsData) {
            if (data.Get ("basePoint") == nullptr || data.Get ("ownerElementId") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'basePoint' or 'ownerElementId' field."));
                continue;
            }
            const auto sizeError = CheckOpeningSize (data);
            if (sizeError.HasValue ()) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, sizeError.Get ()));
                continue;
            }
            const API_Coord3D basePoint = Get3DCoordinateFromObjectState (*data.Get ("basePoint"));

#ifndef ServerMainVers_2900
            API_Element element = {};
#ifdef ServerMainVers_2600
            element.header.type   = API_OpeningID;
#else
            element.header.typeID = API_OpeningID;
#endif
            GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare opening defaults."));
                continue;
            }

            element.opening.owner = GetGuidFromObjectState (*data.Get ("ownerElementId"));
            element.opening.extrusionGeometryData.frame.basePoint = basePoint;
            element.opening.extrusionGeometryData.frame.axisX = {-1.0, 0.0, 0.0};
            element.opening.extrusionGeometryData.frame.axisY = {0.0, 0.0, 1.0};
            element.opening.extrusionGeometryData.frame.axisZ = {0.0, 1.0, 0.0};

            data.Get ("width", element.opening.extrusionGeometryData.parameters.width);
            data.Get ("height", element.opening.extrusionGeometryData.parameters.height);

            err = ACAPI_Element_Create (&element, nullptr);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create opening."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
#else
            ACAPI::Result<ACAPI::Element::OpeningDefault> openingDefault = ACAPI::Element::CreateOpeningDefault ();
            if (openingDefault.IsErr ()) {
                elements.Push (CreateErrorResponse (openingDefault.UnwrapErr ().kind, GS::UniString (openingDefault.UnwrapErr ().text.c_str ())));
                continue;
            }

            double width = 0.0;
            double height = 0.0;
            data.Get ("width", width);
            data.Get ("height", height);
            GS::Array<Point2D> polygonCorners { {0, 0}, {width, 0}, {width, height}, {0, height} };
            Geometry::Polygon2D polygon = Geometry::Polygon2D::Create (polygonCorners, 0 /*Geometry::PolyCreateFlags*/).PopLargest ();

            ACAPI::UniqueID parentElemId (APIGuid2GSGuid (GetGuidFromObjectState (*data.Get ("ownerElementId"))), ACAPI_GetToken ());
            ACAPI::Result<ACAPI::UniqueID> resultId = openingDefault->PlacePolygonal (parentElemId, basePoint, polygon);
            if (resultId.IsErr ()) {
                elements.Push (CreateErrorResponse (resultId.UnwrapErr ().kind, GS::UniString (resultId.UnwrapErr ().text.c_str())));
                continue;
            }
            elements.Push (CreateElementIdObjectState (GSGuid2APIGuid (resultId.Unwrap ().GetGuid ())));
#endif
        }
    });
}

CreateMorphsCommand::CreateMorphsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateMorphsCommand::GetName () const
{
    return "CreateMorphs";
}

GS::Optional<GS::UniString> CreateMorphsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "morphsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "basePoint": { "$ref": "#/Coordinate3D" },
                        "size": { "$ref": "#/Dimensions3D" },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "floorIndex": { "type": "integer", "description": "Optional floor index. If omitted, derived from the basePoint's z value." }
                    },
                    "additionalProperties": false,
                    "required": ["basePoint", "size"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["morphsData"]
    })";
}

GS::Optional<GS::UniString> CreateMorphsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateMorphsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> morphsData;
    auto error = GetElementArray (parameters, "morphsData", morphsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    const Stories stories = GetStories ();

    return ExecuteCreateWithElements ("Create Morphs", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : morphsData) {
            API_Element element = {};
            #ifdef ServerMainVers_2600
            element.header.type = API_MorphID;
            #else
            element.header.typeID = API_MorphID;
            #endif
            GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare morph defaults."));
                continue;
            }

            if (data.Get ("basePoint") == nullptr || data.Get ("size") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'basePoint' or 'size' field."));
                continue;
            }
            const API_Coord3D basePoint = Get3DCoordinateFromObjectState (*data.Get ("basePoint"));
            const API_Coord3D size = Get3DCoordinateFromObjectState (*data.Get ("size"));
            if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Morph 'size' values must be positive."));
                continue;
            }

            // GetDefaults leaves floorInd at whatever story is currently active in the UI,
            // regardless of basePoint's z - a morph built far above/below that story's own
            // elevation would get correctly placed in absolute 3D space (tmx below stays absolute)
            // but assigned to the wrong story for floor-plan/story-based queries. Only floorInd is
            // derived here; tmx[11] intentionally stays basePoint.z (absolute), matching how the
            // rest of this command already places the morph in world space.
            element.header.floorInd = ResolveFloorIndexAndOffset (data, "floorIndex", basePoint.z, stories).first;

            auto buildingMaterialId = GetOptionalObjectState (data, "buildingMaterialId");

            if (buildingMaterialId.HasValue ()) {
                API_AttributeIndex buildingMaterialIndex = APIInvalidAttributeIndex;
                if (!ResolveAttributeIndex (buildingMaterialId.Get (), API_BuildingMaterialID, buildingMaterialIndex)) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, "Invalid morph building material."));
                    continue;
                }
                element.morph.buildingMaterial = buildingMaterialIndex;
            }

            double* tmx = element.morph.tranmat.tmx;
            tmx[0] = 1.0;  tmx[4] = 0.0;  tmx[8] = 0.0;
            tmx[1] = 0.0;  tmx[5] = 1.0;  tmx[9] = 0.0;
            tmx[2] = 0.0;  tmx[6] = 0.0;  tmx[10] = 1.0;
            tmx[3] = basePoint.x;
            tmx[7] = basePoint.y;
            tmx[11] = basePoint.z;

            API_ElementMemo memo = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
            });

            if (!BuildCuboidMorphMemo (size.x, size.y, size.z, element.morph.buildingMaterial, memo)) {
                elements.Push (CreateErrorResponse (APIERR_GENERAL, "Failed to build morph body."));
                continue;
            }

            err = ACAPI_Element_Create (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create morph."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

CreateRoofsCommand::CreateRoofsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateRoofsCommand::GetName () const
{
    return "CreateRoofs";
}

GS::Optional<GS::UniString> CreateRoofsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "roofsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "level": { "type": "number" },
                        "floorIndex": { "type": "integer", "description": "Optional floor index. If omitted, derived from level." },
                        "thickness": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "polygonCoordinates": {
                            "type": "array",
                            "items": { "$ref": "#/Coordinate2D" },
                            "minItems": 3
                        },
                        "polygonArcs": {
                            "type": "array",
                            "items": { "$ref": "#/PolyArc" }
                        },
                        "holes": { "$ref": "#/Holes2D" },
                        "eavesOverhang": { "type": "number" },
                        "levels": {
                            "type": "array",
                            "minItems": 1,
                            "maxItems": 16,
                            "items": {
                                "type": "object",
                                "properties": {
                                    "levelHeight": { "type": "number" },
                                    "levelAngle": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true }
                                },
                                "additionalProperties": false,
                                "required": ["levelHeight", "levelAngle"]
                            }
                        },
                        "structureType": {
                            "type": "string",
                            "enum": ["Basic", "Composite"]
                        },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "compositeId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["level", "polygonCoordinates"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["roofsData"]
    })";
}

GS::Optional<GS::UniString> CreateRoofsCommand::GetResponseSchema () const
{
    return CreateMorphsCommand ().GetResponseSchema ();
}

GS::ObjectState CreateRoofsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> roofsData;
    auto error = GetElementArray (parameters, "roofsData", roofsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    const Stories stories = GetStories ();

    return ExecuteCreateWithElements ("Create Roofs", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : roofsData) {
            API_Element element = {};
            #ifdef ServerMainVers_2600
            element.header.type = API_RoofID;
            #else
            element.header.typeID = API_RoofID;
            #endif
            element.roof.roofClass = API_PolyRoofID;
            GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare roof defaults."));
                continue;
            }

            bool changed = false;
            {
                auto error = ApplyRoofStructure (element, nullptr, data, changed);
                if (error.HasValue ()) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                    continue;
                }
            }
            {
                auto error = ApplyRoofDetails (element, nullptr, data, stories, changed);
                if (error.HasValue ()) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                    continue;
                }
            }

            GS::Array<GS::ObjectState> polygonOutline;
            GS::Array<GS::ObjectState> polygonArcs;
            GS::Array<GS::ObjectState> holes;
            data.Get ("polygonCoordinates", polygonOutline);
            data.Get ("polygonArcs", polygonArcs);
            data.Get ("holes", holes);

            API_ElementMemo memo = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
            });
            auto error = BuildRoofMemoFromGeometry (element, memo, polygonOutline, polygonArcs, holes);
            if (error.HasValue ()) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                continue;
            }

            // Multi-plane roof creation is NOT yet supported. ACAPI_Element_Create for an
            // API_PolyRoofID requires pivot-edge plane data (memo.pivotPolyEdges holding
            // per-edge API_RoofSegmentData keyed by the pivot polygon's edge unique IDs),
            // which is not built here. Calling Create without it makes the Archicad core
            // THROW a GSException AND pop a modal dialog that hangs the JSON API. Until that
            // plane setup is implemented, reject cleanly without ever calling Create so the
            // command can never crash or hang the application.
            (void) err;
            elements.Push (CreateErrorResponse (APIERR_GENERAL, "Multi-plane roof creation is not yet supported (incomplete pivot-edge plane setup)."));
        }
    });
}

CreateAssociativeDimensionsCommand::CreateAssociativeDimensionsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateAssociativeDimensionsCommand::GetName () const
{
    return "CreateAssociativeDimensions";
}

GS::Optional<GS::UniString> CreateAssociativeDimensionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "dimensionsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "referencePoint": { "$ref": "#/Coordinate2D" },
                        "direction": { "$ref": "#/Coordinate2D" },
                        "floorIndex": { "type": "number" },
                        "witnessPoints": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "elementId": { "$ref": "#/ElementId" },
                                    "line": { "type": "boolean" },
                                    "inIndex": { "type": "integer" },
                                    "special": { "type": "integer" },
                                    "nodeType": { "type": "integer" },
                                    "nodeStatus": { "type": "integer" },
                                    "nodeId": { "type": "number", "minimum": 0.0 }
                                },
                                "additionalProperties": false,
                                "required": ["elementId"]
                            },
                            "minItems": 2
                        }
                    },
                    "additionalProperties": false,
                    "required": ["referencePoint", "direction", "witnessPoints"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["dimensionsData"]
    })";
}

GS::Optional<GS::UniString> CreateAssociativeDimensionsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateAssociativeDimensionsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> dimensionsData;
    auto error = GetElementArray (parameters, "dimensionsData", dimensionsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Associative Dimensions", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : dimensionsData) {
            GS::Array<GS::ObjectState> witnessPointsData;
            {
                auto error = GetElementArray (data, "witnessPoints", witnessPointsData);
                if (error.HasValue ()) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                    continue;
                }
            }

            if (data.Get ("direction") == nullptr || data.Get ("referencePoint") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'direction' or 'referencePoint' field."));
                continue;
            }
            const API_Coord directionCoord = Get2DCoordinateFromObjectState (*data.Get ("direction"));
            if (directionCoord.x == 0.0 && directionCoord.y == 0.0) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Dimension direction must be non-zero."));
                continue;
            }

            GS::Array<AssociativeDimensionPoint> witnessPoints;
            bool invalidWitnessPoint = false;
            for (const auto& witnessPointData : witnessPointsData) {
                AssociativeDimensionPoint witnessPoint;
                auto error = ParseAssociativeDimensionPoint (witnessPointData, witnessPoint);
                if (error.HasValue ()) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                    invalidWitnessPoint = true;
                    break;
                }
                witnessPoints.Push (witnessPoint);
            }
            if (invalidWitnessPoint) {
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
            });

            #ifdef ServerMainVers_2600
            element.header.type = API_DimensionID;
            #else
            element.header.typeID = API_DimensionID;
            #endif
            GSErrCode err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare associative dimension defaults."));
                continue;
            }

            TryApplyDimensionFloorIndex (witnessPoints, GetOptionalDouble (data, "floorIndex"), element);
            FillDimensionDefaults (
                element,
                Get2DCoordinateFromObjectState (*data.Get ("referencePoint")),
                {directionCoord.x, directionCoord.y}
            );

            auto error = PopulateAssociativeDimensionMemo (witnessPoints, element, memo);
            if (error.HasValue ()) {
                elements.Push (CreateErrorResponse (APIERR_MEMFULL, error.Get ()));
                continue;
            }

            err = ACAPI_Element_Create (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create associative dimension."));
                continue;
            }

            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

CreateAssociativeDimensionsOnSectionCommand::CreateAssociativeDimensionsOnSectionCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateAssociativeDimensionsOnSectionCommand::GetName () const
{
    return "CreateAssociativeDimensionsOnSection";
}

GS::Optional<GS::UniString> CreateAssociativeDimensionsOnSectionCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "dimensionsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "sectionElementId": { "$ref": "#/ElementId" },
                        "referencePoint": { "$ref": "#/Coordinate2D" },
                        "preset": {
                            "type": "string",
                            "enum": [
                                "WallCompositeFaces",
                                "WallSkinBorders",
                                "SlabCompositeFaces",
                                "SlabSkinBorders",
                                "BeamOrColumnRefLineEndPoints",
                                "BeamOrColumnBoundingBoxCorners",
                                "DoorWindowWallHoleCorners",
                                "DoorWindowModelHotspots"
                            ]
                        },
                        "direction": { "$ref": "#/Coordinate2D" },
                        "skinBorderIndices": {
                            "type": "array",
                            "items": { "type": "integer" },
                            "minItems": 1
                        },
                        "beginPlane": { "type": "boolean" },
                        "totalSizePlane": { "type": "boolean" },
                        "placeOnTop": { "type": "boolean" }
                    },
                    "additionalProperties": false,
                    "required": ["sectionElementId", "referencePoint", "preset"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["dimensionsData"]
    })";
}

GS::Optional<GS::UniString> CreateAssociativeDimensionsOnSectionCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateAssociativeDimensionsOnSectionCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> dimensionsData;
    auto error = GetElementArray (parameters, "dimensionsData", dimensionsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Associative Dimensions On Section", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : dimensionsData) {
            GS::Array<AssociativeDimensionPoint> witnessPoints;
            API_Vector defaultDirection = {1.0, 0.0};
            {
                auto error = BuildSectionAssociativeDimensionPoints (data, witnessPoints, defaultDirection);
                if (error.HasValue ()) {
                    elements.Push (CreateErrorResponse (APIERR_BADPARS, error.Get ()));
                    continue;
                }
            }

            API_Coord directionCoord = {defaultDirection.x, defaultDirection.y};
            auto overrideDirection = GetOptionalCoordinate2D (data, "direction");
            if (overrideDirection.HasValue ()) {
                directionCoord = overrideDirection.Get ();
            }
            if (directionCoord.x == 0.0 && directionCoord.y == 0.0) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Dimension direction must be non-zero."));
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
            });

            #ifdef ServerMainVers_2600
            element.header.type = API_DimensionID;
            #else
            element.header.typeID = API_DimensionID;
            #endif
            GSErrCode err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare section associative dimension defaults."));
                continue;
            }

            if (data.Get ("referencePoint") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'referencePoint' field."));
                continue;
            }
            FillDimensionDefaults (
                element,
                Get2DCoordinateFromObjectState (*data.Get ("referencePoint")),
                {directionCoord.x, directionCoord.y}
            );

            auto error = PopulateAssociativeDimensionMemo (witnessPoints, element, memo);

            if (error.HasValue ()) {
                elements.Push (CreateErrorResponse (APIERR_MEMFULL, error.Get ()));
                continue;
            }

            err = ACAPI_Element_Create (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create associative section dimension."));
                continue;
            }

            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

CreateWallThicknessDimensionsCommand::CreateWallThicknessDimensionsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateWallThicknessDimensionsCommand::GetName () const
{
    return "CreateWallThicknessDimensions";
}

GS::Optional<GS::UniString> CreateWallThicknessDimensionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "dimensionsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "wallId": { "$ref": "#/ElementId" },
                        "referencePoint": { "$ref": "#/Coordinate2D" },
                        "direction": { "$ref": "#/Coordinate2D" }
                    },
                    "additionalProperties": false,
                    "required": ["wallId", "referencePoint", "direction"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["dimensionsData"]
    })";
}

GS::Optional<GS::UniString> CreateWallThicknessDimensionsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateWallThicknessDimensionsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> dimensionsData;
    auto error = GetElementArray (parameters, "dimensionsData", dimensionsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Wall Thickness Dimensions", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : dimensionsData) {
            if (data.Get ("wallId") == nullptr || data.Get ("direction") == nullptr || data.Get ("referencePoint") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'wallId', 'direction', or 'referencePoint' field."));
                continue;
            }
            API_Element wall = {};
            wall.header.guid = GetGuidFromObjectState (*data.Get ("wallId"));
            GSErrCode err = ACAPI_Element_Get (&wall);
            if (err != NoError || GetElemTypeId (wall.header) != API_WallID) {
                elements.Push (CreateErrorResponse (APIERR_BADID, "Failed to load wall for associative dimension."));
                continue;
            }

            const API_Coord directionCoord = Get2DCoordinateFromObjectState (*data.Get ("direction"));
            if (directionCoord.x == 0.0 && directionCoord.y == 0.0) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Dimension direction must be non-zero."));
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
            });

            #ifdef ServerMainVers_2600
            element.header.type = API_DimensionID;
            #else
            element.header.typeID = API_DimensionID;
            #endif
            err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare dimension defaults."));
                continue;
            }

            element.header.floorInd = wall.header.floorInd;
            element.dimension.dimAppear = APIApp_Normal;
            element.dimension.textPos = APIPos_Above;
            element.dimension.textWay = APIDir_Parallel;
            element.dimension.defStaticDim = false;
            element.dimension.usedIn3D = false;
            element.dimension.horizontalText = false;
            element.dimension.refC = Get2DCoordinateFromObjectState (*data.Get ("referencePoint"));
            element.dimension.direction = {directionCoord.x, directionCoord.y};
            element.dimension.nDimElem = 2;

            memo.dimElems = reinterpret_cast<API_DimElem**> (BMhAllClear (element.dimension.nDimElem * sizeof (API_DimElem)));
            if (memo.dimElems == nullptr || *memo.dimElems == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_MEMFULL, "Failed to allocate dimension witness data."));
                continue;
            }

            const Int32 wallInIndices[2] = {11, 21};
            for (Int32 dimElemIndex = 0; dimElemIndex < element.dimension.nDimElem; ++dimElemIndex) {
                API_DimElem& dimElem = (*memo.dimElems)[dimElemIndex];
#ifdef ServerMainVers_2600
                dimElem.base.base.type = API_ElemType (API_WallID);
#else
                dimElem.base.base.typeID = API_WallID;
#endif
                dimElem.base.base.guid = wall.header.guid;
                dimElem.base.base.line = true;
                dimElem.base.base.inIndex = wallInIndices[dimElemIndex];
                dimElem.base.base.special = 0;
                dimElem.base.base.node_id = 0;
                dimElem.base.base.node_status = 0;
                dimElem.base.base.node_typ = 0;
                dimElem.note = element.dimension.defNote;
                dimElem.witnessVal = element.dimension.defWitnessVal;
                dimElem.witnessForm = element.dimension.defWitnessForm;
            }

            err = ACAPI_Element_Create (&element, &memo);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create wall thickness dimension."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

ModifyWallsCommand::ModifyWallsCommand (const bool atomicIn) :
    CommandBase (CommonSchema::Used),
    atomic (atomicIn)
{
}

GS::String ModifyWallsCommand::GetName () const
{
    return "ModifyWalls";
}

GS::Optional<GS::UniString> ModifyWallsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "wallsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "begCoordinate": { "$ref": "#/Coordinate2D" },
                        "endCoordinate": { "$ref": "#/Coordinate2D" },
                        "arcAngle": { "type": "number", "description": "Arc angle in radians; non-zero makes the wall curved (begCoordinate/endCoordinate are the chord endpoints)." },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "thickness": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "bottomOffset": { "type": "number" },
                        "offset": { "type": "number" },
                        "structureType": {
                            "type": "string",
                            "enum": ["Basic", "Composite", "Profile"]
                        },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "compositeId": { "$ref": "#/AttributeId" },
                        "profileId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["wallsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyWallsCommand::GetResponseSchema () const
{
    return R"({"type":"object","properties":{"executionResults":{"$ref":"#/ExecutionResults"},"transactionRolledBack":{"type":"boolean"},"transactionError":{"type":"integer"}},"additionalProperties":false,"required":["executionResults"]})";
}

GS::ObjectState ModifyWallsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "wallsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteModifyWithResults ("Modify Walls", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load wall."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = ApplyWallDetails (element, mask, item);
            auto error = ApplyWallStructure (element, &mask, item, changed);
            if (error.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                continue;
            }
            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No wall fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify wall."));
        }
    }, atomic);
}

ModifyBeamsCommand::ModifyBeamsCommand (const bool atomicIn) :
    CommandBase (CommonSchema::Used),
    atomic (atomicIn)
{
}

GS::String ModifyBeamsCommand::GetName () const
{
    return "ModifyBeams";
}

GS::Optional<GS::UniString> ModifyBeamsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "beamsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "begCoordinate": { "$ref": "#/Coordinate2D" },
                        "endCoordinate": { "$ref": "#/Coordinate2D" },
                        "level": { "type": "number" },
                        "offset": { "type": "number" },
                        "slantAngle": { "type": "number" },
                        "isSlanted": { "type": "boolean" },
                        "profileAngle": { "type": "number" },
                        "arcAngle": { "type": "number" },
                        "verticalCurveHeight": { "type": "number" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "isWidthAndHeightLinked": { "type": "boolean" },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "profileId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["beamsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyBeamsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyBeamsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "beamsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteModifyWithResults ("Modify Beams", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load beam."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            auto sectionError = ValidateBeamSectionPayload (item);
            if (sectionError.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, sectionError.Get ()));
                continue;
            }

            const bool hasSectionFields = HasBeamSectionFields (item);
            if (hasSectionFields) {
                err = ValidateBeamSectionMemo (element.header.guid);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Beam cross section memo is not available for update."));
                    continue;
                }
            }

            const bool changed = ApplyBeamDetails (element, mask, item);
            if (!changed && !hasSectionFields) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No beam fields to modify."));
                continue;
            }

            if (changed) {
                err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Failed to modify beam."));
                    continue;
                }
            }

            if (hasSectionFields) {
                err = ApplyBeamSectionToMemo (element.header.guid, item);
                results.Push (err == NoError
                    ? CreateSuccessfulExecutionResult ()
                    : CreateFailedExecutionResult (err, "Failed to modify beam cross section."));
                continue;
            }

            results.Push (CreateSuccessfulExecutionResult ());
        }
    }, atomic);
}

ModifySlabsCommand::ModifySlabsCommand (const bool atomicIn) :
    CommandBase (CommonSchema::Used),
    atomic (atomicIn)
{
}

GS::String ModifySlabsCommand::GetName () const
{
    return "ModifySlabs";
}

GS::Optional<GS::UniString> ModifySlabsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "slabsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "zCoordinate": { "type": "number" },
                        "thickness": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "referencePlaneLocation": {
                            "type": "string",
                            "enum": ["Top", "CoreTop", "CoreBottom", "Bottom"]
                        },
                        "structureType": {
                            "type": "string",
                            "enum": ["Basic", "Composite"]
                        },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "compositeId": { "$ref": "#/AttributeId" },
                        "polygonOutline": {
                            "type": "array",
                            "items": { "$ref": "#/Coordinate2D" },
                            "minItems": 3
                        },
                        "polygonArcs": {
                            "type": "array",
                            "items": { "$ref": "#/PolyArc" }
                        },
                        "holes": {
                            "$ref": "#/Holes2D",
                            "description": "Can be given on its own, without polygonOutline, to add/remove/clear holes in place (an empty array clears all holes) - the slab's current outline is reused unchanged."
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["slabsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifySlabsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifySlabsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "slabsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    const Stories stories = GetStories ();

    return ExecuteModifyWithResults ("Modify Slabs", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load slab."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = ApplySlabDetails (element, mask, item, stories);
            auto error = ApplySlabStructure (element, &mask, item, changed);
            if (error.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                continue;
            }

            GS::Array<GS::ObjectState> polygonOutline;
            const bool hasNewOutline = item.Get ("polygonOutline", polygonOutline);
            GS::Array<GS::ObjectState> holes;
            const bool hasHolesField = item.Get ("holes", holes);
            if (hasNewOutline || hasHolesField) {
                // holes can be given on its own (no polygonOutline) to add/remove/clear holes
                // in place - including an explicit empty array to clear all holes - without
                // forcing the caller to resend the (possibly large) outline unchanged. Previously
                // this whole branch was gated on polygonOutline alone, so a holes-only request
                // fell through to "No slab fields to modify." below (issue #452).
                GS::Array<GS::ObjectState> polygonArcs;
                if (hasNewOutline) {
                    if (polygonOutline.GetSize () < 3) {
                        results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "'polygonOutline' must contain at least 3 coordinates."));
                        continue;
                    }
                    item.Get ("polygonArcs", polygonArcs);
                } else {
                    GS::ObjectState currentGeometry;
                    AddPolygonWithHolesFromMemoCoords (element.header.guid, currentGeometry, "polygonOutline", "polygonArcs", "holes", "polygonOutline", "polygonArcs");
                    currentGeometry.Get ("polygonOutline", polygonOutline);
                    currentGeometry.Get ("polygonArcs", polygonArcs);
                }

                API_ElementMemo memo = {};
                const GS::OnExit cleanup ([&]() {
                    ACAPI_DisposeElemMemoHdls (&memo);
                });
                ACAPI_Element_GetMemo (element.header.guid, &memo);

                auto error = ApplySlabPolygonChange (memo, element.slab.sideMat, polygonOutline, polygonArcs, holes);
                if (error.HasValue ()) {
                    results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                    continue;
                }

                // Non-geometry field changes (thickness/level/etc, accumulated into mask above)
                // are applied first via ACAPI_Element_Change; the polygon itself goes through
                // ACAPI_Element_ChangeMemo, matching the DevKit's own reference example
                // (ApplySlabPolygonChange mutates the memo via Graphisoft's polygon-editing
                // primitives, not a from-scratch replacement, so element.slab.poly's counts are
                // deliberately not touched here).
                if (changed) {
                    err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                    if (err != NoError) {
                        results.Push (CreateFailedExecutionResult (err, "Failed to modify slab."));
                        continue;
                    }
                }

                API_Guid slabGuid = element.header.guid;
                err = ACAPI_Element_ChangeMemo (slabGuid, APIMemoMask_Polygon | APIMemoMask_SideMaterials | APIMemoMask_EdgeTrims, &memo);
                results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify slab geometry."));
                continue;
            }

            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No slab fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify slab."));
        }
    }, atomic);
}

ModifyRoofsCommand::ModifyRoofsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ModifyRoofsCommand::GetName () const
{
    return "ModifyRoofs";
}

GS::Optional<GS::UniString> ModifyRoofsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "roofsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "level": { "type": "number" },
                        "thickness": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "eavesOverhang": { "type": "number" },
                        "levels": {
                            "type": "array",
                            "minItems": 1,
                            "maxItems": 16,
                            "items": {
                                "type": "object",
                                "properties": {
                                    "levelHeight": { "type": "number" },
                                    "levelAngle": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true }
                                },
                                "additionalProperties": false,
                                "required": ["levelHeight", "levelAngle"]
                            }
                        },
                        "structureType": {
                            "type": "string",
                            "enum": ["Basic", "Composite"]
                        },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "compositeId": { "$ref": "#/AttributeId" },
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
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["roofsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyRoofsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyRoofsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "roofsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    const Stories stories = GetStories ();

    return ExecuteModifyWithResults ("Modify Roofs", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load roof."));
                continue;
            }
            if (element.roof.roofClass != API_PolyRoofID) {
                results.Push (CreateFailedExecutionResult (APIERR_NOTSUPPORTED, "Only multi-plane roofs are supported.")); 
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            {
                auto error = ApplyRoofStructure (element, &mask, item, changed);
                if (error.HasValue ()) {
                    results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                    continue;
                }
            }
            auto error = ApplyRoofDetails (element, &mask, item, stories, changed);
            if (error.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                continue;
            }
\
            GS::Array<GS::ObjectState> polygonOutline;
            if (item.Get ("polygonOutline", polygonOutline)) {
                if (polygonOutline.GetSize () < 3) {
                    results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "'polygonOutline' must contain at least 3 coordinates."));
                    continue;
                }

                API_ElementMemo memo = {};
                const GS::OnExit cleanup ([&]() {
                    ACAPI_DisposeElemMemoHdls (&memo);
                });
                ACAPI_Element_GetMemo (element.header.guid, &memo);

                GS::Array<GS::ObjectState> polygonArcs;
                GS::Array<GS::ObjectState> holes;
                item.Get ("polygonArcs", polygonArcs);
                item.Get ("holes", holes);
                auto error = BuildRoofMemoFromGeometry (element, memo, polygonOutline, polygonArcs, holes);
                if (error.HasValue ()) {
                    results.Push (CreateFailedExecutionResult (APIERR_BADPARS, error.Get ()));
                    continue;
                }

                err = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_AdditionalPolygon, true);
                results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify roof geometry."));
                continue;
            }

            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No roof fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify roof."));
        }
    });
}

// ============================================================================
// GetDimensionData
// ============================================================================

GetDimensionDataCommand::GetDimensionDataCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetDimensionDataCommand::GetName () const
{
    return "GetDimensionData";
}

GS::Optional<GS::UniString> GetDimensionDataCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "type": "array",
                "description": "The identifier of the dimension elements.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::Optional<GS::UniString> GetDimensionDataCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "dimensionsData": {
                "type": "array",
                "items": {
                    "$ref": "#/DimensionDataOrError"
                }
            }
        },
        "additionalProperties": false,
        "required": ["dimensionsData"]
    })";
}

static GS::UniString WitnessFormToString (API_WitnessID witnessForm)
{
    switch (witnessForm) {
        case APIWtn_None:   return "None";
        case APIWtn_Small:  return "Small";
        case APIWtn_Large:  return "Large";
        case APIWtn_Fix:    return "Fix";
        default:            return "Unknown";
    }
}

GS::ObjectState GetDimensionDataCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::ObjectState response;
    const auto& dimensionsData = response.AddList<GS::ObjectState> ("dimensionsData");

    for (const GS::ObjectState& elementObj : elements) {
        const GS::ObjectState* elementId = elementObj.Get ("elementId");
        if (elementId == nullptr) {
            dimensionsData (CreateErrorResponse (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        API_Element element = {};
        element.header.guid = GetGuidFromObjectState (*elementId);
        GSErrCode err = ACAPI_Element_Get (&element);
        if (err != NoError) {
            dimensionsData (CreateErrorResponse (err, "Failed to get element"));
            continue;
        }

        const API_ElemTypeID typeID = GetElemTypeId (element.header);
        if (typeID != API_DimensionID) {
            dimensionsData (CreateErrorResponse (APIERR_BADID, "Element is not a Dimension"));
            continue;
        }

        API_ElementMemo memo = {};
        const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
        err = ACAPI_Element_GetMemo (element.header.guid, &memo);
        if (err != NoError) {
            dimensionsData (CreateErrorResponse (err, "Failed to get element memo"));
            continue;
        }

        GS::ObjectState dimensionData;
        dimensionData.Add ("elementId", CreateGuidObjectState (element.header.guid));
        dimensionData.Add ("direction", Create2DCoordinateObjectState (element.dimension.direction));
        dimensionData.Add ("dimensionLinePosition", Create2DCoordinateObjectState (element.dimension.refC));

        const auto& witnessPoints = dimensionData.AddList<GS::ObjectState> ("witnessPoints");

        if (memo.dimElems != nullptr && *memo.dimElems != nullptr) {
            for (Int32 i = 0; i < element.dimension.nDimElem; ++i) {
                const API_DimElem& dimElem = (*memo.dimElems)[i];

                GS::ObjectState witnessPoint;

                witnessPoint.Add ("coordinate", Create2DCoordinateObjectState (dimElem.base.loc));
                witnessPoint.Add ("coordinate3D", Create3DCoordinateObjectState (dimElem.base.loc3D));
                witnessPoint.Add ("dimensionPosition", Create2DCoordinateObjectState (dimElem.pos));
                witnessPoint.Add ("dimensionValue", dimElem.dimVal);
                witnessPoint.Add ("witnessForm", WitnessFormToString (dimElem.witnessForm));
                witnessPoint.Add ("witnessVal", dimElem.witnessVal);

                const API_Guid& baseGuid = dimElem.base.base.guid;
                if (baseGuid != APINULLGuid) {
                    witnessPoint.Add ("baseElementId", CreateGuidObjectState (baseGuid));
                }

                witnessPoints (witnessPoint);
            }
        }

        dimensionsData (dimensionData);
    }

    return response;
}

// ============================================================================
// GetNativeDimensionDetails (additive to legacy GetDimensionData)
// ============================================================================

namespace {

GS::ObjectState DimensionPoint2D (const API_Coord& point)
{
    return GS::ObjectState ("x", point.x, "y", point.y, "unit", "project-meter");
}

GS::ObjectState DimensionPoint3D (const API_Coord3D& point)
{
    return GS::ObjectState ("x", point.x, "y", point.y, "z", point.z, "unit", "project-meter");
}

Int32 DimensionAttributeIndex (const API_AttributeIndex& index)
{
#if TAPIR_AC_VERSION == 25
    return static_cast<Int32> (index);
#else
    return index.ToInt32_Deprecated ();
#endif
}

GS::ObjectState DimensionBaseDetails (const API_DimBase& base)
{
    GS::ObjectState state;
#if TAPIR_AC_VERSION == 25
    const Int32 type = static_cast<Int32> (base.base.typeID);
    state.Add ("elementVariationId", GS::ObjectState ("status", "not-available-in-ac25-API_Base"));
#else
    const Int32 type = static_cast<Int32> (base.base.type.typeID);
    state.Add ("elementVariationId", static_cast<Int32> (base.base.type.variationID));
#endif
    state.Add ("elementTypeId", type);
    state.Add ("lineReference", base.base.line);
    state.Add ("specialReference", static_cast<Int32> (base.base.special));
    state.Add ("nodeIndex", base.base.inIndex);
    state.Add ("elementId", CreateGuidObjectState (base.base.guid));
    // node_id is a meaningful Base identity field (a polygon-vertex ID on
    // AC28; the AC25 API documents this storage as the native node ID). Keep
    // it losslessly, without exposing the reserved node_status/node_typ bits.
    state.Add ("nativeNodeId", static_cast<UInt64> (base.base.node_id));
    state.Add ("sectionNodeStatus", static_cast<Int32> (base.base.node_status));
    state.Add ("sectionNodeType", static_cast<Int32> (base.base.node_typ));
    state.Add ("sectionNodeFactsStatus", "native-read-only-internal-fields");
    state.Add ("location", DimensionPoint2D (base.loc));
    state.Add ("location3D", DimensionPoint3D (base.loc3D));
    state.Add ("associationStatus", base.base.guid == APINULLGuid ? "no-element-guid" : "element-guid-present-type-not-independently-resolved");
    return state;
}

// Defaults are a separate internal diagnostic surface. Keep it typed and do
// not carry the section-dimension-only node_status/node_typ bits exposed by
// the historical element reader.
GS::ObjectState RadialDefaultsBaseDetails (const API_DimBase& base)
{
    GS::ObjectState state;
#if TAPIR_AC_VERSION == 25
    state.Add ("elementVariationId", GS::ObjectState ("status", "not-available-in-ac25-API_Base"));
    state.Add ("elementTypeId", static_cast<Int32> (base.base.typeID));
#else
    state.Add ("elementVariationId", static_cast<Int32> (base.base.type.variationID));
    state.Add ("elementTypeId", static_cast<Int32> (base.base.type.typeID));
#endif
    state.Add ("lineReference", base.base.line);
    state.Add ("specialReference", static_cast<Int32> (base.base.special));
    state.Add ("nodeIndex", base.base.inIndex);
    state.Add ("elementId", CreateGuidObjectState (base.base.guid));
    state.Add ("nativeNodeId", static_cast<UInt64> (base.base.node_id));
    state.Add ("location", DimensionPoint2D (base.loc));
    state.Add ("location3D", DimensionPoint3D (base.loc3D));
    state.Add ("associationStatus", base.base.guid == APINULLGuid ? "no-element-guid" : "element-guid-present-type-not-independently-resolved");
    return state;
}

GS::ObjectState DimensionFixedContentDetails (const API_NoteType& note)
{
    constexpr std::size_t capacity = sizeof (note.content);
    std::size_t byteLength = 0;
    while (byteLength < capacity && note.content[byteLength] != '\0') ++byteLength;
    const bool nulTerminated = byteLength < capacity;
    bool asciiNulTerminated = nulTerminated;
    std::string bytesHex;
    bytesHex.reserve (byteLength * 2);
    static const char hexDigits[] = "0123456789ABCDEF";
    for (GS::USize index = 0; index < byteLength; ++index) {
        const unsigned char byte = static_cast<unsigned char> (note.content[index]);
        asciiNulTerminated = asciiNulTerminated && byte < 0x80;
        bytesHex.push_back (hexDigits[byte >> 4]);
        bytesHex.push_back (hexDigits[byte & 0x0F]);
    }
    GS::ObjectState fixedContent;
    fixedContent.Add ("status", asciiNulTerminated ? "ascii-nul-terminated-verified" : "encoding-unproven");
    fixedContent.Add ("byteLength", static_cast<Int32> (byteLength));
    fixedContent.Add ("bufferCapacity", static_cast<Int32> (capacity));
    fixedContent.Add ("nulTerminated", nulTerminated);
    fixedContent.Add ("bytesHex", GS::UniString (bytesHex.c_str (), CC_UTF8));
    if (asciiNulTerminated) {
        char ascii[sizeof (note.content) + 1] = {};
        std::copy (note.content, note.content + byteLength, ascii);
        fixedContent.Add ("value", GS::UniString (ascii, CC_UTF8));
    }
    return fixedContent;
}

GS::ObjectState DimensionNoteDetails (const API_NoteType& note, const GS::UniString* verifiedUnicodeContent = nullptr, const bool allowNativeUnicodePointer = true)
{
    GS::ObjectState state;
    state.Add ("pen", static_cast<Int32> (note.notePen));
    state.Add ("font", static_cast<Int32> (note.noteFont));
    state.Add ("layer", DimensionAttributeIndex (note.noteLayer));
    state.Add ("textHeight", note.noteSize);
    state.Add ("textHeightUnit", "millimeter-paper-size");
    state.Add ("angle", note.noteAngle);
    state.Add ("angleUnit", "radian");
    state.Add ("position", DimensionPoint2D (note.pos));
    GS::UniString returnedContent;
    const char* returnedContentStatus = nullptr;
    const char* returnedContentSource = nullptr;
    if (verifiedUnicodeContent != nullptr) {
        returnedContent = *verifiedUnicodeContent;
        returnedContentStatus = "unicode-content-available";
        returnedContentSource = "verified-caller-bound-unicode-contentUStr";
    } else if (allowNativeUnicodePointer && note.contentUStr != nullptr) {
        returnedContent = *note.contentUStr;
        returnedContentStatus = "unicode-content-available";
        returnedContentSource = "memo-owned-unicode-contentUStr";
    } else {
        char fixedContent[sizeof (note.content) + 1] = {};
        std::copy (note.content, note.content + sizeof (note.content), fixedContent);
        GS::UInt32 byteLength = 0;
        while (byteLength < sizeof (note.content) && fixedContent[byteLength] != '\0') ++byteLength;
        returnedContent = GS::UniString (fixedContent);
        returnedContentStatus = note.contentUStr == nullptr ? "fixed-buffer-content-preserved-encoding-unproven" : "fixed-buffer-content-unicode-pointer-unverified";
        returnedContentSource = note.contentUStr == nullptr ? "fixed-buffer-content" : "fixed-buffer-content-unverified-unicode-pointer-not-dereferenced";
        state.Add ("contentByteLength", static_cast<Int32> (byteLength));
    }
    state.Add ("content", returnedContent);
    state.Add ("contentStatus", returnedContentStatus);
    state.Add ("contentSource", returnedContentSource);
    state.Add ("nativeContent", GS::ObjectState (
        "value", returnedContent,
        "accessStatus", returnedContentStatus,
        "accessSource", returnedContentSource,
        "meaningStatus", "returned-native-content-expression-meaning-unproven"));
    state.Add ("fixedContent", DimensionFixedContentDetails (note));
    state.Add ("sourceExpression", GS::ObjectState (
        "status", "unproven",
        "reason", "returned-native-content-does-not-prove-original-note-expression"));
    state.Add ("contentType", static_cast<Int32> (note.contentType));
    state.Add ("contentMode", note.contentType == API_NoteContent_Measured ? "measured" : note.contentType == API_NoteContent_Custom ? "custom" : "unknown");
    state.Add ("fixPos", note.fixPos);
    state.Add ("styleBits", static_cast<Int32> (note.faceBits));
    state.Add ("opaque", note.opaque);
    state.Add ("frame", note.frame);
    state.Add ("backgroundPen", static_cast<Int32> (note.backgroundPen));
    state.Add ("framePen", static_cast<Int32> (note.framePen));
    state.Add ("useLeaderLine", note.useLeaderLine);
    state.Add ("anchorAtTextEnd", note.anchorAtTextEnd);
    state.Add ("leaderBegin", DimensionPoint2D (note.begC));
    state.Add ("leaderMiddle", DimensionPoint2D (note.midC));
    state.Add ("leaderEnd", DimensionPoint2D (note.endC));
    state.Add ("leaderPen", static_cast<Int32> (note.leaderPen));
    state.Add ("leaderLineType", DimensionAttributeIndex (note.leaderLineType));
    state.Add ("leaderShape", static_cast<Int32> (note.leaderShape));
    state.Add ("leaderArrowType", static_cast<Int32> (note.arrowData.arrowType));
    state.Add ("leaderArrowAtBegin", note.arrowData.begArrow);
    state.Add ("leaderArrowAtEnd", note.arrowData.endArrow);
    state.Add ("leaderArrowPen", static_cast<Int32> (note.arrowData.arrowPen));
    state.Add ("leaderArrowSize", note.arrowData.arrowSize);
    state.Add ("anchorPoint", static_cast<Int32> (note.anchorPoint));
    state.Add ("justification", static_cast<Int32> (note.just));
    state.Add ("widthFactor", note.widthFactor);
    state.Add ("spacingFactor", note.spacingFactor);
    state.Add ("rowAnchor", static_cast<Int32> (note.rowAnchor));
    state.Add ("effectBits", static_cast<Int32> (note.effectBits));
    state.Add ("observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "native-note-source-does-not-independently-prove-rendered-display"));
    return state;
}

GS::ObjectState DimensionMarkerDetails (const API_MarkerData& marker)
{
    return GS::ObjectState (
        "markerType", static_cast<Int32> (marker.markerType),
        "markerPen", static_cast<Int32> (marker.markerPen),
        "markerSize", marker.markerSize,
        "markerSizeUnit", "millimeter-paper-size");
}

void AddDimensionUnits (GS::ObjectState& row)
{
    API_DimensionPrefs preferences = {};
    const GSErrCode preferencesError = ACAPI_ProjectSetting_GetPreferences (&preferences, APIPrefs_DimensionsID);
    GS::ObjectState preferenceState;
    preferenceState.Add ("status", preferencesError == NoError ? "available" : "unavailable");
    preferenceState.Add ("source", "ACAPI_ProjectSetting_GetPreferences/APIPrefs_DimensionsID");
    preferenceState.Add ("scope", "project-dimension-preferences");
    preferenceState.Add ("savedViewEffectiveFormat", "not-proven-by-project-preferences-read");
    preferenceState.Add ("formatCoverage", preferencesError == NoError ? "complete-project-context" : "unavailable");
    if (preferencesError == NoError) {
        const auto lengthFormat = [] (const API_LengthDimFormat& format) {
            return GS::ObjectState (
                "unit", static_cast<Int32> (format.unit),
                "roundInch", static_cast<Int32> (format.roundInch),
                "decimals", static_cast<Int32> (format.lenDecimals),
                "showZeroWhole", format.show0Whole,
                "showZeroInch", static_cast<Int32> (format.show0Inch),
                "showSmall5", static_cast<Int32> (format.showSmall5),
                "hideZeroDecimals", format.hide0Dec);
        };
        preferenceState.Add ("linear", lengthFormat (preferences.linear));
        preferenceState.Add ("radial", lengthFormat (preferences.radial));
        preferenceState.Add ("level", lengthFormat (preferences.level));
        preferenceState.Add ("elevation", lengthFormat (preferences.elevation));
        preferenceState.Add ("doorWindow", lengthFormat (preferences.doorwindow));
        preferenceState.Add ("parapet", lengthFormat (preferences.parapet));
        preferenceState.Add ("area", GS::ObjectState (
            "unit", static_cast<Int32> (preferences.area.unit),
            "decimals", static_cast<Int32> (preferences.area.lenDecimals),
            "hideZeroDecimals", preferences.area.hide0Dec));
        preferenceState.Add ("angle", GS::ObjectState (
            "unit", static_cast<Int32> (preferences.angle.unit),
            "decimals", static_cast<Int32> (preferences.angle.angleDecimals),
            "accuracy", static_cast<Int32> (preferences.angle.angleAccuracy),
            "hideZeroDecimals", preferences.angle.hide0Dec));
        preferenceState.Add ("dimensionStyleAttributeIndex", static_cast<Int32> (preferences.index));
    } else {
        preferenceState.Add ("errorCode", static_cast<Int32> (preferencesError));
    }
    row.Add ("units", GS::ObjectState (
        "coordinates", "project-meter",
        "linearMeasurements", "project-length-values-with-project-preferences-attached",
        "angles", "radian",
        "noteHeights", "millimeter-paper-size",
        "markerSizes", "millimeter-paper-size"));
    row.Add ("dimensionPreferences", preferenceState);
}

void AddDimensionIdentity (GS::ObjectState& row, const API_Element& element)
{
    const API_Elem_Head& head = element.header;
    row.Add ("elementId", CreateGuidObjectState (head.guid));
    row.Add ("nativeElementTypeId", static_cast<Int32> (GetElemTypeId (head)));
    row.Add ("modificationStamp", static_cast<Int64> (head.modiStamp));
#if TAPIR_AC_VERSION == 25
    row.Add ("elementVariationId", static_cast<Int32> (head.variationID));
#else
    row.Add ("elementVariationId", static_cast<Int32> (head.type.variationID));
#endif
    row.Add ("storyIndex", static_cast<Int32> (head.floorInd));
    row.Add ("layerIndex", DimensionAttributeIndex (head.layer));
    row.Add ("groupElementId", CreateGuidObjectState (head.groupGuid));
    row.Add ("hotlinkElementId", CreateGuidObjectState (head.hotlinkGuid));
    row.Add ("drawingOrderIndex", static_cast<Int32> (head.drwIndex));
    row.Add ("teamworkOwnerId", static_cast<Int32> (head.userId));
    row.Add ("teamworkLockId", static_cast<Int32> (head.lockId));
    row.Add ("referenceGroupLayerIndex", DimensionAttributeIndex (head.rgr_layer));
    row.Add ("hasMemo", head.hasMemo);
    row.Add ("renovationStatus", static_cast<Int32> (head.renovationStatus));
    row.Add ("renovationFilterId", CreateGuidObjectState (head.renovationFilterGuid));

    API_DatabaseInfo database = {};
    const GSErrCode databaseError = ACAPI_Database_GetContainingDatabase (&head.guid, &database);
    GS::ObjectState databaseState;
    databaseState.Add ("status", databaseError == NoError ? "available" : "unavailable");
    if (databaseError == NoError) {
        databaseState.Add ("databaseId", CreateGuidObjectState (database.databaseUnId.elemSetId));
        databaseState.Add ("name", GS::UniString (database.name));
        databaseState.Add ("title", GS::UniString (database.title));
        databaseState.Add ("reference", GS::UniString (database.ref));
        databaseState.Add ("databaseType", static_cast<Int32> (database.typeID));
        databaseState.Add ("databaseIndex", static_cast<Int32> (database.index));
        databaseState.Add ("linkedElementId", CreateGuidObjectState (database.linkedElement));
        databaseState.Add ("linkedDatabaseId", CreateGuidObjectState (database.linkedDatabaseUnId.elemSetId));
        databaseState.Add ("linkedSegmentMemoIndex", database.linkedSegmentMemoIndex);
    } else {
        databaseState.Add ("errorCode", static_cast<Int32> (databaseError));
    }
    row.Add ("database", databaseState);

    API_Elem_Head header = {};
    header.guid = head.guid;
    API_Box3D bounds = {};
    const GSErrCode boundsError = ACAPI_Element_CalcBounds (&header, &bounds);
    GS::ObjectState boundsState;
    boundsState.Add ("status", boundsError == NoError ? "available" : "unavailable");
    if (boundsError == NoError) {
        boundsState.Add ("xMin", bounds.xMin);
        boundsState.Add ("yMin", bounds.yMin);
        boundsState.Add ("xMax", bounds.xMax);
        boundsState.Add ("yMax", bounds.yMax);
        boundsState.Add ("zMin", bounds.zMin);
        boundsState.Add ("zMax", bounds.zMax);
        boundsState.Add ("unit", "project-meter");
    } else {
        boundsState.Add ("errorCode", static_cast<Int32> (boundsError));
    }
    row.Add ("bounds", boundsState);
}

GS::ObjectState DimensionElementError (const GS::UniString& guidText, const API_Guid& guid, const GSErrCode error, const char* reason)
{
    GS::ObjectState row;
    row.Add ("status", "error");
    row.Add ("requestedGuid", guidText);
    row.Add ("elementId", CreateGuidObjectState (guid));
    row.Add ("elementType", GS::ObjectState ("status", "unavailable", "reason", "exact-element-type-read-failed"));
    row.Add ("errorCode", static_cast<Int32> (error));
    row.Add ("error", reason);
    return row;
}

const char* KnownDimensionUnsupportedNativeTypeName (API_ElemTypeID type)
{
    switch (type) {
        case API_WallID: return "Wall";
        case API_SlabID: return "Slab";
        case API_ColumnID: return "Column";
        case API_BeamID: return "Beam";
        case API_DoorID: return "Door";
        case API_WindowID: return "Window";
        case API_ObjectID: return "Object";
        case API_MeshID: return "Mesh";
        case API_RoofID: return "Roof";
        case API_SectElemID: return "Section";
        default: return "UnsupportedNativeElement";
    }
}

GS::ObjectState DimensionUnsupportedElementError (const GS::UniString& guidText, const API_Element& element)
{
    const API_ElemTypeID type = GetElemTypeId (element.header);
    GS::ObjectState row;
    row.Add ("status", "error");
    row.Add ("requestedGuid", guidText);
    row.Add ("elementId", CreateGuidObjectState (element.header.guid));
    row.Add ("elementType", KnownDimensionUnsupportedNativeTypeName (type));
    row.Add ("elementTypeReadStatus", "available-exact-native-header");
    row.Add ("nativeElementTypeId", static_cast<Int32> (type));
#if TAPIR_AC_VERSION == 25
    row.Add ("elementVariationId", static_cast<Int32> (element.header.variationID));
#else
    row.Add ("elementVariationId", static_cast<Int32> (element.header.type.variationID));
#endif
    row.Add ("errorCode", static_cast<Int32> (APIERR_BADID));
    row.Add ("error", "Exact GUID resolves to a native element outside the supported dimension families.");
    return row;
}

} // namespace

GetNativeDimensionDetailsCommand::GetNativeDimensionDetailsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionDetailsCommand::GetName () const { return "GetNativeDimensionDetails"; }
GS::Optional<GS::UniString> GetNativeDimensionDetailsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"guids":{"type":"array","minItems":1,"maxItems":25,"items":{"type":"string","format":"uuid"}}},"additionalProperties":false,"required":["guids"]})";
}
GS::Optional<GS::UniString> GetNativeDimensionDetailsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionDetailsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::UniString> guids;
    if (!parameters.Get ("guids", guids) || guids.GetSize () < 1 || guids.GetSize () > 25)
        return CreateErrorResponse (APIERR_BADPARS, "GetNativeDimensionDetails requires between one and 25 exact GUIDs.");

    GS::Array<API_Guid> parsedGuids;
    for (const GS::UniString& text : guids) {
        const API_Guid guid = APIGuidFromString (text.ToCStr ());
        if (guid == APINULLGuid || parsedGuids.Contains (guid))
            return CreateErrorResponse (APIERR_BADPARS, "Dimension GUIDs must be valid and unique.");
        parsedGuids.Push (guid);
    }

    GS::ObjectState response;
    response.Add ("schemaVersion", "native-dimension-read/v1");
    response.Add ("status", "success");
    const auto& rows = response.AddList<GS::ObjectState> ("elements");
    for (UInt32 requestIndex = 0; requestIndex < guids.GetSize (); ++requestIndex) {
        const GS::UniString& guidText = guids[requestIndex];
        API_Element element = {};
        element.header.guid = parsedGuids[requestIndex];
        const GSErrCode getError = ACAPI_Element_Get (&element);
        if (getError != NoError) {
            rows (DimensionElementError (guidText, parsedGuids[requestIndex], getError, "Failed to read the exact dimension GUID."));
            continue;
        }

        const API_ElemTypeID type = GetElemTypeId (element.header);
        const UInt64 initialModificationStamp = element.header.modiStamp;
        const bool linear = type == API_DimensionID;
        const bool radial = type == API_RadialDimensionID;
        const bool angle = type == API_AngleDimensionID;
        const bool level = type == API_LevelDimensionID;
        if (!linear && !radial && !angle && !level) {
            rows (DimensionUnsupportedElementError (guidText, element));
            continue;
        }

        // Fixed-width note buffers cannot prove full Unicode source.  Seed
        // caller-owned values on the family-resolved element and issue a
        // second exact read.  Only use a value when the API preserves that
        // precise pointer and populates its known storage; never dereference a
        // pointer returned by the host unless it is still our own storage.
        const GS::UniString probeSentinel = "<Tapir-Native-Dimension-Read-Probe-Do-Not-Persist>";
        GS::UniString defaultNoteProbe = probeSentinel;
        GS::UniString noteProbe1 = probeSentinel;
        GS::UniString noteProbe2 = probeSentinel;
        GS::UniString prefixProbe = probeSentinel;
        API_Element textProbeElement = {};
        SetNativeElementTypeHeader (textProbeElement, type);
        textProbeElement.header.guid = parsedGuids[requestIndex];
        if (linear) textProbeElement.dimension.defNote.contentUStr = &defaultNoteProbe;
        if (radial) { textProbeElement.radialDimension.note.contentUStr = &noteProbe1; textProbeElement.radialDimension.prefixUStr = &prefixProbe; }
        if (angle) textProbeElement.angleDimension.note.contentUStr = &noteProbe1;
        if (level) { textProbeElement.levelDimension.note1.contentUStr = &noteProbe1; textProbeElement.levelDimension.note2.contentUStr = &noteProbe2; }
        const GSErrCode unicodeProbeError = ACAPI_Element_Get (&textProbeElement);
        const bool unicodeProbeReadSucceeded = unicodeProbeError == NoError;
        const bool unicodeProbeSameRevision = unicodeProbeReadSucceeded
            && textProbeElement.header.guid == element.header.guid
            && GetElemTypeId (textProbeElement.header) == type
            && textProbeElement.header.modiStamp == initialModificationStamp;
        const bool defaultNoteUnicodeVerified = unicodeProbeSameRevision && linear && textProbeElement.dimension.defNote.contentUStr == &defaultNoteProbe && defaultNoteProbe != probeSentinel;
        const bool standaloneNote1UnicodeVerified = unicodeProbeSameRevision && (radial ? textProbeElement.radialDimension.note.contentUStr == &noteProbe1 && noteProbe1 != probeSentinel
            : angle ? textProbeElement.angleDimension.note.contentUStr == &noteProbe1 && noteProbe1 != probeSentinel
            : level ? textProbeElement.levelDimension.note1.contentUStr == &noteProbe1 && noteProbe1 != probeSentinel : false);
        const bool standaloneNote2UnicodeVerified = unicodeProbeSameRevision && level && textProbeElement.levelDimension.note2.contentUStr == &noteProbe2 && noteProbe2 != probeSentinel;
        const bool radialPrefixUnicodeVerified = unicodeProbeSameRevision && radial && textProbeElement.radialDimension.prefixUStr == &prefixProbe && prefixProbe != probeSentinel;

        GS::ObjectState row;
        row.Add ("requestedGuid", guidText);
        row.Add ("elementType", linear ? "Dimension" : radial ? "RadialDimension" : angle ? "AngleDimension" : "LevelDimension");
        AddDimensionIdentity (row, element);
        AddDimensionUnits (row);

        GS::ObjectState identity;
        GS::ObjectState geometry;
        GS::Array<GS::ObjectState> standaloneNotes;
        GS::Array<GS::ObjectState> points;
        identity.Add ("family", linear ? "linear-chain" : radial ? "radial" : angle ? "angle" : "level");
        identity.Add ("contextStatus", "database-story-layer-from-native-element-header");

        API_ElementMemo memo = {};
        const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
        const GSErrCode memoError = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_All);
        if (memoError != NoError) {
            row.Add ("status", "error");
            row.Add ("error", "The dimension memo could not be read; the row is partial.");
            row.Add ("memo", GS::ObjectState ("status", "unavailable", "errorCode", static_cast<Int32> (memoError)));
            row.Add ("coverage", "partial-memo-read-failed");
            row.Add ("identity", identity);
            rows (row);
            continue;
        }
        row.Add ("memo", GS::ObjectState ("status", "available"));
        bool unicodeCoverageComplete = unicodeProbeReadSucceeded;
        row.Add ("unicodeProbeStatus", unicodeProbeReadSucceeded ? "second-exact-element-read-completed" : "second-exact-element-read-failed");
        row.Add ("unicodeProbeConsistency", unicodeProbeSameRevision ? "same-guid-type-and-modification-stamp" : "different-or-unverified-native-revision");

        if (linear) {
            const API_DimensionType& dimension = element.dimension;
            geometry.Add ("linePosition", DimensionPoint2D (dimension.refC));
            geometry.Add ("direction", GS::ObjectState ("x", dimension.direction.x, "y", dimension.direction.y));
            geometry.Add ("directionUnit", "unitless-vector");
            geometry.Add ("threeDimensionalPosition", DimensionPoint3D (dimension.refC3D));
            geometry.Add ("threeDimensionalDirection", GS::ObjectState ("x", dimension.direction3D.x, "y", dimension.direction3D.y, "z", dimension.direction3D.z));
            geometry.Add ("threeDimensionalNormal", GS::ObjectState ("x", dimension.normVect3D.x, "y", dimension.normVect3D.y, "z", dimension.normVect3D.z));
            geometry.Add ("arcAnchor", DimensionBaseDetails (dimension.arcBase));
            geometry.Add ("segmentCount", dimension.nDimElem);
            geometry.Add ("linePen", static_cast<Int32> (dimension.linPen));
            geometry.Add ("textPosition", static_cast<Int32> (dimension.textPos));
            geometry.Add ("textDirection", static_cast<Int32> (dimension.textWay));
            geometry.Add ("appearance", static_cast<Int32> (dimension.dimAppear));
            geometry.Add ("detailedOpeningDimension", dimension.detailedHoleDim);
            geometry.Add ("nominalOpeningDimension", static_cast<Int32> (dimension.nominalHoleDim));
            geometry.Add ("compositeThicknessMode", static_cast<Int32> (dimension.compAutoThick));
            geometry.Add ("defaultStaticSetting", dimension.defStaticDim);
            geometry.Add ("defaultWitnessForm", static_cast<Int32> (dimension.defWitnessForm));
            geometry.Add ("defaultWitnessValue", dimension.defWitnessVal);
            geometry.Add ("defaultNote", DimensionNoteDetails (dimension.defNote, defaultNoteUnicodeVerified ? &defaultNoteProbe : nullptr, false));
            geometry.Add ("defaultNoteUnicodeStatus", defaultNoteUnicodeVerified ? "verified-full-unicode-content" : "fixed-buffer-or-unverified-unicode-pointer");
            unicodeCoverageComplete = unicodeCoverageComplete && defaultNoteUnicodeVerified;
            geometry.Add ("marker", DimensionMarkerDetails (dimension.markerData));
            geometry.Add ("elevationMarkerForm", static_cast<Int32> (dimension.ed_dimForm));
            geometry.Add ("elevationArrowAngleCode", static_cast<Int32> (dimension.ed_arrowAng));
            geometry.Add ("elevationArrowFilled", dimension.ed_arrowFill);
            geometry.Add ("elevationArrowUp", dimension.ed_arrowUp);
            geometry.Add ("elevationNeedsPlus", dimension.ed_needPlus);
            geometry.Add ("usedIn3D", dimension.usedIn3D);
            geometry.Add ("horizontalText", dimension.horizontalText);
            geometry.Add ("onlyDimensionText", dimension.onlyDimensionText);
            geometry.Add ("elevationOrigin", static_cast<Int32> (dimension.ed_origin));
            geometry.Add ("circleLevelFill", static_cast<Int32> (dimension.ed_circleLevelDimensionFill));
            geometry.Add ("layout", static_cast<Int32> (dimension.dimLayout));
            geometry.Add ("pushNeighborEnabled", dimension.pushNeighborEnabled);
            geometry.Add ("placeUnderEnabled", dimension.placeUnderEnabled);
            geometry.Add ("advancedLayout", dimension.advanced);
            geometry.Add ("clipOtherSide", dimension.clipOtherSide);
#if TAPIR_AC_VERSION >= 26
            geometry.Add ("textFillOrder", static_cast<Int32> (dimension.textFillOrder));
#else
            geometry.Add ("textFillOrder", GS::ObjectState ("status", "not-applicable-in-ac25"));
#endif
            geometry.Add ("measurement", GS::ObjectState ("source", "per-point-native-dimVal", "display", "unavailable-unless-independently-observed"));
            geometry.Add ("staticAnchorEvidence", "per-point-anchor-fields-only-defaultStaticDim-is-not-proof");

            const Int32 count = dimension.nDimElem;
            const UInt64 requiredMemoBytes = count < 0 ? 0 : static_cast<UInt64> (count) * sizeof (API_DimElem);
            const UInt64 actualMemoBytes = memo.dimElems == nullptr ? 0 : static_cast<UInt64> (BMGetHandleSize ((GSHandle) memo.dimElems));
            if (count < 0 || count > 100000 || (count > 0 && (memo.dimElems == nullptr || *memo.dimElems == nullptr)) || actualMemoBytes < requiredMemoBytes) {
                row.Add ("status", "error");
                row.Add ("coverage", "memo-count-or-handle-invalid");
                row.Add ("error", "Linear dimension memo count is invalid or its complete handle is missing or shorter than the declared point count.");
                row.Add ("identity", identity);
                row.Add ("geometryAndSettings", geometry);
                rows (row);
                continue;
            }
            for (Int32 index = 0; index < count; ++index) {
                const API_DimElem& item = (*memo.dimElems)[index];
                GS::ObjectState point;
                point.Add ("index", index);
                point.Add ("anchor", DimensionBaseDetails (item.base));
                point.Add ("dimensionPosition", DimensionPoint2D (item.pos));
                point.Add ("dimensionPosition3D", DimensionPoint3D (item.pos3D));
                point.Add ("measuredValueFromPreviousPoint", item.dimVal);
                point.Add ("measurementSource", "native-API_DimElem.dimVal");
                point.Add ("witnessForm", static_cast<Int32> (item.witnessForm));
                point.Add ("witnessValue", item.witnessVal);
                point.Add ("fixedPosition", item.fixedPos);
                point.Add ("note", DimensionNoteDetails (item.note));
                if (item.note.contentUStr == nullptr)
                    unicodeCoverageComplete = false;
                points.Push (point);
            }
            geometry.Add ("pointCount", count);
            geometry.Add ("pointMemoHandleBytes", static_cast<Int64> (actualMemoBytes));
            geometry.Add ("pointMemoStatus", "complete-ordered-count-validated");
        } else if (radial) {
            const API_RadialDimensionType& dimension = element.radialDimension;
            geometry.Add ("base", DimensionBaseDetails (dimension.base));
            geometry.Add ("endPosition", DimensionPoint2D (dimension.endC));
            geometry.Add ("measuredRadius", dimension.dimVal);
            geometry.Add ("measurementSource", "native-API_RadialDimensionType.dimVal");
            geometry.Add ("display", "unavailable-unless-independently-observed");
            geometry.Add ("linePen", static_cast<Int32> (dimension.linPen));
            geometry.Add ("textPosition", static_cast<Int32> (dimension.textPos));
            geometry.Add ("textDirection", static_cast<Int32> (dimension.textWay));
            geometry.Add ("showOrigin", dimension.showOrigo);
            geometry.Add ("onlyDimensionText", dimension.onlyDimensionText);
            if (radialPrefixUnicodeVerified) {
                geometry.Add ("prefix", prefixProbe);
                geometry.Add ("prefixSource", "verified-caller-bound-unicode-prefixUStr");
            } else {
                char fixedPrefix[sizeof (dimension.prefix) + 1] = {};
                std::copy (dimension.prefix, dimension.prefix + sizeof (dimension.prefix), fixedPrefix);
                GS::UInt32 prefixLength = 0;
                while (prefixLength < sizeof (dimension.prefix) && fixedPrefix[prefixLength] != '\0') ++prefixLength;
                geometry.Add ("prefix", GS::UniString (fixedPrefix));
                geometry.Add ("prefixByteLength", static_cast<Int32> (prefixLength));
                geometry.Add ("prefixBufferCapacity", static_cast<Int32> (sizeof (dimension.prefix)));
                geometry.Add ("prefixSource", dimension.prefixUStr == nullptr ? "bounded-fixed-buffer-prefix-encoding-unproven" : "bounded-fixed-buffer-prefix-unverified-pointer-not-dereferenced");
            }
            geometry.Add ("prefixUnicodeStatus", radialPrefixUnicodeVerified ? "verified-full-unicode-content" : "bounded-prefix-unicode-unproven");
            geometry.Add ("marker", DimensionMarkerDetails (dimension.markerData));
            standaloneNotes.Push (GS::ObjectState ("slot", "note", "applicability", "always", "details", DimensionNoteDetails (dimension.note, standaloneNote1UnicodeVerified ? &noteProbe1 : nullptr, false)));
            unicodeCoverageComplete = unicodeCoverageComplete && standaloneNote1UnicodeVerified && radialPrefixUnicodeVerified;
            geometry.Add ("staticAnchorEvidence", "native-base-anchor-fields-only");
        } else if (angle) {
            const API_AngleDimensionType& dimension = element.angleDimension;
            geometry.Add ("arcPosition", DimensionPoint2D (dimension.pos));
            geometry.Add ("arcOrigin", DimensionPoint2D (dimension.origo));
            geometry.Add ("measuredAngle", dimension.dimVal);
            geometry.Add ("measurementUnit", "radian");
            geometry.Add ("measurementSource", "native-API_AngleDimensionType.dimVal");
            geometry.Add ("display", "unavailable-unless-independently-observed");
            geometry.Add ("arcRadius", dimension.radius);
            geometry.Add ("arcRadiusUnit", "project-meter");
            geometry.Add ("linePen", static_cast<Int32> (dimension.linPen));
            geometry.Add ("smallArc", dimension.smallArc);
            geometry.Add ("onlyDimensionText", dimension.onlyDimensionText);
            geometry.Add ("textPosition", static_cast<Int32> (dimension.textPos));
            geometry.Add ("textDirection", static_cast<Int32> (dimension.textWay));
            geometry.Add ("witnessForm", static_cast<Int32> (dimension.witnessForm));
            geometry.Add ("witnessValue", dimension.witnessVal);
            geometry.Add ("marker", DimensionMarkerDetails (dimension.markerData));
            GS::Array<GS::ObjectState> anchors;
            for (Int32 index = 0; index < 4; ++index) anchors.Push (DimensionBaseDetails (dimension.base[index]));
            geometry.Add ("anchors", anchors);
            standaloneNotes.Push (GS::ObjectState ("slot", "note", "applicability", "always", "details", DimensionNoteDetails (dimension.note, standaloneNote1UnicodeVerified ? &noteProbe1 : nullptr, false)));
            unicodeCoverageComplete = unicodeCoverageComplete && standaloneNote1UnicodeVerified;
        } else {
            const API_LevelDimensionType& dimension = element.levelDimension;
            geometry.Add ("markerPosition", DimensionPoint2D (dimension.loc));
            geometry.Add ("levelValue", dimension.level);
            geometry.Add ("levelMeasurementSource", "native-API_LevelDimensionType.level");
            geometry.Add ("display", "unavailable-unless-independently-observed");
            geometry.Add ("pen", static_cast<Int32> (dimension.pen));
            geometry.Add ("format", static_cast<Int32> (dimension.dimForm));
            geometry.Add ("needsPlus", dimension.needPlus);
            geometry.Add ("staticLevel", dimension.staticLevel);
            geometry.Add ("markerSize", dimension.markerSize);
            geometry.Add ("markerSizeUnit", "millimeter-paper-size");
            geometry.Add ("markerAngle", dimension.angle);
            geometry.Add ("markerAngleUnit", "radian");
#if TAPIR_AC_VERSION == 25
            const Int32 parentType = static_cast<Int32> (dimension.parentType);
            const bool parentIsFloor = dimension.parentType == API_ZombieElemID;
            geometry.Add ("parentTypeVariationId", GS::ObjectState ("status", "not-available-in-ac25-API_LevelDimensionType"));
#else
            const Int32 parentType = static_cast<Int32> (dimension.parentType.typeID);
            const bool parentIsFloor = dimension.parentType.typeID == API_ZombieElemID;
            geometry.Add ("parentTypeVariationId", static_cast<Int32> (dimension.parentType.variationID));
#endif
            geometry.Add ("parentTypeId", parentType);
            geometry.Add ("parentElementId", CreateGuidObjectState (dimension.parentGuid));
            geometry.Add ("parentAssociationStatus", parentIsFloor ? "floor-level-parent" : dimension.parentGuid == APINULLGuid ? "no-parent-guid-static-or-unresolved" : "element-parent-guid-present");
            geometry.Add ("parentFloorSemantics", parentIsFloor ? "native-type-identifies-floor" : "not-a-native-floor-type");
            geometry.Add ("origin", static_cast<Int32> (dimension.origin));
            standaloneNotes.Push (GS::ObjectState ("slot", "note1", "applicability", "always", "details", DimensionNoteDetails (dimension.note1, standaloneNote1UnicodeVerified ? &noteProbe1 : nullptr, false)));
            const bool note2Applicable = dimension.dimForm == 8 || dimension.dimForm == 9;
            standaloneNotes.Push (GS::ObjectState ("slot", "note2", "applicability", note2Applicable ? "applicable" : "stored-but-not-applicable-for-current-format", "details", DimensionNoteDetails (dimension.note2, standaloneNote2UnicodeVerified ? &noteProbe2 : nullptr, false)));
            unicodeCoverageComplete = unicodeCoverageComplete && standaloneNote1UnicodeVerified && standaloneNote2UnicodeVerified;
        }

        API_Elem_Head finalHeader = {};
        finalHeader.guid = element.header.guid;
        const GSErrCode finalHeaderError = ACAPI_Element_GetHeader (&finalHeader);
        const bool stableReadRevision = unicodeProbeSameRevision && finalHeaderError == NoError
            && finalHeader.guid == element.header.guid
            && GetElemTypeId (finalHeader) == type
            && finalHeader.modiStamp == initialModificationStamp;
        row.Add ("readConsistency", GS::ObjectState (
            "status", stableReadRevision ? "same-guid-type-and-modification-stamp-through-memo-read" : "native-revision-changed-or-unverified",
            "initialModificationStamp", static_cast<Int64> (initialModificationStamp),
            "probeModificationStamp", static_cast<Int64> (textProbeElement.header.modiStamp),
            "finalHeaderErrorCode", static_cast<Int32> (finalHeaderError),
            "finalModificationStamp", static_cast<Int64> (finalHeader.modiStamp)));
        row.Add ("status", stableReadRevision ? "success" : "error");
        if (!stableReadRevision) row.Add ("error", "The exact element GUID, type, or modification stamp changed during Unicode and memo reads.");
        row.Add ("unicodeAccessCoverage", GS::ObjectState ("status", !stableReadRevision ? "unverified" : unicodeCoverageComplete ? "complete" : "partial",
            "reason", !stableReadRevision ? "native-revision-coherence-unverified" : "exact-pointer-and-memo-Unicode-access-only"));
        row.Add ("sourceFidelityCoverage", GS::ObjectState ("status", "unproven",
            "reason", "native-Unicode-content-access-or-requested-input-equality-does-not-prove-original-expression"));
        row.Add ("identity", identity);
        row.Add ("coverage", !stableReadRevision ? "partial-read-revision-unverified" : unicodeCoverageComplete ? "complete-for-versioned-dimension-fields" : "partial-unicode-source-unproven");
        row.Add ("geometryAndSettings", geometry);
        row.Add ("orderedPoints", points);
        row.Add ("standaloneNotes", standaloneNotes);
        if (stableReadRevision) {
            const GS::Optional<GS::UniString> factualFingerprint = NativeDimensionFactualFingerprint (row);
            if (factualFingerprint.HasValue ()) {
                row.Add ("fingerprint", factualFingerprint.Get ());
                row.Add ("fingerprintAlgorithm", "native-dimension-fnv1a-64-typed-facts-v2");
                row.Add ("fingerprintCoverage", "partial");
            }
        }
        rows (row);
    }
    return response;
}

namespace {

struct RadialDefaultsContext {
    GSErrCode projectError = APIERR_GENERAL;
    GSErrCode windowError = APIERR_GENERAL;
    GSErrCode databaseError = APIERR_GENERAL;
    GSErrCode storyError = APIERR_GENERAL;
    GS::UniString projectPath;
    bool projectPathPresent = false;
    bool savedSolo = false;
    API_WindowInfo window = {};
    API_DatabaseInfo database = {};
    Int32 activeStory = -1;
    GSErrCode drawingScaleError = APIERR_GENERAL;
    double drawingScale = 0.0;
    GS::Optional<GS::UniString> fileScopeKey;
};

constexpr auto NativeDimensionAuditBudget = std::chrono::seconds (30);
constexpr API_ElemTypeID NativeDimensionAuditFamilies[] = {
	API_DimensionID, API_RadialDimensionID, API_AngleDimensionID, API_LevelDimensionID
};

struct NativeDimensionAuditCandidate {
	API_Guid guid = APINULLGuid;
	GS::UniString guidText;
	API_ElemTypeID type = API_ZombieElemID;
	Int32 variationId = 0;
	Int32 storyIndex = 0;
	UInt64 modificationStamp = 0;
	NativeDimensionAuditPolicy::Bounds2D bounds = {};
};

struct NativeDimensionAuditManifest {
	GS::ObjectState state;
	std::vector<NativeDimensionAuditCandidate> candidates;
	UInt64 rawCandidateCount = 0;
	UInt64 checkedCount = 0;
	UInt64 unreadableCount = 0;
	UInt64 duplicateCount = 0;
	UInt64 wrongTypeCount = 0;
	UInt64 wrongStoryCount = 0;
	UInt64 unavailableBoundsCount = 0;
	UInt64 matchedCount = 0;
	UInt64 errorCount = 0;
	bool withinCandidateLimit = true;
	bool complete = true;
	bool cancelled = false;
	GS::UniString reason;
};

GS::ObjectState RadialDefaultsContextDetails (const RadialDefaultsContext& context);
bool RadialDefaultsContextAvailable (const RadialDefaultsContext& context);

bool NativeDimensionAuditExpired (const std::chrono::steady_clock::time_point& started, GS::ProcessControl& processControl)
{
	return processControl.TestBreak () || std::chrono::steady_clock::now () - started >= NativeDimensionAuditBudget;
}

std::string NativeDimensionAuditPathKey (const GS::UniString& value)
{
	GS::UniString normalized (value);
	normalized.ReplaceAll ("\\", "/");
	normalized = normalized.ToLowerCase ();
	return FixtureUtf8 (normalized);
}

GS::ObjectState NativeDimensionAuditContextFacts (const RadialDefaultsContext& context, const GS::ObjectState& preferences)
{
	GS::ObjectState result = RadialDefaultsContextDetails (context);
	GS::ObjectState scale (
		"status", context.drawingScaleError == NoError && std::isfinite (context.drawingScale) && context.drawingScale > 0.0 ? "available" : "unavailable",
		"errorCode", static_cast<Int32> (context.drawingScaleError),
		"source", "TAPIR_Drawing_GetDrawingScale-current-FloorPlan-context");
	if (context.drawingScaleError == NoError && std::isfinite (context.drawingScale) && context.drawingScale > 0.0) {
		scale.Add ("value", context.drawingScale);
		scale.Add ("unit", "drawing-scale-ratio");
	}
	result.Add ("drawingScale", scale);
	if (context.projectError == NoError && context.projectPathPresent)
		result.Add ("projectPath", context.projectPath);
	if (context.fileScopeKey.HasValue ())
		result.Add ("fileScopeKey", context.fileScopeKey.Get ());
	result.Add ("preferences", preferences);
	return result;
}

// V2 adds exact current-window facts without changing the historic v1
// context shape. The Floor Plan window is identified by its SDK type; retain
// its returned index and databaseUnId exactly (the latter may be APINULLGuid).
// The current database's concrete non-null identity is carried separately.
GS::ObjectState NativeDimensionAuditWindowIdentityFacts (const RadialDefaultsContext& context)
{
	GS::ObjectState window (
		"status", context.windowError == NoError ? "available" : "unavailable",
		"errorCode", static_cast<Int32> (context.windowError));
	if (context.windowError == NoError) {
		window.Add ("typeId", static_cast<Int32> (context.window.typeID));
		window.Add ("index", static_cast<Int32> (context.window.index));
		window.Add ("databaseId", APIGuidToString (context.window.databaseUnId.elemSetId));
	}
	return window;
}

bool SameNativeDimensionAuditWindowIdentity (const RadialDefaultsContext& before, const RadialDefaultsContext& after)
{
	return before.windowError == NoError && after.windowError == NoError
		&& before.window.typeID == after.window.typeID
		&& before.window.index == after.window.index
		&& before.window.databaseUnId.elemSetId == after.window.databaseUnId.elemSetId;
}

GS::ObjectState NativeDimensionAuditPreferences ()
{
	GS::ObjectState holder;
	AddDimensionUnits (holder);
	GS::ObjectState preferences, units;
	const bool preferencesPresent = holder.Get ("dimensionPreferences", preferences);
	const bool unitsPresent = holder.Get ("units", units);
	GS::UniString preferenceStatus;
	const bool preferenceValuesAvailable = preferencesPresent && preferences.Get ("status", preferenceStatus) && preferenceStatus == "available";
	return GS::ObjectState (
		"status", preferenceValuesAvailable && unitsPresent ? "available" : "partial",
		"source", "ACAPI_ProjectSetting_GetPreferences/APIPrefs_DimensionsID-plus-reader-unit-declarations",
		"scope", "project-dimension-preferences-and-reader-unit-declarations",
		"dimensionPreferences", preferencesPresent ? preferences : GS::ObjectState (
			"status", "unavailable", "source", "ACAPI_ProjectSetting_GetPreferences/APIPrefs_DimensionsID",
			"scope", "project-dimension-preferences", "savedViewEffectiveFormat", "not-proven-by-project-preferences-read",
			"formatCoverage", "unavailable", "errorCode", static_cast<Int32> (APIERR_GENERAL)),
		"units", unitsPresent ? units : GS::ObjectState ("status", "unavailable", "reason", "unit-declarations-not-returned"));
}

bool NativeDimensionAuditPreferencesAvailable (const GS::ObjectState& preferences)
{
	GS::UniString status;
	return preferences.Get ("status", status) && status == "available";
}

bool NativeDimensionAuditFactsText (const GS::ObjectState& facts, GS::UniString& serialized, std::uint64_t& byteCount)
{
	if (JSON::CreateFromObjectState (facts, serialized, false) != NoError)
		return false;
	byteCount = static_cast<std::uint64_t> (FixtureUtf8 (serialized).size ());
	return true;
}

bool NativeDimensionAuditContextMatchesRequest (
	const RadialDefaultsContext& context,
	const GS::UniString& projectPath,
	const GS::UniString& fileScopeKey,
	const API_Guid& databaseGuid,
	const Int32 storyIndex)
{
	return RadialDefaultsContextAvailable (context)
		&& NativeDimensionAuditPathKey (context.projectPath) == NativeDimensionAuditPathKey (projectPath)
		&& context.fileScopeKey.HasValue () && context.fileScopeKey.Get () == fileScopeKey
        && context.database.databaseUnId.elemSetId == databaseGuid
		&& context.drawingScaleError == NoError && std::isfinite (context.drawingScale) && context.drawingScale > 0.0
		&& context.activeStory == storyIndex;
}

bool NativeDimensionAuditRowMatchesCandidate (
	const GS::ObjectState& row,
	const NativeDimensionAuditCandidate& candidate,
	const API_Guid& databaseGuid,
	const Int32 databaseTypeId,
	const Int32 storyIndex,
	const GS::ObjectState& preferences,
	GS::UniString& mismatchReason)
{
	auto mismatch = [&mismatchReason] (const char* reason) {
		mismatchReason = reason;
		return false;
	};
	Int32 typeId = 0;
	Int32 variationId = 0;
	Int32 returnedStory = 0;
	Int64 modificationStamp = -1;
	if (!row.Get ("nativeElementTypeId", typeId) || typeId != static_cast<Int32> (candidate.type)) return mismatch ("native-element-type-differs-from-inventory-header");
	if (!row.Get ("elementVariationId", variationId) || variationId != candidate.variationId) return mismatch ("element-variation-differs-from-inventory-header");
	if (!row.Get ("storyIndex", returnedStory) || returnedStory != storyIndex || returnedStory != candidate.storyIndex) return mismatch ("story-differs-from-request-or-inventory-header");
	if (!row.Get ("modificationStamp", modificationStamp) || modificationStamp < 0 || static_cast<UInt64> (modificationStamp) != candidate.modificationStamp) return mismatch ("modification-stamp-differs-from-inventory-header");
	GS::ObjectState database;
	GS::UniString databaseStatus;
	GS::ObjectState databaseId;
	GS::UniString returnedDatabaseGuid;
	Int32 returnedDatabaseType = 0;
	if (!row.Get ("database", database) || !database.Get ("status", databaseStatus) || databaseStatus != "available"
		|| !database.Get ("databaseId", databaseId) || !databaseId.Get ("guid", returnedDatabaseGuid)
		|| !database.Get ("databaseType", returnedDatabaseType) || returnedDatabaseType != databaseTypeId
		|| NativeDimensionAuditPathKey (returnedDatabaseGuid) != NativeDimensionAuditPathKey (APIGuidToString (databaseGuid)))
		return mismatch ("reader-database-differs-from-verified-floor-plan-context");
	GS::ObjectState bounds;
	GS::UniString boundsStatus, boundsUnit;
	double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
	if (!row.Get ("bounds", bounds) || !bounds.Get ("status", boundsStatus) || boundsStatus != "available"
		|| !bounds.Get ("xMin", xMin) || !bounds.Get ("yMin", yMin) || !bounds.Get ("xMax", xMax) || !bounds.Get ("yMax", yMax)
		|| !bounds.Get ("unit", boundsUnit) || boundsUnit != "project-meter"
		|| !NativeDimensionAuditPolicy::IsFiniteOrderedBounds ({ xMin, yMin, xMax, yMax })
		|| xMin != candidate.bounds.xMin || yMin != candidate.bounds.yMin || xMax != candidate.bounds.xMax || yMax != candidate.bounds.yMax)
		return mismatch ("reader-bounds-differ-from-inventory-membership-facts");
	GS::ObjectState consistency;
	GS::UniString consistencyStatus;
	Int64 initialStamp = -1, probeStamp = -1, finalStamp = -1;
	Int32 finalHeaderError = APIERR_GENERAL;
	if (!row.Get ("readConsistency", consistency)
		|| !consistency.Get ("status", consistencyStatus) || consistencyStatus != "same-guid-type-and-modification-stamp-through-memo-read"
		|| !consistency.Get ("initialModificationStamp", initialStamp)
		|| !consistency.Get ("probeModificationStamp", probeStamp)
		|| !consistency.Get ("finalHeaderErrorCode", finalHeaderError) || finalHeaderError != NoError
		|| !consistency.Get ("finalModificationStamp", finalStamp)
		|| static_cast<UInt64> (initialStamp) != candidate.modificationStamp
		|| static_cast<UInt64> (probeStamp) != candidate.modificationStamp
		|| static_cast<UInt64> (finalStamp) != candidate.modificationStamp)
		return mismatch ("reader-internal-revision-check-does-not-match-inventory-header");
	GS::ObjectState rowUnits, expectedUnits, rowDimensionPreferences, expectedDimensionPreferences;
	if (!row.Get ("units", rowUnits) || !preferences.Get ("units", expectedUnits)
		|| !row.Get ("dimensionPreferences", rowDimensionPreferences) || !preferences.Get ("dimensionPreferences", expectedDimensionPreferences))
		return mismatch ("reader-units-or-dimension-preferences-are-unavailable");
	GS::UniString actualFactsText, expectedFactsText;
	std::uint64_t ignoredBytes = 0;
	if (!NativeDimensionAuditFactsText (rowUnits, actualFactsText, ignoredBytes)
		|| !NativeDimensionAuditFactsText (expectedUnits, expectedFactsText, ignoredBytes)
		|| actualFactsText != expectedFactsText)
		return mismatch ("reader-unit-declarations-differ-from-audit-context");
	if (!NativeDimensionAuditFactsText (rowDimensionPreferences, actualFactsText, ignoredBytes)
		|| !NativeDimensionAuditFactsText (expectedDimensionPreferences, expectedFactsText, ignoredBytes)
		|| actualFactsText != expectedFactsText)
		return mismatch ("reader-dimension-preferences-differ-from-audit-context");
	return true;
}

NativeDimensionAuditManifest ReadNativeDimensionAuditManifest (
	const NativeDimensionAuditPolicy::Bounds2D& requestedBounds,
	const Int32 requestedStory,
	const std::chrono::steady_clock::time_point& started,
	GS::ProcessControl& processControl)
{
	NativeDimensionAuditManifest manifest;
	const auto& familyStates = manifest.state.AddList<GS::ObjectState> ("families");
	std::set<std::string> seen;
	bool allFamilyListsInspected = true;
	for (std::size_t familyIndex = 0; familyIndex < std::size (NativeDimensionAuditFamilies); ++familyIndex) {
		const API_ElemTypeID family = NativeDimensionAuditFamilies[familyIndex];
		if (NativeDimensionAuditExpired (started, processControl)) {
			allFamilyListsInspected = false;
			manifest.cancelled = true;
			manifest.complete = false;
			manifest.reason = processControl.TestBreak () ? "cooperative-cancelled-before-family-list" : "cooperative-deadline-before-family-list";
			for (std::size_t remaining = familyIndex; remaining < std::size (NativeDimensionAuditFamilies); ++remaining) {
				GS::ObjectState skipped ("requestedNativeElementTypeId", static_cast<Int32> (NativeDimensionAuditFamilies[remaining]), "listStatus", "not-read", "candidateCountStatus", "not-read-after-cooperative-stop", "checkedCount", static_cast<Int64> (0));
				GS::Array<GS::ObjectState> noCandidates;
				skipped.Add ("candidates", noCandidates);
				familyStates (skipped);
			}
			break;
		}
		GS::ObjectState familyState;
		familyState.Add ("requestedNativeElementTypeId", static_cast<Int32> (family));
		GS::Array<API_Guid> guids;
		const GSErrCode listError = ACAPI_Element_GetElemList (family, &guids, APIFilt_OnActFloor);
		familyState.Add ("listErrorCode", static_cast<Int32> (listError));
		familyState.Add ("listStatus", listError == NoError ? "available" : "unavailable");
		if (listError != NoError) {
			allFamilyListsInspected = false;
			manifest.errorCount++;
			manifest.complete = false;
			familyState.Add ("candidateCountStatus", "unavailable-list-error-not-known-zero");
			familyState.Add ("checkedCount", static_cast<Int64> (0));
			GS::Array<GS::ObjectState> noCandidates;
			familyState.Add ("candidates", noCandidates);
			familyStates (familyState);
			continue;
		}
		const UInt64 familyCount = guids.GetSize ();
		manifest.rawCandidateCount += familyCount;
		familyState.Add ("candidateCount", static_cast<Int64> (familyCount));
		if (!NativeDimensionAuditPolicy::WithinCandidateLimit (manifest.rawCandidateCount)) {
			allFamilyListsInspected = false;
			manifest.withinCandidateLimit = false;
			manifest.complete = false;
			manifest.reason = "typed-current-story-candidate-limit-exceeded";
			familyState.Add ("checkedCount", static_cast<Int64> (0));
			familyState.Add ("candidatesStatus", "not-inspected-after-combined-candidate-limit");
			familyStates (familyState);
			continue;
		}
		std::vector<API_Guid> orderedGuids;
		orderedGuids.reserve (guids.GetSize ());
		for (const API_Guid& guid : guids) orderedGuids.push_back (guid);
		std::sort (orderedGuids.begin (), orderedGuids.end (), [] (const API_Guid& left, const API_Guid& right) {
			return FixtureUtf8 (APIGuidToString (left).ToLowerCase ()) < FixtureUtf8 (APIGuidToString (right).ToLowerCase ());
		});
		const auto& candidatesState = familyState.AddList<GS::ObjectState> ("candidates");
		UInt64 familyMatched = 0;
		UInt64 familyChecked = 0;
		for (const API_Guid& guid : orderedGuids) {
			if (NativeDimensionAuditExpired (started, processControl)) {
				allFamilyListsInspected = false;
				manifest.cancelled = true;
				manifest.complete = false;
				manifest.reason = processControl.TestBreak () ? "cooperative-cancelled" : "cooperative-deadline-exceeded";
				break;
			}
			manifest.checkedCount++;
			familyChecked++;
			NativeDimensionAuditCandidate candidate;
			candidate.guid = guid;
			candidate.guidText = APIGuidToString (guid);
			GS::ObjectState entry;
			entry.Add ("elementId", CreateGuidObjectState (guid));
			const std::string key = FixtureUtf8 (candidate.guidText.ToLowerCase ());
			if (!seen.insert (key).second) {
				entry.Add ("status", "duplicate-guid");
				manifest.duplicateCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			API_Elem_Head header = {};
			header.guid = guid;
			const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
			entry.Add ("headerErrorCode", static_cast<Int32> (headerError));
			if (headerError != NoError) {
				entry.Add ("status", "unavailable-header");
				manifest.unreadableCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			if (header.guid != guid || header.guid == APINULLGuid) {
				entry.Add ("returnedHeaderGuid", CreateGuidObjectState (header.guid));
				entry.Add ("status", "unavailable-header-identity-mismatch");
				manifest.unreadableCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			candidate.type = GetElemTypeId (header);
#if TAPIR_AC_VERSION == 25
			candidate.variationId = static_cast<Int32> (header.variationID);
#else
			candidate.variationId = static_cast<Int32> (header.type.variationID);
#endif
			candidate.storyIndex = static_cast<Int32> (header.floorInd);
			candidate.modificationStamp = header.modiStamp;
			entry.Add ("nativeElementTypeId", static_cast<Int32> (candidate.type));
			entry.Add ("elementVariationId", candidate.variationId);
			entry.Add ("storyIndex", candidate.storyIndex);
			entry.Add ("modificationStamp", static_cast<Int64> (candidate.modificationStamp));
			if (candidate.type != family) {
				entry.Add ("status", "wrong-family-header");
				manifest.wrongTypeCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			if (candidate.storyIndex != requestedStory) {
				entry.Add ("status", "wrong-story-header");
				manifest.wrongStoryCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			API_Box3D nativeBounds = {};
			API_Elem_Head boundsHeader = {};
			boundsHeader.guid = guid;
			const GSErrCode boundsError = ACAPI_Element_CalcBounds (&boundsHeader, &nativeBounds);
			entry.Add ("boundsErrorCode", static_cast<Int32> (boundsError));
			if (boundsError != NoError) {
				entry.Add ("status", "unavailable-bounds");
				manifest.unavailableBoundsCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			candidate.bounds = { nativeBounds.xMin, nativeBounds.yMin, nativeBounds.xMax, nativeBounds.yMax };
			if (!NativeDimensionAuditPolicy::IsFiniteOrderedBounds (candidate.bounds)) {
				entry.Add ("status", "invalid-bounds");
				manifest.unavailableBoundsCount++;
				manifest.complete = false;
				manifest.errorCount++;
				candidatesState (entry);
				continue;
			}
			entry.Add ("bounds", GS::ObjectState (
				"xMin", candidate.bounds.xMin, "yMin", candidate.bounds.yMin,
				"xMax", candidate.bounds.xMax, "yMax", candidate.bounds.yMax,
				"unit", "project-meter"));
			const bool matched = NativeDimensionAuditPolicy::IntersectsInclusive (candidate.bounds, requestedBounds);
			entry.Add ("status", matched ? "matched" : "outside-rectangle");
			if (matched) {
				manifest.matchedCount++;
				familyMatched++;
				manifest.candidates.push_back (candidate);
			}
			candidatesState (entry);
		}
		familyState.Add ("checkedCount", static_cast<Int64> (familyChecked));
		familyState.Add ("familyMatchedCount", static_cast<Int64> (familyMatched));
		familyStates (familyState);
		if (manifest.cancelled) {
			for (std::size_t remaining = familyIndex + 1; remaining < std::size (NativeDimensionAuditFamilies); ++remaining) {
				GS::ObjectState skipped ("requestedNativeElementTypeId", static_cast<Int32> (NativeDimensionAuditFamilies[remaining]), "listStatus", "not-read", "candidateCountStatus", "not-read-after-cooperative-stop", "checkedCount", static_cast<Int64> (0));
				GS::Array<GS::ObjectState> noCandidates;
				skipped.Add ("candidates", noCandidates);
				familyStates (skipped);
			}
			break;
		}
	}
	std::sort (manifest.candidates.begin (), manifest.candidates.end (), [] (const auto& left, const auto& right) {
		return FixtureUtf8 (left.guidText.ToLowerCase ()) < FixtureUtf8 (right.guidText.ToLowerCase ());
	});
	manifest.state.Add ("rawCandidateCount", static_cast<Int64> (manifest.rawCandidateCount));
	manifest.state.Add ("checkedCount", static_cast<Int64> (manifest.checkedCount));
	manifest.state.Add ("unreadableCount", static_cast<Int64> (manifest.unreadableCount));
	manifest.state.Add ("duplicateCount", static_cast<Int64> (manifest.duplicateCount));
	manifest.state.Add ("wrongTypeCount", static_cast<Int64> (manifest.wrongTypeCount));
	manifest.state.Add ("wrongStoryCount", static_cast<Int64> (manifest.wrongStoryCount));
	manifest.state.Add ("unavailableBoundsCount", static_cast<Int64> (manifest.unavailableBoundsCount));
	manifest.state.Add ("matchedCount", static_cast<Int64> (manifest.matchedCount));
	manifest.state.Add ("errorCount", static_cast<Int64> (manifest.errorCount));
	manifest.complete = manifest.complete && allFamilyListsInspected && manifest.errorCount == 0
		&& NativeDimensionAuditPolicy::WithinCandidateLimit (manifest.rawCandidateCount);
	manifest.state.Add ("status", manifest.complete ? "complete" : "partial");
	if (!manifest.reason.IsEmpty ()) manifest.state.Add ("reason", manifest.reason);
	return manifest;
}

void ReadRadialDefaultsContext (RadialDefaultsContext& context)
{
    // API_ProjectInfo owns four heap pointers and must not be shallow-copied or
    // returned by value. Copy only the path/flags needed by this snapshot;
    // its destructor releases the SDK allocations on this stack frame.
    API_ProjectInfo project = {};
    context.projectError = ACAPI_ProjectOperation_Project (&project);
    if (context.projectError == NoError) {
        context.savedSolo = project.projectPath != nullptr && !project.untitled && !project.teamwork;
        if (project.projectPath != nullptr) {
            context.projectPath = *project.projectPath;
            context.projectPathPresent = true;
            context.fileScopeKey = FixtureCurrentScope (project);
        }
    }
    context.windowError = ACAPI_Window_GetCurrentWindow (&context.window);
    context.databaseError = ACAPI_Database_GetCurrentDatabase (&context.database);

    API_StoryInfo storyInfo = {};
    const GS::OnExit storyInfoGuard ([&storyInfo] () {
        if (storyInfo.data != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&storyInfo.data));
    });
    context.storyError = ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
    if (context.storyError == NoError)
        context.activeStory = static_cast<Int32> (storyInfo.actStory);
}

void ReadNativeDimensionAuditContext (RadialDefaultsContext& context)
{
	ReadRadialDefaultsContext (context);
	context.drawingScaleError = TAPIR_Drawing_GetDrawingScale (&context.drawingScale);
}

GS::ObjectState RadialDefaultsContextDetails (const RadialDefaultsContext& context)
{
    GS::ObjectState project;
    project.Add ("status", context.projectError == NoError ? "available" : "unavailable");
    project.Add ("errorCode", static_cast<Int32> (context.projectError));
    if (context.projectError == NoError) {
        project.Add ("savedSolo", context.savedSolo);
        if (context.fileScopeKey.HasValue ())
            project.Add ("fileScopeKey", context.fileScopeKey.Get ());
    }

    GS::ObjectState window;
    window.Add ("status", context.windowError == NoError ? "available" : "unavailable");
    window.Add ("errorCode", static_cast<Int32> (context.windowError));
    if (context.windowError == NoError) {
        window.Add ("typeId", static_cast<Int32> (context.window.typeID));
        window.Add ("isFloorPlan", context.window.typeID == APIWind_FloorPlanID);
    }

    GS::ObjectState database;
    database.Add ("status", context.databaseError == NoError ? "available" : "unavailable");
    database.Add ("errorCode", static_cast<Int32> (context.databaseError));
    if (context.databaseError == NoError) {
        database.Add ("typeId", static_cast<Int32> (context.database.typeID));
        database.Add ("isFloorPlan", context.database.typeID == APIWind_FloorPlanID);
        database.Add ("databaseId", APIGuidToString (context.database.databaseUnId.elemSetId));
    }

    GS::ObjectState story;
    story.Add ("status", context.storyError == NoError ? "available" : "unavailable");
    story.Add ("errorCode", static_cast<Int32> (context.storyError));
    if (context.storyError == NoError)
        story.Add ("activeStoryIndex", context.activeStory);

    return GS::ObjectState (
        "project", project,
        "window", window,
        "database", database,
        "story", story);
}

bool RadialDefaultsContextAvailable (const RadialDefaultsContext& context)
{
    return context.projectError == NoError && context.projectPathPresent
        && context.savedSolo && context.fileScopeKey.HasValue ()
        && context.windowError == NoError && context.window.typeID == APIWind_FloorPlanID
        && context.databaseError == NoError && context.database.typeID == APIWind_FloorPlanID
        && context.storyError == NoError;
}

bool RadialDefaultsContextFactsAvailable (const RadialDefaultsContext& context)
{
    return context.projectError == NoError && context.windowError == NoError
        && context.databaseError == NoError && context.storyError == NoError;
}

bool SameRadialDefaultsContext (const RadialDefaultsContext& before, const RadialDefaultsContext& after)
{
    return RadialDefaultsContextAvailable (before) && RadialDefaultsContextAvailable (after)
        && before.fileScopeKey.Get () == after.fileScopeKey.Get ()
        && before.window.typeID == after.window.typeID
        && before.database.typeID == after.database.typeID
        && before.database.databaseUnId.elemSetId == after.database.databaseUnId.elemSetId
        && before.activeStory == after.activeStory;
}

bool IsExactTaskOwnedRadialDefaultsCopy (
    const GS::UniString& runId,
    const GS::UniString& suppliedProjectPath,
    const GS::UniString& requestedFileScopeKey,
    const RadialDefaultsContext& context)
{
    if (!RadialDefaultsContextAvailable (context) || context.fileScopeKey.Get () != requestedFileScopeKey)
        return false;
    const std::string runIdUtf8 = FixtureUtf8 (runId);
    if (runIdUtf8.empty () || runIdUtf8.size () > 48
        || !std::all_of (runIdUtf8.begin (), runIdUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; }))
        return false;

    char localAppData[MAX_PATH + 1] = {};
    const DWORD localAppDataLength = GetEnvironmentVariableA ("LOCALAPPDATA", localAppData, MAX_PATH);
    if (localAppDataLength == 0 || localAppDataLength >= MAX_PATH)
        return false;
#if TAPIR_AC_VERSION == 25
    constexpr const char* ArchicadMajor = "25";
#else
    constexpr const char* ArchicadMajor = "28";
#endif
    const std::string expectedCopy = std::string (localAppData) + "\\ArchicadTapirMcp\\runner\\scratch\\AC" + ArchicadMajor + "\\mutation-" + runIdUtf8 + ".pln";
    const std::string suppliedPath = FixtureUtf8 (suppliedProjectPath);
    const std::string openedPath = FixtureUtf8 (context.projectPath);
    std::string expectedCanonical, expectedFull, suppliedCanonical, openedCanonical;
    if (!FixtureCanonicalExistingPath (expectedCopy, expectedCanonical) || !FixtureFullPath (expectedCopy, expectedFull)
        || !FixtureCanonicalExistingPath (suppliedPath, suppliedCanonical) || !FixtureCanonicalExistingPath (openedPath, openedCanonical))
        return false;
    return FixturePathKey (expectedCanonical) == FixturePathKey (expectedFull)
        && FixturePathKey (suppliedPath) == FixturePathKey (expectedCopy)
        && FixturePathKey (openedPath) == FixturePathKey (expectedCopy)
        && FixturePathKey (suppliedCanonical) == FixturePathKey (expectedCanonical)
        && FixturePathKey (openedCanonical) == FixturePathKey (expectedCanonical);
}

bool ReadNativeDimensionCreateProcessIdentity (std::string& processId, std::string& startTicks)
{
#if defined (_WIN32)
	FILETIME creation = {}, exitTime = {}, kernel = {}, user = {};
	if (!GetProcessTimes (GetCurrentProcess (), &creation, &exitTime, &kernel, &user)) return false;
	ULARGE_INTEGER fileTime = {};
	fileTime.LowPart = creation.dwLowDateTime;
	fileTime.HighPart = creation.dwHighDateTime;
	constexpr UInt64 DotNetTicksAtFileTimeEpoch = 504911232000000000ULL;
	if (fileTime.QuadPart == 0 || fileTime.QuadPart > std::numeric_limits<UInt64>::max () - DotNetTicksAtFileTimeEpoch) return false;
	processId = std::to_string (GetCurrentProcessId ());
	startTicks = std::to_string (fileTime.QuadPart + DotNetTicksAtFileTimeEpoch);
	return !processId.empty () && processId != "0";
#else
	(void) processId;
	(void) startTicks;
	return false;
#endif
}

bool CaptureNativeDimensionCreateContext (
	const GS::UniString& runId,
	const GS::UniString& expectedProjectPath,
	const GS::UniString& fileScopeKey,
	NativeDimensionCreateWire::Context& context,
	GS::ObjectState& facts)
{
	RadialDefaultsContext native;
	ReadRadialDefaultsContext (native);
	const bool taskCopy = IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, native);
	std::string processId, processStartTicks;
	const bool processIdentity = ReadNativeDimensionCreateProcessIdentity (processId, processStartTicks);
	IO::Location addonLocation;
	const GSErrCode addonError = ACAPI_GetOwnLocation (&addonLocation);
	const bool databaseIdentity = native.databaseError == NoError && native.database.typeID == APIWind_FloorPlanID
		&& native.database.databaseUnId.elemSetId != APINULLGuid;
	const bool windowValid = native.windowError == NoError && native.window.typeID == APIWind_FloorPlanID;
	const bool storyValid = native.storyError == NoError;
#if TAPIR_AC_VERSION == 25
	constexpr const char* major = "25";
#else
	constexpr const char* major = "28";
#endif
	if (native.projectError == NoError && native.projectPathPresent) context.projectPath = FixtureUtf8 (native.projectPath);
	if (native.fileScopeKey.HasValue ()) context.fileScopeKey = FixtureUtf8 (native.fileScopeKey.Get ());
	context.kind = "nativeDimensionCreateCandidateContext";
	context.schemaVersion = "native-dimension-create-context/v1";
	context.runId = FixtureUtf8 (runId);
	context.processId = processId;
	context.processStartUtcTicks = processStartTicks;
	context.processStartEpoch = "dotnet-ticks-utc-v1";
	context.archicadMajor = major;
	context.buildId = TAPIR_BUILD_ID;
	context.addonLocation = addonError == NoError ? FixtureUtf8 (addonLocation.ToDisplayText ()) : std::string ();
	context.windowType = windowValid ? "FloorPlan" : "unavailable";
	context.windowIndex = windowValid ? std::to_string (native.window.index) : "-1";
	context.windowDatabaseGuid = windowValid
		? (native.window.databaseUnId.elemSetId == APINULLGuid ? "null" : FixtureUtf8 (APIGuidToString (native.window.databaseUnId.elemSetId)))
		: "null";
	context.currentDatabaseGuid = databaseIdentity ? FixtureUtf8 (APIGuidToString (native.database.databaseUnId.elemSetId)) : "00000000-0000-0000-0000-000000000000";
	context.storyIndex = storyValid ? std::to_string (native.activeStory) : "-1";
	context.contractVersion = "native-crud-v2";
	context.recipe = NativeDimensionCreateWire::Recipe;
	context.policyVersion = "native-dimension-create-policy/v1";
	context.candidateEnabled = TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE ? "true" : "false";
	context.postCreateFaultEnabled = TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT ? "true" : "false";
	facts = GS::ObjectState (
		"projectErrorCode", static_cast<Int32> (native.projectError),
		"windowErrorCode", static_cast<Int32> (native.windowError),
		"databaseErrorCode", static_cast<Int32> (native.databaseError),
		"storyErrorCode", static_cast<Int32> (native.storyError),
		"savedSolo", native.savedSolo,
		"taskOwnedCopyExact", taskCopy,
		"processIdentityAvailable", processIdentity,
		"addonLocationErrorCode", static_cast<Int32> (addonError),
		"window", NativeDimensionAuditWindowIdentityFacts (native),
		"database", NativeDimensionCreateWire::EmitObservedDatabaseIdentityObjectState (
			native.databaseError == NoError ? static_cast<Int32> (native.database.typeID) : -1, context.currentDatabaseGuid),
		"storyIndex", native.activeStory);
	return taskCopy && processIdentity && addonError == NoError && databaseIdentity && windowValid && storyValid
		&& native.savedSolo
		&& context.windowIndex != "-1" && !context.fileScopeKey.empty () && !context.projectPath.empty ();
}

GS::ObjectState RadialDefaultsResponse (
    const char* status,
    const GS::UniString& runId,
    const GS::UniString& requestedFileScopeKey,
    const Int32 requestedStory,
    const GS::ObjectState& contextBefore,
    const GS::ObjectState& contextAfter,
    const GS::ObjectState& defaults,
    const GS::ObjectState& error)
{
    return GS::ObjectState (
        "schemaVersion", "native-dimension-radial-defaults/v1",
        "status", status,
        "meaning", "sdk-defaults-only-not-existing-element-facts-source-proof-or-create-validity",
        "request", GS::ObjectState ("runId", runId, "fileScopeKey", requestedFileScopeKey, "storyIndex", requestedStory),
        "contextBefore", contextBefore,
        "contextAfter", contextAfter,
        "defaults", defaults,
        "error", error);
}

GS::ObjectState RadialDefaultsError (const char* code, const GS::UniString& message, const Int32 nativeErrorCode = 0)
{
    return GS::ObjectState ("status", "present", "code", code, "message", message, "nativeErrorCode", nativeErrorCode);
}

GS::ObjectState RadialDefaultsNoError ()
{
    return GS::ObjectState ("status", "none");
}

GS::ObjectState RadialDefaultsNotRequested ()
{
    return GS::ObjectState ("status", "not-requested");
}

GS::ObjectState RadialDefaultsNotCheckedContext ()
{
    return GS::ObjectState ("status", "not-checked-before-defaults-call");
}

GS::ObjectState RadialDefaultsDiscardedContextChange ()
{
    return GS::ObjectState ("status", "discarded-after-context-change");
}

GS::ObjectState RadialDefaultsFixedBuffer (const char* buffer, const std::size_t capacity)
{
    std::size_t byteLength = 0;
    while (byteLength < capacity && buffer[byteLength] != '\0') ++byteLength;
    const std::string bytes (buffer, byteLength);
    return GS::ObjectState (
        "status", byteLength < capacity ? "bounded-nul-terminated-fixed-buffer" : "bounded-buffer-without-terminator",
        "byteLength", static_cast<Int32> (byteLength),
        "bufferCapacity", static_cast<Int32> (capacity),
        "nulTerminated", byteLength < capacity,
        "bytesHex", GS::UniString (FixtureByteHex (bytes).c_str (), CC_UTF8),
        "encoding", "raw-bytes-encoding-unproven");
}

GS::ObjectState RadialDefaultsNoteDetails (const API_NoteType& note)
{
    return GS::ObjectState (
        "noteUnicodePointer", GS::ObjectState (
            "status", note.contentUStr == nullptr ? "absent" : "present-access-not-attempted",
            "access", "unavailable-pointer-never-dereferenced-or-freed"),
        "fixedContent", RadialDefaultsFixedBuffer (note.content, sizeof (note.content)),
        "pen", static_cast<Int32> (note.notePen),
        "font", static_cast<Int32> (note.noteFont),
        "layerIndex", DimensionAttributeIndex (note.noteLayer),
        "textHeight", note.noteSize,
        "textHeightUnit", "millimeter-paper-size",
        "angle", note.noteAngle,
        "angleUnit", "radian",
        "position", DimensionPoint2D (note.pos),
        "contentType", static_cast<Int32> (note.contentType),
        "fixPos", note.fixPos,
        "styleBits", static_cast<Int32> (note.faceBits),
        "opaque", note.opaque,
        "frame", note.frame,
        "backgroundPen", static_cast<Int32> (note.backgroundPen),
        "framePen", static_cast<Int32> (note.framePen),
        "useLeaderLine", note.useLeaderLine,
        "anchorAtTextEnd", note.anchorAtTextEnd,
        "leaderBegin", DimensionPoint2D (note.begC),
        "leaderMiddle", DimensionPoint2D (note.midC),
        "leaderEnd", DimensionPoint2D (note.endC),
        "leaderPen", static_cast<Int32> (note.leaderPen),
        "leaderLineType", DimensionAttributeIndex (note.leaderLineType),
        "leaderShape", static_cast<Int32> (note.leaderShape),
        "leaderArrowType", static_cast<Int32> (note.arrowData.arrowType),
        "leaderArrowAtBegin", note.arrowData.begArrow,
        "leaderArrowAtEnd", note.arrowData.endArrow,
        "leaderArrowPen", static_cast<Int32> (note.arrowData.arrowPen),
        "leaderArrowSize", note.arrowData.arrowSize,
        "anchorPoint", static_cast<Int32> (note.anchorPoint),
        "justification", static_cast<Int32> (note.just),
        "widthFactor", note.widthFactor,
        "spacingFactor", note.spacingFactor,
        "rowAnchor", static_cast<Int32> (note.rowAnchor),
        "effectBits", static_cast<Int32> (note.effectBits));
}

GS::ObjectState RadialDefaultsMemoDetails (const API_ElementMemo& memo)
{
    const bool fieldPresent = memo.dimElems != nullptr;
    const bool handlePresent = fieldPresent && *memo.dimElems != nullptr;
    GS::ObjectState dimensionElements;
    dimensionElements.Add ("status", !fieldPresent ? "handle-field-absent" : handlePresent ? "handle-present" : "handle-null");
    dimensionElements.Add ("applicability", "known-API_ElementMemo-dimElems-field-for-dimension-records");
    if (handlePresent) {
        const GSSize byteLength = BMhGetSize (reinterpret_cast<GSHandle> (memo.dimElems));
        dimensionElements.Add ("byteLength", static_cast<Int64> (byteLength));
        dimensionElements.Add ("entrySize", static_cast<Int32> (sizeof (API_DimElem)));
        dimensionElements.Add ("byteLengthStatus", byteLength <= 16 * 1024 * 1024 ? "bounded-size-read" : "present-over-16MiB-diagnostic-limit");
    }
    return GS::ObjectState (
        "status", "returned-sdk-memo-inspected-only-for-known-dimension-handle",
        "ownership", "sdk-owned-handles-disposed-once-by-NativeMemoGuard",
        "handles", GS::ObjectState ("dimElems", dimensionElements));
}

GS::ObjectState RadialDefaultsFacts (const API_Element& element, const API_ElementMemo& memo)
{
    const API_RadialDimensionType& dimension = element.radialDimension;
    GS::ObjectState geometry (
        "base", RadialDefaultsBaseDetails (dimension.base),
        "location", DimensionPoint2D (dimension.base.loc),
        "location3D", DimensionPoint3D (dimension.base.loc3D),
        "endPosition", DimensionPoint2D (dimension.endC),
        "measuredRadius", dimension.dimVal,
        "measuredRadiusUnit", "project-meter",
        "measurementSource", "default-API_RadialDimensionType.dimVal",
        "linePen", static_cast<Int32> (dimension.linPen),
        "textPosition", static_cast<Int32> (dimension.textPos),
        "textDirection", static_cast<Int32> (dimension.textWay),
        "showOrigin", dimension.showOrigo,
        "onlyDimensionText", dimension.onlyDimensionText,
        "marker", DimensionMarkerDetails (dimension.markerData),
        "prefixFixedBuffer", RadialDefaultsFixedBuffer (dimension.prefix, sizeof (dimension.prefix)),
        "prefixUnicodePointer", GS::ObjectState (
            "status", dimension.prefixUStr == nullptr ? "absent" : "present-access-not-attempted",
            "access", "unavailable-pointer-never-dereferenced-or-freed"),
        "note", RadialDefaultsNoteDetails (dimension.note));
    return GS::ObjectState (
        "status", "available",
        "elementType", "RadialDimension",
        "nativeElementTypeId", static_cast<Int32> (API_RadialDimensionID),
        "geometryAndSettings", geometry,
        "memo", RadialDefaultsMemoDetails (memo));
}

constexpr std::size_t MaxNativeArcHotspots = 32;
constexpr auto NativeArcHotspotsBudget = std::chrono::milliseconds (5000);
constexpr double NativeArcHotspotCoordinateTolerance = 1.0e-8;

struct ArcHotspotHostSnapshot {
    GSErrCode error = APIERR_GENERAL;
    API_Guid requestedGuid = APINULLGuid;
    API_Guid returnedGuid = APINULLGuid;
    API_ElemTypeID type = API_ZombieElemID;
    short storyIndex = -1;
    UInt64 modificationStamp = 0;
    API_Coord center = {};
    double radius = 0.0;
    double axisAngle = 0.0;
    double axisRatio = 0.0;
    double beginAngle = 0.0;
    double endAngle = 0.0;
    bool reflected = false;
    bool whole = false;
};

bool IsFiniteArcHost (const ArcHotspotHostSnapshot& host)
{
    return std::isfinite (host.center.x) && std::isfinite (host.center.y)
        && std::isfinite (host.radius) && std::isfinite (host.axisAngle)
        && std::isfinite (host.axisRatio) && std::isfinite (host.beginAngle)
        && std::isfinite (host.endAngle);
}

ArcHotspotHostSnapshot ReadArcHotspotHost (const API_Guid& guid)
{
    ArcHotspotHostSnapshot snapshot;
    snapshot.requestedGuid = guid;
    API_Element element = {};
    element.header.guid = guid;
    snapshot.error = ACAPI_Element_Get (&element);
    if (snapshot.error != NoError)
        return snapshot;

    snapshot.returnedGuid = element.header.guid;
    snapshot.type = GetElemTypeId (element.header);
    snapshot.storyIndex = element.header.floorInd;
    snapshot.modificationStamp = element.header.modiStamp;
    if (snapshot.returnedGuid == guid && snapshot.type == API_ArcID) {
        const API_ArcType& arc = element.arc;
        snapshot.center = arc.origC;
        snapshot.radius = arc.r;
        snapshot.axisAngle = arc.angle;
        snapshot.axisRatio = arc.ratio;
        snapshot.beginAngle = arc.begAng;
        snapshot.endAngle = arc.endAng;
        snapshot.reflected = arc.reflected;
        snapshot.whole = arc.whole;
    }
    return snapshot;
}

bool SameArcHotspotHost (const ArcHotspotHostSnapshot& before, const ArcHotspotHostSnapshot& after)
{
    return before.error == NoError && after.error == NoError
        && before.returnedGuid == before.requestedGuid && after.returnedGuid == after.requestedGuid
        && before.returnedGuid == after.returnedGuid && before.type == API_ArcID && after.type == API_ArcID
        && before.storyIndex == after.storyIndex
        && before.storyIndex == after.storyIndex && before.modificationStamp == after.modificationStamp
        && before.center.x == after.center.x && before.center.y == after.center.y
        && before.radius == after.radius && before.axisAngle == after.axisAngle
        && before.axisRatio == after.axisRatio && before.beginAngle == after.beginAngle
        && before.endAngle == after.endAngle && before.reflected == after.reflected && before.whole == after.whole
        && IsFiniteArcHost (before) && IsFiniteArcHost (after);
}

GS::ObjectState ArcHotspotRecheckConsistency (
    const ArcHotspotHostSnapshot& before,
    const ArcHotspotHostSnapshot& after,
    const bool hostStable,
    const bool contextStable,
    const bool completeObservation)
{
    const bool stampsAvailable = before.error == NoError && after.error == NoError;
    if (!stampsAvailable) {
        return GS::ObjectState (
            "status", "changed-or-unverified",
            "hostStable", hostStable,
            "contextStable", contextStable);
    }

    return GS::ObjectState (
        "status", completeObservation ? "stable" : "changed-or-incomplete",
        "hostStable", hostStable,
        "contextStable", contextStable,
        "initialModificationStamp", GS::UniString (std::to_string (before.modificationStamp).c_str (), CC_UTF8),
        "finalModificationStamp", GS::UniString (std::to_string (after.modificationStamp).c_str (), CC_UTF8));
}

GS::ObjectState ArcHotspotHostDetails (const ArcHotspotHostSnapshot& host)
{
    if (host.error != NoError)
        return GS::ObjectState (
            "status", "unavailable",
            "requestedGuid", CreateGuidObjectState (host.requestedGuid),
            "nativeErrorCode", static_cast<Int32> (host.error));

    GS::ObjectState details (
        "status", host.type == API_ArcID && host.returnedGuid == host.requestedGuid ? "available" : "wrong-type-or-identity",
        "requestedGuid", CreateGuidObjectState (host.requestedGuid),
        "elementId", CreateGuidObjectState (host.returnedGuid),
        "nativeElementTypeId", static_cast<Int32> (host.type),
        "storyIndex", static_cast<Int32> (host.storyIndex),
        "modificationStamp", GS::UniString (std::to_string (host.modificationStamp).c_str (), CC_UTF8));
    if (host.type == API_ArcID && host.returnedGuid == host.requestedGuid && IsFiniteArcHost (host)) {
        details.Add ("geometry", GS::ObjectState (
            "center", DimensionPoint2D (host.center),
            "radius", host.radius,
            "radiusUnit", "project-meter",
            "axisAngle", host.axisAngle,
            "axisAngleUnit", "radian",
            "axisRatio", host.axisRatio,
            "beginAngle", host.beginAngle,
            "endAngle", host.endAngle,
            "angleUnit", "radian",
            "reflected", host.reflected,
            "whole", host.whole));
    } else {
        details.Add ("geometryStatus", "unavailable-or-nonfinite");
    }
    return details;
}

bool IsFiniteHotspotCoord (const API_Coord3D& coordinate)
{
    return std::isfinite (coordinate.x) && std::isfinite (coordinate.y) && std::isfinite (coordinate.z);
}

GS::ObjectState ArcHotspotNeigDetails (const API_Neig& neig)
{
    return GS::ObjectState (
        "neigId", static_cast<Int32> (neig.neigID),
        "guid", CreateGuidObjectState (neig.guid),
        "inIndex", neig.inIndex,
        "flags", neig.flags,
        "elementPartType", static_cast<Int32> (neig.elemPartType),
        "elementPartIndex", static_cast<Int64> (neig.elemPartIndex),
        "subtype", static_cast<Int32> (neig.subType));
}

GS::ObjectState ArcHotspotError (const char* code, const GS::UniString& message, const Int32 nativeErrorCode = 0)
{
    return GS::ObjectState ("status", "present", "code", code, "message", message, "nativeErrorCode", nativeErrorCode);
}

GS::ObjectState ArcHotspotNoError ()
{
    return GS::ObjectState ("status", "none");
}

GS::ObjectState ArcHotspotResponse (
    const char* status,
    const GS::UniString& runId,
    const GS::UniString& requestedProjectPath,
    const GS::UniString& requestedFileScopeKey,
    const Int32 requestedStory,
    const API_Guid& hostGuid,
    const GS::ObjectState& contextBefore,
    const GS::ObjectState& contextAfter,
    const GS::ObjectState& hostBefore,
    const GS::ObjectState& hostAfter,
    const GS::ObjectState& readConsistency,
    const Int64 returnedCount,
    const bool complete,
    const bool conversionComplete,
    const char* coverageReason,
    const GS::Array<GS::ObjectState>& hotspots,
    const GS::ObjectState& error)
{
    return GS::ObjectState (
        "schemaVersion", "native-dimension-arc-hotspots/v1",
        "status", status,
        "meaning", "returned-Arc-hotspots-and-coordinate-conversion-only-not-dimension-anchor-or-create-proof",
        "request", GS::ObjectState (
            "runId", runId,
            "expectedProjectPath", requestedProjectPath,
            "fileScopeKey", requestedFileScopeKey,
            "storyIndex", requestedStory,
            "hostGuid", APIGuidToString (hostGuid)),
        "contextBefore", contextBefore,
        "contextAfter", contextAfter,
        "hostBefore", hostBefore,
        "hostAfter", hostAfter,
        "readConsistency", readConsistency,
        "hotspotCount", returnedCount,
        "complete", complete,
        "coverage", GS::ObjectState (
            "status", complete ? "complete" : "partial",
            "reason", coverageReason,
            "coordinateTolerance", NativeArcHotspotCoordinateTolerance,
            "coordinateToleranceUnit", "project-meter"),
        "conversionCoverage", GS::ObjectState (
            "status", conversionComplete ? "complete" : "partial",
            "meaning", "per-returned-neig-conversion-attempts-are-reported-independently"),
        "hotspots", hotspots,
        "error", error);
}

bool ArcHotspotStopped (GS::ProcessControl& processControl, const std::chrono::steady_clock::time_point& started, bool& cancelled)
{
    cancelled = processControl.TestBreak ();
    return cancelled || std::chrono::steady_clock::now () - started >= NativeArcHotspotsBudget;
}

GS::ObjectState ArcHotspotStoppedError (const bool cancelled)
{
    return cancelled
        ? ArcHotspotError ("COOPERATIVE_CANCELLED", "The read-only Arc hotspot diagnostic stopped at a cooperative cancellation point.")
        : ArcHotspotError ("COOPERATIVE_DEADLINE_REACHED", "The read-only Arc hotspot diagnostic reached its cooperative time budget between SDK calls.");
}

} // namespace

GetNativeDimensionAuditCaptureCommand::GetNativeDimensionAuditCaptureCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionAuditCaptureCommand::GetName () const { return "GetNativeDimensionAuditCapture"; }
GS::Optional<GS::UniString> GetNativeDimensionAuditCaptureCommand::GetInputParametersSchema () const
{
	return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-audit-capture-request/v1","native-dimension-audit-scope-request/v2"]},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"expectedDatabaseGuid":{"type":"string","format":"uuid"},"storyIndex":{"type":"integer","minimum":-32768,"maximum":32767},"bounds":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"}},"required":["xMin","yMin","xMax","yMax"],"additionalProperties":false}},"required":["schemaVersion","expectedProjectPath","fileScopeKey","storyIndex","bounds"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionAuditCaptureCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionAuditCaptureCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
#if TAPIR_AC_VERSION == 25
	constexpr Int32 NativeDimensionAuditArchicadMajor = 25;
#else
	constexpr Int32 NativeDimensionAuditArchicadMajor = 28;
#endif
	GS::UniString schemaVersion, expectedProjectPath, fileScopeKey, databaseGuidText;
	bool scopeRequestV2 = false;
	Int32 requestedStory = 0;
	GS::ObjectState requestedBoundsState;
	GS::ObjectState request;
	GS::ObjectState scopeRequest;
	API_Guid requestedDatabaseGuid = APINULLGuid;
	NativeDimensionAuditPolicy::Bounds2D requestedBounds = {};
	if (!parameters.Get ("schemaVersion", schemaVersion)) {
		return GS::ObjectState ("schemaVersion", "native-dimension-audit-capture/v1", "status", "refused", "reason", "invalid-closed-request");
	}
	scopeRequestV2 = schemaVersion == "native-dimension-audit-scope-request/v2";
	const bool requestV1 = schemaVersion == "native-dimension-audit-capture-request/v1";
	const UInt32 expectedFieldCount = requestV1 ? 6 : 5;
	if ((!requestV1 && !scopeRequestV2) || parameters.GetFieldNames ().GetSize () != expectedFieldCount
		|| !parameters.Get ("expectedProjectPath", expectedProjectPath) || expectedProjectPath.IsEmpty ()
		|| !parameters.Get ("fileScopeKey", fileScopeKey) || fileScopeKey.IsEmpty ()
		|| (requestV1 && !parameters.Get ("expectedDatabaseGuid", databaseGuidText))
		|| !parameters.Get ("storyIndex", requestedStory)
		|| !parameters.Get ("bounds", requestedBoundsState)
		|| requestedBoundsState.GetFieldNames ().GetSize () != 4
		|| !requestedBoundsState.Get ("xMin", requestedBounds.xMin)
		|| !requestedBoundsState.Get ("yMin", requestedBounds.yMin)
		|| !requestedBoundsState.Get ("xMax", requestedBounds.xMax)
		|| !requestedBoundsState.Get ("yMax", requestedBounds.yMax)
		|| !NativeDimensionAuditPolicy::IsFiniteOrderedBounds (requestedBounds)) {
		return GS::ObjectState ("schemaVersion", "native-dimension-audit-capture/v1", "status", "refused", "reason", "invalid-closed-request");
	}
	scopeRequest = GS::ObjectState (
		"schemaVersion", "native-dimension-audit-scope-request/v2",
		"expectedProjectPath", expectedProjectPath,
		"fileScopeKey", fileScopeKey,
		"storyIndex", requestedStory,
		"bounds", GS::ObjectState ("xMin", requestedBounds.xMin, "yMin", requestedBounds.yMin, "xMax", requestedBounds.xMax, "yMax", requestedBounds.yMax));
	if (requestV1) {
		requestedDatabaseGuid = APIGuidFromString (databaseGuidText.ToCStr ());
		if (requestedDatabaseGuid == APINULLGuid)
			return GS::ObjectState ("schemaVersion", "native-dimension-audit-capture/v1", "status", "refused", "reason", "invalid-database-guid");
		request = GS::ObjectState (
			"schemaVersion", schemaVersion,
			"expectedProjectPath", expectedProjectPath,
			"fileScopeKey", fileScopeKey,
			"expectedDatabaseGuid", APIGuidToString (requestedDatabaseGuid),
			"storyIndex", requestedStory,
			"bounds", GS::ObjectState ("xMin", requestedBounds.xMin, "yMin", requestedBounds.yMin, "xMax", requestedBounds.xMax, "yMax", requestedBounds.yMax));
	}

	const auto started = std::chrono::steady_clock::now ();
	RadialDefaultsContext contextBefore;
	ReadNativeDimensionAuditContext (contextBefore);
	if (scopeRequestV2 && contextBefore.databaseError == NoError)
		requestedDatabaseGuid = contextBefore.database.databaseUnId.elemSetId;
	const bool resolvedDatabaseAvailable = requestedDatabaseGuid != APINULLGuid;
	if (scopeRequestV2 && resolvedDatabaseAvailable) {
		request = GS::ObjectState (
			"schemaVersion", "native-dimension-audit-capture-request/v1",
			"expectedProjectPath", expectedProjectPath,
			"fileScopeKey", fileScopeKey,
			"expectedDatabaseGuid", APIGuidToString (requestedDatabaseGuid),
			"storyIndex", requestedStory,
			"bounds", GS::ObjectState ("xMin", requestedBounds.xMin, "yMin", requestedBounds.yMin, "xMax", requestedBounds.xMax, "yMax", requestedBounds.yMax));
	}
	const GS::ObjectState preferencesBefore = NativeDimensionAuditPreferences ();
	const GS::ObjectState contextBeforeFacts = NativeDimensionAuditContextFacts (contextBefore, preferencesBefore);
	RadialDefaultsContext contextAfter;
	GS::ObjectState preferencesAfter;
	GS::ObjectState contextAfterFacts;
	bool finalContextRead = false;
	bool finalContextFactsAvailable = false;
	auto makeResponse = [&] (const char* status, const char* reason, const GS::ObjectState& facts, const char* factsStatus, const std::uint64_t factBytes) {
		GS::ObjectState budget (
			"rawCandidateLimit", static_cast<Int64> (NativeDimensionAuditPolicy::MaxRawCandidates),
			"matchedParentLimit", static_cast<Int64> (NativeDimensionAuditPolicy::MaxMatchedParents),
			"batchParentLimit", static_cast<Int64> (NativeDimensionAuditPolicy::MaxBatchParents),
			"storedNoteLimit", static_cast<Int64> (NativeDimensionAuditPolicy::MaxStoredNotes),
			"serializedFactByteLimit", static_cast<Int64> (NativeDimensionAuditPolicy::MaxSerializedFactBytes),
			"serializedFactBytes", static_cast<Int64> (factBytes),
			"allocationBound", "SDK-family-arrays-are-materialized-before-post-call-candidate-cap-check",
			"deadline", "30-second-cooperative-between-stage-and-batch-check-not-hard-SDK-call-timeout");
		GS::ObjectState response (
			"schemaVersion", "native-dimension-audit-capture/v1",
			"archicadMajor", NativeDimensionAuditArchicadMajor,
			"status", status,
			"reason", reason,
			"request", request,
			"contextBefore", contextBeforeFacts,
			"factsStatus", factsStatus,
			"budget", budget,
			"coverage", GS::ObjectState (
				"inventory", "complete-only-if-both-four-family-manifests-match",
				"readerFacts", "partial-or-complete-per-row",
				"memo", "preflighted-without-child-truncation",
				"unicodeAccess", "copied-native-reader-coverage-only",
				"sourceFidelity", "unproven",
				"display", "unavailable-unless-independently-observed",
				"noteCorrespondence", "unproven",
				"inverseClosure", "unavailable"));
		if (scopeRequestV2) {
			response = GS::ObjectState (
				"schemaVersion", "native-dimension-audit-capture/v2",
				"archicadMajor", NativeDimensionAuditArchicadMajor,
				"status", status,
				"reason", reason,
				"request", scopeRequest,
				"resolvedDatabaseGuid", APIGuidToString (requestedDatabaseGuid),
				"contextBefore", contextBeforeFacts,
				"windowIdentityBefore", NativeDimensionAuditWindowIdentityFacts (contextBefore),
				"factsStatus", factsStatus,
				"budget", budget,
				"coverage", GS::ObjectState (
					"inventory", "complete-only-if-both-four-family-manifests-match",
					"readerFacts", "partial-or-complete-per-row",
					"memo", "preflighted-without-child-truncation",
					"unicodeAccess", "copied-native-reader-coverage-only",
					"sourceFidelity", "unproven",
					"display", "unavailable-unless-independently-observed",
					"noteCorrespondence", "unproven",
					"inverseClosure", "unavailable"));
			if (resolvedDatabaseAvailable) response.Add ("captureRequest", request);
			if (finalContextRead && finalContextFactsAvailable) {
				response.Add ("contextAfter", contextAfterFacts);
				response.Add ("windowIdentityAfter", NativeDimensionAuditWindowIdentityFacts (contextAfter));
			} else if (factsStatus[0] != 'n') {
				response.Add ("contextAfter", GS::ObjectState ("status", "not-read"));
				response.Add ("windowIdentityAfter", GS::ObjectState ("status", "not-read"));
			}
		}
		if (factsStatus[0] == 'i') response.Add ("facts", facts);
		return response;
	};

	if (scopeRequestV2 && !resolvedDatabaseAvailable)
		return makeResponse ("refused", "scope-database-floorplan-or-story-mismatch-before-inventory", GS::ObjectState (), "not-read", 0);
	if (!NativeDimensionAuditContextMatchesRequest (contextBefore, expectedProjectPath, fileScopeKey, requestedDatabaseGuid, requestedStory))
		return makeResponse ("refused", "scope-database-floorplan-or-story-mismatch-before-inventory", GS::ObjectState (), "not-read", 0);
	if (NativeDimensionAuditExpired (started, processControl))
		return makeResponse (processControl.TestBreak () ? "partial" : "refused", processControl.TestBreak () ? "cooperative-cancelled-before-inventory" : "cooperative-deadline-before-inventory", GS::ObjectState (), "not-read", 0);

	NativeDimensionAuditManifest manifestBefore = ReadNativeDimensionAuditManifest (requestedBounds, requestedStory, started, processControl);
	std::vector<GS::ObjectState> parentRows;
	std::vector<GS::ObjectState> preflightRows;
	std::vector<GS::ObjectState> errors;
	UInt64 estimatedStoredNotes = 0;
	bool notePreflightComplete = manifestBefore.complete;
	const bool parentBudgetOk = NativeDimensionAuditPolicy::WithinMatchedParentLimit (manifestBefore.matchedCount);
	if (!parentBudgetOk) notePreflightComplete = false;
	if (manifestBefore.complete && parentBudgetOk) {
		for (const NativeDimensionAuditCandidate& candidate : manifestBefore.candidates) {
			if (NativeDimensionAuditExpired (started, processControl)) {
				notePreflightComplete = false;
				errors.push_back (GS::ObjectState ("stage", "note-cardinality-preflight", "status", processControl.TestBreak () ? "cancelled" : "deadline-exceeded"));
				break;
			}
			GS::ObjectState row;
			row.Add ("elementId", CreateGuidObjectState (candidate.guid));
			row.Add ("nativeElementTypeId", static_cast<Int32> (candidate.type));
			row.Add ("expectedModificationStamp", static_cast<Int64> (candidate.modificationStamp));
			API_Element element = {};
			element.header.guid = candidate.guid;
			const GSErrCode getError = ACAPI_Element_Get (&element);
			row.Add ("elementGetErrorCode", static_cast<Int32> (getError));
			UInt64 noteCount = 0;
			bool parentCountAvailable = false;
			if (getError == NoError && element.header.guid == candidate.guid && GetElemTypeId (element.header) == candidate.type
				&& element.header.modiStamp == candidate.modificationStamp && static_cast<Int32> (element.header.floorInd) == requestedStory) {
				if (candidate.type == API_DimensionID) {
					const Int32 count = element.dimension.nDimElem;
					if (count >= 0 && count <= 100000) { noteCount = static_cast<UInt64> (count) + 1; parentCountAvailable = true; }
					else notePreflightComplete = false;
				} else if (candidate.type == API_LevelDimensionID) { noteCount = 2; parentCountAvailable = true; }
				else { noteCount = 1; parentCountAvailable = true; }
			} else {
				notePreflightComplete = false;
			}
			row.Add ("status", parentCountAvailable ? "available" : "unavailable");
			if (!parentCountAvailable) {
				row.Add ("reason", "exact-element-count-or-identity-unavailable");
				errors.push_back (GS::ObjectState ("stage", "note-cardinality-preflight", "status", "parent-count-unavailable", "elementId", CreateGuidObjectState (candidate.guid)));
			}
			row.Add ("estimatedStoredNoteCount", static_cast<Int64> (noteCount));
			row.Add ("countBasis", "fixed-family-note-slots-from-exact-element-read-including-default-and-inactive-level-note");
			preflightRows.push_back (row);
			estimatedStoredNotes += noteCount;
			if (!NativeDimensionAuditPolicy::WithinStoredNoteLimit (estimatedStoredNotes)) {
				notePreflightComplete = false;
				errors.push_back (GS::ObjectState ("stage", "note-cardinality-preflight", "status", "stored-note-limit-exceeded-no-parent-memos-read"));
				break;
			}
		}
	}
	const bool mayReadDetails = manifestBefore.complete && notePreflightComplete && parentBudgetOk
		&& NativeDimensionAuditPolicy::WithinStoredNoteLimit (estimatedStoredNotes);
	if (mayReadDetails) {
		for (std::size_t offset = 0; offset < manifestBefore.candidates.size (); offset += NativeDimensionAuditPolicy::MaxBatchParents) {
			if (NativeDimensionAuditExpired (started, processControl)) {
				GS::ObjectState stop ("status", processControl.TestBreak () ? "cancelled" : "deadline-exceeded", "stage", "parent-reader-batch");
				errors.push_back (stop);
				for (std::size_t remaining = offset; remaining < manifestBefore.candidates.size (); ++remaining)
					parentRows.push_back (GS::ObjectState ("status", "not-read", "reason", processControl.TestBreak () ? "cooperative-cancelled-before-parent-reader-batch" : "deadline-exceeded-before-parent-reader-batch", "elementId", CreateGuidObjectState (manifestBefore.candidates[remaining].guid)));
				break;
			}
			GS::Array<GS::UniString> batchGuids;
			const std::size_t end = std::min (manifestBefore.candidates.size (), offset + static_cast<std::size_t> (NativeDimensionAuditPolicy::MaxBatchParents));
			for (std::size_t index = offset; index < end; ++index) batchGuids.Push (manifestBefore.candidates[index].guidText);
			GS::ObjectState readerRequest;
			readerRequest.Add ("guids", batchGuids);
			const GS::ObjectState readerResponse = GetNativeDimensionDetailsCommand ().Execute (readerRequest, processControl);
			GS::Array<GS::ObjectState> batchRows;
			if (!readerResponse.Get ("elements", batchRows) || batchRows.GetSize () != batchGuids.GetSize ()) {
				errors.push_back (GS::ObjectState ("stage", "exact-parent-reader", "status", "batch-response-invalid", "batchOffset", static_cast<Int64> (offset)));
				for (std::size_t index = offset; index < end; ++index)
					parentRows.push_back (GS::ObjectState ("status", "not-read", "reason", "exact-parent-reader-batch-response-invalid", "elementId", CreateGuidObjectState (manifestBefore.candidates[index].guid)));
				continue;
			}
			for (UInt32 index = 0; index < batchRows.GetSize (); ++index) {
				GS::ObjectState row = batchRows[index];
				GS::ObjectState id;
				GS::UniString returnedGuid;
				const bool hasGuid = row.Get ("elementId", id) && id.Get ("guid", returnedGuid);
				if (!hasGuid || NativeDimensionAuditPathKey (returnedGuid) != NativeDimensionAuditPathKey (batchGuids[index])) {
					const NativeDimensionAuditCandidate& expected = manifestBefore.candidates[offset + index];
					errors.push_back (GS::ObjectState (
						"stage", "exact-parent-reader", "status", "returned-guid-mismatch",
						"expectedGuid", batchGuids[index], "returnedRow", row));
					parentRows.push_back (GS::ObjectState (
						"status", "error", "elementType", FixtureElementTypeName (expected.type),
						"elementId", CreateGuidObjectState (expected.guid),
						"errorCode", static_cast<Int32> (APIERR_BADID),
						"error", "The exact reader returned a different GUID; raw mismatched row is preserved in readerCoverage.errors."));
					continue;
				}
				const NativeDimensionAuditCandidate& expected = manifestBefore.candidates[offset + index];
				GS::UniString rowMismatchReason;
				if (!NativeDimensionAuditRowMatchesCandidate (row, expected, requestedDatabaseGuid, static_cast<Int32> (contextBefore.database.typeID), requestedStory, preferencesBefore, rowMismatchReason)) {
					errors.push_back (GS::ObjectState (
						"stage", "exact-parent-reader-correlation", "status", "reader-facts-differ-from-independent-audit-context",
						"expectedGuid", batchGuids[index], "reason", rowMismatchReason, "returnedRow", row));
					parentRows.push_back (GS::ObjectState (
						"status", "error", "elementType", FixtureElementTypeName (expected.type),
						"elementId", CreateGuidObjectState (expected.guid),
						"nativeElementTypeId", static_cast<Int32> (expected.type),
						"errorCode", static_cast<Int32> (APIERR_BADID),
						"error", "The exact reader row did not agree with the independent family, scope, bounds, revision, units, and preferences facts."));
					continue;
				}
				parentRows.push_back (row);
			}
		}
	} else {
		for (const NativeDimensionAuditCandidate& candidate : manifestBefore.candidates)
			parentRows.push_back (GS::ObjectState ("status", "not-read", "reason", "inventory-or-stored-note-preflight-incomplete", "elementId", CreateGuidObjectState (candidate.guid)));
	}

	const bool stoppedBeforeFinal = NativeDimensionAuditExpired (started, processControl);
	NativeDimensionAuditManifest manifestAfter;
	if (!stoppedBeforeFinal) {
		manifestAfter = ReadNativeDimensionAuditManifest (requestedBounds, requestedStory, started, processControl);
	} else {
		manifestAfter.state = GS::ObjectState ("status", "not-read", "reason", processControl.TestBreak () ? "cooperative-cancelled" : "cooperative-deadline-exceeded");
		manifestAfter.complete = false;
	}
	// The final context must bracket the enumeration itself.  Read it only after
	// the final manifest and refuse a coherent result if that last SDK stage has
	// exhausted the cooperative command budget.
	const bool stoppedAfterFinalManifest = NativeDimensionAuditExpired (started, processControl);
	const bool stoppedBeforeFinalContext = stoppedBeforeFinal || stoppedAfterFinalManifest;
	if (!stoppedBeforeFinalContext) {
		ReadNativeDimensionAuditContext (contextAfter);
		preferencesAfter = NativeDimensionAuditPreferences ();
		finalContextRead = true;
	}
	const bool stoppedAtFinal = stoppedBeforeFinalContext || NativeDimensionAuditExpired (started, processControl);
	finalContextFactsAvailable = !stoppedAtFinal && finalContextRead;
	contextAfterFacts = stoppedAtFinal ? GS::ObjectState ("status", "not-read") : NativeDimensionAuditContextFacts (contextAfter, preferencesAfter);
	GS::UniString beforeContextText, afterContextText, beforeManifestText, afterManifestText;
	std::uint64_t ignoredBytes = 0;
	const bool contextSerialized = NativeDimensionAuditFactsText (contextBeforeFacts, beforeContextText, ignoredBytes)
		&& !stoppedAtFinal && NativeDimensionAuditFactsText (contextAfterFacts, afterContextText, ignoredBytes);
	const bool manifestSerialized = NativeDimensionAuditFactsText (manifestBefore.state, beforeManifestText, ignoredBytes)
		&& !stoppedAtFinal && NativeDimensionAuditFactsText (manifestAfter.state, afterManifestText, ignoredBytes);
	const bool contextStable = contextSerialized && beforeContextText == afterContextText
		&& NativeDimensionAuditPreferencesAvailable (preferencesBefore) && NativeDimensionAuditPreferencesAvailable (preferencesAfter)
		&& NativeDimensionAuditContextMatchesRequest (contextAfter, expectedProjectPath, fileScopeKey, requestedDatabaseGuid, requestedStory)
		&& (!scopeRequestV2 || SameNativeDimensionAuditWindowIdentity (contextBefore, contextAfter));
	const bool inventoryStable = manifestSerialized && manifestBefore.complete && manifestAfter.complete
		&& beforeManifestText == afterManifestText;

	GS::Array<GS::ObjectState> parents;
	for (const GS::ObjectState& row : parentRows) parents.Push (row);
	GS::Array<GS::ObjectState> preflight;
	for (const GS::ObjectState& row : preflightRows) preflight.Push (row);
	GS::Array<GS::ObjectState> errorRows;
	for (const GS::ObjectState& row : errors) errorRows.Push (row);
	UInt64 readerErrorCount = 0;
	UInt64 unicodeCompleteCount = 0;
	UInt64 unicodePartialCount = 0;
	UInt64 unicodeNotReadCount = 0;
	for (const GS::ObjectState& row : parentRows) {
		GS::UniString rowStatus;
		if (!row.Get ("status", rowStatus) || rowStatus != "success") {
			readerErrorCount++;
			unicodeNotReadCount++;
			continue;
		}
		GS::ObjectState rowUnicode;
		GS::UniString unicodeStatus;
		if (row.Get ("unicodeAccessCoverage", rowUnicode) && rowUnicode.Get ("status", unicodeStatus) && unicodeStatus == "complete") unicodeCompleteCount++;
		else unicodePartialCount++;
	}
	if (parentRows.size () < manifestBefore.candidates.size ())
		unicodeNotReadCount += manifestBefore.candidates.size () - parentRows.size ();
	const bool allParentRowsCaptured = mayReadDetails && parentRows.size () == manifestBefore.candidates.size () && readerErrorCount == 0;
	const char* unicodeCoverageStatus = unicodePartialCount == 0 && unicodeNotReadCount == 0 ? "complete" : "partial";
	GS::ObjectState facts (
		"contextBefore", contextBeforeFacts,
		"contextAfter", contextAfterFacts,
		"preferencesBefore", preferencesBefore,
		"preferencesAfter", stoppedAtFinal ? GS::ObjectState ("status", "not-read") : preferencesAfter,
		"contextStable", contextStable,
		"inventoryCoverage", GS::ObjectState ("status", inventoryStable ? "complete" : "partial", "before", manifestBefore.state, "after", manifestAfter.state),
		"readerPreflight", GS::ObjectState ("status", notePreflightComplete ? "complete" : "partial", "estimatedStoredNoteCount", static_cast<Int64> (estimatedStoredNotes), "parents", preflight),
		"readerCoverage", GS::ObjectState ("status", allParentRowsCaptured ? "captured" : "partial", "returnedRowCount", static_cast<Int64> (parentRows.size ()), "readerErrorCount", static_cast<Int64> (readerErrorCount), "parents", parents, "errors", errorRows),
		"fieldCoverage", GS::ObjectState ("status", allParentRowsCaptured ? "all-requested-reader-rows-retained" : "partial-or-not-read", "meaning", "T1-versioned-native-reader-fields-preserved-without-grouping-or-inference"),
		"memoCoverage", GS::ObjectState ("status", notePreflightComplete && mayReadDetails ? "preflighted-no-truncation" : "partial-or-not-read", "storedNoteCountLowerBound", static_cast<Int64> (estimatedStoredNotes)),
		"unicodeAccessCoverage", GS::ObjectState (
			"status", unicodeCoverageStatus,
			"meaning", "exact-reader-Unicode-pointer-access-only-original-source-unproven",
			"expectedParentCount", static_cast<Int64> (manifestBefore.matchedCount),
			"completeParentCount", static_cast<Int64> (unicodeCompleteCount),
			"partialParentCount", static_cast<Int64> (unicodePartialCount),
			"notReadParentCount", static_cast<Int64> (unicodeNotReadCount)),
		"sourceFidelity", GS::ObjectState ("status", "unproven", "reason", "unicode-content-and-drawing-observations-do-not-prove-original-source"),
		"displayCoverage", GS::ObjectState ("status", "unavailable-unless-independently-observed"),
		"noteCorrespondence", GS::ObjectState ("status", "unproven"),
		"revisionCoverage", GS::ObjectState ("status", inventoryStable && contextStable ? "stable-manifests-and-project-context" : "partial-or-changed"),
		"counts", GS::ObjectState (
			"rawCandidates", static_cast<Int64> (manifestBefore.rawCandidateCount),
			"matchedParents", static_cast<Int64> (manifestBefore.matchedCount),
			"estimatedStoredNotes", static_cast<Int64> (estimatedStoredNotes),
			"candidatePoolCompleteBeforeRectangle", manifestBefore.complete));
	GS::UniString serializedFacts;
	std::uint64_t serializedFactBytes = 0;
	const bool serialized = NativeDimensionAuditFactsText (facts, serializedFacts, serializedFactBytes);
	const bool factsWithinLimit = serialized && NativeDimensionAuditPolicy::WithinSerializedFactLimit (serializedFactBytes);
	const bool expiredAfterFactSerialization = NativeDimensionAuditExpired (started, processControl);
	const bool complete = !expiredAfterFactSerialization && contextStable && inventoryStable && allParentRowsCaptured
		&& errors.empty () && factsWithinLimit;
	const bool resourceRefusal = !manifestBefore.withinCandidateLimit || !parentBudgetOk
		|| !NativeDimensionAuditPolicy::WithinStoredNoteLimit (estimatedStoredNotes)
		|| (serialized && !factsWithinLimit);
	const char* outcome = complete ? "complete" : resourceRefusal ? "refused" : "partial";
	const char* reason = complete ? "complete-bounded-capture" : expiredAfterFactSerialization ? (processControl.TestBreak () ? "cooperative-cancelled-during-response-finalization" : "cooperative-deadline-during-response-finalization") : resourceRefusal ? "fixed-resource-limit-exceeded-no-successful-prefix" : !contextStable ? "context-or-preferences-changed-or-unavailable" : !inventoryStable ? "initial-final-manifest-incomplete-or-changed" : !mayReadDetails ? "reader-preflight-or-coverage-incomplete" : "reader-or-fact-serialization-incomplete";
	const char* factsStatus = factsWithinLimit ? "included" : serialized ? "omitted-over-16MiB-limit" : "omitted-serialization-failed";
	GS::ObjectState response = makeResponse (outcome, reason, facts, factsStatus, serializedFactBytes);
	GS::UniString serializedResponse;
	std::uint64_t ignoredResponseBytes = 0;
	const bool responseSerialized = NativeDimensionAuditFactsText (response, serializedResponse, ignoredResponseBytes);
	if (responseSerialized && NativeDimensionAuditExpired (started, processControl) && complete)
		response = makeResponse ("partial", processControl.TestBreak () ? "cooperative-cancelled-during-response-finalization" : "cooperative-deadline-during-response-finalization", facts, factsStatus, serializedFactBytes);
	else if (!responseSerialized && complete)
		response = makeResponse ("partial", "response-serialization-unavailable-after-fact-capture", facts, factsStatus, serializedFactBytes);
	return response;
}

namespace {

constexpr const char* NativeDimensionLayerControlSchema = "native-dimension-layer-control/v2";
constexpr const char* NativeDimensionLayerControlPreparationPolicy = "absent-owned-entry-flags-0-or-1-class-1/v2";
constexpr std::uint64_t NativeDimensionLayerControlBudgetSeconds = 30;

struct NativeDimensionLayerEntryFact {
	Int32 layerIndex = 0;
	GS::UniString layerGuid;
	GS::UniString resolution;
	GSErrCode lookupError = NoError;
	Int32 rawFlags = 0;
	Int32 connectionClass = 0;
};

struct NativeDimensionLayerFact {
	GS::UniString guid;
	Int32 index = 0;
	Int32 typeId = static_cast<Int32> (API_LayerID);
	Int32 flags = 0;
	GS::UniString name;
	Int32 connectionClass = 0;
	UInt64 modificationTime = 0;
};

struct NativeDimensionLayerCombinationFact {
	GS::UniString guid;
	Int32 index = 0;
	Int32 typeId = static_cast<Int32> (API_LayerCombID);
	Int32 flags = 0;
	GS::UniString name;
	UInt64 modificationTime = 0;
	Int32 declaredLayerCount = 0;
	bool inEffect = false;
	std::vector<NativeDimensionLayerEntryFact> entries;
};

struct NativeDimensionLayerSlotFact {
	Int32 index = 0;
	NativeDimensionLayerControlPolicy::SlotState state = NativeDimensionLayerControlPolicy::SlotState::Live;
	GSErrCode errorCode = NoError;
	NativeDimensionLayerFact layer;
};

struct NativeDimensionLayerCombinationSlotFact {
	Int32 index = 0;
	NativeDimensionLayerControlPolicy::SlotState state = NativeDimensionLayerControlPolicy::SlotState::Live;
	GSErrCode errorCode = NoError;
	NativeDimensionLayerCombinationFact combination;
};

struct NativeDimensionLayerInventory {
	GS::ObjectState facts;
	std::vector<NativeDimensionLayerFact> layers;
	std::vector<NativeDimensionLayerSlotFact> layerSlots;
	std::vector<NativeDimensionLayerCombinationFact> combinations;
	std::vector<NativeDimensionLayerCombinationSlotFact> combinationSlots;
	UInt64 layerReportedCount = 0;
	UInt64 combinationReportedCount = 0;
	UInt64 layerMaximumIndex = 0;
	UInt64 combinationMaximumIndex = 0;
	GS::UniString countSemantics;
	GS::UniString serialized;
	UInt64 serializedBytes = 0;
	GSErrCode currentCombinationError = APIERR_GENERAL;
	Int32 currentCombinationIndex = -1;
	bool complete = false;
	GS::UniString error;
};

struct NativeDimensionLayerControlRun {
	bool active = false;
	GS::UniString preparationPolicy;
	NativeDimensionLayerControlPolicy::Phase phase = NativeDimensionLayerControlPolicy::Phase::Empty;
	GS::UniString runId;
	std::string runIdUtf8;
	GS::UniString runRef;
	GS::UniString nextNonce;
	GS::UniString projectPath;
	GS::UniString fileScopeKey;
	API_Guid databaseGuid = APINULLGuid;
	Int32 storyIndex = -1;
	UInt32 processId = 0;
	GS::UniString layerName;
	API_Guid layerGuid = APINULLGuid;
	API_AttributeIndex layerIndex = APIInvalidAttributeIndex;
	API_Guid parentGuid = APINULLGuid;
	UInt64 parentModificationStamp = 0;
	std::string initialLayerFacts;
	std::string initialCombinationFacts;
	std::string preparedCombinationFacts;
	std::string preparedOtherLayerFacts;
	std::vector<NativeDimensionLayerFact> initialLayers;
	std::vector<NativeDimensionLayerSlotFact> initialLayerSlots;
	std::vector<NativeDimensionLayerCombinationFact> initialCombinations;
	std::vector<NativeDimensionLayerCombinationSlotFact> initialCombinationSlots;
	GSErrCode initialCurrentCombinationError = APIERR_GENERAL;
	Int32 initialCurrentCombinationIndex = -1;
	std::vector<NativeDimensionLayerFact> preparedLayers;
	std::vector<NativeDimensionLayerSlotFact> preparedLayerSlots;
	std::vector<NativeDimensionLayerCombinationFact> preparedCombinations;
	std::vector<NativeDimensionLayerCombinationSlotFact> preparedCombinationSlots;
	GSErrCode preparedCurrentCombinationError = APIERR_GENERAL;
	Int32 preparedCurrentCombinationIndex = -1;
	std::string preparedParentFacts;
	std::string initialFocusFacts;
	GS::ObjectState initialFocus;
	GS::ObjectState beginFacts;
	GS::ObjectState prepareFacts;
	std::chrono::steady_clock::time_point started;
};

struct NativeDimensionLayerFocusSnapshot {
	GS::ObjectState facts;
	std::string serialized;
	bool complete = false;
};

NativeDimensionLayerFocusSnapshot ReadNativeDimensionLayerFocus ()
{
	NativeDimensionLayerFocusSnapshot snapshot;
	API_SelectionInfo selection = {};
	GS::Array<API_Neig> neigs;
	const GSErrCode error = ACAPI_Selection_Get (&selection, &neigs, false);
	const GS::OnExit selectionGuard ([&selection] () { ReleaseSelectionInfoHandles (selection); });
	if (error != NoError && error != APIERR_NOSEL) {
		snapshot.facts = GS::ObjectState ("status", "unavailable", "errorCode", static_cast<Int32> (error));
		return snapshot;
	}
	const bool empty = error == APIERR_NOSEL;
	const Int32 returnedSelectionCount = empty ? 0 : neigs.GetSize ();
	if (selection.sel_nElem < 0 || selection.sel_nElem != returnedSelectionCount) {
		snapshot.facts = GS::ObjectState ("status", "unavailable", "reason", "selection-count-does-not-match-exact-neig-list",
			"selectionErrorCode", static_cast<Int32> (error), "reportedCount", selection.sel_nElem,
			"returnedNeigCount", returnedSelectionCount);
		return snapshot;
	}
	GS::ObjectState focus ("status", "available", "selectionType", static_cast<Int32> (empty ? API_SelEmpty : selection.typeID),
		"selectedElementCount", returnedSelectionCount, "selectionErrorCode", static_cast<Int32> (error));
	const auto& selected = focus.AddList<GS::ObjectState> ("selectedElements");
	if (!empty) {
		if (neigs.GetSize () > 4096) {
			snapshot.facts = GS::ObjectState ("status", "refused", "reason", "selection-element-budget-exceeded", "selectedElementCount", static_cast<Int32> (neigs.GetSize ()));
			return snapshot;
		}
		for (const API_Neig& neig : neigs) selected (GS::ObjectState (
			"guid", APIGuidToString (neig.guid), "neigId", static_cast<Int32> (neig.neigID),
			"inIndex", neig.inIndex, "flags", neig.flags, "elementPartType", static_cast<Int32> (neig.elemPartType),
			"elementPartIndex", static_cast<Int64> (neig.elemPartIndex), "subtype", static_cast<Int32> (neig.subType)));
	}
	const bool marquee = !empty && (selection.typeID == API_MarqueePoly || selection.typeID == API_MarqueeHorBox || selection.typeID == API_MarqueeRotBox);
	if (marquee) {
		if (!std::isfinite (selection.marquee.box.xMin) || !std::isfinite (selection.marquee.box.yMin)
			|| !std::isfinite (selection.marquee.box.xMax) || !std::isfinite (selection.marquee.box.yMax)
			|| !std::isfinite (selection.marquee.boxRotAngle)) {
			snapshot.facts = GS::ObjectState ("status", "unavailable", "reason", "marquee-coordinates-nonfinite");
			return snapshot;
		}
		GS::ObjectState marqueeFacts ("multiStory", selection.multiStory, "rotation", selection.marquee.boxRotAngle,
			"bounds", GS::ObjectState ("xMin", selection.marquee.box.xMin, "yMin", selection.marquee.box.yMin,
				"xMax", selection.marquee.box.xMax, "yMax", selection.marquee.box.yMax));
		if (selection.typeID == API_MarqueePoly) {
			const GSSize bytes = selection.marquee.coords != nullptr && *selection.marquee.coords != nullptr
				? BMhGetSize (reinterpret_cast<GSHandle> (selection.marquee.coords)) : 0;
			if (selection.marquee.coords == nullptr || *selection.marquee.coords == nullptr || selection.marquee.nCoords < 1
				|| selection.marquee.nCoords > 200000 || bytes < static_cast<GSSize> (selection.marquee.nCoords + 1) * sizeof (API_Coord)) {
				snapshot.facts = GS::ObjectState ("status", "unavailable", "reason", "marquee-polygon-storage-unavailable-or-out-of-bounds");
				return snapshot;
			}
			const auto& points = marqueeFacts.AddList<GS::ObjectState> ("polygonPoints");
			for (Int32 index = 1; index <= selection.marquee.nCoords; ++index) {
				const API_Coord& point = (*selection.marquee.coords)[index];
				if (!std::isfinite (point.x) || !std::isfinite (point.y)) {
					snapshot.facts = GS::ObjectState ("status", "unavailable", "reason", "marquee-polygon-coordinate-nonfinite");
					return snapshot;
				}
				points (GS::ObjectState ("x", point.x, "y", point.y));
			}
		}
		focus.Add ("marquee", marqueeFacts);
	}
	snapshot.facts = focus;
	GS::UniString serialized;
	if (JSON::CreateFromObjectState (snapshot.facts, serialized, false) != NoError) {
		snapshot.facts = GS::ObjectState ("status", "unavailable", "reason", "focus-serialization-failed");
		return snapshot;
	}
	snapshot.serialized = FixtureUtf8 (serialized);
	snapshot.complete = true;
	return snapshot;
}

bool NativeDimensionLayerFactEqual (const NativeDimensionLayerFact& left, const NativeDimensionLayerFact& right)
{
	return left.guid == right.guid && left.index == right.index && left.typeId == right.typeId
		&& left.flags == right.flags && left.name == right.name && left.connectionClass == right.connectionClass
		&& left.modificationTime == right.modificationTime;
}

bool NativeDimensionLayerEntryEqual (const NativeDimensionLayerEntryFact& left, const NativeDimensionLayerEntryFact& right)
{
	return left.layerIndex == right.layerIndex && left.layerGuid == right.layerGuid && left.resolution == right.resolution
		&& left.lookupError == right.lookupError
		&& left.rawFlags == right.rawFlags && left.connectionClass == right.connectionClass;
}

bool NativeDimensionLayerCombinationEqual (const NativeDimensionLayerCombinationFact& left,
	const NativeDimensionLayerCombinationFact& right, const bool allowCreateTimeAndOneLayer);

bool NativeDimensionLayerSlotEqual (const NativeDimensionLayerSlotFact& left, const NativeDimensionLayerSlotFact& right)
{
	return left.index == right.index && left.state == right.state && left.errorCode == right.errorCode
		&& (left.state != NativeDimensionLayerControlPolicy::SlotState::Live || NativeDimensionLayerFactEqual (left.layer, right.layer));
}

bool NativeDimensionLayerCombinationSlotEqual (const NativeDimensionLayerCombinationSlotFact& left, const NativeDimensionLayerCombinationSlotFact& right)
{
	return left.index == right.index && left.state == right.state && left.errorCode == right.errorCode
		&& (left.state != NativeDimensionLayerControlPolicy::SlotState::Live
			|| NativeDimensionLayerCombinationEqual (left.combination, right.combination, false));
}

bool NativeDimensionLayerCombinationEqual (const NativeDimensionLayerCombinationFact& left,
	const NativeDimensionLayerCombinationFact& right, const bool allowCreateTimeAndOneLayer)
{
	if (left.guid != right.guid || left.index != right.index || left.typeId != right.typeId || left.flags != right.flags
		|| left.name != right.name || left.inEffect != right.inEffect
		|| left.declaredLayerCount != right.declaredLayerCount + (allowCreateTimeAndOneLayer ? 1 : 0)
		|| (!allowCreateTimeAndOneLayer && left.modificationTime != right.modificationTime)) return false;
	if (allowCreateTimeAndOneLayer) {
		if (left.entries.size () != right.entries.size () + 1) return false;
		std::size_t added = 0;
		for (const auto& entry : left.entries) {
			const auto old = std::find_if (right.entries.begin (), right.entries.end (), [&entry] (const auto& previous) { return NativeDimensionLayerEntryEqual (entry, previous); });
			if (old == right.entries.end ()) {
				++added;
				if ((entry.rawFlags & (APILay_Hidden | APILay_Locked)) != 0) return false;
			}
		}
		return added == 1;
	}
	if (left.entries.size () != right.entries.size ()) return false;
	for (std::size_t index = 0; index < left.entries.size (); ++index)
		if (!NativeDimensionLayerEntryEqual (left.entries[index], right.entries[index])) return false;
	return true;
}

bool NativeDimensionLayerCombinationPreparationDeltaExact (const NativeDimensionLayerCombinationFact& after,
	const NativeDimensionLayerCombinationFact& before, const Int32 ownedIndex, const GS::UniString& ownedGuid)
{
	if (after.guid != before.guid || after.index != before.index || after.typeId != before.typeId || after.flags != before.flags
		|| after.name != before.name || after.inEffect != before.inEffect) return false;
	const auto oldOwned = std::find_if (before.entries.begin (), before.entries.end (), [ownedIndex] (const auto& entry) { return entry.layerIndex == ownedIndex; });
	const auto newOwned = std::find_if (after.entries.begin (), after.entries.end (), [ownedIndex] (const auto& entry) { return entry.layerIndex == ownedIndex; });
	if (newOwned == after.entries.end () || newOwned->resolution != "live-guid"
		|| NativeDimensionAuditPathKey (newOwned->layerGuid) != NativeDimensionAuditPathKey (ownedGuid)) return false;
	const bool oldIndexWasLive = oldOwned != before.entries.end () && oldOwned->resolution == "live-guid";
	const bool oldEntryPresent = oldOwned != before.entries.end ();
	const auto oldEntryState = !oldEntryPresent ? NativeDimensionLayerControlPolicy::SlotState::Absent
		: oldOwned->resolution == "tombstone" ? NativeDimensionLayerControlPolicy::SlotState::Tombstone
		: oldOwned->resolution == "absent-from-live-enumeration" ? NativeDimensionLayerControlPolicy::SlotState::Absent
		: NativeDimensionLayerControlPolicy::SlotState::Live;
	const bool oldEntryFactsPreserved = oldEntryPresent && oldOwned->rawFlags == newOwned->rawFlags
		&& oldOwned->connectionClass == newOwned->connectionClass;
	const bool newEntryLive = newOwned->resolution == "live-guid";
	const auto change = NativeDimensionLayerControlPolicy::ClassifyOwnedEntryChange (oldIndexWasLive, oldEntryPresent,
		oldEntryState, oldEntryFactsPreserved, newEntryLive);
	if (change == NativeDimensionLayerControlPolicy::OwnedEntryChange::Invalid) return false;
	if (change == NativeDimensionLayerControlPolicy::OwnedEntryChange::Add) {
		if (!NativeDimensionLayerControlPolicy::IsValidAbsentOwnedEntryAddition (newOwned->rawFlags, newOwned->connectionClass)
			|| after.entries.size () != before.entries.size () + 1
			|| after.declaredLayerCount != before.declaredLayerCount + 1) return false;
	} else if (after.entries.size () != before.entries.size () || after.declaredLayerCount != before.declaredLayerCount
		|| !NativeDimensionLayerControlPolicy::IsVisibleUnlockedStoredEntry (newOwned->rawFlags)) return false;
	for (const auto& oldEntry : before.entries) {
		if (oldEntry.layerIndex == ownedIndex) continue;
		const auto current = std::find_if (after.entries.begin (), after.entries.end (), [&oldEntry] (const auto& entry) { return entry.layerIndex == oldEntry.layerIndex; });
		if (current == after.entries.end () || !NativeDimensionLayerEntryEqual (*current, oldEntry)) return false;
	}
	return true;
}

bool NativeDimensionLayerInventoryMatchesInitial (const NativeDimensionLayerControlRun& run, const NativeDimensionLayerInventory& current)
{
	if (!current.complete || current.layers.size () != run.initialLayers.size () || current.combinations.size () != run.initialCombinations.size ()) return false;
	if (current.layerSlots.size () != run.initialLayerSlots.size () || current.combinationSlots.size () != run.initialCombinationSlots.size ()) return false;
	for (std::size_t index = 0; index < run.initialLayerSlots.size (); ++index)
		if (!NativeDimensionLayerSlotEqual (current.layerSlots[index], run.initialLayerSlots[index])) return false;
	for (std::size_t index = 0; index < run.initialCombinationSlots.size (); ++index)
		if (!NativeDimensionLayerCombinationSlotEqual (current.combinationSlots[index], run.initialCombinationSlots[index])) return false;
	if (current.currentCombinationError != run.initialCurrentCombinationError || current.currentCombinationIndex != run.initialCurrentCombinationIndex) return false;
	for (std::size_t index = 0; index < run.initialLayers.size (); ++index)
		if (!NativeDimensionLayerFactEqual (current.layers[index], run.initialLayers[index])) return false;
	for (std::size_t index = 0; index < run.initialCombinations.size (); ++index)
		if (!NativeDimensionLayerCombinationEqual (current.combinations[index], run.initialCombinations[index], false)) return false;
	return true;
}

bool NativeDimensionLayerPreparationDeltaExact (const NativeDimensionLayerInventory& before,
	const NativeDimensionLayerInventory& after, const API_Guid ownedGuid)
{
	if (!before.complete || !after.complete || after.layers.size () != before.layers.size () + 1
		|| after.combinations.size () != before.combinations.size () || after.combinationSlots.size () != before.combinationSlots.size ()
		|| after.combinationReportedCount != before.combinationReportedCount || after.countSemantics != before.countSemantics
		|| before.currentCombinationError != after.currentCombinationError
		|| before.currentCombinationIndex != after.currentCombinationIndex) return false;
	const GS::UniString ownedGuidText = APIGuidToString (ownedGuid);
	if (std::any_of (before.layers.begin (), before.layers.end (), [&ownedGuidText] (const auto& item) {
		return NativeDimensionAuditPathKey (item.guid) == NativeDimensionAuditPathKey (ownedGuidText);
	})) return false;
	std::size_t matched = 0;
	for (const auto& oldLayer : before.layers) {
		const auto found = std::find_if (after.layers.begin (), after.layers.end (), [&oldLayer] (const auto& item) { return item.guid == oldLayer.guid; });
		if (found == after.layers.end () || !NativeDimensionLayerFactEqual (*found, oldLayer)) return false;
		++matched;
	}
	const auto addedLayer = std::find_if (after.layers.begin (), after.layers.end (), [&ownedGuidText] (const auto& item) { return item.guid == ownedGuidText; });
	if (addedLayer == after.layers.end () || matched != before.layers.size ()
		|| !NativeDimensionLayerControlPolicy::IsExactNewOwnedLayerState (addedLayer->flags, addedLayer->connectionClass)) return false;
	const auto oldOwnedSlot = std::find_if (before.layerSlots.begin (), before.layerSlots.end (), [&addedLayer] (const auto& slot) { return slot.index == addedLayer->index; });
	if (oldOwnedSlot != before.layerSlots.end () && oldOwnedSlot->state == NativeDimensionLayerControlPolicy::SlotState::Live) return false;
	if (oldOwnedSlot != before.layerSlots.end () && oldOwnedSlot->state != NativeDimensionLayerControlPolicy::SlotState::Tombstone) return false;
	const UInt64 expectedSlotDelta = before.countSemantics == "actual-live-count" || oldOwnedSlot == before.layerSlots.end () ? 1 : 0;
	if (after.layerSlots.size () != before.layerSlots.size () + expectedSlotDelta
		|| after.layerReportedCount != before.layerReportedCount + expectedSlotDelta) return false;
	const auto newOwnedSlot = std::find_if (after.layerSlots.begin (), after.layerSlots.end (), [&addedLayer] (const auto& slot) { return slot.index == addedLayer->index; });
	if (newOwnedSlot == after.layerSlots.end () || newOwnedSlot->state != NativeDimensionLayerControlPolicy::SlotState::Live
		|| !NativeDimensionLayerFactEqual (newOwnedSlot->layer, *addedLayer)) return false;
	for (const auto& oldSlot : before.layerSlots) {
		if (oldSlot.index == addedLayer->index) continue;
		const auto current = std::find_if (after.layerSlots.begin (), after.layerSlots.end (), [&oldSlot] (const auto& slot) { return slot.index == oldSlot.index; });
		if (current == after.layerSlots.end () || !NativeDimensionLayerSlotEqual (*current, oldSlot)) return false;
	}
	for (const auto& oldCombo : before.combinations) {
		const auto found = std::find_if (after.combinations.begin (), after.combinations.end (), [&oldCombo] (const auto& item) { return item.guid == oldCombo.guid; });
		if (found == after.combinations.end () || !NativeDimensionLayerCombinationPreparationDeltaExact (*found, oldCombo, addedLayer->index, addedLayer->guid)) return false;
	}
	for (std::size_t index = 0; index < before.combinationSlots.size (); ++index) {
		const auto& oldSlot = before.combinationSlots[index];
		const auto current = std::find_if (after.combinationSlots.begin (), after.combinationSlots.end (), [&oldSlot] (const auto& slot) { return slot.index == oldSlot.index; });
		if (current == after.combinationSlots.end ()) return false;
		if (oldSlot.state != NativeDimensionLayerControlPolicy::SlotState::Live) {
			if (!NativeDimensionLayerCombinationSlotEqual (*current, oldSlot)) return false;
		} else {
			const auto priorCombo = std::find_if (before.combinations.begin (), before.combinations.end (), [&oldSlot] (const auto& combo) { return combo.guid == oldSlot.combination.guid; });
			if (priorCombo == before.combinations.end ()) return false;
			const auto updated = std::find_if (after.combinations.begin (), after.combinations.end (), [&priorCombo] (const auto& combo) { return combo.guid == priorCombo->guid; });
			if (updated == after.combinations.end () || updated->index != oldSlot.index) return false;
		}
	}
	return true;
}

bool NativeDimensionLayerStageFactsUnchanged (const NativeDimensionLayerControlRun& run,
	const NativeDimensionLayerInventory& current, const bool expectedHidden, const bool expectedLocked)
{
	if (!current.complete || current.layers.size () != run.preparedLayers.size ()
		|| current.combinations.size () != run.preparedCombinations.size ()
		|| current.layerSlots.size () != run.preparedLayerSlots.size () || current.combinationSlots.size () != run.preparedCombinationSlots.size ()
		|| current.currentCombinationError != run.preparedCurrentCombinationError
		|| current.currentCombinationIndex != run.preparedCurrentCombinationIndex) return false;
	for (std::size_t index = 0; index < run.preparedLayerSlots.size (); ++index) {
		const auto& expected = run.preparedLayerSlots[index];
		const auto& observed = current.layerSlots[index];
		if (expected.index != observed.index || expected.state != observed.state) return false;
		if (expected.state == NativeDimensionLayerControlPolicy::SlotState::Tombstone && expected.errorCode != observed.errorCode) return false;
		if (expected.state == NativeDimensionLayerControlPolicy::SlotState::Live && expected.layer.guid != APIGuidToString (run.layerGuid)
			&& !NativeDimensionLayerFactEqual (expected.layer, observed.layer)) return false;
	}
	for (std::size_t index = 0; index < run.preparedCombinationSlots.size (); ++index)
		if (!NativeDimensionLayerCombinationSlotEqual (current.combinationSlots[index], run.preparedCombinationSlots[index])) return false;
	for (std::size_t index = 0; index < run.preparedLayers.size (); ++index) {
		const auto& expected = run.preparedLayers[index];
		const auto& observed = current.layers[index];
		if (expected.guid != observed.guid || expected.index != observed.index || expected.typeId != observed.typeId
			|| expected.name != observed.name || expected.connectionClass != observed.connectionClass) return false;
		const Int32 expectedFlags = static_cast<Int32> (expected.flags & ~(APILay_Hidden | APILay_Locked))
			| (expected.guid == APIGuidToString (run.layerGuid) ? (expectedHidden ? APILay_Hidden : 0) | (expectedLocked ? APILay_Locked : 0)
				: (expected.flags & (APILay_Hidden | APILay_Locked)));
		if (observed.flags != expectedFlags) return false;
		if (expected.guid != APIGuidToString (run.layerGuid) && observed.modificationTime != expected.modificationTime) return false;
	}
	for (std::size_t index = 0; index < run.preparedCombinations.size (); ++index)
		if (!NativeDimensionLayerCombinationEqual (current.combinations[index], run.preparedCombinations[index], false)) return false;
	return true;
}

NativeDimensionLayerControlRun gNativeDimensionLayerControlRun;
std::set<std::string> gUsedNativeDimensionLayerControlNonces;
std::atomic<std::uint64_t> gNativeDimensionLayerControlCounter { 0 };

bool NativeDimensionLayerControlExpired (const std::chrono::steady_clock::time_point& started, GS::ProcessControl& processControl)
{
	return processControl.TestBreak () || std::chrono::steady_clock::now () - started >= std::chrono::seconds (NativeDimensionLayerControlBudgetSeconds);
}

GS::UniString NativeDimensionLayerControlToken ()
{
	const auto now = std::chrono::steady_clock::now ().time_since_epoch ().count ();
	const auto counter = gNativeDimensionLayerControlCounter.fetch_add (1, std::memory_order_relaxed) + 1;
	const std::string token = std::to_string (GetCurrentProcessId ()) + "-" + std::to_string (now) + "-" + std::to_string (counter);
	return GS::UniString (token.c_str (), CC_UTF8);
}

bool NativeDimensionLayerControlAttributeCount (const API_AttrTypeID typeId, UInt64& count, GSErrCode& error)
{
#ifdef ServerMainVers_2800
	UInt32 rawCount = 0;
	error = ACAPI_Attribute_GetNum (typeId, rawCount);
	count = rawCount;
#else
	API_AttributeIndex rawCount = 0;
	error = ACAPI_Attribute_GetNum (typeId, &rawCount);
	count = static_cast<UInt64> (GetAttributeIndex (rawCount));
#endif
	return error == NoError;
}

GS::ObjectState NativeDimensionLayerEntryState (const NativeDimensionLayerEntryFact& entry)
{
	GS::ObjectState state ("layerIndex", entry.layerIndex, "resolution", entry.resolution,
		"rawFlags", entry.rawFlags, "isHidden", (entry.rawFlags & APILay_Hidden) != 0,
		"isLocked", (entry.rawFlags & APILay_Locked) != 0, "isWireframe", (entry.rawFlags & APILay_ForceToWire) != 0,
		"connectionClass", entry.connectionClass);
	if (entry.resolution == "live-guid") state.Add ("layerGuid", entry.layerGuid);
	else state.Add ("lookupErrorCode", static_cast<Int32> (entry.lookupError));
	return state;
}

GS::ObjectState NativeDimensionLayerFactState (const NativeDimensionLayerFact& layer)
{
	return GS::ObjectState ("attributeGuid", layer.guid, "attributeIndex", layer.index, "typeId", layer.typeId,
		"flags", layer.flags, "isHidden", (layer.flags & APILay_Hidden) != 0,
		"isLocked", (layer.flags & APILay_Locked) != 0, "isWireframe", (layer.flags & APILay_ForceToWire) != 0,
		"name", layer.name, "connectionClass", layer.connectionClass, "modificationTime", static_cast<Int64> (layer.modificationTime));
}

GS::ObjectState NativeDimensionLayerCombinationFactState (const NativeDimensionLayerCombinationFact& combination)
{
	GS::ObjectState state ("attributeGuid", combination.guid, "attributeIndex", combination.index,
		"typeId", combination.typeId, "flags", combination.flags, "name", combination.name,
		"modificationTime", static_cast<Int64> (combination.modificationTime),
		"declaredLayerCount", combination.declaredLayerCount, "inEffect", combination.inEffect);
	const auto& entries = state.AddList<GS::ObjectState> ("entries");
	for (const NativeDimensionLayerEntryFact& entry : combination.entries) entries (NativeDimensionLayerEntryState (entry));
	return state;
}

GS::ObjectState NativeDimensionLayerInventoryState (const NativeDimensionLayerInventory& inventory,
	const GS::UniString& status, const GS::UniString& reason = GS::UniString ())
{
	const UInt64 layerTombstones = static_cast<UInt64> (std::count_if (inventory.layerSlots.begin (), inventory.layerSlots.end (), [] (const auto& slot) {
		return slot.state == NativeDimensionLayerControlPolicy::SlotState::Tombstone;
	}));
	const UInt64 combinationTombstones = static_cast<UInt64> (std::count_if (inventory.combinationSlots.begin (), inventory.combinationSlots.end (), [] (const auto& slot) {
		return slot.state == NativeDimensionLayerControlPolicy::SlotState::Tombstone;
	}));
	GS::ObjectState facts ("status", status, "schemaVersion", "native-dimension-layer-inventory-facts/v2",
		"sdkMajor", TAPIR_AC_VERSION, "layerInventory", GS::ObjectState ("reportedCount", static_cast<Int64> (inventory.layerReportedCount),
			"countSemantics", inventory.countSemantics, "liveCount", static_cast<Int64> (inventory.layers.size ()),
			"maximumObservedIndex", static_cast<Int64> (inventory.layerMaximumIndex), "tombstoneCount", static_cast<Int64> (layerTombstones)),
		"combinationInventory", GS::ObjectState ("reportedCount", static_cast<Int64> (inventory.combinationReportedCount),
			"countSemantics", inventory.countSemantics, "liveCount", static_cast<Int64> (inventory.combinations.size ()),
			"maximumObservedIndex", static_cast<Int64> (inventory.combinationMaximumIndex), "tombstoneCount", static_cast<Int64> (combinationTombstones)),
		"entryCount", static_cast<Int64> (std::accumulate (inventory.combinations.begin (), inventory.combinations.end (), UInt64 (0), [] (UInt64 total, const auto& combo) { return total + combo.entries.size (); })),
		"currentCombination", GS::ObjectState ("status", inventory.currentCombinationError == NoError ? "available" : "unavailable",
			"errorCode", static_cast<Int32> (inventory.currentCombinationError), "index", inventory.currentCombinationError == NoError ? inventory.currentCombinationIndex : -1));
	if (!reason.IsEmpty ()) facts.Add ("reason", reason);
	const auto& layerSlots = facts.AddList<GS::ObjectState> ("layerSlots");
	for (const auto& slot : inventory.layerSlots) {
		GS::ObjectState state ("index", slot.index, "status", slot.state == NativeDimensionLayerControlPolicy::SlotState::Live ? "live" : "tombstone");
		if (slot.state == NativeDimensionLayerControlPolicy::SlotState::Live) state.Add ("layer", NativeDimensionLayerFactState (slot.layer));
		else state.Add ("errorCode", static_cast<Int32> (slot.errorCode));
		layerSlots (state);
	}
	const auto& combinationSlots = facts.AddList<GS::ObjectState> ("combinationSlots");
	for (const auto& slot : inventory.combinationSlots) {
		GS::ObjectState state ("index", slot.index, "status", slot.state == NativeDimensionLayerControlPolicy::SlotState::Live ? "live" : "tombstone");
		if (slot.state == NativeDimensionLayerControlPolicy::SlotState::Live) state.Add ("combination", NativeDimensionLayerCombinationFactState (slot.combination));
		else state.Add ("errorCode", static_cast<Int32> (slot.errorCode));
		combinationSlots (state);
	}
	return facts;
}

NativeDimensionLayerInventory ReadNativeDimensionLayerInventory (const std::chrono::steady_clock::time_point& started, GS::ProcessControl& processControl)
{
	NativeDimensionLayerInventory inventory;
	if (NativeDimensionLayerControlExpired (started, processControl)) {
		inventory.error = "cooperative-deadline-before-inventory";
		inventory.facts = GS::ObjectState ("status", "refused", "reason", inventory.error);
		return inventory;
	}
	UInt64 layerCount = 0, combinationCount = 0;
	GSErrCode layerCountError = NoError, combinationCountError = NoError;
	if (!NativeDimensionLayerControlAttributeCount (API_LayerID, layerCount, layerCountError)
		|| !NativeDimensionLayerControlAttributeCount (API_LayerCombID, combinationCount, combinationCountError)) {
		inventory.error = "attribute-count-read-failed";
		inventory.facts = GS::ObjectState ("status", "unavailable", "layerCountErrorCode", static_cast<Int32> (layerCountError), "combinationCountErrorCode", static_cast<Int32> (combinationCountError));
		return inventory;
	}
	if (!NativeDimensionLayerControlPolicy::IsValidReadCount (layerCount, NativeDimensionLayerControlPolicy::MaxLayers)
		|| !NativeDimensionLayerControlPolicy::IsValidReadCount (combinationCount, NativeDimensionLayerControlPolicy::MaxCombinations)) {
		inventory.error = "attribute-count-limit-exceeded-no-prefix";
		inventory.facts = GS::ObjectState ("status", "refused", "layerCount", static_cast<Int64> (layerCount), "combinationCount", static_cast<Int64> (combinationCount));
		return inventory;
	}
	inventory.layerReportedCount = layerCount;
	inventory.combinationReportedCount = combinationCount;
#if TAPIR_AC_VERSION == 25
	inventory.countSemantics = "highest-index-slot-bound";
	inventory.layerMaximumIndex = layerCount;
	inventory.combinationMaximumIndex = combinationCount;
#else
	inventory.countSemantics = "actual-live-count";
#endif
	std::map<Int32, GS::UniString> guidByLayerIndex;
	std::map<Int32, NativeDimensionLayerControlPolicy::SlotState> layerStateByIndex;
	std::set<std::string> layerGuids;
#if TAPIR_AC_VERSION == 25
	for (UInt64 rawIndex = 1; rawIndex <= layerCount; ++rawIndex) {
		if (NativeDimensionLayerControlExpired (started, processControl)) {
			inventory.error = "cooperative-deadline-during-layer-inventory-no-prefix-success";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-enumeration", "observedCount", static_cast<Int64> (inventory.layers.size ()), "expectedCount", static_cast<Int64> (layerCount));
			return inventory;
		}
		API_Attribute attribute = {};
		GS::UniString name;
		attribute.header.typeID = API_LayerID;
		attribute.header.index = ACAPI_CreateAttributeIndex (static_cast<Int32> (rawIndex));
		attribute.header.uniStringNamePtr = &name;
		const GSErrCode getError = ACAPI_Attribute_Get (&attribute);
		const GS::OnExit attributeGuard ([&attribute] () { DisposeAttribute (attribute); });
		const Int32 index = GetAttributeIndex (attribute.header.index);
		if (getError == APIERR_DELETED) {
			NativeDimensionLayerSlotFact slot;
			slot.index = static_cast<Int32> (rawIndex);
			slot.state = NativeDimensionLayerControlPolicy::SlotState::Tombstone;
			slot.errorCode = getError;
			inventory.layerSlots.push_back (slot);
			layerStateByIndex.emplace (slot.index, slot.state);
			continue;
		}
		const GS::UniString guid = APIGuidToString (attribute.header.guid);
		if (getError != NoError || attribute.header.typeID != API_LayerID || index != static_cast<Int32> (rawIndex)
			|| attribute.header.guid == APINULLGuid || name.IsEmpty () || !layerGuids.insert (NativeDimensionAuditPathKey (guid)).second) {
			inventory.error = "layer-enumeration-incomplete-or-duplicate";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-enumeration", "errorCode", static_cast<Int32> (getError), "index", static_cast<Int32> (rawIndex));
			return inventory;
		}
		NativeDimensionLayerFact fact;
		fact.guid = guid;
		fact.index = index;
		fact.flags = attribute.header.flags;
		fact.name = name;
		fact.connectionClass = attribute.layer.conClassId;
		fact.modificationTime = attribute.header.modiTime;
		NativeDimensionLayerSlotFact slot;
		slot.index = index;
		slot.layer = fact;
		inventory.layerSlots.push_back (slot);
		layerStateByIndex.emplace (index, NativeDimensionLayerControlPolicy::SlotState::Live);
		guidByLayerIndex.emplace (index, guid);
		inventory.layers.push_back (fact);
	}
#else
	GS::Array<API_Attribute> enumeratedLayers;
	const GSErrCode layerEnumerationError = ACAPI_Attribute_GetAttributesByType (API_LayerID, enumeratedLayers);
	const GS::OnExit enumeratedLayerGuard ([&enumeratedLayers] () { for (API_Attribute& item : enumeratedLayers) DisposeAttribute (item); });
	if (layerEnumerationError != NoError || static_cast<UInt64> (enumeratedLayers.GetSize ()) != layerCount) {
		inventory.error = "layer-by-type-count-disagreement";
		inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-by-type-enumeration",
			"errorCode", static_cast<Int32> (layerEnumerationError), "reportedCount", static_cast<Int64> (layerCount),
			"returnedCount", enumeratedLayers.GetSize ());
		return inventory;
	}
	std::vector<std::pair<Int32, API_Guid>> layerIdentities;
	std::set<Int32> enumeratedLayerIndices;
	for (const API_Attribute& item : enumeratedLayers) {
		const Int32 index = GetAttributeIndex (item.header.index);
		if (item.header.typeID != API_LayerID || index < 1 || item.header.guid == APINULLGuid
			|| !enumeratedLayerIndices.insert (index).second || !layerGuids.insert (NativeDimensionAuditPathKey (APIGuidToString (item.header.guid))).second) {
			inventory.error = "layer-by-type-identity-duplicate-or-invalid";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-by-type-enumeration", "index", index);
			return inventory;
		}
		layerIdentities.emplace_back (index, item.header.guid);
	}
	std::sort (layerIdentities.begin (), layerIdentities.end (), [] (const auto& left, const auto& right) { return left.first < right.first; });
	for (const auto& identity : layerIdentities) {
		const Int32 index = identity.first;
		const API_Guid expectedGuid = identity.second;
		if (NativeDimensionLayerControlExpired (started, processControl)) {
			inventory.error = "cooperative-deadline-during-layer-details-no-prefix-success";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-details", "observedCount", static_cast<Int64> (inventory.layers.size ()), "expectedCount", static_cast<Int64> (layerCount));
			return inventory;
		}
		API_Attribute attribute = {};
		GS::UniString name;
		attribute.header.typeID = API_LayerID;
		attribute.header.index = ACAPI_CreateAttributeIndex (index);
		attribute.header.uniStringNamePtr = &name;
		const GSErrCode getError = ACAPI_Attribute_Get (&attribute);
		const GS::OnExit attributeGuard ([&attribute] () { DisposeAttribute (attribute); });
		if (getError != NoError || attribute.header.typeID != API_LayerID || GetAttributeIndex (attribute.header.index) != index
			|| attribute.header.guid != expectedGuid || name.IsEmpty ()) {
			inventory.error = "layer-by-type-detail-identity-drift";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "layer-details", "errorCode", static_cast<Int32> (getError), "index", index);
			return inventory;
		}
		NativeDimensionLayerFact fact;
		fact.guid = APIGuidToString (attribute.header.guid); fact.index = index; fact.flags = attribute.header.flags;
		fact.name = name; fact.connectionClass = attribute.layer.conClassId; fact.modificationTime = attribute.header.modiTime;
		NativeDimensionLayerSlotFact slot; slot.index = index; slot.layer = fact;
		inventory.layerSlots.push_back (slot); layerStateByIndex.emplace (index, NativeDimensionLayerControlPolicy::SlotState::Live);
		guidByLayerIndex.emplace (index, fact.guid); inventory.layers.push_back (fact);
		inventory.layerMaximumIndex = std::max (inventory.layerMaximumIndex, static_cast<UInt64> (index));
	}
#endif
	UInt64 totalEntries = 0;
	std::set<std::string> combinationGuids;
	std::vector<std::pair<Int32, API_Guid>> combinationIdentities;
#if TAPIR_AC_VERSION == 25
	for (UInt64 rawIndex = 1; rawIndex <= combinationCount; ++rawIndex) {
		if (NativeDimensionLayerControlExpired (started, processControl)) {
			inventory.error = "cooperative-deadline-during-combination-inventory-no-prefix-success";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-enumeration", "observedCount", static_cast<Int64> (inventory.combinations.size ()), "expectedCount", static_cast<Int64> (combinationCount));
			return inventory;
		}
		API_Attribute attribute = {};
		attribute.header.typeID = API_LayerCombID;
		attribute.header.index = ACAPI_CreateAttributeIndex (static_cast<Int32> (rawIndex));
		const GSErrCode getError = ACAPI_Attribute_Get (&attribute);
		const Int32 index = GetAttributeIndex (attribute.header.index);
		if (getError == APIERR_DELETED) {
			NativeDimensionLayerCombinationSlotFact slot; slot.index = static_cast<Int32> (rawIndex);
			slot.state = NativeDimensionLayerControlPolicy::SlotState::Tombstone; slot.errorCode = getError;
			inventory.combinationSlots.push_back (slot);
			continue;
		}
		if (getError != NoError || attribute.header.typeID != API_LayerCombID || index != static_cast<Int32> (rawIndex)
			|| attribute.header.guid == APINULLGuid
			|| !combinationGuids.insert (NativeDimensionAuditPathKey (APIGuidToString (attribute.header.guid))).second) {
			inventory.error = "combination-enumeration-incomplete-or-duplicate";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-header", "errorCode", static_cast<Int32> (getError), "index", static_cast<Int32> (rawIndex));
			return inventory;
		}
		combinationIdentities.emplace_back (index, attribute.header.guid);
	}
#else
	GS::Array<API_Attribute> enumeratedCombinations;
	const GSErrCode combinationEnumerationError = ACAPI_Attribute_GetAttributesByType (API_LayerCombID, enumeratedCombinations);
	const GS::OnExit enumeratedCombinationGuard ([&enumeratedCombinations] () { for (API_Attribute& item : enumeratedCombinations) DisposeAttribute (item); });
	if (combinationEnumerationError != NoError || static_cast<UInt64> (enumeratedCombinations.GetSize ()) != combinationCount) {
		inventory.error = "combination-by-type-count-disagreement";
		inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-by-type-enumeration",
			"errorCode", static_cast<Int32> (combinationEnumerationError), "reportedCount", static_cast<Int64> (combinationCount),
			"returnedCount", enumeratedCombinations.GetSize ());
		return inventory;
	}
	std::set<Int32> enumeratedCombinationIndices;
	for (const API_Attribute& item : enumeratedCombinations) {
		const Int32 index = GetAttributeIndex (item.header.index);
		if (item.header.typeID != API_LayerCombID || index < 1 || item.header.guid == APINULLGuid
			|| !enumeratedCombinationIndices.insert (index).second
			|| !combinationGuids.insert (NativeDimensionAuditPathKey (APIGuidToString (item.header.guid))).second) {
			inventory.error = "combination-by-type-identity-duplicate-or-invalid";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-by-type-enumeration", "index", index);
			return inventory;
		}
		combinationIdentities.emplace_back (index, item.header.guid);
	}
	std::sort (combinationIdentities.begin (), combinationIdentities.end (), [] (const auto& left, const auto& right) { return left.first < right.first; });
#endif
	for (const auto& identity : combinationIdentities) {
		const Int32 requestedIndex = identity.first;
	const API_Guid expectedGuid = identity.second;
		if (NativeDimensionLayerControlExpired (started, processControl)) {
			inventory.error = "cooperative-deadline-during-combination-inventory-no-prefix-success";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-enumeration", "observedCount", static_cast<Int64> (inventory.combinations.size ()), "expectedCount", static_cast<Int64> (combinationCount));
			return inventory;
		}
		API_Attribute attribute = {};
		GS::UniString name;
		attribute.header.typeID = API_LayerCombID;
		attribute.header.index = ACAPI_CreateAttributeIndex (requestedIndex);
		attribute.header.uniStringNamePtr = &name;
		const GSErrCode getError = ACAPI_Attribute_Get (&attribute);
		const GS::OnExit attributeGuard ([&attribute] () { DisposeAttribute (attribute); });
		const Int32 index = GetAttributeIndex (attribute.header.index);
		if (getError != NoError || attribute.header.typeID != API_LayerCombID || index != requestedIndex
			|| attribute.header.guid != expectedGuid || name.IsEmpty ()) {
			inventory.error = "combination-detail-identity-drift";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-details", "errorCode", static_cast<Int32> (getError), "index", requestedIndex);
			return inventory;
		}
		API_AttributeDef definition = {};
		const GS::OnExit definitionGuard ([&definition] () { ACAPI_DisposeAttrDefsHdls (&definition); });
		const GSErrCode definitionError = ACAPI_Attribute_GetDef (API_LayerCombID, attribute.header.index, &definition);
		if (definitionError != NoError || attribute.layerComb.lNumb < 0
			|| !NativeDimensionLayerControlPolicy::IsValidEntryCount (static_cast<UInt64> (attribute.layerComb.lNumb), NativeDimensionLayerControlPolicy::MaxLayers)) {
			inventory.error = "combination-entry-count-or-definition-unavailable";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-definition", "errorCode", static_cast<Int32> (definitionError), "index", index, "declaredLayerCount", attribute.layerComb.lNumb, "maximumEntryCount", static_cast<Int64> (NativeDimensionLayerControlPolicy::MaxLayers));
			return inventory;
		}
		NativeDimensionLayerCombinationFact fact;
		fact.guid = APIGuidToString (attribute.header.guid);
		fact.index = index;
		fact.flags = attribute.header.flags;
		fact.name = name;
		fact.modificationTime = attribute.header.modiTime;
		fact.declaredLayerCount = attribute.layerComb.lNumb;
		fact.inEffect = attribute.layerComb.inEffect;
		std::set<Int32> seenEntryIndices;
		bool unresolvedReference = false;
		auto appendEntry = [&] (const Int32 entryIndex, const Int32 rawFlags, const Int32 connectionClass) {
			if (!seenEntryIndices.insert (entryIndex).second) return false;
			NativeDimensionLayerEntryFact entry;
			entry.layerIndex = entryIndex; entry.rawFlags = rawFlags; entry.connectionClass = connectionClass;
			const auto live = guidByLayerIndex.find (entryIndex);
			if (live != guidByLayerIndex.end ()) {
				entry.layerGuid = live->second; entry.resolution = "live-guid";
			} else {
#if TAPIR_AC_VERSION == 25
				const auto slot = layerStateByIndex.find (entryIndex);
				if (slot != layerStateByIndex.end () && slot->second == NativeDimensionLayerControlPolicy::SlotState::Tombstone) {
					entry.resolution = "tombstone"; entry.lookupError = APIERR_DELETED;
				} else {
					API_Attribute lookup = {};
					lookup.header.typeID = API_LayerID; lookup.header.index = ACAPI_CreateAttributeIndex (entryIndex);
					entry.lookupError = ACAPI_Attribute_Get (&lookup);
					const GS::OnExit lookupGuard ([&lookup] () { DisposeAttribute (lookup); });
					entry.resolution = "unresolved-reference"; unresolvedReference = true;
				}
#else
				API_Attribute lookup = {};
				lookup.header.typeID = API_LayerID; lookup.header.index = ACAPI_CreateAttributeIndex (entryIndex);
				entry.lookupError = ACAPI_Attribute_Get (&lookup);
				const GS::OnExit lookupGuard ([&lookup] () { DisposeAttribute (lookup); });
				if (entry.lookupError == APIERR_BADINDEX || entry.lookupError == APIERR_DELETED) entry.resolution = "absent-from-live-enumeration";
				else { entry.resolution = "unresolved-reference"; unresolvedReference = true; }
#endif
			}
			fact.entries.push_back (entry);
			return true;
		};
#ifdef ServerMainVers_2800
		if (definition.layer_statItems == nullptr && fact.declaredLayerCount != 0) {
			inventory.error = "combination-layer-map-absent";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-definition", "index", index);
			return inventory;
		}
		if (definition.layer_statItems != nullptr) {
			for (auto iterator = definition.layer_statItems->BeginPairs (); iterator != nullptr; ++iterator) {
				if (NativeDimensionLayerControlExpired (started, processControl)) {
					inventory.error = "cooperative-deadline-during-entry-inventory-no-prefix-success";
					inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-entry", "index", index);
					return inventory;
				}
				const Int32 entryIndex = GetAttributeIndex (iterator->key);
				if (entryIndex < 1 || !appendEntry (entryIndex, iterator->value.lFlags, iterator->value.conClassId)) {
					inventory.error = "combination-entry-index-unmapped-or-duplicate";
					inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-entry", "index", index, "entryIndex", entryIndex);
					return inventory;
				}
			}
		}
#else
		if (fact.declaredLayerCount > 0 && (definition.layer_statItems == nullptr || *definition.layer_statItems == nullptr)) {
			inventory.error = "combination-layer-array-absent";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-definition", "index", index);
			return inventory;
		}
		// layer_statItems is the SDK-owned handle (API_LayerStat**), not the
		// dereferenced payload.  Measure the handle before indexing its entries.
		if (fact.declaredLayerCount > 0 && !NativeDimensionLayerControlPolicy::HasStorageForEntries (
			static_cast<UInt64> (BMGetHandleSize (reinterpret_cast<GSHandle> (definition.layer_statItems))),
			static_cast<UInt64> (fact.declaredLayerCount), sizeof (API_LayerStat))) {
			inventory.error = "combination-layer-array-shorter-than-declared-count";
			inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-definition", "index", index,
				"declaredLayerCount", fact.declaredLayerCount,
				"minimumBytes", static_cast<Int64> (fact.declaredLayerCount) * static_cast<Int64> (sizeof (API_LayerStat)));
			return inventory;
		}
		for (Int32 entry = 0; entry < fact.declaredLayerCount; ++entry) {
			if (NativeDimensionLayerControlExpired (started, processControl)) {
				inventory.error = "cooperative-deadline-during-entry-inventory-no-prefix-success";
				inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-entry", "index", index);
				return inventory;
			}
			const API_LayerStat& layerStat = (*definition.layer_statItems)[entry];
			const Int32 entryIndex = GetAttributeIndex (layerStat.lInd);
			if (entryIndex < 1 || !appendEntry (entryIndex, layerStat.lFlags, layerStat.conClassId)) {
				inventory.error = "combination-entry-index-unmapped-or-duplicate";
				inventory.facts = GS::ObjectState ("status", "partial", "stage", "combination-entry", "index", index, "entryIndex", entryIndex);
				return inventory;
			}
		}
#endif
		if (unresolvedReference || fact.entries.size () != static_cast<std::size_t> (fact.declaredLayerCount)
			|| !NativeDimensionLayerControlPolicy::IsValidEntryCount (fact.entries.size (), NativeDimensionLayerControlPolicy::MaxLayers)) {
			inventory.error = "combination-entry-enumeration-incomplete";
			inventory.combinations.push_back (fact);
			NativeDimensionLayerCombinationSlotFact observedSlot; observedSlot.index = index; observedSlot.combination = fact;
			inventory.combinationSlots.push_back (observedSlot);
			inventory.facts = NativeDimensionLayerInventoryState (inventory, "partial", inventory.error);
			inventory.facts.Add ("stage", "combination-entry");
			inventory.facts.Add ("index", index);
			inventory.facts.Add ("observedEntryCount", static_cast<Int64> (fact.entries.size ()));
			inventory.facts.Add ("declaredEntryCount", fact.declaredLayerCount);
			inventory.facts.Add ("unresolvedReference", unresolvedReference);
			return inventory;
		}
		std::sort (fact.entries.begin (), fact.entries.end (), [] (const auto& left, const auto& right) { return left.layerIndex < right.layerIndex; });
		totalEntries += fact.entries.size ();
		if (!NativeDimensionLayerControlPolicy::IsValidReadCount (totalEntries, NativeDimensionLayerControlPolicy::MaxLayerEntries)) {
			inventory.error = "combination-entry-budget-exceeded";
			inventory.facts = GS::ObjectState ("status", "refused", "reason", inventory.error);
			return inventory;
		}
		NativeDimensionLayerCombinationSlotFact slot; slot.index = index; slot.combination = fact;
		inventory.combinationSlots.push_back (slot);
		inventory.combinationMaximumIndex = std::max (inventory.combinationMaximumIndex, static_cast<UInt64> (index));
		inventory.combinations.push_back (std::move (fact));
	}
	API_AttributeIndex currentCombination = APIInvalidAttributeIndex;
#if TAPIR_AC_VERSION == 25
	const GSErrCode currentCombinationError = ACAPI_Environment (APIEnv_GetCurrLayerCombID, &currentCombination);
#else
	const GSErrCode currentCombinationError = ACAPI_Navigator_GetCurrLayerComb (&currentCombination);
#endif
	inventory.currentCombinationError = currentCombinationError;
	inventory.currentCombinationIndex = currentCombinationError == NoError ? GetAttributeIndex (currentCombination) : -1;
	std::sort (inventory.layerSlots.begin (), inventory.layerSlots.end (), [] (const auto& left, const auto& right) { return left.index < right.index; });
	std::sort (inventory.combinationSlots.begin (), inventory.combinationSlots.end (), [] (const auto& left, const auto& right) { return left.index < right.index; });
	const UInt64 layerTombstones = static_cast<UInt64> (std::count_if (inventory.layerSlots.begin (), inventory.layerSlots.end (), [] (const auto& slot) { return slot.state == NativeDimensionLayerControlPolicy::SlotState::Tombstone; }));
	const UInt64 combinationTombstones = static_cast<UInt64> (std::count_if (inventory.combinationSlots.begin (), inventory.combinationSlots.end (), [] (const auto& slot) { return slot.state == NativeDimensionLayerControlPolicy::SlotState::Tombstone; }));
	const bool highestIndexIsBound = TAPIR_AC_VERSION == 25;
	if (!NativeDimensionLayerControlPolicy::IsConsistentSparseCount (highestIndexIsBound, layerCount,
		static_cast<UInt64> (inventory.layers.size ()), layerTombstones, static_cast<UInt64> (inventory.layerSlots.size ()), inventory.layerMaximumIndex)
		|| !NativeDimensionLayerControlPolicy::IsConsistentSparseCount (highestIndexIsBound, combinationCount,
			static_cast<UInt64> (inventory.combinations.size ()), combinationTombstones, static_cast<UInt64> (inventory.combinationSlots.size ()), inventory.combinationMaximumIndex)) {
		inventory.error = "sparse-inventory-count-semantics-inconsistent";
		inventory.facts = GS::ObjectState ("status", "partial", "stage", "inventory-count-validation",
			"layerReportedCount", static_cast<Int64> (layerCount), "layerLiveCount", static_cast<Int64> (inventory.layers.size ()),
			"layerTombstoneCount", static_cast<Int64> (layerTombstones), "layerSlotCount", static_cast<Int64> (inventory.layerSlots.size ()),
			"combinationReportedCount", static_cast<Int64> (combinationCount), "combinationLiveCount", static_cast<Int64> (inventory.combinations.size ()),
			"combinationTombstoneCount", static_cast<Int64> (combinationTombstones), "combinationSlotCount", static_cast<Int64> (inventory.combinationSlots.size ()));
		return inventory;
	}
	GS::ObjectState facts = NativeDimensionLayerInventoryState (inventory, "complete");
	GS::UniString serialized;
	if (JSON::CreateFromObjectState (facts, serialized, false) != NoError) {
		inventory.error = "inventory-serialization-failed";
		inventory.facts = GS::ObjectState ("status", "unavailable", "reason", inventory.error);
		return inventory;
	}
	if (NativeDimensionLayerControlExpired (started, processControl)) {
		inventory.error = "cooperative-deadline-after-inventory-serialization";
		inventory.facts = GS::ObjectState ("status", "partial", "reason", inventory.error, "serializedFactBytes", static_cast<Int64> (FixtureUtf8 (serialized).size ()));
		return inventory;
	}
	inventory.serializedBytes = FixtureUtf8 (serialized).size ();
	if (inventory.serializedBytes > NativeDimensionLayerControlPolicy::MaxSerializedFactBytes) {
		inventory.error = "inventory-serialized-limit-exceeded-no-successful-prefix";
		inventory.facts = GS::ObjectState ("status", "refused", "reason", inventory.error, "serializedFactBytes", static_cast<Int64> (inventory.serializedBytes));
		return inventory;
	}
	inventory.facts = facts;
	inventory.serialized = serialized;
	inventory.complete = true;
	return inventory;
}

bool NativeDimensionLayerControlGuidEqual (const GS::UniString& left, const GS::UniString& right)
{
	return NativeDimensionAuditPathKey (left) == NativeDimensionAuditPathKey (right);
}

const char* NativeDimensionLayerControlPhaseName (const NativeDimensionLayerControlPolicy::Phase phase)
{
	switch (phase) {
		case NativeDimensionLayerControlPolicy::Phase::Empty: return "empty";
		case NativeDimensionLayerControlPolicy::Phase::Registered: return "registered";
		case NativeDimensionLayerControlPolicy::Phase::Prepared: return "prepared";
		case NativeDimensionLayerControlPolicy::Phase::Hidden: return "hidden";
		case NativeDimensionLayerControlPolicy::Phase::Locked: return "locked";
		case NativeDimensionLayerControlPolicy::Phase::Unknown: return "unknown";
	}
	return "unknown";
}

GS::ObjectState NativeDimensionLayerControlResponse (const char* status, const char* reason, const GS::UniString& action,
	const GS::UniString& runRef, const GS::UniString& phase, const GS::ObjectState& facts)
{
	return GS::ObjectState ("schemaVersion", NativeDimensionLayerControlSchema, "status", status, "reason", reason,
		"preparationPolicy", NativeDimensionLayerControlPreparationPolicy,
		"action", action, "runRef", runRef, "phase", phase, "facts", facts);
}

} // namespace

NativeDimensionLayerControlCommand::NativeDimensionLayerControlCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String NativeDimensionLayerControlCommand::GetName () const { return "NativeDimensionLayerControl"; }
GS::Optional<GS::UniString> NativeDimensionLayerControlCommand::GetInputParametersSchema () const
{
	return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-layer-control/v2"]},"preparationPolicy":{"type":"string","enum":["absent-owned-entry-flags-0-or-1-class-1/v2"]},"action":{"type":"string","enum":["begin","prepare","setHidden","setLocked","inspect"]},"runId":{"type":"string","minLength":1,"maxLength":32,"pattern":"^[A-Za-z0-9][A-Za-z0-9_-]*$"},"initialNonce":{"type":"string","minLength":16,"maxLength":96,"pattern":"^[A-Za-z0-9_-]+$"},"runRef":{"type":"string","minLength":1,"maxLength":96},"stageNonce":{"type":"string","minLength":1,"maxLength":96},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer"}},"required":["schemaVersion","preparationPolicy","action"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> NativeDimensionLayerControlCommand::GetResponseSchema () const { return {}; }
GS::ObjectState NativeDimensionLayerControlCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
	GS::UniString schemaVersion, preparationPolicy, action, runId, initialNonce, runRef, stageNonce, expectedProjectPath, fileScopeKey;
	Int32 requestedStory = -1;
	if (!parameters.Get ("schemaVersion", schemaVersion) || schemaVersion != NativeDimensionLayerControlSchema
		|| !parameters.Get ("preparationPolicy", preparationPolicy) || preparationPolicy != NativeDimensionLayerControlPreparationPolicy
		|| !parameters.Get ("action", action) || (action != "begin" && action != "prepare" && action != "setHidden" && action != "setLocked" && action != "inspect"))
		return NativeDimensionLayerControlResponse ("refused", "invalid-closed-action", action, GS::UniString (), "empty", GS::ObjectState ());
	if (action == "begin") {
		if (parameters.GetFieldNames ().GetSize () != 8 || !parameters.Get ("runId", runId) || !parameters.Get ("initialNonce", initialNonce)
			|| !parameters.Get ("expectedProjectPath", expectedProjectPath) || !parameters.Get ("fileScopeKey", fileScopeKey)
			|| !parameters.Get ("storyIndex", requestedStory))
			return NativeDimensionLayerControlResponse ("refused", "begin-request-shape-invalid", action, GS::UniString (), "empty", GS::ObjectState ());
	} else {
		const UInt32 expectedFields = action == "inspect" ? 4 : 5;
		if (parameters.GetFieldNames ().GetSize () != expectedFields || !parameters.Get ("runRef", runRef)
			|| (action == "inspect" ? false : !parameters.Get ("stageNonce", stageNonce)))
			return NativeDimensionLayerControlResponse ("refused", "stage-request-shape-invalid", action, runRef, "unknown", GS::ObjectState ());
	}
	const auto started = std::chrono::steady_clock::now ();
	if (action == "begin") {
		const std::string runUtf8 = FixtureUtf8 (runId), nonceUtf8 = FixtureUtf8 (initialNonce);
		if (runUtf8.empty () || runUtf8.size () > 32 || !std::all_of (runUtf8.begin (), runUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; })
			|| nonceUtf8.size () < 16 || nonceUtf8.size () > 96 || !std::all_of (nonceUtf8.begin (), nonceUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; })
			|| expectedProjectPath.IsEmpty () || FixtureUtf8 (expectedProjectPath).size () > 1024
			|| fileScopeKey.IsEmpty () || FixtureUtf8 (fileScopeKey).size () > 1100
			|| requestedStory < std::numeric_limits<short>::min () || requestedStory > std::numeric_limits<short>::max ())
			return NativeDimensionLayerControlResponse ("refused", "begin-values-out-of-bounds", action, GS::UniString (), "empty", GS::ObjectState ());
		if (gNativeDimensionLayerControlRun.active || gUsedNativeDimensionLayerControlNonces.count (nonceUtf8) != 0)
			return NativeDimensionLayerControlResponse ("refused", gNativeDimensionLayerControlRun.active ? "one-run-limit" : "initial-nonce-replay", action, GS::UniString (), "empty", GS::ObjectState ());
		if (gUsedNativeDimensionLayerControlNonces.size () >= 512)
			return NativeDimensionLayerControlResponse ("refused", "finite-replay-ledger-full", action, GS::UniString (), "empty", GS::ObjectState ());
		RadialDefaultsContext context;
		ReadNativeDimensionAuditContext (context);
		if (!IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, context)
			|| context.window.typeID != APIWind_FloorPlanID || context.database.typeID != APIWind_FloorPlanID
			|| context.activeStory != requestedStory || context.database.databaseUnId.elemSetId == APINULLGuid)
			return NativeDimensionLayerControlResponse ("refused", "saved-copy-floor-plan-database-story-or-path-mismatch", action, GS::UniString (), "empty", RadialDefaultsContextDetails (context));
		NativeDimensionLayerFocusSnapshot initialFocus = ReadNativeDimensionLayerFocus ();
		if (!initialFocus.complete)
			return NativeDimensionLayerControlResponse ("refused", "selection-or-marquee-focus-unavailable-before-begin", action, GS::UniString (), "empty", initialFocus.facts);
		NativeDimensionLayerInventory inventory = ReadNativeDimensionLayerInventory (started, processControl);
		if (!inventory.complete)
			return NativeDimensionLayerControlResponse ("refused", "protected-layer-combination-manifest-incomplete", action, GS::UniString (), "empty", inventory.facts);
		NativeDimensionLayerControlRun run;
		run.active = true;
		run.preparationPolicy = preparationPolicy;
		run.phase = NativeDimensionLayerControlPolicy::Phase::Registered;
		run.runId = runId;
		run.runIdUtf8 = runUtf8;
		run.runRef = NativeDimensionLayerControlToken ();
		run.nextNonce = NativeDimensionLayerControlToken ();
		run.projectPath = context.projectPath;
		run.fileScopeKey = fileScopeKey;
		run.databaseGuid = context.database.databaseUnId.elemSetId;
		run.storyIndex = requestedStory;
		run.processId = GetCurrentProcessId ();
		run.started = started;
		run.initialLayers = inventory.layers;
		run.initialLayerSlots = inventory.layerSlots;
		run.initialCombinations = inventory.combinations;
		run.initialCombinationSlots = inventory.combinationSlots;
		run.initialCurrentCombinationError = inventory.currentCombinationError;
		run.initialCurrentCombinationIndex = inventory.currentCombinationIndex;
		run.initialFocus = initialFocus.facts;
		run.initialFocusFacts = initialFocus.serialized;
		const std::string suffix = FixtureUtf8 (NativeDimensionLayerControlToken ());
		const std::string asciiName = "T3I_" + suffix.substr (suffix.size () > 12 ? suffix.size () - 12 : 0);
		run.layerName = GS::UniString (asciiName.c_str (), CC_UTF8);
		run.initialLayerFacts = "";
		run.initialCombinationFacts = "";
		GS::ObjectState initialLayerFactState;
		const auto& layerFacts = initialLayerFactState.AddList<GS::ObjectState> ("layers");
		for (const auto& layer : inventory.layers) layerFacts (NativeDimensionLayerFactState (layer));
		GS::ObjectState initialComboFactState;
		const auto& comboFacts = initialComboFactState.AddList<GS::ObjectState> ("combinations");
		for (const auto& combo : inventory.combinations) comboFacts (NativeDimensionLayerCombinationFactState (combo));
		GS::UniString layerText, comboText;
		if (JSON::CreateFromObjectState (initialLayerFactState, layerText, false) != NoError || JSON::CreateFromObjectState (initialComboFactState, comboText, false) != NoError)
			return NativeDimensionLayerControlResponse ("refused", "protected-manifest-serialization-failed", action, GS::UniString (), "empty", inventory.facts);
		run.initialLayerFacts = FixtureUtf8 (layerText);
		run.initialCombinationFacts = FixtureUtf8 (comboText);
		run.beginFacts = inventory.facts;
		GS::ObjectState facts ("runId", runId, "runRef", run.runRef, "preparationPolicy", run.preparationPolicy,
			"nextAction", "prepare", "nextStageNonce", run.nextNonce,
			"processId", static_cast<Int32> (run.processId), "fileScopeKey", fileScopeKey,
			"databaseGuid", APIGuidToString (run.databaseGuid), "storyIndex", requestedStory,
			"layerNamePrefix", "T3I_", "runTokenStatus", "process-local-single-use",
			"context", RadialDefaultsContextDetails (context), "focus", initialFocus.facts, "originalManifest", inventory.facts);
		gUsedNativeDimensionLayerControlNonces.insert (nonceUtf8);
		gNativeDimensionLayerControlRun = std::move (run);
		return NativeDimensionLayerControlResponse ("registered", "read-only-begin-complete", action, gNativeDimensionLayerControlRun.runRef, "registered", facts);
	}

	NativeDimensionLayerControlRun& run = gNativeDimensionLayerControlRun;
	if (!run.active || run.runRef != runRef || run.preparationPolicy != preparationPolicy || GetCurrentProcessId () != run.processId)
		return NativeDimensionLayerControlResponse ("refused", "unknown-run-or-process-restart", action, runRef, "unknown", GS::ObjectState ());
	if (action == "inspect") {
		RadialDefaultsContext context;
		ReadNativeDimensionAuditContext (context);
		NativeDimensionLayerInventory inventory = ReadNativeDimensionLayerInventory (started, processControl);
		NativeDimensionLayerFocusSnapshot focus = ReadNativeDimensionLayerFocus ();
		GS::ObjectState facts ("context", RadialDefaultsContextDetails (context), "focus", focus.facts, "inventory", inventory.facts,
			"ownedLayerGuid", run.layerGuid == APINULLGuid ? GS::UniString () : APIGuidToString (run.layerGuid),
			"ownedParentGuid", run.parentGuid == APINULLGuid ? GS::UniString () : APIGuidToString (run.parentGuid),
			"prepareFacts", run.prepareFacts,
			"phase", NativeDimensionLayerControlPhaseName (run.phase), "permission", "read-only-inspect-never-restores-write-permission");
		return NativeDimensionLayerControlResponse ("observed", inventory.complete ? "inspection-read-only" : "inspection-incomplete", action, run.runRef, "inspection", facts);
	}
	if (run.phase == NativeDimensionLayerControlPolicy::Phase::Unknown || run.nextNonce != stageNonce)
		return NativeDimensionLayerControlResponse ("refused", "wrong-replayed-or-terminal-stage-token", action, run.runRef, "unknown", GS::ObjectState ("tokenConsumed", true));
	const NativeDimensionLayerControlPolicy::Action requestedStage = action == "prepare" ? NativeDimensionLayerControlPolicy::Action::Prepare
		: action == "setHidden" ? NativeDimensionLayerControlPolicy::Action::SetHidden : NativeDimensionLayerControlPolicy::Action::SetLocked;
	const bool expectedAction = NativeDimensionLayerControlPolicy::NextPhase (run.phase, requestedStage) != NativeDimensionLayerControlPolicy::Phase::Unknown;
	if (!expectedAction)
		return NativeDimensionLayerControlResponse ("refused", "out-of-order-stage", action, run.runRef, "registered", GS::ObjectState ("tokenConsumed", false));
	RadialDefaultsContext beforeContext;
	ReadNativeDimensionAuditContext (beforeContext);
	if (!IsExactTaskOwnedRadialDefaultsCopy (run.runId, run.projectPath, run.fileScopeKey, beforeContext)
		|| beforeContext.window.typeID != APIWind_FloorPlanID || beforeContext.database.typeID != APIWind_FloorPlanID
		|| beforeContext.database.databaseUnId.elemSetId != run.databaseGuid || beforeContext.activeStory != run.storyIndex
		|| NativeDimensionLayerControlExpired (started, processControl))
		return NativeDimensionLayerControlResponse ("refused", "fresh-scope-or-deadline-guard-failed-before-write", action, run.runRef, "unchanged", RadialDefaultsContextDetails (beforeContext));
	NativeDimensionLayerInventory beforeInventory = ReadNativeDimensionLayerInventory (started, processControl);
	if (!beforeInventory.complete)
		return NativeDimensionLayerControlResponse ("refused", "fresh-layer-combination-inventory-incomplete-before-write", action, run.runRef, "unchanged", beforeInventory.facts);
	if ((action == "prepare" && !NativeDimensionLayerInventoryMatchesInitial (run, beforeInventory))
		|| (action != "prepare" && !NativeDimensionLayerStageFactsUnchanged (run, beforeInventory,
			run.phase == NativeDimensionLayerControlPolicy::Phase::Hidden,
			run.phase == NativeDimensionLayerControlPolicy::Phase::Locked)))
		return NativeDimensionLayerControlResponse ("refused", "protected-layer-or-combination-facts-changed-before-write", action, run.runRef, "unchanged", beforeInventory.facts);
	GS::ObjectState parentBeforeRaw;
	if (action != "prepare") {
		parentBeforeRaw = ReadNativeDimensionFixtureRow (run.parentGuid, processControl);
		GS::ObjectState parentBeforeRow;
		GS::UniString parentBeforeText;
		UInt64 parentBeforeBytes = 0;
		if (!ExtractExactNativeDimensionFixtureRow (parentBeforeRaw, run.parentGuid, parentBeforeRow)
			|| !NativeDimensionAuditFactsText (parentBeforeRow, parentBeforeText, parentBeforeBytes)
			|| FixtureUtf8 (parentBeforeText) != run.preparedParentFacts)
			return NativeDimensionLayerControlResponse ("refused", "parent-facts-or-revision-changed-before-write", action, run.runRef, "unchanged", parentBeforeRaw);
	}
	if ((action == "setHidden" || action == "setLocked") && (run.layerGuid == APINULLGuid || run.parentGuid == APINULLGuid))
		return NativeDimensionLayerControlResponse ("refused", "owned-layer-or-parent-identity-unavailable", action, run.runRef, "unchanged", GS::ObjectState ());
	if (action == "setHidden" || action == "setLocked") {
		const bool expectedVisible = run.phase != NativeDimensionLayerControlPolicy::Phase::Hidden;
		const bool expectedEditable = run.phase == NativeDimensionLayerControlPolicy::Phase::Prepared;
		const bool onActualFloor = ACAPI_Element_Filter (run.parentGuid, APIFilt_OnActFloor);
		const bool onVisibleLayer = ACAPI_Element_Filter (run.parentGuid, APIFilt_OnVisLayer);
		const bool isEditable = ACAPI_Element_Filter (run.parentGuid, APIFilt_IsEditable);
		if (!onActualFloor || onVisibleLayer != expectedVisible || isEditable != expectedEditable)
			return NativeDimensionLayerControlResponse ("refused", "parent-current-filter-facts-changed-before-write", action, run.runRef, "unchanged",
				GS::ObjectState ("onActualFloor", onActualFloor, "onVisibleLayer", onVisibleLayer, "isEditable", isEditable,
					"expectedOnVisibleLayer", expectedVisible, "expectedIsEditable", expectedEditable));
	}
	NativeDimensionLayerFocusSnapshot focusBeforeWrite = ReadNativeDimensionLayerFocus ();
	if (!focusBeforeWrite.complete || focusBeforeWrite.serialized != run.initialFocusFacts)
		return NativeDimensionLayerControlResponse ("refused", "selection-or-marquee-focus-changed-before-write", action, run.runRef, "unchanged", GS::ObjectState ("focus", focusBeforeWrite.facts, "expectedFocus", run.initialFocus));

	// Stage nonce is consumed immediately before the first SDK write. A timeout
	// or error after this point can never issue a retry token.
	const GS::UniString consumedToken = run.nextNonce;
	GS::ObjectState stageFacts;
	if (action == "prepare") {
		for (const auto& layer : beforeInventory.layers) {
			if (NativeDimensionAuditPathKey (layer.name) == NativeDimensionAuditPathKey (run.layerName))
				return NativeDimensionLayerControlResponse ("refused", "fixed-layer-name-collision-before-write", action, run.runRef, "registered", NativeDimensionLayerFactState (layer));
		}
		PreparedLinearStaticFixtureDefaults defaults;
		GS::ObjectState defaultsFacts;
		if (!ReadLinearStaticFixtureDefaultFacts (defaultsFacts, defaults) || defaults.linear.dimension.defNote.contentUStr != nullptr
			|| defaults.memo.dimElems != nullptr || defaults.linear.dimension.nDimElem != 0)
			return NativeDimensionLayerControlResponse ("refused", "linear-default-or-memo-ownership-not-proven-before-first-write", action, run.runRef, "registered", defaultsFacts);
		if (NativeDimensionLayerControlExpired (started, processControl))
			return NativeDimensionLayerControlResponse ("refused", "cooperative-stop-before-first-write", action, run.runRef, "registered", defaultsFacts);
		NativeDimensionLayerFocusSnapshot focusAtFirstWrite = ReadNativeDimensionLayerFocus ();
		if (!focusAtFirstWrite.complete || focusAtFirstWrite.serialized != run.initialFocusFacts || NativeDimensionLayerControlExpired (started, processControl))
			return NativeDimensionLayerControlResponse ("refused", "selection-or-marquee-focus-changed-before-layer-create", action, run.runRef, "registered", GS::ObjectState ("focus", focusAtFirstWrite.facts, "expectedFocus", run.initialFocus));
		run.nextNonce.Clear ();
		run.phase = NativeDimensionLayerControlPolicy::Phase::Unknown;
		API_Attribute newLayer = {};
		newLayer.header.typeID = API_LayerID;
		newLayer.header.uniStringNamePtr = &run.layerName;
		const GSErrCode layerCreateError = ACAPI_Attribute_Create (&newLayer, nullptr);
		stageFacts.Add ("layerCreate", GS::ObjectState ("status", layerCreateError == NoError ? "returned" : "error", "errorCode", static_cast<Int32> (layerCreateError), "guid", APIGuidToString (newLayer.header.guid), "index", GetAttributeIndex (newLayer.header.index), "name", run.layerName, "undoable", false));
		if (layerCreateError != NoError || newLayer.header.guid == APINULLGuid) {
			run.phase = NativeDimensionLayerControlPolicy::Phase::Unknown;
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "nonundoable-layer-create-outcome-not-qualified", action, run.runRef, "unknown", stageFacts);
		}
		run.layerGuid = newLayer.header.guid;
		run.layerIndex = newLayer.header.index;
		// Exact owned identity is retained in process state before any following SDK write.
		API_Attribute verifyLayer = {};
		GS::UniString verifiedName;
		verifyLayer.header.typeID = API_LayerID;
		verifyLayer.header.guid = run.layerGuid;
		verifyLayer.header.uniStringNamePtr = &verifiedName;
		const GSErrCode verifyLayerError = ACAPI_Attribute_Get (&verifyLayer);
		const bool layerIdentityExact = verifyLayerError == NoError && verifyLayer.header.guid == run.layerGuid
			&& GetAttributeIndex (verifyLayer.header.index) == GetAttributeIndex (run.layerIndex)
			&& verifiedName == run.layerName
			&& NativeDimensionLayerControlPolicy::IsExactNewOwnedLayerState (verifyLayer.header.flags, verifyLayer.layer.conClassId);
		stageFacts.Add ("layerReadback", GS::ObjectState ("status", layerIdentityExact ? "exact-flags0-class0-owned-layer" : "identity-or-default-state-unavailable", "errorCode", static_cast<Int32> (verifyLayerError), "guid", APIGuidToString (verifyLayer.header.guid), "index", GetAttributeIndex (verifyLayer.header.index), "flags", verifyLayer.header.flags, "connectionClass", verifyLayer.layer.conClassId, "name", verifiedName));
		if (!layerIdentityExact || NativeDimensionLayerControlExpired (started, processControl)) {
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "created-layer-identity-or-default-state-unverified", action, run.runRef, "unknown", stageFacts);
		}
		// Attribute_Create is non-undoable.  Before the separate parent write,
		// retain and inspect a fresh complete inventory and independently prove
		// the exact one-layer/one-entry-per-combination delta.
		NativeDimensionLayerInventory afterLayerCreate = ReadNativeDimensionLayerInventory (started, processControl);
		RadialDefaultsContext afterLayerContext;
		ReadNativeDimensionAuditContext (afterLayerContext);
		NativeDimensionLayerFocusSnapshot afterLayerFocus = ReadNativeDimensionLayerFocus ();
		const bool layerCreateContextStable = IsExactTaskOwnedRadialDefaultsCopy (run.runId, run.projectPath, run.fileScopeKey, afterLayerContext)
			&& afterLayerContext.window.typeID == APIWind_FloorPlanID && afterLayerContext.database.typeID == APIWind_FloorPlanID
			&& afterLayerContext.database.databaseUnId.elemSetId == run.databaseGuid && afterLayerContext.activeStory == run.storyIndex
			&& beforeContext.database.databaseUnId.elemSetId == afterLayerContext.database.databaseUnId.elemSetId
			&& beforeContext.activeStory == afterLayerContext.activeStory;
		stageFacts.Add ("postLayerCreateInventory", afterLayerCreate.facts);
		stageFacts.Add ("contextAfterLayerCreate", RadialDefaultsContextDetails (afterLayerContext));
		stageFacts.Add ("focusAfterLayerCreate", afterLayerFocus.facts);
		stageFacts.Add ("layerCreateContextStable", layerCreateContextStable);
		const bool layerPreparationExact = afterLayerCreate.complete
			&& NativeDimensionLayerPreparationDeltaExact (beforeInventory, afterLayerCreate, run.layerGuid)
			&& layerCreateContextStable && afterLayerFocus.complete && afterLayerFocus.serialized == run.initialFocusFacts
			&& !NativeDimensionLayerControlExpired (started, processControl);
		stageFacts.Add ("layerPreparationDeltaExactBeforeParentWrite", layerPreparationExact);
		run.prepareFacts = stageFacts;
		if (!layerPreparationExact)
			return NativeDimensionLayerControlResponse ("unknown", "nonundoable-layer-delta-or-context-unproven-parent-write-stopped", action, run.runRef, "unknown", stageFacts);
		defaults.linear.header.floorInd = static_cast<short> (run.storyIndex);
		defaults.linear.header.layer = run.layerIndex;
		defaults.linear.dimension.dimAppear = APIApp_Normal;
		defaults.linear.dimension.textPos = APIPos_Above;
		defaults.linear.dimension.textWay = APIDir_Horizontal;
		defaults.linear.dimension.defStaticDim = true;
		defaults.linear.dimension.usedIn3D = false;
		defaults.linear.dimension.horizontalText = false;
		defaults.linear.dimension.pushNeighborEnabled = false;
		defaults.linear.dimension.refC = { 20.0, 20.0 };
		defaults.linear.dimension.direction = { 1.0, 0.0 };
		defaults.linear.dimension.defNote.contentType = API_NoteContent_Measured;
		defaults.linear.dimension.defNote.contentUStr = nullptr;
		std::fill (defaults.linear.dimension.defNote.content, defaults.linear.dimension.defNote.content + sizeof (defaults.linear.dimension.defNote.content), '\0');
		defaults.linear.dimension.defNote.fixPos = false;
		defaults.linear.dimension.defNote.useLeaderLine = false;
		API_DimElem** memoElems = reinterpret_cast<API_DimElem**> (BMhAllClear (2 * sizeof (API_DimElem)));
		if (memoElems == nullptr || *memoElems == nullptr) {
			if (memoElems != nullptr) BMKillHandle (reinterpret_cast<GSHandle*> (&memoElems));
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "parent-memo-allocation-failed-after-layer-create", action, run.runRef, "unknown", stageFacts);
		}
		defaults.memo.dimElems = memoElems;
		defaults.linear.dimension.nDimElem = 2;
		for (Int32 index = 0; index < 2; ++index) {
			API_DimElem& point = (*defaults.memo.dimElems)[index];
			point.base.loc = { 20.0 + 4.0 * index, 15.0 };
			point.pos = { 20.0 + 4.0 * index, 20.0 };
			point.note = defaults.linear.dimension.defNote;
			point.note.contentType = API_NoteContent_Measured;
			point.note.contentUStr = nullptr;
			std::fill (point.note.content, point.note.content + sizeof (point.note.content), '\0');
			point.note.fixPos = false;
			point.note.useLeaderLine = false;
			point.witnessVal = defaults.linear.dimension.defWitnessVal;
			point.witnessForm = defaults.linear.dimension.defWitnessForm;
			point.fixedPos = true;
		}
		if (NativeDimensionLayerControlExpired (started, processControl)) {
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "cooperative-stop-after-nonundoable-layer-create-before-parent-create", action, run.runRef, "unknown", stageFacts);
		}
		NativeDimensionLayerFocusSnapshot focusAtParentCreate;
		bool parentCreateFocusAccepted = false;
		GSErrCode parentCreateError = APIERR_GENERAL;
		parentCreateError = ACAPI_CallUndoableCommand ("Create T3i Layer Inventory Dimension Fixture", [&] () -> GSErrCode {
			if (NativeDimensionLayerControlExpired (started, processControl)) return APIERR_CANCEL;
			focusAtParentCreate = ReadNativeDimensionLayerFocus ();
			parentCreateFocusAccepted = focusAtParentCreate.complete && focusAtParentCreate.serialized == run.initialFocusFacts
				&& !NativeDimensionLayerControlExpired (started, processControl);
			if (!parentCreateFocusAccepted) return APIERR_CANCEL;
			return ACAPI_Element_Create (&defaults.linear, &defaults.memo);
		});
		stageFacts.Add ("focusAtParentCreate", focusAtParentCreate.facts);
		stageFacts.Add ("parentCreateFocusAccepted", parentCreateFocusAccepted);
		stageFacts.Add ("parentCreate", GS::ObjectState ("status", parentCreateError == NoError && defaults.linear.header.guid != APINULLGuid ? "created" : "error-or-unknown", "errorCode", static_cast<Int32> (parentCreateError), "guid", APIGuidToString (defaults.linear.header.guid), "nativeTypeId", static_cast<Int32> (GetElemTypeId (defaults.linear.header)), "layerIndex", GetAttributeIndex (defaults.linear.header.layer), "undoable", true));
		if (parentCreateError != NoError || defaults.linear.header.guid == APINULLGuid) {
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "parent-create-outcome-not-qualified-layer-remains-no-compensation", action, run.runRef, "unknown", stageFacts);
		}
		run.parentGuid = defaults.linear.header.guid;
		const NativeDimensionLayerInventory afterInventory = ReadNativeDimensionLayerInventory (started, processControl);
		GS::ObjectState parentRaw = ReadNativeDimensionFixtureRow (run.parentGuid, processControl);
		GS::UniString parentText;
		UInt64 parentBytes = 0;
		const bool parentSerialized = NativeDimensionAuditFactsText (parentRaw, parentText, parentBytes);
		stageFacts.Add ("postPrepareInventory", afterInventory.facts);
		stageFacts.Add ("parentReader", parentRaw);
		stageFacts.Add ("parentReaderStatus", parentSerialized ? "raw-preserved" : "serialization-unavailable");
		stageFacts.Add ("contextAtPrepare", RadialDefaultsContextDetails (beforeContext));
		RadialDefaultsContext afterPrepareContext;
		ReadNativeDimensionAuditContext (afterPrepareContext);
		const bool prepareContextStable = IsExactTaskOwnedRadialDefaultsCopy (run.runId, run.projectPath, run.fileScopeKey, afterPrepareContext)
			&& afterPrepareContext.window.typeID == APIWind_FloorPlanID && afterPrepareContext.database.typeID == APIWind_FloorPlanID
			&& afterPrepareContext.database.databaseUnId.elemSetId == run.databaseGuid && afterPrepareContext.activeStory == run.storyIndex
			&& beforeContext.database.databaseUnId.elemSetId == afterPrepareContext.database.databaseUnId.elemSetId
			&& beforeContext.activeStory == afterPrepareContext.activeStory;
		stageFacts.Add ("contextAfterPrepare", RadialDefaultsContextDetails (afterPrepareContext));
		stageFacts.Add ("contextStable", prepareContextStable);
		GS::ObjectState parentRow;
		const bool parentEnvelopeExact = ExtractExactNativeDimensionFixtureRow (parentRaw, run.parentGuid, parentRow);
		GS::UniString parentRowText;
		UInt64 parentRowBytes = 0;
		const bool parentRowSerialized = parentEnvelopeExact && NativeDimensionAuditFactsText (parentRow, parentRowText, parentRowBytes);
		if (!afterInventory.complete || !parentSerialized || !parentRowSerialized
			|| !NativeDimensionLayerPreparationDeltaExact (beforeInventory, afterInventory, run.layerGuid)
			|| !prepareContextStable || NativeDimensionLayerControlExpired (started, processControl)) {
			stageFacts.Add ("preparationDeltaExact", false);
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "post-prepare-facts-incomplete-or-protected-delta", action, run.runRef, "unknown", stageFacts);
		}
		stageFacts.Add ("preparationDeltaExact", true);
		run.preparedCombinationFacts = FixtureUtf8 (afterInventory.serialized);
		run.preparedLayers = afterInventory.layers;
		run.preparedLayerSlots = afterInventory.layerSlots;
		run.preparedCombinations = afterInventory.combinations;
		run.preparedCombinationSlots = afterInventory.combinationSlots;
		run.preparedCurrentCombinationError = afterInventory.currentCombinationError;
		run.preparedCurrentCombinationIndex = afterInventory.currentCombinationIndex;
		run.preparedParentFacts = parentRowSerialized ? FixtureUtf8 (parentRowText) : std::string ();
		GS::ObjectState otherLayers;
		const auto& oldLayerList = otherLayers.AddList<GS::ObjectState> ("layers");
		for (const auto& layer : afterInventory.layers) if (layer.guid != APIGuidToString (run.layerGuid)) oldLayerList (NativeDimensionLayerFactState (layer));
		GS::UniString oldLayersText;
		if (JSON::CreateFromObjectState (otherLayers, oldLayersText, false) != NoError) {
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "post-prepare-protected-layer-serialization-failed", action, run.runRef, "unknown", stageFacts);
		}
		run.preparedOtherLayerFacts = FixtureUtf8 (oldLayersText);
		GS::UniString returnedParentGuid;
		Int32 returnedType = -1;
		Int32 returnedLayerIndex = -1;
		Int32 returnedStory = -1;
		GS::ObjectState rowId;
		GS::ObjectState rowDatabase, rowDatabaseId;
		GS::UniString returnedDatabaseGuid;
		Int32 returnedDatabaseType = -1;
		const bool rowExact = parentRowSerialized && parentRow.Get ("elementId", rowId) && rowId.Get ("guid", returnedParentGuid)
			&& NativeDimensionLayerControlGuidEqual (returnedParentGuid, APIGuidToString (run.parentGuid))
			&& parentRow.Get ("nativeElementTypeId", returnedType) && returnedType == static_cast<Int32> (API_DimensionID)
			&& parentRow.Get ("layerIndex", returnedLayerIndex) && returnedLayerIndex == GetAttributeIndex (run.layerIndex)
			&& parentRow.Get ("storyIndex", returnedStory) && returnedStory == run.storyIndex
			&& parentRow.Get ("database", rowDatabase) && rowDatabase.Get ("databaseType", returnedDatabaseType)
			&& returnedDatabaseType == static_cast<Int32> (APIWind_FloorPlanID)
			&& rowDatabase.Get ("databaseId", rowDatabaseId) && rowDatabaseId.Get ("guid", returnedDatabaseGuid)
			&& NativeDimensionLayerControlGuidEqual (returnedDatabaseGuid, APIGuidToString (run.databaseGuid))
			&& [&] () {
				Int64 stamp = 0;
				if (!parentRow.Get ("modificationStamp", stamp) || stamp <= 0) return false;
				run.parentModificationStamp = static_cast<UInt64> (stamp);
				return true;
			} ();
		stageFacts.Add ("parentExactIdentity", rowExact);
		if (!rowExact) {
			run.prepareFacts = stageFacts;
			return NativeDimensionLayerControlResponse ("unknown", "created-parent-reader-identity-not-exact", action, run.runRef, "unknown", stageFacts);
		}
		run.prepareFacts = stageFacts;
		run.phase = NativeDimensionLayerControlPolicy::Phase::Prepared;
		run.nextNonce = NativeDimensionLayerControlToken ();
		GS::ObjectState facts ("stageNonceConsumed", consumedToken, "createdLayerGuid", APIGuidToString (run.layerGuid),
			"createdLayerIndex", GetAttributeIndex (run.layerIndex), "createdParentGuid", APIGuidToString (run.parentGuid),
			"nextAction", "setHidden", "nextStageNonce", run.nextNonce, "rawStageFacts", stageFacts,
			"inventoryStatus", afterInventory.complete ? "complete" : "partial", "preparationDelta", "one-owned-layer-plus-one-parent-and-layer-entry-per-combination");
		return NativeDimensionLayerControlResponse ("prepared", "nonundoable-layer-then-separate-undoable-parent-created", action, run.runRef, "prepared-visible", facts);
	}

	const bool settingHidden = action == "setHidden";
	const GS::UniString expectedGuid = APIGuidToString (run.layerGuid);
	API_Attribute layer = {};
	GS::UniString layerName;
	layer.header.typeID = API_LayerID;
	layer.header.guid = run.layerGuid;
	layer.header.uniStringNamePtr = &layerName;
	const GSErrCode layerReadError = ACAPI_Attribute_Get (&layer);
	const bool layerIdentityExact = layerReadError == NoError && layer.header.guid == run.layerGuid
		&& GetAttributeIndex (layer.header.index) == GetAttributeIndex (run.layerIndex) && layerName == run.layerName;
	const Int32 expectedFlags = settingHidden ? APILay_Hidden : APILay_Locked;
	const bool expectedPriorFlags = layerIdentityExact
		&& (((layer.header.flags & APILay_Hidden) != 0) == (run.phase == NativeDimensionLayerControlPolicy::Phase::Hidden))
		&& (((layer.header.flags & APILay_Locked) != 0) == (run.phase == NativeDimensionLayerControlPolicy::Phase::Locked));
	if (!layerIdentityExact || !expectedPriorFlags
		|| NativeDimensionLayerControlExpired (started, processControl))
		return NativeDimensionLayerControlResponse ("refused", "owned-layer-identity-or-expected-prior-flags-changed-before-write", action, run.runRef, "unchanged", GS::ObjectState ("errorCode", static_cast<Int32> (layerReadError), "guid", APIGuidToString (layer.header.guid), "index", GetAttributeIndex (layer.header.index), "flags", layer.header.flags, "expectedPriorFlags", expectedPriorFlags));
	NativeDimensionLayerFocusSnapshot focusAtFlagWrite = ReadNativeDimensionLayerFocus ();
	if (!focusAtFlagWrite.complete || focusAtFlagWrite.serialized != run.initialFocusFacts || NativeDimensionLayerControlExpired (started, processControl))
		return NativeDimensionLayerControlResponse ("refused", "selection-or-marquee-focus-changed-before-attribute-modify", action, run.runRef, "unchanged", GS::ObjectState ("focus", focusAtFlagWrite.facts, "expectedFocus", run.initialFocus));
	// Preserve every non-hidden/locked flag and set both target bits explicitly.
	layer.header.flags = static_cast<short> ((layer.header.flags & ~(APILay_Hidden | APILay_Locked)) | expectedFlags);
	run.nextNonce.Clear ();
	run.phase = NativeDimensionLayerControlPolicy::Phase::Unknown;
	const GSErrCode modifyError = ACAPI_Attribute_Modify (&layer, nullptr);
	stageFacts.Add ("attributeModify", GS::ObjectState ("status", modifyError == NoError ? "returned" : "error", "errorCode", static_cast<Int32> (modifyError), "guid", APIGuidToString (layer.header.guid), "index", GetAttributeIndex (layer.header.index), "flags", layer.header.flags, "requestedHidden", settingHidden, "requestedLocked", !settingHidden));
	if (modifyError != NoError) {
		run.prepareFacts.Add ("lastStageFacts", stageFacts);
		return NativeDimensionLayerControlResponse ("unknown", "attribute-modify-outcome-unknown-no-next-stage", action, run.runRef, "unknown", stageFacts);
	}
	API_Attribute changed = {};
	GS::UniString changedName;
	changed.header.typeID = API_LayerID;
	changed.header.guid = run.layerGuid;
	changed.header.uniStringNamePtr = &changedName;
	const GSErrCode changedReadError = ACAPI_Attribute_Get (&changed);
	const bool exactChangedLayer = changedReadError == NoError && changed.header.guid == run.layerGuid
		&& GetAttributeIndex (changed.header.index) == GetAttributeIndex (run.layerIndex) && changedName == run.layerName
		&& ((changed.header.flags & APILay_Hidden) != 0) == settingHidden
		&& ((changed.header.flags & APILay_Locked) != 0) == !settingHidden
		&& (changed.header.flags & ~(APILay_Hidden | APILay_Locked)) == (layer.header.flags & ~(APILay_Hidden | APILay_Locked));
	stageFacts.Add ("layerReadback", GS::ObjectState ("status", exactChangedLayer ? "exact" : "mismatch-or-unavailable", "errorCode", static_cast<Int32> (changedReadError), "guid", APIGuidToString (changed.header.guid), "index", GetAttributeIndex (changed.header.index), "flags", changed.header.flags, "name", changedName));
	NativeDimensionLayerInventory afterInventory = ReadNativeDimensionLayerInventory (started, processControl);
	GS::ObjectState parentRaw = ReadNativeDimensionFixtureRow (run.parentGuid, processControl);
	stageFacts.Add ("inventory", afterInventory.facts);
	stageFacts.Add ("parentReader", parentRaw);
	GS::ObjectState parentRow;
	const bool parentEnvelopeExact = ExtractExactNativeDimensionFixtureRow (parentRaw, run.parentGuid, parentRow);
	bool parentIdentityMatches = false;
	GS::ObjectState parentId;
	GS::UniString parentGuid;
	Int32 parentType = -1;
	Int64 parentStamp = -1;
	parentIdentityMatches = parentEnvelopeExact && parentRow.Get ("elementId", parentId) && parentId.Get ("guid", parentGuid)
		&& NativeDimensionLayerControlGuidEqual (parentGuid, APIGuidToString (run.parentGuid))
		&& parentRow.Get ("nativeElementTypeId", parentType) && parentType == static_cast<Int32> (API_DimensionID)
		&& parentRow.Get ("modificationStamp", parentStamp) && parentStamp > 0
		&& static_cast<UInt64> (parentStamp) == run.parentModificationStamp;
	GS::ObjectState parentElementFilter;
	parentElementFilter.Add ("onActFloor", ACAPI_Element_Filter (run.parentGuid, APIFilt_OnActFloor));
	parentElementFilter.Add ("onVisibleLayer", ACAPI_Element_Filter (run.parentGuid, APIFilt_OnVisLayer));
	parentElementFilter.Add ("isEditable", ACAPI_Element_Filter (run.parentGuid, APIFilt_IsEditable));
	stageFacts.Add ("parentFilterControls", parentElementFilter);
	GS::ObjectState afterParentIdentity = parentRow;
	GS::UniString parentSerialized;
	UInt64 parentBytes = 0;
	const bool parentSerializedOk = NativeDimensionAuditFactsText (afterParentIdentity, parentSerialized, parentBytes);
	const bool parentFactsUnchanged = parentSerializedOk && FixtureUtf8 (parentSerialized) == run.preparedParentFacts;
	const bool layerAndCombinationFactsUnchanged = NativeDimensionLayerStageFactsUnchanged (run, afterInventory, settingHidden, !settingHidden);
	RadialDefaultsContext afterContext;
	ReadNativeDimensionAuditContext (afterContext);
	const bool scopeStable = IsExactTaskOwnedRadialDefaultsCopy (run.runId, run.projectPath, run.fileScopeKey, afterContext)
		&& afterContext.window.typeID == APIWind_FloorPlanID && afterContext.database.typeID == APIWind_FloorPlanID
		&& afterContext.database.databaseUnId.elemSetId == run.databaseGuid && afterContext.activeStory == run.storyIndex
		&& beforeContext.database.databaseUnId.elemSetId == afterContext.database.databaseUnId.elemSetId
		&& beforeContext.activeStory == afterContext.activeStory;
	stageFacts.Add ("parentFactsUnchanged", parentFactsUnchanged);
	stageFacts.Add ("originalLayerAndCombinationFactsUnchanged", layerAndCombinationFactsUnchanged);
	stageFacts.Add ("contextBefore", RadialDefaultsContextDetails (beforeContext));
	stageFacts.Add ("contextAfter", RadialDefaultsContextDetails (afterContext));
	stageFacts.Add ("scopeStable", scopeStable);
	const bool stageFactsCoherent = exactChangedLayer && afterInventory.complete && parentIdentityMatches && parentFactsUnchanged && layerAndCombinationFactsUnchanged && scopeStable
		&& (settingHidden ? (!ACAPI_Element_Filter (run.parentGuid, APIFilt_OnVisLayer) && !ACAPI_Element_Filter (run.parentGuid, APIFilt_IsEditable) && ACAPI_Element_Filter (run.parentGuid, APIFilt_OnActFloor))
			: (ACAPI_Element_Filter (run.parentGuid, APIFilt_OnVisLayer) && !ACAPI_Element_Filter (run.parentGuid, APIFilt_IsEditable) && ACAPI_Element_Filter (run.parentGuid, APIFilt_OnActFloor)));
	if (!stageFactsCoherent || NativeDimensionLayerControlExpired (started, processControl)) {
		run.phase = NativeDimensionLayerControlPolicy::Phase::Unknown;
		return NativeDimensionLayerControlResponse ("unknown", "postcondition-or-cooperative-deadline-unproven-no-next-write", action, run.runRef, "unknown", stageFacts);
	}
	if (settingHidden) run.phase = NativeDimensionLayerControlPolicy::Phase::Hidden;
	else run.phase = NativeDimensionLayerControlPolicy::Phase::Locked;
	run.nextNonce = NativeDimensionLayerControlToken ();
	GS::ObjectState facts ("stageNonceConsumed", consumedToken, "layerGuid", expectedGuid,
		"nextAction", settingHidden ? "setLocked" : "inspect", "nextStageNonce", run.nextNonce,
		"rawStageFacts", stageFacts, "storedCombinations", "re-read-complete-not-mutated-by-command");
	return NativeDimensionLayerControlResponse ("stage-complete", settingHidden ? "hidden-state-observed" : "locked-state-observed", action, run.runRef, settingHidden ? "hidden" : "locked", facts);
}

GetNativeDimensionCreateCandidateContextCommand::GetNativeDimensionCreateCandidateContextCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionCreateCandidateContextCommand::GetName () const { return "GetNativeDimensionCreateCandidateContext"; }
GS::Optional<GS::UniString> GetNativeDimensionCreateCandidateContextCommand::GetInputParametersSchema () const
{
	return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-create-context-request/v1"]},"runId":{"type":"string","pattern":"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$"},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer"}},"required":["schemaVersion","runId","expectedProjectPath","fileScopeKey","storyIndex"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionCreateCandidateContextCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionCreateCandidateContextCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
	GS::UniString schema, runId, expectedProjectPath, fileScopeKey;
	Int32 expectedStory = 0;
	if (parameters.GetFieldNames ().GetSize () != 5
		|| !parameters.Get ("schemaVersion", schema) || schema != "native-dimension-create-context-request/v1"
		|| !parameters.Get ("runId", runId) || !parameters.Get ("expectedProjectPath", expectedProjectPath)
		|| !parameters.Get ("fileScopeKey", fileScopeKey) || !parameters.Get ("storyIndex", expectedStory))
		return GS::ObjectState ("schemaVersion", "native-dimension-create-context-response/v1", "status", "refused", "reason", "invalid-closed-context-request");
	NativeDimensionCreateWire::Context context;
	GS::ObjectState observed;
	const bool available = CaptureNativeDimensionCreateContext (runId, expectedProjectPath, fileScopeKey, context, observed)
		&& context.storyIndex == std::to_string (expectedStory);
	Int32 windowIndex = -1;
	try { windowIndex = static_cast<Int32> (std::stol (context.windowIndex)); } catch (...) { windowIndex = -1; }
	Int32 observedStoryIndex = -1;
	try {
		const long parsedStory = std::stol (context.storyIndex);
		if (parsedStory >= std::numeric_limits<Int32>::min () && parsedStory <= std::numeric_limits<Int32>::max ()
			&& std::to_string (parsedStory) == context.storyIndex) observedStoryIndex = static_cast<Int32> (parsedStory);
	} catch (...) {}
	GS::ObjectState contextState = NativeDimensionCreateWire::EmitContextObjectState (context, windowIndex, observedStoryIndex);
	return GS::ObjectState (
		"schemaVersion", "native-dimension-create-context-response/v1",
		"status", available ? "available" : "unavailable",
		"reason", available ? "exact-owned-saved-solo-floor-plan-context" : "owned-path-process-window-database-or-story-context-unavailable",
		"requestedStoryIndex", expectedStory,
		"context", contextState,
		"observed", observed);
}

namespace {

GS::ObjectState NativeDimensionCreateContextObject (const NativeDimensionCreateWire::Context& context)
{
	Int32 windowIndex = -1, storyIndex = -1;
	try { windowIndex = static_cast<Int32> (std::stol (context.windowIndex)); } catch (...) {}
	try { storyIndex = static_cast<Int32> (std::stol (context.storyIndex)); } catch (...) {}
	return NativeDimensionCreateWire::EmitContextObjectState (context, windowIndex, storyIndex);
}

GS::ObjectState NativeDimensionCreateCandidateResult (
	const char* status,
	const char* reason,
	const NativeDimensionCreateWire::Request* request,
	const NativeDimensionCreateWire::Context* expected,
	const NativeDimensionCreateWire::Context* before,
	const NativeDimensionCreateWire::Context* insideBefore,
	const NativeDimensionCreateWire::Context* after,
	const GS::ObjectState* rawReadback,
	const GS::ObjectState* inputWire,
	const GSErrCode callbackError,
	const GSErrCode transactionError,
	const bool dispatchAttempted,
	const bool postCreateFaultInjected,
	const API_Guid* knownGuid)
{
	GS::ObjectState result (
		"schemaVersion", "native-dimension-create-candidate-result/v1",
		"kind", "dimensionCreateCandidateResult",
		"status", status,
		"reason", reason,
		"dispatchAttempted", dispatchAttempted,
		"callbackErrorCode", static_cast<Int32> (callbackError),
		"transactionErrorCode", static_cast<Int32> (transactionError),
		"postCreateFaultInjected", postCreateFaultInjected);
	if (request != nullptr) {
		NativeDimensionCreateWire::AddCandidateResultCorrelation (result, *request, static_cast<Int32> (std::stol (request->storyIndex)));
	}
	if (expected != nullptr) result.Add ("expectedContext", NativeDimensionCreateContextObject (*expected));
	if (before != nullptr) result.Add ("contextBefore", NativeDimensionCreateContextObject (*before));
	if (insideBefore != nullptr) result.Add ("contextInsideBefore", NativeDimensionCreateContextObject (*insideBefore));
	if (after != nullptr) result.Add ("contextAfter", NativeDimensionCreateContextObject (*after));
	if (rawReadback != nullptr) result.Add ("rawReadback", *rawReadback);
	if (inputWire != nullptr) {
		result.Add ("inputWire", *inputWire);
		GS::UniString wireHash;
		if (inputWire->Get ("wireHash", wireHash)) result.Add ("wireHash", wireHash);
	}
	if (knownGuid != nullptr && *knownGuid != APINULLGuid) {
		const auto& guids = result.AddList<GS::UniString> ("knownGuids");
		guids (APIGuidToString (*knownGuid));
	}
	return result;
}

}

GS::ObjectState ExecuteNativeDimensionCreateCandidate (const GS::ObjectState& parameters, GS::ProcessControl& processControl)
{
	static_cast<void> (processControl); // Used only in the separately compiled candidate branch.
	const char* invalid = "rejected";
	if (parameters.GetFieldNames ().GetSize () != 8) return NativeDimensionCreateCandidateResult (invalid, "closed-native-envelope-required", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
	std::string contract, operation, elementType, operationId, fileScope, outerHash, description;
	GS::Array<GS::ObjectState> items;
	if (!NativeDimensionCreateWire::ReadExactString (parameters, "contractVersion", contract) || contract != "native-crud-v2"
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "operation", operation) || operation != "create"
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "elementType", elementType) || elementType != "Dimension"
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "operationId", operationId) || operationId.empty ()
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "expectedFileScopeKey", fileScope) || fileScope.empty ()
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "requestHash", outerHash)
		|| !NativeDimensionCreateWire::ReadExactString (parameters, "description", description) || description.empty ()
		|| !parameters.Get ("items", items) || items.GetSize () != 1)
		return NativeDimensionCreateCandidateResult (invalid, "invalid-closed-candidate-envelope", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
	const GS::ObjectState inputWire = items[0];
	NativeDimensionCreateWire::Request request;
	NativeDimensionCreateWire::Context expected;
	std::string wireHash;
	if (!NativeDimensionCreateWire::ParseEnvelope (inputWire, request, expected, wireHash)
		|| request.operationId != operationId
		|| request.fileScopeKey != fileScope
		|| request.requestHash != outerHash
		|| request.description != description)
		return NativeDimensionCreateCandidateResult (invalid, "wire-request-or-outer-identity-invalid", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
#if !TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
	return NativeDimensionCreateCandidateResult (invalid, "native-candidate-profile-disabled", &request, &expected, nullptr, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
#else
	if (!NativeDimensionCreateWire::CompiledProfileMatches (expected))
		return NativeDimensionCreateCandidateResult (invalid, "compiled-native-candidate-policy-mismatch", &request, &expected, nullptr, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
	const GS::UniString expectedRunId (expected.runId.c_str (), CC_UTF8);
	const GS::UniString expectedProjectPath (expected.projectPath.c_str (), CC_UTF8);
	const GS::UniString expectedFileScopeKey (expected.fileScopeKey.c_str (), CC_UTF8);
	NativeDimensionCreateWire::Context contextBefore;
	GS::ObjectState contextBeforeFacts;
	if (!CaptureNativeDimensionCreateContext (expectedRunId, expectedProjectPath, expectedFileScopeKey, contextBefore, contextBeforeFacts)
		|| !NativeDimensionCreateWire::ValidateExpectedContext (expected, contextBefore).accepted
		|| !NativeDimensionCreateWire::RequestMatchesContext (request, contextBefore))
		return NativeDimensionCreateCandidateResult (invalid, "pretransaction-native-context-refused", &request, &expected, &contextBefore, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
	const long requestedStoryLong = std::stol (request.storyIndex);
	if (requestedStoryLong < std::numeric_limits<Int16>::min () || requestedStoryLong > std::numeric_limits<Int16>::max ())
		return NativeDimensionCreateCandidateResult (invalid, "story-index-outside-native-floor-index-range", &request, &expected, &contextBefore, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
	GS::ObjectState defaultFacts;
	PreparedLinearStaticFixtureDefaults defaults;
	if (!ReadLinearStaticFixtureDefaultFacts (defaultFacts, defaults)) {
		GS::ObjectState refused = NativeDimensionCreateCandidateResult (invalid, "default-or-memo-ownership-unavailable", &request, &expected, &contextBefore, nullptr, nullptr, nullptr, &inputWire, APIERR_BADPARS, APIERR_BADPARS, false, false, nullptr);
		refused.Add ("defaultAvailability", defaultFacts);
		return refused;
	}
	defaults.linear.header.floorInd = static_cast<Int16> (requestedStoryLong);
	GS::ObjectState rawReadback;
	NativeDimensionCreateWire::Context contextInsideBefore, contextAfter;
	GS::ObjectState contextInsideFacts, contextAfterFacts;
	GSErrCode callbackError = APIERR_CANCEL, transactionError = APIERR_CANCEL;
	bool dispatchAttempted = false, injectedFault = false, exactReadback = false, createdInCallback = false;
	API_Guid knownGuid = APINULLGuid;
	transactionError = NativeUndoableCommand ("Create Native Static Measured Dimension Candidate", [&] () -> GSErrCode {
		if (!CaptureNativeDimensionCreateContext (expectedRunId, expectedProjectPath, expectedFileScopeKey, contextInsideBefore, contextInsideFacts)
			|| !NativeDimensionCreateWire::ValidateExpectedContext (expected, contextInsideBefore).accepted
			|| !NativeDimensionCreateWire::ValidateExpectedContext (contextBefore, contextInsideBefore).accepted)
			return callbackError = APIERR_CANCEL;
		if (defaults.linear.dimension.defNote.contentUStr != nullptr || defaults.linear.dimension.nDimElem != 0 || defaults.memo.dimElems != nullptr)
			return callbackError = APIERR_BADPARS;
		GSErrCode prepareError = BuildFixedStaticMeasuredTwoPointDimension (defaults.linear, defaults.memo,
			static_cast<Int16> (std::stoi (request.storyIndex)));
		if (prepareError != NoError) return callbackError = prepareError;
		dispatchAttempted = true;
		GSErrCode createError = ACAPI_Element_Create (&defaults.linear, &defaults.memo);
		callbackError = createError;
		if (defaults.linear.header.guid != APINULLGuid) knownGuid = defaults.linear.header.guid;
		if (createError != NoError) return createError;
		createdInCallback = true;
#if TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT
		injectedFault = true;
		callbackError = APIERR_GENERAL;
		return callbackError;
#endif
		rawReadback = ReadNativeDimensionFixtureRow (knownGuid, processControl);
		GS::ObjectState row;
		const API_Guid expectedDatabaseGuid = APIGuidFromString (GS::UniString (contextInsideBefore.currentDatabaseGuid.c_str (), CC_UTF8).ToCStr ());
		UInt64 observedModificationStamp = 0;
		exactReadback = ExtractExactNativeDimensionFixtureRow (rawReadback, knownGuid, row)
			&& expectedDatabaseGuid != APINULLGuid
			&& IsNativeDimensionMaskedPositionStage (rawReadback, knownGuid, expectedDatabaseGuid,
				static_cast<Int32> (requestedStoryLong), 20.0, observedModificationStamp)
			&& NativeDimensionCreateWire::FixedCandidateReaderRowMatches (row, APIGuidToString (knownGuid),
				APIGuidToString (expectedDatabaseGuid), static_cast<Int32> (requestedStoryLong));
		if (!exactReadback) return callbackError = APIERR_BADID;
		if (!CaptureNativeDimensionCreateContext (expectedRunId, expectedProjectPath, expectedFileScopeKey, contextAfter, contextAfterFacts)
			|| !NativeDimensionCreateWire::ValidateExpectedContext (expected, contextAfter).accepted
			|| !NativeDimensionCreateWire::ValidateExpectedContext (contextInsideBefore, contextAfter).accepted)
			return callbackError = APIERR_CANCEL;
		callbackError = NoError;
		return NoError;
	});
	if (transactionError == NoError && callbackError == NoError && dispatchAttempted && exactReadback) {
		return NativeDimensionCreateCandidateResult ("created", "native-create-and-exact-reader-row-observed", &request, &expected, &contextBefore, &contextInsideBefore, &contextAfter, &rawReadback, &inputWire, callbackError, transactionError, true, false, &knownGuid);
	}
	if (!dispatchAttempted) {
		return NativeDimensionCreateCandidateResult (invalid, "candidate-refused-before-element-create", &request, &expected, &contextBefore,
			contextInsideBefore.kind.empty () ? nullptr : &contextInsideBefore, contextAfter.kind.empty () ? nullptr : &contextAfter,
			rawReadback.GetFieldNames ().IsEmpty () ? nullptr : &rawReadback, &inputWire, callbackError, transactionError, false, injectedFault, nullptr);
	}
	if (createdInCallback && callbackError != NoError && transactionError != NoError && knownGuid != APINULLGuid) {
		NativeDimensionCreateWire::Context afterUndo;
		GS::ObjectState afterUndoFacts;
		const bool scopeStable = CaptureNativeDimensionCreateContext (expectedRunId, expectedProjectPath, expectedFileScopeKey, afterUndo, afterUndoFacts)
			&& NativeDimensionCreateWire::ValidateExpectedContext (expected, afterUndo).accepted
			&& NativeDimensionCreateWire::ValidateExpectedContext (contextInsideBefore, afterUndo).accepted;
		API_Element exactProbe = {};
		exactProbe.header.guid = knownGuid;
		const GSErrCode absenceProbeError = ACAPI_Element_Get (&exactProbe);
		if (scopeStable && absenceProbeError == APIERR_BADID) {
			GS::ObjectState observedRollback = NativeDimensionCreateCandidateResult ("rollbackObserved", "callback-and-undo-errors-with-exact-created-guid-absence-observed",
				&request, &expected, &contextBefore, &contextInsideBefore, &afterUndo,
				rawReadback.GetFieldNames ().IsEmpty () ? nullptr : &rawReadback, &inputWire, callbackError, transactionError, true, injectedFault, &knownGuid);
			observedRollback.Add ("rollbackEvidence", GS::ObjectState (
				"status", "exact-guid-absent-after-undo", "guid", APIGuidToString (knownGuid),
				"elementGetErrorCode", static_cast<Int32> (absenceProbeError), "absenceCode", "APIERR_BADID",
				"contextStable", true, "coverage", "exact-created-guid-only"));
			return observedRollback;
		}
		if (scopeStable) contextAfter = afterUndo;
	}
	return NativeDimensionCreateCandidateResult ("unresolved", injectedFault ? "compiled-post-create-fault-injected" : "create-transaction-or-postcondition-unresolved", &request, &expected, &contextBefore, &contextInsideBefore,
		contextAfter.kind.empty () ? nullptr : &contextAfter, rawReadback.GetFieldNames ().IsEmpty () ? nullptr : &rawReadback, &inputWire,
		callbackError, transactionError, dispatchAttempted, injectedFault, &knownGuid);
#endif
}

GetNativeDimensionRadialDefaultsCommand::GetNativeDimensionRadialDefaultsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionRadialDefaultsCommand::GetName () const { return "GetNativeDimensionRadialDefaults"; }
GS::Optional<GS::UniString> GetNativeDimensionRadialDefaultsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-radial-defaults-request/v1"]},"runId":{"type":"string","pattern":"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$"},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer"}},"required":["schemaVersion","runId","expectedProjectPath","fileScopeKey","storyIndex"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionRadialDefaultsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionRadialDefaultsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::UniString schemaVersion, runId, expectedProjectPath, requestedFileScopeKey;
    Int32 requestedStory = 0;
    if (!parameters.GetFieldNames ().IsEmpty () && (parameters.GetFieldNames ().GetSize () != 5
        || !parameters.Get ("schemaVersion", schemaVersion) || schemaVersion != "native-dimension-radial-defaults-request/v1"
        || !parameters.Get ("runId", runId) || !parameters.Get ("expectedProjectPath", expectedProjectPath)
        || !parameters.Get ("fileScopeKey", requestedFileScopeKey) || !parameters.Get ("storyIndex", requestedStory))) {
        return RadialDefaultsResponse (
            "refused", runId, requestedFileScopeKey, requestedStory,
            GS::ObjectState ("status", "not-read"), RadialDefaultsNotCheckedContext (), RadialDefaultsNotRequested (),
            RadialDefaultsError ("INVALID_REQUEST", "Request must contain exactly the five documented radial-defaults fields."));
    }
    if (parameters.GetFieldNames ().IsEmpty ()) {
        return RadialDefaultsResponse (
            "refused", runId, requestedFileScopeKey, requestedStory,
            GS::ObjectState ("status", "not-read"), RadialDefaultsNotCheckedContext (), RadialDefaultsNotRequested (),
            RadialDefaultsError ("INVALID_REQUEST", "Request cannot be empty."));
    }
    const std::string runIdUtf8 = FixtureUtf8 (runId);
    if (schemaVersion != "native-dimension-radial-defaults-request/v1" || runIdUtf8.empty () || runIdUtf8.size () > 48
        || !std::all_of (runIdUtf8.begin (), runIdUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; })
        || expectedProjectPath.IsEmpty () || FixtureUtf8 (expectedProjectPath).size () > 1024
        || requestedFileScopeKey.IsEmpty () || FixtureUtf8 (requestedFileScopeKey).size () > 1100) {
        return RadialDefaultsResponse (
            "refused", runId, requestedFileScopeKey, requestedStory,
            GS::ObjectState ("status", "not-read"), RadialDefaultsNotCheckedContext (), RadialDefaultsNotRequested (),
            RadialDefaultsError ("INVALID_REQUEST", "Request values are outside their closed fixed bounds."));
    }

    RadialDefaultsContext before;
    ReadRadialDefaultsContext (before);
    const GS::ObjectState beforeState = RadialDefaultsContextDetails (before);
    if (requestedStory < std::numeric_limits<short>::min () || requestedStory > std::numeric_limits<short>::max ()
        || !IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, before)
        || before.window.typeID != APIWind_FloorPlanID || before.database.typeID != APIWind_FloorPlanID
        || before.activeStory != requestedStory) {
        return RadialDefaultsResponse (
            "refused", runId, requestedFileScopeKey, requestedStory, beforeState, RadialDefaultsNotCheckedContext (), RadialDefaultsNotRequested (),
            RadialDefaultsError ("RADIAL_DEFAULTS_SCOPE_REFUSED", "Requires the exact saved Solo task-owned copy, matching file scope, current Floor Plan window/database and active story."));
    }

    API_Element element = {};
    SetNativeElementTypeHeader (element, API_RadialDimensionID);
    API_ElementMemo memo = {};
    NativeMemoGuard memoGuard (memo);
    const GSErrCode defaultsError = ACAPI_Element_GetDefaults (&element, &memo);

    RadialDefaultsContext after;
    ReadRadialDefaultsContext (after);
    const GS::ObjectState afterState = RadialDefaultsContextDetails (after);
    if (!SameRadialDefaultsContext (before, after) || !IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, after)
        || after.window.typeID != APIWind_FloorPlanID || after.database.typeID != APIWind_FloorPlanID || after.activeStory != requestedStory) {
        return RadialDefaultsResponse (
            "refused", runId, requestedFileScopeKey, requestedStory, beforeState, afterState, RadialDefaultsDiscardedContextChange (),
            RadialDefaultsError ("RADIAL_DEFAULTS_CONTEXT_CHANGED", "Context or scope changed across the defaults read; SDK defaults were discarded."));
    }
    if (defaultsError != NoError) {
        return RadialDefaultsResponse (
            "unavailable", runId, requestedFileScopeKey, requestedStory, beforeState, afterState,
            GS::ObjectState ("status", "unavailable", "nativeErrorCode", static_cast<Int32> (defaultsError)),
            RadialDefaultsError ("GET_DEFAULTS_FAILED", "ACAPI_Element_GetDefaults failed for RadialDimension.", static_cast<Int32> (defaultsError)));
    }

    return RadialDefaultsResponse (
        "available", runId, requestedFileScopeKey, requestedStory, beforeState, afterState,
        RadialDefaultsFacts (element, memo), RadialDefaultsNoError ());
}

GetNativeDimensionArcHotspotsCommand::GetNativeDimensionArcHotspotsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionArcHotspotsCommand::GetName () const { return "GetNativeDimensionArcHotspots"; }
GS::Optional<GS::UniString> GetNativeDimensionArcHotspotsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-arc-hotspots-request/v1"]},"runId":{"type":"string","pattern":"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$"},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer"},"hostGuid":{"type":"string","format":"uuid"}},"required":["schemaVersion","runId","expectedProjectPath","fileScopeKey","storyIndex","hostGuid"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionArcHotspotsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionArcHotspotsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    const auto started = std::chrono::steady_clock::now ();
    GS::UniString schemaVersion, runId, expectedProjectPath, requestedFileScopeKey, hostGuidText;
    Int32 requestedStory = 0;
    const bool fieldsRead = parameters.GetFieldNames ().GetSize () == 6
        && parameters.Get ("schemaVersion", schemaVersion) && parameters.Get ("runId", runId)
        && parameters.Get ("expectedProjectPath", expectedProjectPath)
        && parameters.Get ("fileScopeKey", requestedFileScopeKey)
        && parameters.Get ("storyIndex", requestedStory) && parameters.Get ("hostGuid", hostGuidText);
    API_Guid hostGuid = APINULLGuid;
    if (fieldsRead)
        hostGuid = APIGuidFromString (hostGuidText.ToCStr ());

    const GS::ObjectState notRead ("status", "not-read");
    const GS::ObjectState notChecked ("status", "not-checked-before-hotspot-call");
    const GS::ObjectState consistencyNotRead ("status", "not-read");
    const GS::Array<GS::ObjectState> noHotspots;
    auto response = [&] (
        const char* status,
        const GS::ObjectState& contextBefore,
        const GS::ObjectState& contextAfter,
        const GS::ObjectState& hostBefore,
        const GS::ObjectState& hostAfter,
        const GS::ObjectState& consistency,
        const Int64 count,
        const bool complete,
        const bool conversionComplete,
        const char* reason,
        const GS::Array<GS::ObjectState>& hotspots,
        const GS::ObjectState& error) {
        return ArcHotspotResponse (status, runId, expectedProjectPath, requestedFileScopeKey, requestedStory,
            hostGuid, contextBefore, contextAfter, hostBefore, hostAfter, consistency, count, complete,
            conversionComplete, reason, hotspots, error);
    };

    const GS::UniString lowerGuid = hostGuidText.ToLowerCase ();
    const GS::UniString canonicalGuid = hostGuid == APINULLGuid ? GS::UniString () : APIGuidToString (hostGuid).ToLowerCase ();
    const std::string runIdUtf8 = FixtureUtf8 (runId);
    const bool requestIsValid = fieldsRead
        && schemaVersion == "native-dimension-arc-hotspots-request/v1"
        && !runIdUtf8.empty () && runIdUtf8.size () <= 48
        && std::all_of (runIdUtf8.begin (), runIdUtf8.end (), [] (unsigned char c) { return std::isalnum (c) || c == '_' || c == '-'; })
        && !expectedProjectPath.IsEmpty () && FixtureUtf8 (expectedProjectPath).size () <= 1024
        && !requestedFileScopeKey.IsEmpty () && FixtureUtf8 (requestedFileScopeKey).size () <= 1100
        && requestedStory >= std::numeric_limits<short>::min () && requestedStory <= std::numeric_limits<short>::max ()
        && hostGuid != APINULLGuid && lowerGuid == canonicalGuid;
    if (!requestIsValid) {
        return response ("refused", notRead, notChecked, notRead, notRead, consistencyNotRead, 0, false, false,
            "invalid-fixed-request", noHotspots,
            ArcHotspotError ("INVALID_REQUEST", "Request must contain exactly the six fixed fields and one canonical non-null host GUID."));
    }

    RadialDefaultsContext before;
    ReadRadialDefaultsContext (before);
    const GS::ObjectState beforeState = RadialDefaultsContextDetails (before);
    if (!IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, before)
        || before.window.typeID != APIWind_FloorPlanID || before.database.typeID != APIWind_FloorPlanID
        || before.activeStory != requestedStory) {
        return response ("refused", beforeState, notChecked, notRead, notRead, consistencyNotRead, 0, false, false,
            "exact-saved-copy-floor-plan-database-and-story-guard-refused", noHotspots,
            ArcHotspotError ("ARC_HOTSPOT_SCOPE_REFUSED", "Requires the exact task-owned saved Solo copy, matching file scope, Floor Plan window/database and active story."));
    }

    bool cancelled = false;
    if (ArcHotspotStopped (processControl, started, cancelled)) {
        return response ("partial", beforeState, notChecked, notRead, notRead, consistencyNotRead, 0, false, false,
            cancelled ? "cancelled-before-host-read" : "deadline-before-host-read", noHotspots, ArcHotspotStoppedError (cancelled));
    }

    const ArcHotspotHostSnapshot hostBeforeSnapshot = ReadArcHotspotHost (hostGuid);
    const GS::ObjectState hostBeforeState = ArcHotspotHostDetails (hostBeforeSnapshot);
    if (hostBeforeSnapshot.error != NoError || hostBeforeSnapshot.returnedGuid != hostGuid || hostBeforeSnapshot.type != API_ArcID
        || hostBeforeSnapshot.storyIndex != requestedStory || !IsFiniteArcHost (hostBeforeSnapshot)) {
        RadialDefaultsContext after;
        ReadRadialDefaultsContext (after);
        const GS::ObjectState afterState = RadialDefaultsContextDetails (after);
        const bool sameContext = SameRadialDefaultsContext (before, after)
            && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, after)
            && after.activeStory == requestedStory;
        const GS::ObjectState consistency (
            "status", sameContext ? "context-stable-host-not-eligible" : "context-changed-or-unverified",
            "contextStable", sameContext,
            "hostStable", false);
        const bool isWrongType = hostBeforeSnapshot.error == NoError
            && (hostBeforeSnapshot.type != API_ArcID || hostBeforeSnapshot.returnedGuid != hostGuid || hostBeforeSnapshot.storyIndex != requestedStory);
        return response (isWrongType ? "refused" : "unavailable", beforeState, afterState, hostBeforeState,
            GS::ObjectState ("status", "not-read"), consistency, 0, false, false,
            isWrongType ? "exact-guid-is-not-an-arc-on-requested-story" : "exact-arc-header-or-geometry-unavailable",
            noHotspots,
            isWrongType
                ? ArcHotspotError ("ARC_HOTSPOT_HOST_REFUSED", "The exact GUID is not an Arc on the requested active story.")
                : ArcHotspotError ("ARC_HOTSPOT_HOST_READ_FAILED", "The exact Arc header or finite geometry could not be verified.", static_cast<Int32> (hostBeforeSnapshot.error)));
    }

    if (ArcHotspotStopped (processControl, started, cancelled)) {
        return response ("partial", beforeState, notChecked, hostBeforeState, GS::ObjectState ("status", "not-read"),
            GS::ObjectState ("status", "unverified-before-hotspot-call"), 0, false, false,
            cancelled ? "cancelled-before-hotspot-call" : "deadline-before-hotspot-call", noHotspots, ArcHotspotStoppedError (cancelled));
    }

    GS::Array<API_ElementHotspot> sdkHotspots;
    const GSErrCode hotspotsError = ACAPI_Element_GetHotspots (hostGuid, &sdkHotspots);
    const Int64 hotspotCount = static_cast<Int64> (sdkHotspots.GetSize ());
    if (hotspotsError != NoError) {
        bool stoppedAfterGet = ArcHotspotStopped (processControl, started, cancelled);
        if (stoppedAfterGet) {
            return response ("partial", beforeState, notChecked, hostBeforeState, GS::ObjectState ("status", "not-read"),
                GS::ObjectState ("status", "unverified-after-hotspot-api-error", "hostStable", false, "contextStable", false),
                hotspotCount, false, false, cancelled ? "cancelled-after-hotspot-api-error" : "deadline-after-hotspot-api-error",
                noHotspots, ArcHotspotError ("GET_HOTSPOTS_FAILED", "ACAPI_Element_GetHotspots failed before cooperative stop.", static_cast<Int32> (hotspotsError)));
        }
        RadialDefaultsContext after;
        ReadRadialDefaultsContext (after);
        const GS::ObjectState afterState = RadialDefaultsContextDetails (after);
        const ArcHotspotHostSnapshot hostAfterSnapshot = ReadArcHotspotHost (hostGuid);
        const GS::ObjectState hostAfterState = ArcHotspotHostDetails (hostAfterSnapshot);
        const bool hostStable = SameArcHotspotHost (hostBeforeSnapshot, hostAfterSnapshot);
        const bool contextStable = SameRadialDefaultsContext (before, after)
            && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, after)
            && after.activeStory == requestedStory;
        return response ("unavailable", beforeState, afterState, hostBeforeState, hostAfterState,
            ArcHotspotRecheckConsistency (hostBeforeSnapshot, hostAfterSnapshot, hostStable, contextStable, hostStable && contextStable),
            hotspotCount, false, false, "hotspot-api-error", noHotspots,
            ArcHotspotError ("GET_HOTSPOTS_FAILED", "ACAPI_Element_GetHotspots failed for the exact Arc GUID.", static_cast<Int32> (hotspotsError)));
    }

    if (static_cast<std::size_t> (sdkHotspots.GetSize ()) > MaxNativeArcHotspots) {
        bool stoppedAfterGet = ArcHotspotStopped (processControl, started, cancelled);
        if (stoppedAfterGet) {
            return response ("partial", beforeState, notChecked, hostBeforeState, GS::ObjectState ("status", "not-read"),
                GS::ObjectState ("status", "unverified-after-overflow"), hotspotCount, false, false,
                cancelled ? "cancelled-after-overflow" : "deadline-after-overflow", noHotspots, ArcHotspotStoppedError (cancelled));
        }
        RadialDefaultsContext after;
        ReadRadialDefaultsContext (after);
        const GS::ObjectState afterState = RadialDefaultsContextDetails (after);
        const ArcHotspotHostSnapshot hostAfterSnapshot = ReadArcHotspotHost (hostGuid);
        const GS::ObjectState hostAfterState = ArcHotspotHostDetails (hostAfterSnapshot);
        const bool hostStable = SameArcHotspotHost (hostBeforeSnapshot, hostAfterSnapshot);
        const bool contextStable = SameRadialDefaultsContext (before, after)
            && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, after)
            && after.activeStory == requestedStory;
        return response ("partial", beforeState, afterState, hostBeforeState, hostAfterState,
            ArcHotspotRecheckConsistency (hostBeforeSnapshot, hostAfterSnapshot, hostStable, contextStable, hostStable && contextStable),
            hotspotCount, false, false, "sdk-result-overflow-no-prefix-selected", noHotspots,
            ArcHotspotError ("HOTSPOT_LIMIT_EXCEEDED", "The complete SDK result exceeds the fixed 32-hotspot diagnostic limit; no prefix was selected."));
    }

    GS::Array<GS::ObjectState> hotspotFacts;
    bool allCoordinatesFinite = true;
    bool conversionComplete = true;
    bool cooperativelyStopped = false;
    bool cooperativeCancelled = false;
    for (UIndex index = 0; index < sdkHotspots.GetSize (); ++index) {
        const API_Neig& returnedNeig = sdkHotspots[index].first;
        const API_Coord3D& returnedCoordinate = sdkHotspots[index].second;
        GS::ObjectState returnedCoordinateState;
        const bool returnedCoordinateFinite = IsFiniteHotspotCoord (returnedCoordinate);
        if (returnedCoordinateFinite) {
            returnedCoordinateState = GS::ObjectState ("status", "available", "point", DimensionPoint3D (returnedCoordinate));
        } else {
            returnedCoordinateState = GS::ObjectState ("status", "unavailable-nonfinite-coordinate");
            allCoordinatesFinite = false;
        }

        const char* skipReason = "none";
        if (returnedNeig.neigID == APINeig_ArcOn) skipReason = "arc-on-neig-skipped-no-arc-on-synthesis";
        else if (returnedNeig.neigID != APINeig_Arc) skipReason = "unsupported-neig-kind";
        else if (returnedNeig.guid != hostGuid) skipReason = "returned-neig-guid-differs-from-exact-host";
        else if (returnedNeig.flags != API_NeigFlg_Normal) skipReason = "unsupported-non-normal-floor-plan-flags";
        else if (returnedNeig.elemPartType != APINeigElemPart_None) skipReason = "unsupported-element-part-kind";

        const bool conversionEligible = std::strcmp (skipReason, "none") == 0;
        GS::ObjectState conversionResult;
        GS::ObjectState convertedNeigDetails ("status", "not-attempted");
        GS::ObjectState convertedCoordinateDetails ("status", "not-attempted");
        GS::ObjectState coordinateComparison (
            "status", "not-attempted",
            "tolerance", NativeArcHotspotCoordinateTolerance,
            "toleranceUnit", "project-meter");

        if (!returnedCoordinateFinite) {
            conversionResult = GS::ObjectState ("status", "not-attempted", "reason", "returned-coordinate-nonfinite", "nativeErrorCode", static_cast<Int32> (NoError));
            conversionComplete = false;
        } else if (!conversionEligible) {
            conversionResult = GS::ObjectState ("status", "skipped", "reason", skipReason);
        } else if (cooperativelyStopped) {
            conversionResult = GS::ObjectState (
                "status", "not-attempted",
                "reason", cooperativeCancelled ? "cooperative-cancelled" : "cooperative-deadline-reached",
                "nativeErrorCode", static_cast<Int32> (NoError));
            convertedNeigDetails = GS::ObjectState ("status", "not-returned");
            convertedCoordinateDetails = GS::ObjectState ("status", "not-returned");
            coordinateComparison = GS::ObjectState ("status", "not-attempted", "tolerance", NativeArcHotspotCoordinateTolerance, "toleranceUnit", "project-meter");
            conversionComplete = false;
        } else {
            bool stoppedBeforeConversion = false;
            if (ArcHotspotStopped (processControl, started, stoppedBeforeConversion)) {
                cooperativelyStopped = true;
                cooperativeCancelled = stoppedBeforeConversion;
                conversionResult = GS::ObjectState (
                    "status", "not-attempted",
                    "reason", cooperativeCancelled ? "cooperative-cancelled" : "cooperative-deadline-reached",
                    "nativeErrorCode", static_cast<Int32> (NoError));
                convertedNeigDetails = GS::ObjectState ("status", "not-returned");
                convertedCoordinateDetails = GS::ObjectState ("status", "not-returned");
                coordinateComparison = GS::ObjectState ("status", "not-attempted", "tolerance", NativeArcHotspotCoordinateTolerance, "toleranceUnit", "project-meter");
                conversionComplete = false;
            } else {
                API_Neig neigForConversion = returnedNeig;
                API_Coord3D convertedCoordinate = {};
#if TAPIR_AC_VERSION == 25
                const GSErrCode conversionError = ACAPI_Goodies (APIAny_NeigToCoordID, &neigForConversion, &convertedCoordinate, nullptr);
#else
                const GSErrCode conversionError = ACAPI_Element_NeigToCoord (&neigForConversion, &convertedCoordinate, nullptr);
#endif
                convertedNeigDetails = GS::ObjectState ("status", "returned", "value", ArcHotspotNeigDetails (neigForConversion));
                if (conversionError != NoError) {
                    conversionResult = GS::ObjectState ("status", "error", "nativeErrorCode", static_cast<Int32> (conversionError));
                    convertedCoordinateDetails = GS::ObjectState ("status", "unavailable-after-sdk-error");
                    coordinateComparison = GS::ObjectState ("status", "unavailable-after-sdk-error", "tolerance", NativeArcHotspotCoordinateTolerance, "toleranceUnit", "project-meter");
                    conversionComplete = false;
                } else if (!IsFiniteHotspotCoord (convertedCoordinate)) {
                    conversionResult = GS::ObjectState ("status", "error", "nativeErrorCode", static_cast<Int32> (NoError), "reason", "converted-coordinate-nonfinite");
                    convertedCoordinateDetails = GS::ObjectState ("status", "unavailable-nonfinite-coordinate");
                    coordinateComparison = GS::ObjectState ("status", "unavailable-nonfinite-coordinate", "tolerance", NativeArcHotspotCoordinateTolerance, "toleranceUnit", "project-meter");
                    conversionComplete = false;
                } else {
                    conversionResult = GS::ObjectState ("status", "converted", "nativeErrorCode", static_cast<Int32> (NoError));
                    convertedCoordinateDetails = GS::ObjectState ("status", "available", "point", DimensionPoint3D (convertedCoordinate));
                    const double dx = convertedCoordinate.x - returnedCoordinate.x;
                    const double dy = convertedCoordinate.y - returnedCoordinate.y;
                    const double dz = convertedCoordinate.z - returnedCoordinate.z;
                    const double distance = std::sqrt (dx * dx + dy * dy + dz * dz);
                    const bool finiteDistance = std::isfinite (distance);
                    coordinateComparison = finiteDistance
                        ? GS::ObjectState (
                            "status", distance <= NativeArcHotspotCoordinateTolerance ? "within-tolerance" : "difference-over-tolerance",
                            "distance", distance,
                            "distanceUnit", "project-meter",
                            "tolerance", NativeArcHotspotCoordinateTolerance,
                            "toleranceUnit", "project-meter")
                        : GS::ObjectState ("status", "unavailable-nonfinite-distance", "tolerance", NativeArcHotspotCoordinateTolerance, "toleranceUnit", "project-meter");
                    if (!finiteDistance) conversionComplete = false;
                }
            }
        }

        GS::ObjectState row (
            "index", static_cast<Int32> (index),
            "returnedNeig", ArcHotspotNeigDetails (returnedNeig),
            "returnedCoordinate", returnedCoordinateState,
            "eligibility", conversionEligible ? "eligible-arc-point-normal-same-host" : "skipped",
            "skipReason", skipReason,
            "conversion", conversionResult,
            "convertedNeig", convertedNeigDetails,
            "convertedCoordinate", convertedCoordinateDetails,
            "coordinateComparison", coordinateComparison);
        hotspotFacts.Push (row);
    }

    if (cooperativelyStopped) {
        return response ("partial", beforeState, notChecked, hostBeforeState, GS::ObjectState ("status", "not-read"),
            GS::ObjectState ("status", "unverified-after-cooperative-stop", "hostStable", false, "contextStable", false),
            hotspotCount, false, false, cooperativeCancelled ? "cancelled-during-conversion" : "deadline-during-conversion",
            hotspotFacts, ArcHotspotStoppedError (cooperativeCancelled));
    }

    bool stoppedBeforeFinalCheck = false;
    if (ArcHotspotStopped (processControl, started, stoppedBeforeFinalCheck)) {
        return response ("partial", beforeState, notChecked, hostBeforeState, GS::ObjectState ("status", "not-read"),
            GS::ObjectState ("status", "unverified-before-final-recheck", "hostStable", false, "contextStable", false),
            hotspotCount, false, conversionComplete, stoppedBeforeFinalCheck ? "cancelled-before-final-recheck" : "deadline-before-final-recheck",
            hotspotFacts, ArcHotspotStoppedError (stoppedBeforeFinalCheck));
    }

    RadialDefaultsContext after;
    ReadRadialDefaultsContext (after);
    const GS::ObjectState afterState = RadialDefaultsContextDetails (after);
    const ArcHotspotHostSnapshot hostAfterSnapshot = ReadArcHotspotHost (hostGuid);
    const GS::ObjectState hostAfterState = ArcHotspotHostDetails (hostAfterSnapshot);
    const bool hostStable = SameArcHotspotHost (hostBeforeSnapshot, hostAfterSnapshot);
    const bool contextStable = SameRadialDefaultsContext (before, after)
        && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, requestedFileScopeKey, after)
        && after.window.typeID == APIWind_FloorPlanID && after.database.typeID == APIWind_FloorPlanID
        && after.activeStory == requestedStory;
    const bool complete = hostStable && contextStable && allCoordinatesFinite;
    const char* coverageReason = !hostStable
        ? (hostAfterSnapshot.error != NoError ? "host-header-or-geometry-unavailable-during-recheck" : "host-guid-type-geometry-or-modification-stamp-changed")
        : !contextStable
            ? (!RadialDefaultsContextFactsAvailable (after) ? "saved-copy-floor-plan-database-or-story-unavailable-during-recheck" : "saved-copy-floor-plan-database-or-story-changed")
            : !allCoordinatesFinite ? "sdk-returned-nonfinite-coordinate"
            : "all-sdk-returned-hotspots-included";
    const GS::ObjectState consistency = ArcHotspotRecheckConsistency (
        hostBeforeSnapshot, hostAfterSnapshot, hostStable, contextStable, complete);
    return response (complete ? "available" : "partial", beforeState, afterState, hostBeforeState, hostAfterState,
        consistency, hotspotCount, complete, conversionComplete, coverageReason, hotspotFacts,
        ArcHotspotNoError ());
}

namespace {

constexpr std::size_t MaxDimensionDrawingPrimitives = 2048;
constexpr std::size_t MaxDimensionDrawingTextRuns = 256;
constexpr std::size_t MaxDimensionDrawingTextUnitsPerRun = 4096;
constexpr std::size_t MaxDimensionDrawingTextUnitsPerParent = 65536;
constexpr std::size_t MaxDimensionAutotextCandidates = 256;
constexpr std::size_t MaxDimensionAutotextHostTypesPerCandidate = 64;
constexpr auto NativeDimensionDrawingRunsBudget = std::chrono::milliseconds (5000);
constexpr double NativeDimensionDrawingRunsGeometryTolerance = 1.0e-9;

bool NativeDimensionDrawingRunsStopped (
    GS::ProcessControl& processControl,
    const std::chrono::steady_clock::time_point& started,
    bool& cancelled);

struct NativeDimensionDrawingTextRun {
    std::size_t primitiveOrdinal = 0;
    std::size_t textRunOrdinal = 0;
    API_PrimText primitive = {};
    NativeDimensionUtf16::CopyResult copiedText;
    bool finiteGeometry = true;
};

struct NativeDimensionDrawingRunsCallbackContext {
    std::size_t primitiveCount = 0;
    std::size_t textRunCount = 0;
    std::size_t textUnits = 0;
    std::array<std::size_t, 256> primitiveTypeCounts = {};
    std::vector<NativeDimensionDrawingTextRun> textRuns;
    bool primitiveLimitExceeded = false;
    bool textRunLimitExceeded = false;
    bool textUnitLimitExceeded = false;
    bool unsupportedPrimitiveType = false;
    bool allocationFailed = false;
    bool malformedUnicode = false;
    bool missingUnicode = false;
    bool invalidGeometry = false;
};

thread_local NativeDimensionDrawingRunsCallbackContext* tl_nativeDimensionDrawingRunsCallback = nullptr;
std::atomic_flag tl_nativeDimensionDrawingRunsActive = ATOMIC_FLAG_INIT;

GSErrCode CollectNativeDimensionDrawingRuns (
    const API_PrimElement* primitive,
    const void* /*par1*/,
    const void* par2,
    const void* /*par3*/)
{
    NativeDimensionDrawingRunsCallbackContext* context = tl_nativeDimensionDrawingRunsCallback;
    if (context == nullptr || primitive == nullptr)
        return APIERR_GENERAL;

    try {
        if (context->primitiveCount >= MaxDimensionDrawingPrimitives) {
            context->primitiveLimitExceeded = true;
            return APIERR_MEMFULL;
        }
        const std::size_t primitiveOrdinal = context->primitiveCount++;
        const Int32 primitiveType = static_cast<Int32> (primitive->header.typeID);
        if (primitiveType < 0 || static_cast<std::size_t> (primitiveType) >= context->primitiveTypeCounts.size ()) {
            context->unsupportedPrimitiveType = true;
            return APIERR_BADPARS;
        }
        ++context->primitiveTypeCounts[static_cast<std::size_t> (primitiveType)];
        if (primitive->header.typeID != API_PrimTextID)
            return NoError;

        const std::size_t textRunOrdinal = context->textRunCount++;
        if (textRunOrdinal >= MaxDimensionDrawingTextRuns) {
            context->textRunLimitExceeded = true;
            return APIERR_MEMFULL;
        }

        NativeDimensionDrawingTextRun run;
        run.primitiveOrdinal = primitiveOrdinal;
        run.textRunOrdinal = textRunOrdinal;
        run.primitive = primitive->text;
        run.finiteGeometry = std::isfinite (run.primitive.loc.x) && std::isfinite (run.primitive.loc.y)
            && std::isfinite (run.primitive.heightMM) && std::isfinite (run.primitive.widthMM)
            && std::isfinite (run.primitive.angle) && std::isfinite (run.primitive.widthFactor)
            && std::isfinite (run.primitive.charSpaceFactor);
        context->invalidGeometry = context->invalidGeometry || !run.finiteGeometry;

        run.copiedText = NativeDimensionUtf16::CopyCallbackBuffer (
            static_cast<const unsigned short*> (par2), MaxDimensionDrawingTextUnitsPerRun);
        if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::NullBuffer)
            context->missingUnicode = true;
        if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::MalformedSurrogates)
            context->malformedUnicode = true;
        if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::Available || run.copiedText.status == NativeDimensionUtf16::CopyStatus::Empty) {
            if (run.copiedText.units.size () > MaxDimensionDrawingTextUnitsPerParent - context->textUnits) {
                run.copiedText.units.clear ();
                run.copiedText.status = NativeDimensionUtf16::CopyStatus::OverLimit;
                context->textUnitLimitExceeded = true;
            }
        }
        if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::OverLimit)
            context->textUnitLimitExceeded = true;
        if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::AllocationFailure) {
            context->allocationFailed = true;
            return APIERR_MEMFULL;
        }
        const std::size_t acceptedUnits = run.copiedText.status == NativeDimensionUtf16::CopyStatus::Available
            ? run.copiedText.units.size () : 0;
        context->textRuns.push_back (std::move (run));
        context->textUnits += acceptedUnits;
        return context->textUnitLimitExceeded ? APIERR_MEMFULL : NoError;
    } catch (...) {
        context->allocationFailed = true;
        return APIERR_MEMFULL;
    }
}

const char* DimensionDrawingUtf16Status (const NativeDimensionUtf16::CopyStatus status)
{
    switch (status) {
        case NativeDimensionUtf16::CopyStatus::Available: return "available";
        case NativeDimensionUtf16::CopyStatus::Empty: return "empty";
        case NativeDimensionUtf16::CopyStatus::NullBuffer: return "unavailable-null-unicode-buffer";
        case NativeDimensionUtf16::CopyStatus::OverLimit: return "incomplete-unit-limit-exceeded-no-prefix";
        case NativeDimensionUtf16::CopyStatus::MalformedSurrogates: return "unavailable-malformed-utf16";
        case NativeDimensionUtf16::CopyStatus::AllocationFailure: return "unavailable-allocation-failure";
        case NativeDimensionUtf16::CopyStatus::InvalidLimit: return "unavailable-invalid-copy-limit";
    }
    return "unavailable-unknown-copy-status";
}

GS::UniString DimensionDrawingTextFromUnits (const std::vector<std::uint16_t>& units)
{
    static_assert (sizeof (GS::UniChar::Layout) == sizeof (std::uint16_t), "GS Unicode layout must be 16-bit.");
    GS::UniString result;
    if (!units.empty ())
        result.Assign (reinterpret_cast<const GS::UniChar::Layout*> (units.data ()), static_cast<USize> (units.size ()));
    return result;
}

GS::ObjectState NativeDimensionDrawingTextRunDetails (const NativeDimensionDrawingTextRun& run)
{
    GS::ObjectState result (
        "primitiveOrdinal", static_cast<Int32> (run.primitiveOrdinal),
        "textRunOrdinal", static_cast<Int32> (run.textRunOrdinal),
        "textStatus", DimensionDrawingUtf16Status (run.copiedText.status),
        "observedCodeUnits", static_cast<Int32> (run.copiedText.observedUnits),
        "geometryStatus", run.finiteGeometry ? "available" : "unavailable-nonfinite-geometry");
    if (run.copiedText.status == NativeDimensionUtf16::CopyStatus::Available
        || run.copiedText.status == NativeDimensionUtf16::CopyStatus::Empty) {
        result.Add ("content", DimensionDrawingTextFromUnits (run.copiedText.units));
        result.Add ("contentUnit", "utf16-code-unit-sequence");
    }
    if (run.finiteGeometry) {
        result.Add ("drawingFacts", GS::ObjectState (
            "ind1", static_cast<Int32> (run.primitive.ind1),
            "ind2", static_cast<Int32> (run.primitive.ind2),
            "paramInd", static_cast<Int32> (run.primitive.paramInd),
            "fontIndex", static_cast<Int32> (run.primitive.font),
            "faceBits", static_cast<Int32> (run.primitive.faceBits),
            "effectBits", static_cast<Int32> (run.primitive.effectBits),
            "protectedAutotextReferenceEffect", (run.primitive.effectBits & APIEffect_Protected) != 0,
            "anchor", static_cast<Int32> (run.primitive.anchor),
            "justification", static_cast<Int32> (run.primitive.just),
            "location", Create2DCoordinateObjectState (run.primitive.loc),
            "locationUnit", "project-meter",
            "height", run.primitive.heightMM,
            "heightUnit", "millimeter-paper-size",
            "width", run.primitive.widthMM,
            "widthUnit", "millimeter-paper-size",
            "angle", run.primitive.angle,
            "angleUnit", "radian",
            "widthFactor", run.primitive.widthFactor,
            "characterSpacingFactor", run.primitive.charSpaceFactor));
    }
    return result;
}

GS::ObjectState NativeDimensionDrawingRunsDetails (const NativeDimensionDrawingRunsCallbackContext& context, const GSErrCode sdkError)
{
    const bool complete = sdkError == NoError && !context.primitiveLimitExceeded && !context.textRunLimitExceeded
        && !context.textUnitLimitExceeded && !context.unsupportedPrimitiveType && !context.allocationFailed
        && !context.malformedUnicode && !context.missingUnicode && !context.invalidGeometry;
    const char* reason = complete ? "all-returned-text-primitives-copied-and-validated"
        : context.primitiveLimitExceeded ? "primitive-count-limit-exceeded-no-prefix"
        : context.textRunLimitExceeded ? "text-run-count-limit-exceeded-no-prefix"
        : context.textUnitLimitExceeded ? "utf16-unit-limit-exceeded-no-prefix"
        : context.allocationFailed ? "bounded-copy-allocation-failed"
        : context.unsupportedPrimitiveType ? "unsupported-primitive-type-id"
        : context.malformedUnicode ? "malformed-utf16-in-returned-text"
        : context.missingUnicode ? "one-or-more-text-primitives-had-no-unicode-buffer"
        : context.invalidGeometry ? "nonfinite-text-primitive-geometry"
        : "shape-primitives-sdk-call-failed";
    GS::ObjectState result (
        "status", complete ? "complete" : "partial",
        "complete", complete,
        "coverageReason", reason,
        "primitiveCount", static_cast<Int32> (context.primitiveCount),
        "textRunCount", static_cast<Int32> (context.textRunCount),
        "textRunCountIncluded", static_cast<Int32> (context.textRuns.size ()),
        "utf16CodeUnitsCopied", static_cast<Int32> (context.textUnits),
        "drawingCallStatus", sdkError == NoError ? "success" : "error",
        "drawingCallErrorCode", static_cast<Int32> (sdkError),
        "limits", GS::ObjectState (
            "primitiveCount", static_cast<Int32> (MaxDimensionDrawingPrimitives),
            "textRuns", static_cast<Int32> (MaxDimensionDrawingTextRuns),
            "codeUnitsPerRun", static_cast<Int32> (MaxDimensionDrawingTextUnitsPerRun),
            "codeUnitsPerParent", static_cast<Int32> (MaxDimensionDrawingTextUnitsPerParent)),
        "allStories", false,
        "dontClip", true,
        "meaning", "parent-correlated-ShapesPrimsExt-text-runs-not-note-source-or-observed-dimension-display");
    const auto& typeCounts = result.AddList<GS::ObjectState> ("primitiveTypeCounts");
    for (std::size_t type = 0; type < context.primitiveTypeCounts.size (); ++type) {
        if (context.primitiveTypeCounts[type] > 0)
            typeCounts (GS::ObjectState ("primitiveTypeId", static_cast<Int32> (type), "count", static_cast<Int32> (context.primitiveTypeCounts[type])));
    }
    const auto& runs = result.AddList<GS::ObjectState> ("textRuns");
    for (const NativeDimensionDrawingTextRun& run : context.textRuns)
        runs (NativeDimensionDrawingTextRunDetails (run));
    return result;
}

GS::ObjectState NativeDimensionAutotextMetadataUnavailable (const char* status, const char* reason, const Int32 errorCode = 0)
{
    GS::ObjectState result (
        "status", status,
        "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
        "coverageReason", reason,
        "candidateCount", 0,
        "keywords", GS::Array<GS::ObjectState> ());
    if (errorCode != 0) result.Add ("nativeErrorCode", errorCode);
    return result;
}

GS::ObjectState ReadNativeDimensionAutotextCandidates (
    const API_ElemTypeID ownerType,
    const bool hasAppearance,
    const Int32 appearanceValue,
    GS::ProcessControl& processControl,
    const std::chrono::steady_clock::time_point& started,
    bool& stopped)
{
#if TAPIR_AC_VERSION < 26
    (void) ownerType;
    (void) hasAppearance;
    (void) appearanceValue;
    (void) processControl;
    (void) started;
    stopped = false;
    return NativeDimensionAutotextMetadataUnavailable ("unavailable", "autotext-metadata-api-not-available-in-ac25");
#else
    stopped = false;
    if ((ownerType == API_DimensionID) != hasAppearance || (hasAppearance && (appearanceValue < APIApp_Normal || appearanceValue > APIApp_Elev)))
        return NativeDimensionAutotextMetadataUnavailable ("unavailable", "fresh-parent-appearance-or-family-facts-invalid", APIERR_BADPARS);

    API_ElemType owner = {};
    owner.typeID = ownerType;
    owner.variationID = APIVarId_Generic;
    GS::Optional<API_AppearID> appearance;
    if (hasAppearance) appearance = static_cast<API_AppearID> (appearanceValue);
    std::unordered_map<API_NoteAutotextID, std::unordered_set<API_ElemType>> candidates;
    const GSErrCode catalogError = ACAPI_AutoText_GetNoteAutotexts (owner, candidates, appearance);
    if (catalogError != NoError)
        return NativeDimensionAutotextMetadataUnavailable ("unavailable", "autotext-candidate-catalog-api-failed", static_cast<Int32> (catalogError));

    if (candidates.size () > MaxDimensionAutotextCandidates)
        return GS::ObjectState (
            "status", "partial",
            "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
            "coverageReason", "autotext-candidate-count-limit-exceeded-no-prefix",
            "candidateCount", static_cast<Int32> (candidates.size ()),
            "candidateLimit", static_cast<Int32> (MaxDimensionAutotextCandidates),
            "keywords", GS::Array<GS::ObjectState> ());
    for (const auto& candidate : candidates) {
        if (candidate.second.size () > MaxDimensionAutotextHostTypesPerCandidate)
            return GS::ObjectState (
                "status", "partial",
                "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
                "coverageReason", "autotext-host-type-set-limit-exceeded-no-prefix",
                "candidateCount", static_cast<Int32> (candidates.size ()),
                "hostTypesPerCandidateLimit", static_cast<Int32> (MaxDimensionAutotextHostTypesPerCandidate),
                "keywords", GS::Array<GS::ObjectState> ());
    }

    struct KeywordEntry {
        Int32 id = 0;
        GS::UniString keyword;
        GS::Array<GS::ObjectState> hostTypes;
        bool general = false;
        bool notRead = false;
        GSErrCode error = NoError;
    };
    std::vector<KeywordEntry> ordered;
    try {
        ordered.reserve (candidates.size ());
        for (const auto& candidate : candidates) {
            KeywordEntry entry;
            entry.id = static_cast<Int32> (candidate.first);
            entry.general = candidate.second.empty ();
            std::vector<API_ElemType> hostTypes (candidate.second.begin (), candidate.second.end ());
            std::sort (hostTypes.begin (), hostTypes.end (), [] (const API_ElemType& left, const API_ElemType& right) {
                if (left.typeID != right.typeID) return left.typeID < right.typeID;
                if (left.variationID != right.variationID) return left.variationID < right.variationID;
                return FixtureUtf8 (APIGuidToString (left.classID)) < FixtureUtf8 (APIGuidToString (right.classID));
            });
            for (const API_ElemType& hostType : hostTypes) {
                entry.hostTypes.Push (GS::ObjectState (
                    "typeId", static_cast<Int32> (hostType.typeID),
                    "variationId", static_cast<Int32> (hostType.variationID),
                    "classId", CreateGuidObjectState (hostType.classID)));
            }
            bool cancelled = false;
            if (NativeDimensionDrawingRunsStopped (processControl, started, cancelled)) {
                stopped = true;
                entry.notRead = true;
            } else {
                entry.error = ACAPI_AutoText_GetNoteKeywordForAutotextID (candidate.first, entry.keyword);
            }
            ordered.push_back (std::move (entry));
        }
    } catch (...) {
        return GS::ObjectState (
            "status", "partial",
            "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
            "coverageReason", "bounded-autotext-catalog-allocation-failed-no-prefix",
            "candidateCount", static_cast<Int32> (candidates.size ()),
            "keywords", GS::Array<GS::ObjectState> ());
    }
    std::sort (ordered.begin (), ordered.end (), [] (const KeywordEntry& left, const KeywordEntry& right) { return left.id < right.id; });

    std::size_t totalKeywordUnits = 0;
    bool anyKeywordError = false;
    bool anyKeywordNotRead = false;
    std::map<std::string, std::size_t> keywordCounts;
    for (const KeywordEntry& entry : ordered) {
        if (entry.notRead) { anyKeywordNotRead = true; continue; }
        if (entry.error != NoError) { anyKeywordError = true; continue; }
        const std::size_t unitCount = static_cast<std::size_t> (entry.keyword.GetLength ());
        if (unitCount > MaxDimensionDrawingTextUnitsPerRun || unitCount > MaxDimensionDrawingTextUnitsPerParent - totalKeywordUnits)
            return GS::ObjectState (
                "status", "partial",
                "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
                "coverageReason", "autotext-keyword-unicode-unit-limit-exceeded-no-prefix",
                "candidateCount", static_cast<Int32> (ordered.size ()),
                "keywordUnitsPerCandidateLimit", static_cast<Int32> (MaxDimensionDrawingTextUnitsPerRun),
                "keywordUnitsTotalLimit", static_cast<Int32> (MaxDimensionDrawingTextUnitsPerParent),
                "keywords", GS::Array<GS::ObjectState> ());
        totalKeywordUnits += unitCount;
        ++keywordCounts[FixtureUtf8 (entry.keyword)];
    }
    GS::ObjectState result (
        "status", anyKeywordError || anyKeywordNotRead ? "partial" : "available",
        "meaning", "evaluable-autotext-candidate-catalog-only-not-used-token-or-note-source-proof",
        "coverageReason", stopped ? "cooperative-cancelled-or-deadline-between-keyword-lookups" : anyKeywordError ? "one-or-more-keyword-lookups-failed" : "complete-candidate-catalog-returned",
        "ownerNativeTypeId", static_cast<Int32> (ownerType),
        "appearanceStatus", hasAppearance ? "available" : "not-applicable-to-this-dimension-family",
        "candidateCount", static_cast<Int32> (ordered.size ()),
        "keywordUnitsTotal", static_cast<Int32> (totalKeywordUnits),
        "hostTypesPerCandidateLimit", static_cast<Int32> (MaxDimensionAutotextHostTypesPerCandidate));
    if (hasAppearance) result.Add ("appearance", appearanceValue);
    const auto& keywords = result.AddList<GS::ObjectState> ("keywords");
    for (const KeywordEntry& entry : ordered) {
        const bool lookupSucceeded = entry.error == NoError;
        GS::ObjectState item (
            "autotextId", entry.id,
            "keywordStatus", entry.notRead ? "not-read" : lookupSucceeded ? "available" : "unavailable",
            "generallyEvaluable", entry.general,
            "evaluableForElementTypes", entry.hostTypes,
            "keywordCollision", lookupSucceeded && keywordCounts[FixtureUtf8 (entry.keyword)] > 1);
        if (!entry.notRead && lookupSucceeded) item.Add ("keyword", entry.keyword);
        else if (!entry.notRead) item.Add ("nativeErrorCode", static_cast<Int32> (entry.error));
        keywords (item);
    }
    return result;
#endif
}

struct NativeDimensionDrawingParentFacts {
    bool valid = false;
    bool supported = false;
    bool hasGuid = false;
    bool hasNativeType = false;
    bool hasVariation = false;
    bool hasStory = false;
    bool hasDatabase = false;
    bool hasStamp = false;
    bool hasGeometry = false;
    bool hasFingerprint = false;
    GS::UniString elementType;
    GS::UniString guid;
    GS::UniString databaseGuid;
    GS::UniString fingerprint;
    Int32 nativeTypeId = 0;
    Int32 variationId = 0;
    Int32 nativeErrorCode = 0;
    Int32 storyIndex = -1;
    Int64 modificationStamp = 0;
    Int32 appearance = -1;
    bool hasAppearance = false;
    GS::ObjectState facts;
    const char* errorReason = "exact-parent-read-unavailable";
};

GS::ObjectState NativeDimensionDrawingParentFactsDetails (const NativeDimensionDrawingParentFacts& facts)
{
    return facts.facts.GetFieldNames ().GetSize () > 0
        ? facts.facts
        : GS::ObjectState ("status", "unavailable", "reason", facts.errorReason);
}

NativeDimensionDrawingParentFacts ReadNativeDimensionDrawingParentFacts (const API_Guid& requestedGuid, GS::ProcessControl& processControl)
{
    GS::Array<GS::UniString> guids;
    guids.Push (APIGuidToString (requestedGuid));
    const GS::ObjectState response = GetNativeDimensionDetailsCommand ().Execute (
        GS::ObjectState ("guids", guids), processControl);
    GS::Array<GS::ObjectState> rows;
    if (!response.Get ("elements", rows) || rows.GetSize () != 1) {
        NativeDimensionDrawingParentFacts facts;
        facts.errorReason = "exact-parent-reader-returned-no-single-row";
        return facts;
    }

    const GS::ObjectState& row = rows[0];
    NativeDimensionDrawingParentFacts facts;
    GS::UniString status;
    const bool rowReadSucceeded = row.Get ("status", status) && status == "success";
    if (!row.Get ("elementType", facts.elementType) || !row.Get ("nativeElementTypeId", facts.nativeTypeId)) {
        facts.errorReason = "exact-element-type-read-unavailable";
        return facts;
    }
    facts.hasNativeType = true;
    const GS::ObjectState* elementId = row.Get ("elementId");
    GS::UniString parentGuid;
    if (elementId == nullptr || !elementId->Get ("guid", parentGuid)) {
        facts.errorReason = "exact-parent-reader-element-guid-unavailable";
        return facts;
    }
    const API_Guid parsedParentGuid = APIGuidFromString (parentGuid.ToCStr ());
    if (parsedParentGuid != requestedGuid) {
        facts.errorReason = "exact-parent-reader-returned-a-different-guid";
        return facts;
    }
    facts.guid = parentGuid;
    facts.hasGuid = true;
    const API_ElemTypeID nativeType = static_cast<API_ElemTypeID> (facts.nativeTypeId);
    facts.supported = nativeType == API_DimensionID || nativeType == API_RadialDimensionID
        || nativeType == API_AngleDimensionID || nativeType == API_LevelDimensionID;
    GS::ObjectState observedFacts (
        "elementType", facts.elementType,
        "nativeElementTypeId", facts.nativeTypeId,
        "elementId", CreateGuidObjectState (requestedGuid));
    Int32 variationId = 0;
    if (row.Get ("elementVariationId", variationId)) {
        facts.variationId = variationId;
        facts.hasVariation = true;
        observedFacts.Add ("elementVariationId", variationId);
    }
    if (!facts.supported) {
        facts.errorReason = "exact-guid-is-not-a-supported-dimension-family";
        facts.facts = observedFacts;
        return facts;
    }

    facts.hasStory = row.Get ("storyIndex", facts.storyIndex);
    facts.hasStamp = row.Get ("modificationStamp", facts.modificationStamp);
    const GS::ObjectState* database = row.Get ("database");
    GS::UniString databaseStatus;
    const GS::ObjectState* databaseId = database == nullptr ? nullptr : database->Get ("databaseId");
    facts.hasDatabase = database != nullptr && database->Get ("status", databaseStatus)
        && databaseStatus == "available" && databaseId != nullptr
        && databaseId->Get ("guid", facts.databaseGuid) && !facts.databaseGuid.IsEmpty ();
    if (facts.hasStory) observedFacts.Add ("storyIndex", facts.storyIndex);
    if (facts.hasDatabase) observedFacts.Add ("databaseId", CreateGuidObjectState (APIGuidFromString (facts.databaseGuid.ToCStr ())));
    if (facts.hasStamp) observedFacts.Add ("modificationStamp", facts.modificationStamp);
    bool hasNativeErrorCode = row.Get ("errorCode", facts.nativeErrorCode);
    if (!hasNativeErrorCode) {
        const GS::ObjectState* memo = row.Get ("memo");
        hasNativeErrorCode = memo != nullptr && memo->Get ("errorCode", facts.nativeErrorCode);
    }
    if (hasNativeErrorCode) observedFacts.Add ("nativeErrorCode", facts.nativeErrorCode);

    if (nativeType == API_DimensionID) {
        const GS::ObjectState* geometry = row.Get ("geometryAndSettings");
        if (geometry == nullptr || !geometry->Get ("appearance", facts.appearance)) {
            facts.errorReason = "linear-parent-appearance-unavailable-for-candidate-metadata";
            facts.facts = observedFacts;
            return facts;
        }
        facts.hasAppearance = true;
    }
    const GS::ObjectState* geometry = row.Get ("geometryAndSettings");
    facts.hasGeometry = geometry != nullptr;
    if (facts.hasGeometry) observedFacts.Add ("geometryAndSettings", *geometry);
    facts.hasFingerprint = row.Get ("fingerprint", facts.fingerprint) && !facts.fingerprint.IsEmpty ();
    if (facts.hasFingerprint) observedFacts.Add ("factualFingerprint", facts.fingerprint);
    facts.facts = observedFacts;
    if (!rowReadSucceeded) {
        facts.errorReason = facts.hasDatabase && facts.hasStory && facts.hasStamp
            ? "exact-parent-reader-returned-a-partial-or-error-row"
            : "exact-parent-reader-returned-an-error-row-with-incomplete-identity";
        return facts;
    }
    if (!facts.hasStory || !facts.hasStamp || !facts.hasDatabase || geometry == nullptr || !facts.hasFingerprint || !facts.hasVariation) {
        facts.errorReason = "exact-parent-reader-omitted-required-identity-or-fingerprint";
        return facts;
    }
    const GS::ObjectState* consistency = row.Get ("readConsistency");
    GS::UniString consistencyStatus;
    if (consistency == nullptr || !consistency->Get ("status", consistencyStatus)
        || consistencyStatus != "same-guid-type-and-modification-stamp-through-memo-read") {
        facts.errorReason = "exact-parent-facts-were-not-a-coherent-native-read";
        return facts;
    }

    facts.valid = true;
    return facts;
}

bool SameNativeDimensionDrawingParentFacts (const NativeDimensionDrawingParentFacts& before, const NativeDimensionDrawingParentFacts& after)
{
    return before.valid && after.valid && before.supported && after.supported
        && before.guid == after.guid && before.elementType == after.elementType
        && before.nativeTypeId == after.nativeTypeId && before.variationId == after.variationId && before.storyIndex == after.storyIndex
        && before.databaseGuid == after.databaseGuid && before.modificationStamp == after.modificationStamp
        && before.fingerprint == after.fingerprint;
}

GS::ObjectState NativeDimensionDrawingParentNotRead (const char* reason)
{
    return GS::ObjectState (
        "status", "not-read",
        "coverageReason", reason,
        "meaning", "no-parent-drawing-or-autotext-facts-were-read");
}

GS::ObjectState NativeDimensionDrawingRunsResponse (
    const char* status,
    const GS::UniString& runId,
    const GS::UniString& requestedProjectPath,
    const GS::UniString& fileScopeKey,
    const Int32 requestedStory,
    const GS::ObjectState& contextBefore,
    const GS::ObjectState& contextAfter,
    const GS::ObjectState& consistency,
    const GS::Array<GS::ObjectState>& rows,
    const GS::ObjectState& error,
    const GS::Array<GS::UniString>& parentGuids = GS::Array<GS::UniString> ())
{
    GS::Array<GS::ObjectState> requestParents;
    for (const GS::UniString& guid : parentGuids)
        requestParents.Push (GS::ObjectState ("elementId", GS::ObjectState ("guid", guid)));
    return GS::ObjectState (
        "schemaVersion", "native-dimension-parent-drawing-runs/v1",
        "archicadMajor", TAPIR_AC_VERSION,
        "status", status,
        "meaning", "read-only-parent-correlated-drawing-text-and-candidate-autotext-metadata-not-note-source-or-display-proof",
        "request", GS::ObjectState (
            "schemaVersion", "native-dimension-parent-drawing-runs-request/v1",
            "runId", runId,
            "expectedProjectPath", requestedProjectPath,
            "fileScopeKey", fileScopeKey,
            "storyIndex", requestedStory,
            "elements", requestParents),
        "contextBefore", contextBefore,
        "contextAfter", contextAfter,
        "readConsistency", consistency,
        "elements", rows,
        "error", error);
}

bool NativeDimensionDrawingRunsStopped (GS::ProcessControl& processControl, const std::chrono::steady_clock::time_point& started, bool& cancelled)
{
    cancelled = processControl.TestBreak ();
    return cancelled || std::chrono::steady_clock::now () - started >= NativeDimensionDrawingRunsBudget;
}

} // namespace

GetNativeDimensionParentDrawingRunsCommand::GetNativeDimensionParentDrawingRunsCommand () : CommandBase (CommonSchema::NotUsed) {}
GS::String GetNativeDimensionParentDrawingRunsCommand::GetName () const { return "GetNativeDimensionParentDrawingRuns"; }
GS::Optional<GS::UniString> GetNativeDimensionParentDrawingRunsCommand::GetInputParametersSchema () const
{
    return R"({"type":"object","properties":{"schemaVersion":{"type":"string","enum":["native-dimension-parent-drawing-runs-request/v1"]},"runId":{"type":"string","pattern":"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$"},"expectedProjectPath":{"type":"string","minLength":1,"maxLength":1024},"fileScopeKey":{"type":"string","minLength":1,"maxLength":1100},"storyIndex":{"type":"integer","minimum":-32768,"maximum":32767},"elements":{"type":"array","minItems":1,"maxItems":10,"items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","format":"uuid"}},"required":["guid"],"additionalProperties":false}},"required":["elementId"],"additionalProperties":false}}},"required":["schemaVersion","runId","expectedProjectPath","fileScopeKey","storyIndex","elements"],"additionalProperties":false})";
}
GS::Optional<GS::UniString> GetNativeDimensionParentDrawingRunsCommand::GetResponseSchema () const { return {}; }
GS::ObjectState GetNativeDimensionParentDrawingRunsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const
{
    GS::UniString schemaVersion, runId, expectedProjectPath, fileScopeKey;
    Int32 requestedStory = -1;
    GS::Array<GS::ObjectState> requested;
    const GS::HashSet<GS::String> names = parameters.GetFieldNames ();
    const bool exactFields = names.GetSize () == 6
        && parameters.Get ("schemaVersion", schemaVersion) && parameters.Get ("runId", runId)
        && parameters.Get ("expectedProjectPath", expectedProjectPath) && parameters.Get ("fileScopeKey", fileScopeKey)
        && parameters.Get ("storyIndex", requestedStory) && parameters.Get ("elements", requested);

    GS::Array<GS::ObjectState> rows;
    GS::ObjectState contextNotRead ("status", "not-read");
    const GS::ObjectState errorNotChecked ("status", "none");
    if (!exactFields || schemaVersion != "native-dimension-parent-drawing-runs-request/v1"
        || runId.IsEmpty () || FixtureUtf8 (runId).size () > 48
        || expectedProjectPath.IsEmpty () || FixtureUtf8 (expectedProjectPath).size () > 1024
        || fileScopeKey.IsEmpty () || FixtureUtf8 (fileScopeKey).size () > 1100
        || requestedStory < std::numeric_limits<short>::min () || requestedStory > std::numeric_limits<short>::max ()
        || requested.GetSize () < 1 || requested.GetSize () > 10) {
        return NativeDimensionDrawingRunsResponse (
            "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
            contextNotRead, contextNotRead,
            GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "invalid-closed-request"), rows,
            GS::ObjectState ("status", "present", "code", "INVALID_REQUEST", "message", "Requires the closed request schema and one to ten exact dimension GUIDs."));
    }
    const std::string runIdUtf8 = FixtureUtf8 (runId);
    if (!std::all_of (runIdUtf8.begin (), runIdUtf8.end (), [] (unsigned char value) { return std::isalnum (value) || value == '_' || value == '-'; })) {
        return NativeDimensionDrawingRunsResponse (
            "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
            contextNotRead, contextNotRead,
            GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "invalid-run-id"), rows, errorNotChecked);
    }

    GS::Array<API_Guid> guids;
    GS::Array<GS::UniString> normalizedGuids;
    GS::HashSet<API_Guid> seen;
    for (const GS::ObjectState& item : requested) {
        const GS::HashSet<GS::String> itemFields = item.GetFieldNames ();
        const GS::ObjectState* elementId = item.Get ("elementId");
        GS::UniString guidText;
        if (itemFields.GetSize () != 1 || elementId == nullptr || item.GetFieldNames ().Contains ("guid")
            || elementId->GetFieldNames ().GetSize () != 1 || !elementId->Get ("guid", guidText)) {
            return NativeDimensionDrawingRunsResponse (
                "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
                contextNotRead, contextNotRead,
                GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "invalid-exact-parent-selector"), rows,
                GS::ObjectState ("status", "present", "code", "INVALID_PARENT_SELECTOR", "message", "Each parent selector must contain only elementId.guid."));
        }
        const API_Guid guid = APIGuidFromString (guidText.ToCStr ());
        if (guid == APINULLGuid || seen.Contains (guid)) {
            return NativeDimensionDrawingRunsResponse (
                "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
                contextNotRead, contextNotRead,
                GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "invalid-or-repeated-parent-guid"), rows,
                GS::ObjectState ("status", "present", "code", "INVALID_PARENT_GUID", "message", "Parent GUIDs must be valid, non-null and unique."));
        }
        const GS::UniString normalized = APIGuidToString (guid);
        if (FixtureUtf8 (normalized).size () != 36) {
            return NativeDimensionDrawingRunsResponse (
                "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
                contextNotRead, contextNotRead,
                GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "invalid-parent-guid-serialization"), rows,
                GS::ObjectState ("status", "present", "code", "INVALID_PARENT_GUID", "message", "A parent GUID did not normalize to canonical text."));
        }
        seen.Add (guid);
        guids.Push (guid);
        normalizedGuids.Push (normalized);
    }

    if (tl_nativeDimensionDrawingRunsActive.test_and_set (std::memory_order_acquire)) {
        for (const GS::UniString& guid : normalizedGuids)
            rows.Push (GS::ObjectState ("requestedGuid", guid, "status", "refused", "coverageReason", "nonreentrant-diagnostic-already-active"));
        return NativeDimensionDrawingRunsResponse (
            "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
            contextNotRead, contextNotRead,
            GS::ObjectState ("status", "not-checked", "contextStable", false, "reason", "diagnostic-reentrancy-guard"), rows,
            GS::ObjectState ("status", "present", "code", "NONREENTRANT_DIAGNOSTIC", "message", "This diagnostic is non-reentrant."), normalizedGuids);
    }
    const GS::OnExit activeGuard ([] () {
        tl_nativeDimensionDrawingRunsCallback = nullptr;
        tl_nativeDimensionDrawingRunsActive.clear (std::memory_order_release);
    });
    const auto started = std::chrono::steady_clock::now ();

    RadialDefaultsContext requestBefore;
    ReadRadialDefaultsContext (requestBefore);
    const GS::ObjectState requestBeforeState = RadialDefaultsContextDetails (requestBefore);
    const bool requestScopeValid = IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, requestBefore)
        && requestBefore.window.typeID == APIWind_FloorPlanID && requestBefore.database.typeID == APIWind_FloorPlanID
        && requestBefore.activeStory == requestedStory;
    if (!requestScopeValid) {
        for (const GS::UniString& guid : normalizedGuids)
            rows.Push (GS::ObjectState ("requestedGuid", guid, "status", "not-read", "coverageReason", "exact-task-owned-floor-plan-story-scope-refused"));
        return NativeDimensionDrawingRunsResponse (
            "refused", runId, expectedProjectPath, fileScopeKey, requestedStory,
            requestBeforeState, GS::ObjectState ("status", "not-checked-before-parent-drawing-call"),
            GS::ObjectState ("status", "not-read", "contextStable", false), rows,
            GS::ObjectState ("status", "present", "code", "PARENT_DRAWING_SCOPE_REFUSED", "message", "Requires the exact saved Solo task-owned copy, current Floor Plan database/window and requested story."), normalizedGuids);
    }

    bool requestCancelled = false;
    if (NativeDimensionDrawingRunsStopped (processControl, started, requestCancelled)) {
        for (const GS::UniString& guid : normalizedGuids)
            rows.Push (GS::ObjectState ("requestedGuid", guid, "status", "not-read", "coverageReason", requestCancelled ? "cooperative-cancelled-before-parent-read" : "cooperative-deadline-before-parent-read"));
        return NativeDimensionDrawingRunsResponse (
            "partial", runId, expectedProjectPath, fileScopeKey, requestedStory,
            requestBeforeState, requestBeforeState,
            GS::ObjectState ("status", "stable-before-read", "contextStable", true), rows,
            GS::ObjectState ("status", "present", "code", requestCancelled ? "COOPERATIVE_CANCELLED" : "COOPERATIVE_DEADLINE_REACHED", "message", "Stopped before parent extraction at a cooperative boundary."), normalizedGuids);
    }

    bool allRowsComplete = true;
    for (UInt32 index = 0; index < guids.GetSize (); ++index) {
        const API_Guid guid = guids[index];
        const GS::UniString guidText = normalizedGuids[index];
        bool stopped = false;
        if (NativeDimensionDrawingRunsStopped (processControl, started, stopped)) {
            allRowsComplete = false;
            for (UInt32 remaining = index; remaining < guids.GetSize (); ++remaining)
                rows.Push (GS::ObjectState ("requestedGuid", normalizedGuids[remaining], "status", "not-read", "coverageReason", stopped ? "cooperative-cancelled-before-parent-read" : "cooperative-deadline-before-parent-read"));
            break;
        }

        RadialDefaultsContext parentContextBefore;
        ReadRadialDefaultsContext (parentContextBefore);
        const GS::ObjectState parentContextBeforeState = RadialDefaultsContextDetails (parentContextBefore);
        if (!SameRadialDefaultsContext (requestBefore, parentContextBefore)
            || !IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, parentContextBefore)
            || parentContextBefore.activeStory != requestedStory) {
            allRowsComplete = false;
            rows.Push (GS::ObjectState (
                "requestedGuid", guidText,
                "status", "refused",
                "contextBefore", parentContextBeforeState,
                "contextAfter", GS::ObjectState ("status", "not-read"),
                "coverageReason", "scope-or-current-context-changed-before-parent-read",
                "drawingRuns", NativeDimensionDrawingRunsDetails (NativeDimensionDrawingRunsCallbackContext (), APIERR_BADID),
                "autotextMetadata", NativeDimensionAutotextMetadataUnavailable ("not-read", "parent-scope-guard-refused"),
                "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "no-note-source-mapping"),
                "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "no-independent-display-channel"),
                "noteCorrespondence", GS::ObjectState ("status", "unproven", "reason", "drawing-runs-have-no-note-identifiers")));
            continue;
        }

        const NativeDimensionDrawingParentFacts before = ReadNativeDimensionDrawingParentFacts (guid, processControl);
        const bool beforeMatchesCurrentDatabase = before.valid
            && before.databaseGuid == APIGuidToString (requestBefore.database.databaseUnId.elemSetId);
        if (!before.valid || !beforeMatchesCurrentDatabase) {
            allRowsComplete = false;
            rows.Push (GS::ObjectState (
                "requestedGuid", guidText,
                "status", before.supported ? "partial" : "refused",
                "parentBefore", NativeDimensionDrawingParentFactsDetails (before),
                "contextBefore", parentContextBeforeState,
                "contextAfter", GS::ObjectState ("status", "not-read"),
                "coverageReason", before.valid ? "exact-parent-database-does-not-match-current-floor-plan-database" : before.errorReason,
                "drawingRuns", GS::ObjectState ("status", "not-read", "coverageReason", "coherent-dimension-parent-facts-unavailable"),
                "autotextMetadata", NativeDimensionAutotextMetadataUnavailable ("not-read", "coherent-dimension-parent-facts-unavailable"),
                "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "no-note-source-mapping"),
                "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "no-independent-display-channel"),
                "noteCorrespondence", GS::ObjectState ("status", "unproven", "reason", "drawing-runs-have-no-note-identifiers")));
            continue;
        }
        if (before.storyIndex != requestedStory) {
            allRowsComplete = false;
            rows.Push (GS::ObjectState (
                "requestedGuid", guidText, "status", "refused", "parentBefore", before.facts,
                "contextBefore", parentContextBeforeState, "contextAfter", GS::ObjectState ("status", "not-read"),
                "coverageReason", "exact-guid-is-not-on-requested-story",
                "drawingRuns", GS::ObjectState ("status", "not-read", "coverageReason", "story-scope-refused"),
                "autotextMetadata", NativeDimensionAutotextMetadataUnavailable ("not-read", "story-scope-refused"),
                "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "no-note-source-mapping"),
                "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "no-independent-display-channel"),
                "noteCorrespondence", GS::ObjectState ("status", "unproven", "reason", "drawing-runs-have-no-note-identifiers")));
            continue;
        }

        NativeDimensionDrawingRunsCallbackContext callbackContext;
        tl_nativeDimensionDrawingRunsCallback = &callbackContext;
        API_Elem_Head parentHead = {};
        parentHead.guid = guid;
        API_ShapePrimsParams shapeParams = {};
        shapeParams.dontClip = true;
        shapeParams.allStories = false;
#if defined(ServerMainVers_2700)
        const GSErrCode shapeError = ACAPI_DrawingPrimitive_ShapePrimsExt (
            parentHead, CollectNativeDimensionDrawingRuns, &shapeParams);
#else
        const GSErrCode shapeError = ACAPI_Element_ShapePrimsExt (parentHead, CollectNativeDimensionDrawingRuns, &shapeParams);
#endif
        tl_nativeDimensionDrawingRunsCallback = nullptr;

        const bool shapeCoverageComplete = shapeError == NoError && !callbackContext.primitiveLimitExceeded
            && !callbackContext.textRunLimitExceeded && !callbackContext.textUnitLimitExceeded
            && !callbackContext.unsupportedPrimitiveType && !callbackContext.allocationFailed
            && !callbackContext.malformedUnicode && !callbackContext.missingUnicode && !callbackContext.invalidGeometry;
        bool stoppedAfterShape = false;
        const bool budgetAfterShape = NativeDimensionDrawingRunsStopped (processControl, started, stoppedAfterShape);
        bool metadataStopped = false;
        GS::ObjectState metadata;
        if (shapeCoverageComplete && !budgetAfterShape)
            metadata = ReadNativeDimensionAutotextCandidates (
                static_cast<API_ElemTypeID> (before.nativeTypeId), before.hasAppearance, before.appearance,
                processControl, started, metadataStopped);
        else {
#if TAPIR_AC_VERSION < 26
            metadata = NativeDimensionAutotextMetadataUnavailable ("unavailable", "autotext-metadata-api-not-available-in-ac25");
#else
            metadata = NativeDimensionAutotextMetadataUnavailable ("not-read", budgetAfterShape ? "cooperative-stop-before-metadata-call" : "drawing-run-coverage-incomplete");
#endif
        }

        NativeDimensionDrawingParentFacts after;
        RadialDefaultsContext parentContextAfter;
        bool afterReadDone = false;
        bool stopBeforeRecheck = false;
        if (!NativeDimensionDrawingRunsStopped (processControl, started, stopBeforeRecheck)) {
            after = ReadNativeDimensionDrawingParentFacts (guid, processControl);
            ReadRadialDefaultsContext (parentContextAfter);
            afterReadDone = true;
        }
        const GS::ObjectState parentAfterState = afterReadDone
            ? NativeDimensionDrawingParentFactsDetails (after)
            : GS::ObjectState ("status", "not-read", "reason", stopBeforeRecheck ? "cooperative-cancelled-before-final-parent-recheck" : "cooperative-deadline-before-final-parent-recheck");
        const GS::ObjectState contextAfterState = afterReadDone ? RadialDefaultsContextDetails (parentContextAfter) : GS::ObjectState ("status", "not-read");
        const bool parentStable = afterReadDone && SameNativeDimensionDrawingParentFacts (before, after)
            && after.storyIndex == requestedStory
            && SameRadialDefaultsContext (parentContextBefore, parentContextAfter)
            && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, parentContextAfter)
            && parentContextAfter.activeStory == requestedStory;
        GS::UniString metadataStatus;
        GS::UniString metadataReason;
        const bool metadataFieldsAvailable = metadata.Get ("status", metadataStatus) && metadata.Get ("coverageReason", metadataReason);
        const bool metadataCoverageComplete = metadataFieldsAvailable && (metadataStatus == "available"
#if TAPIR_AC_VERSION == 25
            || (metadataStatus == "unavailable" && metadataReason == "autotext-metadata-api-not-available-in-ac25")
#endif
            );
        bool afterShapeBudgetPermitsCompletion = true;
#if TAPIR_AC_VERSION >= 26
        afterShapeBudgetPermitsCompletion = !stoppedAfterShape;
#endif
        const bool rowComplete = shapeCoverageComplete && parentStable && metadataCoverageComplete
            && afterShapeBudgetPermitsCompletion && !metadataStopped;
        if (!rowComplete) allRowsComplete = false;

        GS::ObjectState consistency (
            "status", parentStable ? "same-guid-type-story-database-stamp-and-dimension-fingerprint" : "parent-or-context-changed-or-final-recheck-unavailable",
            "stable", parentStable,
            "sameGuid", before.hasGuid && after.hasGuid && before.guid == after.guid,
            "sameFamily", before.hasNativeType && after.hasNativeType && before.hasVariation && after.hasVariation
                && before.nativeTypeId == after.nativeTypeId && before.variationId == after.variationId,
            "sameStory", before.hasStory && after.hasStory && before.storyIndex == after.storyIndex,
            "sameDatabase", before.hasDatabase && after.hasDatabase && before.databaseGuid == after.databaseGuid,
            "sameModificationStamp", before.hasStamp && after.hasStamp && before.modificationStamp == after.modificationStamp,
            "sameFactualGeometry", before.hasGeometry && after.hasGeometry && before.hasFingerprint && after.hasFingerprint
                && before.fingerprint == after.fingerprint);
        GS::ObjectState row (
            "requestedGuid", guidText,
            "status", rowComplete ? "available" : "partial",
            "parentBefore", before.facts,
            "parentAfter", parentAfterState,
            "contextBefore", parentContextBeforeState,
            "contextAfter", contextAfterState,
            "readConsistency", consistency,
            "drawingRuns", NativeDimensionDrawingRunsDetails (callbackContext, shapeError),
            "autotextMetadata", metadata,
            "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "drawing-run-output-and-keyword-catalog-do-not-identify-original-note-expression"),
            "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "ShapePrimsExt-output-is-not-independent-dimension-display-evidence"),
            "noteCorrespondence", GS::ObjectState ("status", "unproven", "reason", "ShapePrimsExt-provides-no-note-or-point-identity"));
        rows.Push (row);
    }

    bool stoppedBeforeRequestAfter = false;
    RadialDefaultsContext requestAfter;
    GS::ObjectState requestAfterState;
    bool requestAfterRead = false;
    if (!NativeDimensionDrawingRunsStopped (processControl, started, stoppedBeforeRequestAfter)) {
        ReadRadialDefaultsContext (requestAfter);
        requestAfterState = RadialDefaultsContextDetails (requestAfter);
        requestAfterRead = true;
    } else {
        requestAfterState = GS::ObjectState ("status", "not-read", "reason", stoppedBeforeRequestAfter ? "cooperative-cancelled-before-request-final-context" : "cooperative-deadline-before-request-final-context");
        allRowsComplete = false;
    }
    const bool requestContextStable = requestAfterRead
        && SameRadialDefaultsContext (requestBefore, requestAfter)
        && IsExactTaskOwnedRadialDefaultsCopy (runId, expectedProjectPath, fileScopeKey, requestAfter)
        && requestAfter.activeStory == requestedStory;
    if (!requestContextStable) allRowsComplete = false;
    const GS::ObjectState requestConsistency (
        "status", requestContextStable ? "stable-exact-saved-copy-floor-plan-database-and-story" : "context-changed-or-final-context-unavailable",
        "contextStable", requestContextStable);
    return NativeDimensionDrawingRunsResponse (
        allRowsComplete ? "available" : "partial", runId, expectedProjectPath, fileScopeKey, requestedStory,
        requestBeforeState, requestAfterState, requestConsistency, rows,
        allRowsComplete ? errorNotChecked : GS::ObjectState (
            "status", "present",
            "code", "PARENT_DRAWING_COVERAGE_PARTIAL",
            "message", "One or more parent drawing-run, metadata, revision, context or cooperative-coverage checks are incomplete."), normalizedGuids);
}

ModifyColumnsCommand::ModifyColumnsCommand (const bool atomicIn) :
    CommandBase (CommonSchema::Used),
    atomic (atomicIn)
{
}

GS::String ModifyColumnsCommand::GetName () const
{
    return "ModifyColumns";
}

GS::Optional<GS::UniString> ModifyColumnsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "columnsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "origin": { "$ref": "#/Coordinate2D" },
                        "zCoordinate": { "type": "number" },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "bottomOffset": { "type": "number" },
                        "axisRotationAngle": { "type": "number" },
                        "slantAngle": { "type": "number" },
                        "isSlanted": { "type": "boolean" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "depth": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "circleBased": { "type": "boolean" },
                        "isWidthAndHeightLinked": { "type": "boolean" },
                        "buildingMaterialId": { "$ref": "#/AttributeId" },
                        "profileId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["columnsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyColumnsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyColumnsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "columnsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    const Stories stories = GetStories ();
    bool faultAfterFirstApplied = false;
    parameters.Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (faultAfterFirstApplied)
        return CreateErrorResponse (APIERR_BADPARS, "Column first-applied rollback fault seam is unavailable in this APX build.");
#else
    if (faultAfterFirstApplied && (!atomic || items.GetSize () != 2))
        return CreateErrorResponse (APIERR_BADPARS, "Column first-applied rollback fault requires an atomic two-item UPDATE.");
#endif

    GS::ObjectState response = ExecuteModifyWithResults ("Modify Columns", [&](GS::Array<GS::ObjectState>& results) {
        const GSSize itemCount = static_cast<GSSize> (items.GetSize ());
        for (GSSize index = 0; index < itemCount; ++index) {
            const auto& item = items[index];
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load column."));
                continue;
            }
            const short originalStoryIndex = element.header.floorInd;

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            auto sectionError = ValidateColumnSectionPayload (item);
            if (sectionError.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, sectionError.Get ()));
                continue;
            }

            const bool hasSectionFields = HasColumnSectionFields (item);
            err = ValidateColumnSupportedUpdateState (element, item);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Column UPDATE requires fresh supported vertical homogeneous rectangular Basic section facts and supported linkage."));
                continue;
            }

            const bool changed = ApplyColumnDetails (element, mask, item, stories);
            if (!changed && !hasSectionFields) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No column fields to modify."));
                continue;
            }

            if (changed) {
                err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Failed to modify column."));
                    continue;
                }
            }

            if (hasSectionFields) {
                err = ApplyColumnSectionToMemo (element.header.guid, item);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Failed to modify column cross section."));
                    continue;
                }
            }

            if (atomic) {
                API_Elem_Head updatedHeader = {};
                updatedHeader.guid = element.header.guid;
                err = ACAPI_Element_GetHeader (&updatedHeader);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Failed to verify Column story preservation."));
                    continue;
                }
                if (updatedHeader.floorInd != originalStoryIndex) {
                    results.Push (CreateFailedExecutionResult (APIERR_COMMANDFAILED,
                        "Native CRUD Column UPDATE changed story; the undoable batch must roll back."));
                    continue;
                }
            }

            results.Push (CreateSuccessfulExecutionResult ());
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
            if (faultAfterFirstApplied && index == 0) {
                // Inject only after API_Element_Change and any Column memo
                // write for item zero have both completed inside this same
                // undoable callback. The failed result makes the atomic
                // wrapper return an error and Archicad roll back the batch.
                results.Push (CreateFailedExecutionResult (APIERR_COMMANDFAILED, "Disposable Column fault seam requested rollback after the first complete item."));
                break;
            }
#endif
        }
    }, atomic);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    bool rolledBack = false;
    if (faultAfterFirstApplied && response.Get ("transactionRolledBack", rolledBack) && rolledBack) {
        response.Add ("testFaultEvidence", GS::ObjectState (
            "schemaVersion", "native-crud-test-fault-evidence/v1",
            "firstAppliedItemIndex", 0,
            "firstItemChangeSucceeded", true,
            "hostRollbackReported", true));
    }
#endif
    return response;
}

ModifyWindowsCommand::ModifyWindowsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ModifyWindowsCommand::GetName () const
{
    return "ModifyWindows";
}

GS::Optional<GS::UniString> ModifyWindowsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "windowsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "sillHeight": { "type": "number" },
                        "centerOffset": { "type": "number", "minimum": 0.0 },
                        "reflected": { "type": "boolean" },
                        "refSide": { "type": "boolean" },
                        "oSide": { "type": "boolean" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["windowsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyWindowsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyWindowsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "windowsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteModifyWithResults ("Modify Windows", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load window."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            const bool changed = ApplyWindowOrDoorDetails (element, mask, item);
            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No window fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify window."));
        }
    });
}

ModifyDoorsCommand::ModifyDoorsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ModifyDoorsCommand::GetName () const
{
    return "ModifyDoors";
}

GS::Optional<GS::UniString> ModifyDoorsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "doorsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "width": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "height": { "type": "number", "minimum": 0.0, "exclusiveMinimum": true },
                        "sillHeight": { "type": "number" },
                        "centerOffset": { "type": "number", "minimum": 0.0 },
                        "reflected": { "type": "boolean" },
                        "refSide": { "type": "boolean" },
                        "oSide": { "type": "boolean" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["doorsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyDoorsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyDoorsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "doorsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteModifyWithResults ("Modify Doors", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load door."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            const bool changed = ApplyWindowOrDoorDetails (element, mask, item);
            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No door fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify door."));
        }
    });
}

ModifyMorphsCommand::ModifyMorphsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ModifyMorphsCommand::GetName () const
{
    return "ModifyMorphs";
}

GS::Optional<GS::UniString> ModifyMorphsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "morphsWithDetails": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "translation": { "$ref": "#/Coordinate3D" },
                        "rotationDegreesZ": { "type": "number" },
                        "buildingMaterialId": { "$ref": "#/AttributeId" }
                    },
                    "additionalProperties": false,
                    "required": ["elementId"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["morphsWithDetails"]
    })";
}

GS::Optional<GS::UniString> ModifyMorphsCommand::GetResponseSchema () const
{
    return ModifyWallsCommand ().GetResponseSchema ();
}

GS::ObjectState ModifyMorphsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> items;
    auto error = GetElementArray (parameters, "morphsWithDetails", items);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteModifyWithResults ("Modify Morphs", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
            if (item.Get ("elementId") == nullptr) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Missing required 'elementId' field."));
                continue;
            }
            API_Element element = {};
            element.header.guid = GetGuidFromObjectState (*item.Get ("elementId"));
            GSErrCode err = ACAPI_Element_Get (&element);
            if (err != NoError) {
                results.Push (CreateFailedExecutionResult (err, "Failed to load morph."));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;

            auto translation = GetOptionalCoordinate3D (item, "translation");

            if (translation.HasValue ()) {
                element.morph.tranmat.tmx[3] += translation->x;
                element.morph.tranmat.tmx[7] += translation->y;
                element.morph.tranmat.tmx[11] += translation->z;
                ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, tranmat);
                changed = true;
            }

            auto rotationDegrees = GetOptionalDouble (item, "rotationDegreesZ");

            if (rotationDegrees.HasValue ()) {
                const double radians = rotationDegrees.Get () * DegreesToRadians;
                const double cosAngle = std::cos (radians);
                const double sinAngle = std::sin (radians);
                const API_Tranmat originalTransform = element.morph.tranmat;
                for (Int32 column = 0; column < 4; ++column) {
                    element.morph.tranmat.tmx[column] = cosAngle * originalTransform.tmx[column] + sinAngle * originalTransform.tmx[8 + column];
                    element.morph.tranmat.tmx[8 + column] = -sinAngle * originalTransform.tmx[column] + cosAngle * originalTransform.tmx[8 + column];
                }
                ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, tranmat);
                changed = true;
            }

            auto buildingMaterialId = GetOptionalObjectState (item, "buildingMaterialId");

            if (buildingMaterialId.HasValue ()) {
                API_AttributeIndex buildingMaterialIndex = APIInvalidAttributeIndex;
                if (!ResolveAttributeIndex (buildingMaterialId.Get (), API_BuildingMaterialID, buildingMaterialIndex)) {
                    results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "Invalid morph building material."));
                    continue;
                }
                element.morph.buildingMaterial = buildingMaterialIndex;
                ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, buildingMaterial);
                changed = true;
            }

            if (!changed) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, "No morph fields to modify."));
                continue;
            }

            err = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            results.Push (err == NoError ? CreateSuccessfulExecutionResult () : CreateFailedExecutionResult (err, "Failed to modify morph."));
        }
    });
}

CreateSectionsCommand::CreateSectionsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateSectionsCommand::GetName () const
{
    return "CreateSections";
}

GS::Optional<GS::UniString> CreateSectionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "sectionsData": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "startCoordinate": { "$ref": "#/Coordinate2D" },
                        "endCoordinate": { "$ref": "#/Coordinate2D" },
                        "depth": { "type": "number" },
                        "name": { "type": "string" },
                        "floorIndex": { "type": "integer" }
                    },
                    "additionalProperties": false,
                    "required": ["startCoordinate", "endCoordinate"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["sectionsData"]
    })";
}

GS::Optional<GS::UniString> CreateSectionsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "transactionRolledBack": {
                "type": "boolean"
            },
            "transactionError": {
                "type": "integer"
            }
        },
        "additionalProperties": false,
        "required": ["elements"]
    })";
}

GS::ObjectState CreateSectionsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const
{
    GS::Array<GS::ObjectState> sectionsData;
    auto error = GetElementArray (parameters, "sectionsData", sectionsData);
    if (error.HasValue ()) {
        return CreateErrorResponse (APIERR_BADPARS, error.Get ());
    }

    return ExecuteCreateWithElements ("Create Sections", [&](GS::Array<GS::ObjectState>& elements) {
        for (const auto& data : sectionsData) {
            API_Element element = {};
            API_ElementMemo memo = {};
            API_SubElement marker = {};
            const GS::OnExit cleanup ([&]() {
                ACAPI_DisposeElemMemoHdls (&memo);
                ACAPI_DisposeElemMemoHdls (&marker.memo);
            });

#ifdef ServerMainVers_2600
            element.header.type = API_CutPlaneID;
#else
            element.header.typeID = API_CutPlaneID;
#endif
            marker.subType = static_cast<API_SubElementType> (APISubElement_MainMarker);

            GSErrCode err = ACAPI_Element_GetDefaultsExt (&element, &memo, 1UL, &marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to prepare section defaults."));
                continue;
            }

            if (data.Get ("startCoordinate") == nullptr || data.Get ("endCoordinate") == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Missing required 'startCoordinate' or 'endCoordinate' field."));
                continue;
            }
            const API_Coord startCoord = Get2DCoordinateFromObjectState (*data.Get ("startCoordinate"));
            const API_Coord endCoord = Get2DCoordinateFromObjectState (*data.Get ("endCoordinate"));

            short floorIndex = 0;
            if (data.Get ("floorIndex", floorIndex)) {
                element.header.floorInd = floorIndex;
            }

            GS::UniString name;
            if (data.Get ("name", name)) {
                GS::ucsncpy (element.cutPlane.segment.cutPlName, name.ToUStr ().Get (), GS::ArraySize (element.cutPlane.segment.cutPlName));
                element.cutPlane.segment.cutPlName[GS::ArraySize (element.cutPlane.segment.cutPlName) - 1] = 0;
            }

            const double dx = endCoord.x - startCoord.x;
            const double dy = endCoord.y - startCoord.y;
            const double lineLength = sqrt (dx * dx + dy * dy);
            if (lineLength < 1e-6) {
                elements.Push (CreateErrorResponse (APIERR_BADPARS, "Start and end coordinates are too close."));
                continue;
            }

            double depth = 1.0;
            data.Get ("depth", depth);

            const double nx = -dy / lineLength;
            const double ny = dx / lineLength;

            element.cutPlane.segment.nMainCoord = 2;
            element.cutPlane.segment.nDepthCoord = 2;
            element.cutPlane.linkData.sourceMarker = true;
            marker.subType = APISubElement_MainMarker;

            API_Coord* const newMainCoords = reinterpret_cast<API_Coord*> (BMpAllClear (2 * sizeof (API_Coord)));
            if (newMainCoords == nullptr) {
                elements.Push (CreateErrorResponse (APIERR_MEMFULL, "Failed to allocate section main coordinates."));
                continue;
            }
            API_Coord* const newDepthCoords = reinterpret_cast<API_Coord*> (BMpAllClear (2 * sizeof (API_Coord)));
            if (newDepthCoords == nullptr) {
                BMpFree (reinterpret_cast<GSPtr> (newMainCoords));
                elements.Push (CreateErrorResponse (APIERR_MEMFULL, "Failed to allocate section depth coordinates."));
                continue;
            }

            newMainCoords[0] = startCoord;
            newMainCoords[1] = endCoord;
            newDepthCoords[0].x = startCoord.x + nx * depth;
            newDepthCoords[0].y = startCoord.y + ny * depth;
            newDepthCoords[1].x = endCoord.x + nx * depth;
            newDepthCoords[1].y = endCoord.y + ny * depth;

            if (memo.sectionSegmentMainCoords != nullptr) {
                BMpFree (reinterpret_cast<GSPtr> (memo.sectionSegmentMainCoords));
            }
            memo.sectionSegmentMainCoords = newMainCoords;
            if (memo.sectionSegmentDepthCoords != nullptr) {
                BMpFree (reinterpret_cast<GSPtr> (memo.sectionSegmentDepthCoords));
            }
            memo.sectionSegmentDepthCoords = newDepthCoords;

            err = ACAPI_Element_CreateExt (&element, &memo, 1UL, &marker);
            if (err != NoError) {
                elements.Push (CreateErrorResponse (err, "Failed to create section."));
                continue;
            }
            elements.Push (CreateElementIdObjectState (element.header.guid));
        }
    });
}

GS::Optional<GS::UniString> BuildMeshPolyMemoFromGeometry (
    API_Element&                       elem,
    API_ElementMemo&                   memo,
    GS::Array<GS::ObjectState>&        polygonCoordinates,
    const GS::Array<GS::ObjectState>&  polygonArcs,
    const GS::Array<GS::ObjectState>&  holes)
{
    if (polygonCoordinates.GetSize () < 3) {
        return "'polygonCoordinates' must contain at least 3 coordinates.";
    }
    if (IsSame2DCoordinate (polygonCoordinates.GetFirst (), polygonCoordinates.GetLast ())) {
        polygonCoordinates.Pop ();
    }

    const GSSize polygonCoordinateCount = static_cast<GSSize> (polygonCoordinates.GetSize ());
    const GSSize polygonArcCount = static_cast<GSSize> (polygonArcs.GetSize ());
    if (polygonCoordinateCount < 3 || polygonCoordinateCount > static_cast<GSSize> (std::numeric_limits<Int32>::max () - 1) ||
        polygonArcCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()))
        return "Invalid mesh polygon dimensions.";

    GSSize totalCoords = polygonCoordinateCount + 1;
    GSSize totalArcs = polygonArcCount;
    GSSize totalSubPolys = 1;

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holeCoords;
        GS::Array<GS::ObjectState> holeArcs;
        if (GetHoleGeometry (hole, holeCoords, holeArcs) && holeCoords.GetSize () >= 3) {
            const GSSize holeCoordinateCount = static_cast<GSSize> (holeCoords.GetSize ());
            const GSSize holeArcCount = static_cast<GSSize> (holeArcs.GetSize ());
            const GSSize maxInt32 = static_cast<GSSize> (std::numeric_limits<Int32>::max ());
            if (totalCoords > maxInt32 - 1 || totalArcs > maxInt32 ||
                totalSubPolys >= maxInt32 ||
                holeCoordinateCount > maxInt32 - totalCoords - 1 ||
                holeArcCount > maxInt32 - totalArcs)
                return "Mesh polygon dimensions exceed the supported range.";
            totalCoords += holeCoordinateCount + 1;
            totalSubPolys += 1;
            totalArcs += holeArcCount;
        }
    }

    elem.mesh.poly.nCoords   = static_cast<Int32> (totalCoords);
    elem.mesh.poly.nSubPolys = static_cast<Int32> (totalSubPolys);
    elem.mesh.poly.nArcs     = static_cast<Int32> (totalArcs);

    const Int32 nCoords   = elem.mesh.poly.nCoords;
    const Int32 nSubPolys = elem.mesh.poly.nSubPolys;
    const Int32 nArcs     = elem.mesh.poly.nArcs;
    if (nCoords < 4 || nSubPolys < 1 || nArcs < 0)
        return "Invalid mesh polygon memo dimensions.";
    const GSSize coordCount = static_cast<GSSize> (nCoords) + 1;
    const GSSize subPolyCount = static_cast<GSSize> (nSubPolys) + 1;
    if (coordCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord) ||
        coordCount > std::numeric_limits<GSSize>::max () / sizeof (double) ||
        coordCount > std::numeric_limits<GSSize>::max () / sizeof (UInt32) ||
        subPolyCount > std::numeric_limits<GSSize>::max () / sizeof (Int32) ||
        (nArcs > 0 && static_cast<GSSize> (nArcs) > std::numeric_limits<GSSize>::max () / sizeof (API_PolyArc)))
        return "Mesh polygon memo dimensions exceed the supported range.";

    if (memo.coords == nullptr) {
        memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle (coordCount * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.coords == nullptr)
            return "Mesh coordinate memo handle is invalid.";
        GSHandle resizedCoords = BMReallocHandle (reinterpret_cast<GSHandle> (memo.coords), coordCount * sizeof (API_Coord), REALLOC_CLEAR, 0);
        if (resizedCoords == nullptr) return "Failed to resize mesh coordinate memo.";
        memo.coords = reinterpret_cast<API_Coord**> (resizedCoords);
    }
    if (memo.meshPolyZ == nullptr) {
        memo.meshPolyZ = reinterpret_cast<double**> (BMAllocateHandle (coordCount * sizeof (double), ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.meshPolyZ == nullptr)
            return "Mesh elevation memo handle is invalid.";
        GSHandle resizedZ = BMReallocHandle (reinterpret_cast<GSHandle> (memo.meshPolyZ), coordCount * sizeof (double), REALLOC_CLEAR, 0);
        if (resizedZ == nullptr) return "Failed to resize mesh elevation memo.";
        memo.meshPolyZ = reinterpret_cast<double**> (resizedZ);
    }
    if (memo.pends == nullptr) {
        memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (subPolyCount * sizeof (Int32), ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.pends == nullptr)
            return "Mesh polygon-end memo handle is invalid.";
        GSHandle resizedPends = BMReallocHandle (reinterpret_cast<GSHandle> (memo.pends), subPolyCount * sizeof (Int32), REALLOC_CLEAR, 0);
        if (resizedPends == nullptr) return "Failed to resize mesh polygon-end memo.";
        memo.pends = reinterpret_cast<Int32**> (resizedPends);
    }

    if (nArcs > 0) {
        if (memo.parcs == nullptr) {
            memo.parcs = reinterpret_cast<API_PolyArc**> (BMAllocateHandle (static_cast<GSSize> (nArcs) * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
        } else {
            if (*memo.parcs == nullptr)
                return "Mesh arc memo handle is invalid.";
            GSHandle resizedArcs = BMReallocHandle (reinterpret_cast<GSHandle> (memo.parcs), static_cast<GSSize> (nArcs) * sizeof (API_PolyArc), REALLOC_CLEAR, 0);
            if (resizedArcs == nullptr)
                return "Failed to resize mesh arc memo.";
            memo.parcs = reinterpret_cast<API_PolyArc**> (resizedArcs);
        }
        if (memo.parcs == nullptr || *memo.parcs == nullptr)
            return "Failed to allocate mesh arc memo.";
    } else if (memo.parcs != nullptr) {
        BMKillHandle (reinterpret_cast<GSHandle*> (&memo.parcs));
        memo.parcs = nullptr;
    }

    if (memo.vertexIDs != nullptr) {
        if (*memo.vertexIDs == nullptr)
            return "Mesh vertex memo handle is invalid.";
        if (coordCount > std::numeric_limits<GSSize>::max () / sizeof (UInt32))
            return "Mesh vertex memo dimensions exceed the supported range.";
        GSHandle resizedVertexIDs = BMReallocHandle (reinterpret_cast<GSHandle> (memo.vertexIDs), coordCount * sizeof (UInt32), REALLOC_CLEAR, 0);
        if (resizedVertexIDs == nullptr)
            return "Failed to resize mesh vertex memo.";
        memo.vertexIDs = reinterpret_cast<UInt32**> (resizedVertexIDs);
    }
    if (memo.coords == nullptr || *memo.coords == nullptr || memo.meshPolyZ == nullptr || *memo.meshPolyZ == nullptr ||
        memo.pends == nullptr || *memo.pends == nullptr)
        return "Failed to allocate mesh polygon memo data.";

    Int32 iCoord = 1;
    Int32 iArc   = 0;
    Int32 iPends = 1;
    GSErrCode memoErr = AddPolyToMemo (polygonCoordinates, polygonArcs, iCoord, iArc, iPends, memo);
    if (memoErr != NoError)
        return "Failed to populate mesh polygon memo data.";

    for (const GS::ObjectState& hole : holes) {
        GS::Array<GS::ObjectState> holeCoords;
        GS::Array<GS::ObjectState> holeArcs;
        if (GetHoleGeometry (hole, holeCoords, holeArcs)) {
            memoErr = AddPolyToMemo (holeCoords, holeArcs, iCoord, iArc, iPends, memo);
            if (memoErr != NoError)
                return "Failed to populate mesh hole memo data.";
        }
    }

    return {};
}

GSErrCode BuildMeshSublinesMemoFromGeometry (
    API_Element&                      elem,
    API_ElementMemo&                  memo,
    const GS::Array<GS::ObjectState>& sublines)
{
    Int32 nTotalCoords = 0;
    Int32 nSubLines    = 0;
    for (const GS::ObjectState& subline : sublines) {
        GS::Array<GS::ObjectState> coords;
        if (subline.Get ("coordinates", coords) && !coords.IsEmpty ()) {
            const GSSize coordinateCount = static_cast<GSSize> (coords.GetSize ());
            if (coordinateCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()) - static_cast<GSSize> (nTotalCoords))
                return APIERR_BADPARS;
            if (nSubLines == std::numeric_limits<Int32>::max ())
                return APIERR_BADPARS;
            ++nSubLines;
            nTotalCoords += static_cast<Int32> (coordinateCount);
        }
    }

    elem.mesh.levelLines.nCoords   = nTotalCoords;
    elem.mesh.levelLines.nSubLines = nSubLines;

    if (nTotalCoords <= 0 || nSubLines <= 0) {
        if (memo.meshLevelCoords != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.meshLevelCoords));
        if (memo.meshLevelEnds != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&memo.meshLevelEnds));
        memo.meshLevelCoords = nullptr;
        memo.meshLevelEnds = nullptr;
        return NoError;
    }

    const GSSize coordsBytes = static_cast<GSSize> (nTotalCoords) * sizeof (API_MeshLevelCoord);
    const GSSize endsBytes = static_cast<GSSize> (nSubLines) * sizeof (Int32);
    if (memo.meshLevelCoords == nullptr) {
        memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (BMAllocateHandle (coordsBytes, ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.meshLevelCoords == nullptr)
            return APIERR_BADPARS;
        GSHandle resizedCoords = BMReallocHandle (reinterpret_cast<GSHandle> (memo.meshLevelCoords), coordsBytes, REALLOC_CLEAR, 0);
        if (resizedCoords == nullptr) return APIERR_MEMFULL;
        memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (resizedCoords);
    }
    if (memo.meshLevelEnds == nullptr) {
        memo.meshLevelEnds = reinterpret_cast<Int32**> (BMAllocateHandle (endsBytes, ALLOCATE_CLEAR, 0));
    } else {
        if (*memo.meshLevelEnds == nullptr)
            return APIERR_BADPARS;
        GSHandle resizedEnds = BMReallocHandle (reinterpret_cast<GSHandle> (memo.meshLevelEnds), endsBytes, REALLOC_CLEAR, 0);
        if (resizedEnds == nullptr) return APIERR_MEMFULL;
        memo.meshLevelEnds = reinterpret_cast<Int32**> (resizedEnds);
    }
    if (memo.meshLevelCoords == nullptr || *memo.meshLevelCoords == nullptr ||
        memo.meshLevelEnds == nullptr || *memo.meshLevelEnds == nullptr)
        return APIERR_MEMFULL;

    Int32 iCoord = 0;
    Int32 iLine  = 0;
    for (const GS::ObjectState& subline : sublines) {
        GS::Array<GS::ObjectState> coords;
        if (!subline.Get ("coordinates", coords) || coords.IsEmpty ())
            continue;
        for (const GS::ObjectState& c : coords) {
            (*memo.meshLevelCoords)[iCoord].c = Get3DCoordinateFromObjectState (c);
            ++iCoord;
        }
        (*memo.meshLevelEnds)[iLine++] = iCoord;
    }
    return iCoord == nTotalCoords && iLine == nSubLines ? NoError : APIERR_BADPARS;
}

ModifyMeshesCommand::ModifyMeshesCommand () :
    CommandBase (CommonSchema::Used)
{}

GS::String ModifyMeshesCommand::GetName () const
{
    return "ModifyMeshes";
}

GS::Optional<GS::UniString> ModifyMeshesCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "meshesData": {
            "type": "array",
            "description": "Array of meshes to modify.",
            "items": {
                "type": "object",
                "properties": {
                    "elementId": {
                        "$ref": "#/ElementId"
                    },
                    "meshData": {
                        "type": "object",
                        "description": "The fields to modify on the Mesh. Only provided fields are changed; omitted fields are left as-is.",
                        "properties": {
                            "floorIndex": {
                                "type": "integer"
                            },
                            "level": {
                                "type": "number",
                                "description": "The Z reference level of coordinates."
                            },
                            "skirtType": {
                                "$ref": "#/MeshSkirtType"
                            },
                            "skirtLevel": {
                                "type": "number",
                                "description": "The height of the skirt."
                            },
                            "ridges": {
                                "type": "string",
                                "description": "How ridges between mesh facets are displayed in 3D.",
                                "enum": ["AllSharp", "AllSmooth", "UserDefined"]
                            },
                            "showLines": {
                                "type": "boolean",
                                "description": "Whether to show secondary mesh lines on plan."
                            },
                            "contourPen": {
                                "type": "integer",
                                "description": "Pen attribute index for the mesh contour line."
                            },
                            "levelPen": {
                                "type": "integer",
                                "description": "Pen attribute index for the mesh level lines."
                            },
                            "lineTypeIndex": {
                                "type": "integer",
                                "description": "Line type attribute index for the mesh contour."
                            },
                            "polygonCoordinates": {
                                "type": "array",
                                "description": "The 3D coordinates of the outline polygon of the mesh. Replaces the existing boundary entirely.",
                                "items": { "$ref": "#/Coordinate3D" },
                                "minItems": 3
                            },
                            "polygonArcs": {
                                "type": "array",
                                "description": "Polygon outline arcs of the mesh.",
                                "items": { "$ref": "#/PolyArc" }
                            },
                            "holes": {
                                "$ref": "#/Holes3D"
                            },
                            "sublines": {
                                "type": "array",
                                "description": "The leveling sublines inside the polygon of the mesh. Replaces existing sublines entirely.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "coordinates": {
                                            "type": "array",
                                            "description": "The 3D coordinates of the leveling subline.",
                                            "items": { "$ref": "#/Coordinate3D" }
                                        }
                                    },
                                    "additionalProperties": false,
                                    "required": ["coordinates"]
                                }
                            }
                        },
                        "additionalProperties": false
                    }
                },
                "additionalProperties": false,
                "required": [
                    "elementId",
                    "meshData"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "meshesData"
    ]
})";
}

GS::Optional<GS::UniString> ModifyMeshesCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": [
            "executionResults"
        ]
    })";
}

GS::ObjectState ModifyMeshesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> meshesData;
    parameters.Get ("meshesData", meshesData);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("ModifyMeshes", [&] () -> GSErrCode {
        for (const GS::ObjectState& meshEntry : meshesData) {
            const GS::ObjectState* elementId = meshEntry.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            API_Element elem = {};
            elem.header.guid = GetGuidFromObjectState (*elementId);
            GSErrCode err = ACAPI_Element_Get (&elem);
            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (err, "Failed to find the element"));
                continue;
            }

#ifdef ServerMainVers_2600
            if (elem.header.type.typeID != API_MeshID) {
#else
            if (elem.header.typeID != API_MeshID) {
#endif
                executionResults (CreateFailedExecutionResult (APIERR_BADID, "Element is not a Mesh."));
                continue;
            }

            const GS::ObjectState* meshData = meshEntry.Get ("meshData");
            if (meshData == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "meshData is missing"));
                continue;
            }

            GS::Array<GS::ObjectState> polygonCoordinates;
            GS::Array<GS::ObjectState> polygonArcs;
            GS::Array<GS::ObjectState> holes;
            GS::Array<GS::ObjectState> sublines;
            const bool hasPolyGeom        = meshData->Get ("polygonCoordinates", polygonCoordinates);
            const bool hasSublines        = meshData->Get ("sublines", sublines);
            const bool isClearingSublines = hasSublines && sublines.IsEmpty ();
            if (hasPolyGeom) {
                meshData->Get ("polygonArcs", polygonArcs);
                meshData->Get ("holes", holes);
            }

            API_ElementMemo memo = {};
            const GS::OnExit memoGuard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });

            {
                // When clearing sublines without a polygon change, also load the current polygon
                // to compute the bounding box for the out-of-bounds dummy sublines (see below).
                const UInt64 loadMask =
                    ((hasPolyGeom || isClearingSublines) ? (APIMemoMask_Polygon | APIMemoMask_MeshPolyZ) : 0) |
                    ((hasSublines && !isClearingSublines) ? APIMemoMask_MeshLevel : 0);
                if (loadMask != 0) {
                    err = ACAPI_Element_GetMemo (elem.header.guid, &memo, loadMask);
                    if (err != NoError) {
                        executionResults (CreateFailedExecutionResult (err, "Failed to get mesh memo"));
                        continue;
                    }
                }
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);

            if (meshData->Get ("floorIndex", elem.header.floorInd)) {
                ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
            }
            if (meshData->Get ("level", elem.mesh.level)) {
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, level);
            }
            if (meshData->Get ("skirtLevel", elem.mesh.skirtLevel)) {
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, skirtLevel);
            }
            GS::UniString skirtType;
            if (meshData->Get ("skirtType", skirtType)) {
                if (skirtType == "SurfaceOnlyWithoutSkirt") {
                    elem.mesh.skirt = 3;
                } else if (skirtType == "WithSkirt") {
                    elem.mesh.skirt = 2;
                } else if (skirtType == "SolidBodyWithSkirt") {
                    elem.mesh.skirt = 1;
                }
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, skirt);
            }
            GS::UniString ridges;
            if (meshData->Get ("ridges", ridges)) {
                if (ridges == "AllSharp") {
                    elem.mesh.smoothRidges = APIRidge_AllSharp;
                } else if (ridges == "AllSmooth") {
                    elem.mesh.smoothRidges = APIRidge_AllSmooth;
                } else if (ridges == "UserDefined") {
                    elem.mesh.smoothRidges = APIRidge_UserSharp;
                }
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, smoothRidges);
            }
            bool showLines = false;
            if (meshData->Get ("showLines", showLines)) {
                elem.mesh.showLines = showLines ? 1 : 0;
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, showLines);
            }
            short contourPen = 0;
            if (meshData->Get ("contourPen", contourPen) && contourPen > 0) {
                elem.mesh.contPen = contourPen;
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, contPen);
            }
            short levelPen = 0;
            if (meshData->Get ("levelPen", levelPen) && levelPen > 0) {
                elem.mesh.levelPen = levelPen;
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, levelPen);
            }
            Int32 lineTypeIndex = 0;
            if (meshData->Get ("lineTypeIndex", lineTypeIndex) && lineTypeIndex > 0) {
                elem.mesh.ltypeInd = ACAPI_CreateAttributeIndex (lineTypeIndex);
                ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, ltypeInd);
            }

            UInt64 memoChangeMask = 0;

            if (hasPolyGeom) {
                auto geoErr = BuildMeshPolyMemoFromGeometry (elem, memo, polygonCoordinates, polygonArcs, holes);
                if (geoErr.HasValue ()) {
                    executionResults (CreateFailedExecutionResult (APIERR_BADPARS, geoErr.Get ()));
                    continue;
                }
                memoChangeMask |= APIMemoMask_Polygon | APIMemoMask_MeshPolyZ;
            }

            if (hasSublines) {
                if (isClearingSublines) {
                    // Clearing level lines: ACAPI_Element_Change ignores null/empty handles for
                    // APIMemoMask_MeshLevel. Workaround: send two valid sublines placed just
                    // outside the polygon bounding box; ArchiCAD clips them out automatically,
                    // resulting in nSubLines == 0 stored in the element.
                    // memo.coords is guaranteed valid here (loaded above for this case).
                    const Int32 nPolyCoords = elem.mesh.poly.nCoords;
                    if (memo.coords == nullptr || *memo.coords == nullptr || nPolyCoords < 2) {
                        executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "Mesh polygon memo is unavailable for clearing sublines."));
                        continue;
                    }
                    double xMin = (*memo.coords)[1].x;
                    double yMin = (*memo.coords)[1].y;
                    for (Int32 j = 2; j < nPolyCoords; ++j) {
                        if ((*memo.coords)[j].x < xMin) xMin = (*memo.coords)[j].x;
                        if ((*memo.coords)[j].y < yMin) yMin = (*memo.coords)[j].y;
                    }

                    const double kOffset = 1.0;
                    const double kStep   = 0.5;
                    const double ox = xMin - kOffset;
                    const double oy = yMin - kOffset;

                    if (memo.meshLevelCoords == nullptr) {
                        memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (BMAllocateHandle (4 * sizeof (API_MeshLevelCoord), ALLOCATE_CLEAR, 0));
                    } else {
                        if (*memo.meshLevelCoords == nullptr) {
                            executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "Mesh subline coordinate memo handle is invalid."));
                            continue;
                        }
                        GSHandle resizedCoords = BMReallocHandle (reinterpret_cast<GSHandle> (memo.meshLevelCoords), 4 * sizeof (API_MeshLevelCoord), REALLOC_CLEAR, 0);
                        if (resizedCoords == nullptr) {
                            executionResults (CreateFailedExecutionResult (APIERR_MEMFULL, "Failed to resize mesh subline coordinates."));
                            continue;
                        }
                        memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (resizedCoords);
                    }
                    if (memo.meshLevelEnds == nullptr) {
                        memo.meshLevelEnds = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
                    } else {
                        if (*memo.meshLevelEnds == nullptr) {
                            executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "Mesh subline endpoint memo handle is invalid."));
                            continue;
                        }
                        GSHandle resizedEnds = BMReallocHandle (reinterpret_cast<GSHandle> (memo.meshLevelEnds), 2 * sizeof (Int32), REALLOC_CLEAR, 0);
                        if (resizedEnds == nullptr) {
                            executionResults (CreateFailedExecutionResult (APIERR_MEMFULL, "Failed to resize mesh subline endpoints."));
                            continue;
                        }
                        memo.meshLevelEnds = reinterpret_cast<Int32**> (resizedEnds);
                    }
                    if (memo.meshLevelCoords == nullptr || *memo.meshLevelCoords == nullptr ||
                        memo.meshLevelEnds == nullptr || *memo.meshLevelEnds == nullptr) {
                        executionResults (CreateFailedExecutionResult (APIERR_MEMFULL, "Failed to allocate mesh subline memo data."));
                        continue;
                    }

                    (*memo.meshLevelCoords)[0].c.x = ox;          (*memo.meshLevelCoords)[0].c.y = oy;          (*memo.meshLevelCoords)[0].c.z = 0.0;
                    (*memo.meshLevelCoords)[1].c.x = ox + kStep;  (*memo.meshLevelCoords)[1].c.y = oy;          (*memo.meshLevelCoords)[1].c.z = 0.0;
                    (*memo.meshLevelCoords)[2].c.x = ox;          (*memo.meshLevelCoords)[2].c.y = oy + kStep;  (*memo.meshLevelCoords)[2].c.z = 0.0;
                    (*memo.meshLevelCoords)[3].c.x = ox + kStep;  (*memo.meshLevelCoords)[3].c.y = oy + kStep;  (*memo.meshLevelCoords)[3].c.z = 0.0;
                    (*memo.meshLevelEnds)[0] = 2;
                    (*memo.meshLevelEnds)[1] = 4;

                    elem.mesh.levelLines.nCoords   = 4;
                    elem.mesh.levelLines.nSubLines = 2;
                    ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, levelLines);
                    memoChangeMask |= APIMemoMask_MeshLevel;
                } else {
                    const GSErrCode sublineErr = BuildMeshSublinesMemoFromGeometry (elem, memo, sublines);
                    if (sublineErr != NoError) {
                        executionResults (CreateFailedExecutionResult (sublineErr, "Failed to allocate mesh subline memo data."));
                        continue;
                    }
                    ACAPI_ELEMENT_MASK_SET (mask, API_MeshType, levelLines);
                    memoChangeMask |= APIMemoMask_MeshLevel;
                }
            }

            err = ACAPI_Element_Change (&elem, &mask, memoChangeMask != 0 ? &memo : nullptr, memoChangeMask, true);
            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (err, "Failed to modify mesh"));
                continue;
            }

            executionResults (CreateSuccessfulExecutionResult ());
        }

        return NoError;
    });

    return response;
}
