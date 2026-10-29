#include "ElementMutationInternal.hpp"
#include "NativeUndoable.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementGDLParameterCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "NotificationCommands.hpp"

#include <cmath>

namespace {

// This is a compile-time, disposable-test-only seam.  It is deliberately not
// read from semanticMetadata or any caller payload: production APX builds keep
// it disabled, and only an internal test build can request the rollback after
// the first successfully applied item.
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
constexpr bool NativeCrudTestFaultAfterFirstApplied = true;
#else
constexpr bool NativeCrudTestFaultAfterFirstApplied = false;
#endif

GS::Optional<GS::UniString> ResolveNativeWallAttributeIndex (
    const GS::ObjectState& details,
    const char* fieldName,
    const API_AttrTypeID attributeType,
    API_AttributeIndex& attributeIndex)
{
    const GS::ObjectState* attributeID = details.Get (fieldName);
    if (attributeID == nullptr)
        return {};

    API_Attribute attribute = {};
    attribute.header.typeID = attributeType;
    attribute.header.guid = GetGuidFromObjectState (*attributeID);
    if (attribute.header.guid == APINULLGuid || ACAPI_Attribute_Get (&attribute) != NoError)
        return GS::UniString::Printf ("Invalid attribute reference in '%s'.", fieldName);

    attributeIndex = attribute.header.index;
    return {};
}

GS::Optional<GS::UniString> ApplyNativeCrudWallStructure (
    API_Element& element,
    API_Element& mask,
    const GS::ObjectState& details,
    bool& changed)
{
    const bool hasBuildingMaterial = details.Contains ("buildingMaterialId");
    const bool hasComposite = details.Contains ("compositeId");
    const bool hasProfile = details.Contains ("profileId");
    const int referenceCount = static_cast<int> (hasBuildingMaterial)
        + static_cast<int> (hasComposite)
        + static_cast<int> (hasProfile);
    if (referenceCount > 1)
        return "Only one of 'buildingMaterialId', 'compositeId' or 'profileId' may be provided at a time.";

    API_AttributeIndex buildingMaterial = APIInvalidAttributeIndex;
    API_AttributeIndex composite = APIInvalidAttributeIndex;
    API_AttributeIndex profile = APIInvalidAttributeIndex;
    auto error = ResolveNativeWallAttributeIndex (details, "buildingMaterialId", API_BuildingMaterialID, buildingMaterial);
    if (error.HasValue ()) return error;
    error = ResolveNativeWallAttributeIndex (details, "compositeId", API_CompWallID, composite);
    if (error.HasValue ()) return error;
    error = ResolveNativeWallAttributeIndex (details, "profileId", API_ProfileID, profile);
    if (error.HasValue ()) return error;

    GS::UniString structureType;
    const bool hasStructureType = details.Contains ("structureType");
    if (hasStructureType && !details.Get ("structureType", structureType))
        return "'structureType' must be a string enum.";
    if (!hasStructureType) {
        if (hasBuildingMaterial) structureType = "Basic";
        else if (hasComposite) structureType = "Composite";
        else if (hasProfile) structureType = "Profile";
    }
    if (structureType.IsEmpty ())
        return {};
    if (structureType != "Basic" && structureType != "Composite" && structureType != "Profile")
        return "Invalid 'structureType'. Use 'Basic', 'Composite' or 'Profile'.";
    if (structureType == "Composite" && !hasComposite)
        return "'structureType=Composite' requires 'compositeId'.";
    if (structureType == "Profile" && !hasProfile)
        return "'structureType=Profile' requires 'profileId'.";
    if (structureType == "Basic" && (hasComposite || hasProfile))
        return "'structureType=Basic' cannot be combined with 'compositeId' or 'profileId'.";
    if (structureType == "Composite" && (hasBuildingMaterial || hasProfile))
        return "'structureType=Composite' cannot be combined with 'buildingMaterialId' or 'profileId'.";
    if (structureType == "Profile" && (hasBuildingMaterial || hasComposite))
        return "'structureType=Profile' cannot be combined with 'buildingMaterialId' or 'compositeId'.";

    if (structureType == "Basic") {
        element.wall.modelElemStructureType = API_BasicStructure;
        if (hasBuildingMaterial) element.wall.buildingMaterial = buildingMaterial;
        element.wall.composite = APIInvalidAttributeIndex;
        element.wall.profileAttr = APIInvalidAttributeIndex;
        if (element.wall.type == APIWtyp_Poly) element.wall.type = APIWtyp_Normal;
    } else if (structureType == "Composite") {
        element.wall.modelElemStructureType = API_CompositeStructure;
        element.wall.composite = composite;
        element.wall.profileAttr = APIInvalidAttributeIndex;
        if (element.wall.type == APIWtyp_Poly) element.wall.type = APIWtyp_Normal;
    } else {
        element.wall.modelElemStructureType = API_ProfileStructure;
        element.wall.profileAttr = profile;
    }

    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, modelElemStructureType);
    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, buildingMaterial);
    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, composite);
    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, profileAttr);
    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, type);
    changed = true;
    return {};
}

GS::Optional<GS::UniString> ApplyNativeCrudWallPlacement (
    API_Element& element,
    API_Element& mask,
    const GS::ObjectState& details,
    const Stories& stories,
    bool& changed)
{
    // `elevation` is the bottom offset above the Wall's own story (D-306).
    (void) stories;
    const bool hasElevation = details.Contains ("elevation");
    if (!hasElevation) return {};
    double targetBottomOffset = 0.0;
    if (!details.Get ("elevation", targetBottomOffset) || !std::isfinite (targetBottomOffset))
        return "'elevation' must be a finite real number.";

    element.wall.bottomOffset = targetBottomOffset;
    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, bottomOffset);
    changed = true;
    return {};
}

