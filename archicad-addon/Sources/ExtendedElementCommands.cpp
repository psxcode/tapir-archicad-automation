#include "ExtendedElementCommands.hpp"

#include "MigrationHelper.hpp"
#include "NotificationCommands.hpp"
#include "NativeOwnership.hpp"

#ifdef ServerMainVers_2900
#include	"ACAPI/Element/Opening/OpeningDefault.hpp"
#include	"ACAPI/Element/Opening/Opening.hpp"
#include "Polygon2D.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;

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

static GSErrCode ValidateColumnSectionMemo (API_Guid elementGuid)
{
    API_ElementMemo memo = {};
    const GS::OnExit cleanup ([&memo] () {
        ACAPI_DisposeElemMemoHdls (&memo);
    });

    const GSErrCode err = ACAPI_Element_GetMemo (elementGuid, &memo, APIMemoMask_ColumnSegment);
    if (err != NoError) {
        return err;
    }
    if (memo.columnSegments == nullptr ||
        BMGetPtrSize (reinterpret_cast<GSPtr> (memo.columnSegments)) < sizeof (API_ColumnSegmentType)) {
        return APIERR_BADPARS;
    }
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
        if (hasWidth) {
            segment.nominalWidth = width;
        }
        if (hasDepth) {
            segment.nominalHeight = depth;
        }
        if (hasCircleBased) {
            segment.circleBased = circleBased;
        }
        if (hasIsWidthAndHeightLinked) {
            segment.isWidthAndHeightLinked = isWidthAndHeightLinked;
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
        if (hasWidth) {
            segment.nominalWidth = width;
        }
        if (hasHeight) {
            segment.nominalHeight = height;
        }
        if (hasIsWidthAndHeightLinked) {
            segment.isWidthAndHeightLinked = isWidthAndHeightLinked;
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

    API_NotifyElementType notification = {};
    notification.notifID = APINotifyElement_BeginEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    ACAPI_CallUndoableCommand (commandName, [&]() -> GSErrCode {
        createFunc (results);
        return NoError;
    });

    notification = {};
    notification.notifID = APINotifyElement_EndEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    return CreateElementListResponse (results);
}

template<typename Func>
GS::ObjectState ExecuteModifyWithResults (const GS::String& commandName, Func&& modifyFunc)
{
    GS::Array<GS::ObjectState> results;

    ACAPI_CallUndoableCommand (commandName, [&]() -> GSErrCode {
        modifyFunc (results);
        return NoError;
    });

    return CreateExecutionResultResponse (results);
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

ModifyWallsCommand::ModifyWallsCommand () :
    CommandBase (CommonSchema::Used)
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
    return R"({"type":"object","properties":{"executionResults":{"$ref":"#/ExecutionResults"}},"additionalProperties":false,"required":["executionResults"]})";
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
    });
}

ModifyBeamsCommand::ModifyBeamsCommand () :
    CommandBase (CommonSchema::Used)
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
    });
}

ModifySlabsCommand::ModifySlabsCommand () :
    CommandBase (CommonSchema::Used)
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
    });
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

ModifyColumnsCommand::ModifyColumnsCommand () :
    CommandBase (CommonSchema::Used)
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

    return ExecuteModifyWithResults ("Modify Columns", [&](GS::Array<GS::ObjectState>& results) {
        for (const auto& item : items) {
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

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            auto sectionError = ValidateColumnSectionPayload (item);
            if (sectionError.HasValue ()) {
                results.Push (CreateFailedExecutionResult (APIERR_BADPARS, sectionError.Get ()));
                continue;
            }

            const bool hasSectionFields = HasColumnSectionFields (item);
            if (hasSectionFields) {
                err = ValidateColumnSectionMemo (element.header.guid);
                if (err != NoError) {
                    results.Push (CreateFailedExecutionResult (err, "Column cross section memo is not available for update."));
                    continue;
                }
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
                results.Push (err == NoError
                    ? CreateSuccessfulExecutionResult ()
                    : CreateFailedExecutionResult (err, "Failed to modify column cross section."));
                continue;
            }

            results.Push (CreateSuccessfulExecutionResult ());
        }
    });
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