bool ApplyNativeCrudWallDetails (
    API_Element& element,
    API_Element& mask,
    const GS::ObjectState& details,
    bool& changed)
{
    const GS::ObjectState* begCoordinate = details.Get ("begCoordinate");
    if (begCoordinate != nullptr) {
        element.wall.begC = Get2DCoordinateFromObjectState (*begCoordinate);
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, begC);
        changed = true;
    }
    const GS::ObjectState* endCoordinate = details.Get ("endCoordinate");
    if (endCoordinate != nullptr) {
        element.wall.endC = Get2DCoordinateFromObjectState (*endCoordinate);
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, endC);
        changed = true;
    }
    double arcAngle = 0.0;
    if (details.Get ("arcAngle", arcAngle)) {
        element.wall.angle = arcAngle;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, angle);
        changed = true;
    }
    double height = 0.0;
    if (details.Get ("height", height)) {
        element.wall.height = height;
        element.wall.relativeTopStory = 0;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, height);
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, relativeTopStory);
        changed = true;
    }
    double offset = 0.0;
    if (details.Get ("offset", offset)) {
        element.wall.offset = offset;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, offset);
        changed = true;
    }
    double thickness = 0.0;
    if (details.Get ("thickness", thickness)) {
        element.wall.thickness = thickness;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness);
        changed = true;
    }
    GS::UniString referenceLineLocation;
    if (details.Get ("referenceLineLocation", referenceLineLocation)) {
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
        } else {
            return false;
        }
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, referenceLineLocation);
        changed = true;
    }
    bool flipped = false;
    if (details.Get ("flipped", flipped)) {
        element.wall.flipped = flipped;
        ACAPI_ELEMENT_MASK_SET (mask, API_WallType, flipped);
        changed = true;
    }
    return true;
}

} // namespace

namespace ElementMutationInternal {

GS::ObjectState ExecuteNativeCrudWallUpdate (const GS::Array<GS::ObjectState>& items)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    const Stories stories = GetStories ();
    GSErrCode callbackError = NoError;
    bool testFaultInjectedAfterFirstApplied = false;

    const GSErrCode undoError = NativeUndoableCommand ("Update Native Walls", [&]() -> GSErrCode {
        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState& item = items[index];
            if (callbackError != NoError) {
                results (CreateFailedExecutionResult (callbackError, "Wall item was not attempted because the batch was rolled back."));
                continue;
            }

            const GS::ObjectState* elementID = item.Get ("elementId");
            const API_Guid guid = elementID == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementID);
            API_Element element = {};
            element.header.guid = guid;
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (error == NoError && GetElemTypeId (element.header) != API_WallID)
                error = APIERR_BADID;
            const short originalStoryIndex = element.header.floorInd;

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            bool storyChanged = false;
            if (error == NoError) {
                const auto placementError = ApplyNativeCrudWallPlacement (element, mask, item, stories, changed);
                if (placementError.HasValue ()) {
                    error = APIERR_BADPARS;
                    results (CreateFailedExecutionResult (error, placementError.Get ()));
                    callbackError = error;
                    continue;
                }
                if (!ApplyNativeCrudWallDetails (element, mask, item, changed)) {
                    error = APIERR_BADPARS;
                    results (CreateFailedExecutionResult (error, "Invalid Wall reference-line location."));
                    callbackError = error;
                    continue;
                }
                const auto structureError = ApplyNativeCrudWallStructure (element, mask, item, changed);
                if (structureError.HasValue ()) {
                    error = APIERR_BADPARS;
                    results (CreateFailedExecutionResult (error, structureError.Get ()));
                    callbackError = error;
                    continue;
                }
                if (IsSame2DCoordinate (element.wall.begC, element.wall.endC)) {
                    error = APIERR_BADPARS;
                    results (CreateFailedExecutionResult (error, "Wall begin and end coordinates must not be identical."));
                    callbackError = error;
                    continue;
                }
                if (!changed) {
                    error = APIERR_BADPARS;
                    results (CreateFailedExecutionResult (error, "No Wall fields to modify."));
                    callbackError = error;
                    continue;
                }
                error = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                if (error == NoError) {
                    API_Elem_Head updatedHeader = {};
                    updatedHeader.guid = guid;
                    error = ACAPI_Element_GetHeader (&updatedHeader);
                    if (error == NoError && updatedHeader.floorInd != originalStoryIndex) {
                        storyChanged = true;
                        error = APIERR_COMMANDFAILED;
                    }
                }
            }
            if (error != NoError) {
                results (CreateFailedExecutionResult (error, storyChanged
                    ? "Native CRUD Wall UPDATE changed story; the undoable batch must roll back."
                    : "Failed to update Wall."));
                callbackError = error;
                continue;
            }
            results (CreateSuccessfulExecutionResult ());
            if (index == 0 && NativeCrudTestFaultAfterFirstApplied) {
                testFaultInjectedAfterFirstApplied = true;
                callbackError = APIERR_COMMANDFAILED;
                results (CreateFailedExecutionResult (callbackError, "Disposable native CRUD test fault seam requested rollback after the first applied Wall item."));
                return callbackError;
            }
        }
        return callbackError == NoError ? NoError : APIERR_COMMANDFAILED;
    });

    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (testFaultInjectedAfterFirstApplied) {
            GS::ObjectState evidence (
                "schemaVersion", "native-crud-test-fault-evidence/v1",
                "firstAppliedItemIndex", 0,
                "firstItemChangeSucceeded", true,
                "hostRollbackReported", true);
            response.Add ("testFaultEvidence", evidence);
        }
#endif
    }
    return response;
}

} // namespace ElementMutationInternal
