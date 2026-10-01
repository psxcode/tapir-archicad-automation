#include "ElementMutationCommands.hpp"

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
#include <vector>

namespace {

constexpr GSSize MaxMutationItems = 256;

// This is a compile-time, disposable-test-only seam.  It is deliberately not
// read from semanticMetadata or any caller payload: production APX builds keep
// it disabled, and only an internal test build can request the rollback after
// the first successfully applied item.
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
constexpr bool NativeCrudTestFaultAfterFirstApplied = true;
#else
constexpr bool NativeCrudTestFaultAfterFirstApplied = false;
#endif

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

bool SameDoorMarkerScalarState (const API_Element& left, const API_Element& right)
{
    return GetElemTypeId (left.header) == API_ObjectID && GetElemTypeId (right.header) == API_ObjectID
        // These are appearance/library/GDL scalars. The marker's story, owner,
        // GUID and position are identity/placement facts checked separately:
        // CreateExt is expected to remap them to the newly created Door.
        && left.header.layer == right.header.layer
        && left.object.pen == right.object.pen && left.object.ltypeInd == right.object.ltypeInd
        && left.object.mat == right.object.mat && left.object.sectFill == right.object.sectFill
        && left.object.sectFillPen == right.object.sectFillPen && left.object.sectBGPen == right.object.sectBGPen
        && left.object.sectContPen == right.object.sectContPen && left.object.useObjPens == right.object.useObjPens
        && left.object.useObjLtypes == right.object.useObjLtypes && left.object.useObjMaterials == right.object.useObjMaterials
        && left.object.useObjSectAttrs == right.object.useObjSectAttrs && left.object.reflected == right.object.reflected
        && left.object.useXYFixSize == right.object.useXYFixSize && left.object.fixPoint == right.object.fixPoint
        && left.object.angle == right.object.angle && left.object.level == right.object.level
        && left.object.xRatio == right.object.xRatio && left.object.yRatio == right.object.yRatio
        && left.object.offset.x == right.object.offset.x && left.object.offset.y == right.object.offset.y
        && left.object.fixedAngle == right.object.fixedAngle
        && left.object.libInd == right.object.libInd
        && left.object.lightIsOn == right.object.lightIsOn && left.object.isAutoOnStoryVisibility == right.object.isAutoOnStoryVisibility
        && left.object.visibility.showOnHome == right.object.visibility.showOnHome
        && left.object.visibility.showAllAbove == right.object.visibility.showAllAbove
        && left.object.visibility.showAllBelow == right.object.visibility.showAllBelow
        && left.object.visibility.showRelAbove == right.object.visibility.showRelAbove
        && left.object.visibility.showRelBelow == right.object.visibility.showRelBelow
        && left.object.linkToSettings.homeStoryDifference == right.object.linkToSettings.homeStoryDifference
        && left.object.linkToSettings.newCreationMode == right.object.linkToSettings.newCreationMode
        && left.object.lightColor.f_red == right.object.lightColor.f_red
        && left.object.lightColor.f_green == right.object.lightColor.f_green
        && left.object.lightColor.f_blue == right.object.lightColor.f_blue
        && left.object.lookId == right.object.lookId;
}

bool SameDoorMarkerOwner (const API_Element& left, const API_Element& right)
{
    if (left.object.owner != right.object.owner) return false;
#ifdef ServerMainVers_2600
    return left.object.ownerType == right.object.ownerType;
#else
    return left.object.ownerID == right.object.ownerID;
#endif
}

bool DoorMarkerOwnedByDoor (const API_Element& marker, const API_Guid& doorGuid)
{
#ifdef ServerMainVers_2600
    return marker.object.owner == doorGuid && marker.object.ownerType == API_DoorID;
#else
    return marker.object.owner == doorGuid && marker.object.ownerID == API_DoorID;
#endif
}

bool SameDoorMarkerPosition (const API_Element& left, const API_Element& right)
{
    return left.object.pos.x == right.object.pos.x && left.object.pos.y == right.object.pos.y;
}

bool SameDoorSourceScalarState (const API_Element& left, const API_Element& right)
{
    return GetElemTypeId (left.header) == API_DoorID && GetElemTypeId (right.header) == API_DoorID
        && left.header.guid == right.header.guid && left.header.floorInd == right.header.floorInd
        && left.header.layer == right.header.layer && left.door.owner == right.door.owner
        && left.door.objLoc == right.door.objLoc && left.door.lower == right.door.lower
        && left.door.openingBase.width == right.door.openingBase.width
        && left.door.openingBase.height == right.door.openingBase.height
        && left.door.openingBase.libInd == right.door.openingBase.libInd
        && left.door.openingBase.markGuid == right.door.openingBase.markGuid
        && left.door.openingBase.reflected == right.door.openingBase.reflected
        && left.door.openingBase.refSide == right.door.openingBase.refSide
        && left.door.openingBase.oSide == right.door.openingBase.oSide
        && left.door.reveal == right.door.reveal && left.door.revealDepthLocation == right.door.revealDepthLocation
        && left.door.revealDepthOffset == right.door.revealDepthOffset
        && left.door.revealDepthFromSide == right.door.revealDepthFromSide
        && left.door.jambDepthHead == right.door.jambDepthHead && left.door.jambDepthSill == right.door.jambDepthSill
        && left.door.jambDepth == right.door.jambDepth && left.door.jambDepth2 == right.door.jambDepth2;
}

bool SameWindowSourceScalarState (const API_Element& left, const API_Element& right)
{
    return GetElemTypeId (left.header) == API_WindowID && GetElemTypeId (right.header) == API_WindowID
        && left.header.guid == right.header.guid && left.header.floorInd == right.header.floorInd
        && left.header.layer == right.header.layer && left.window.owner == right.window.owner
        && left.window.objLoc == right.window.objLoc && left.window.lower == right.window.lower
        && left.window.openingBase.width == right.window.openingBase.width
        && left.window.openingBase.height == right.window.openingBase.height
        && left.window.openingBase.libInd == right.window.openingBase.libInd
        && left.window.openingBase.markGuid == right.window.openingBase.markGuid
        && left.window.openingBase.verticalLink.linkType == right.window.openingBase.verticalLink.linkType
        && left.window.openingBase.verticalLink.linkValue == right.window.openingBase.verticalLink.linkValue
        && left.window.fixPoint == right.window.fixPoint && left.window.directionType == right.window.directionType
        && left.window.openingBase.reflected == right.window.openingBase.reflected
        && left.window.openingBase.refSide == right.window.openingBase.refSide
        && left.window.openingBase.oSide == right.window.openingBase.oSide
        && left.window.reveal == right.window.reveal && left.window.revealDepthLocation == right.window.revealDepthLocation
        && left.window.revealDepthOffset == right.window.revealDepthOffset
        && left.window.revealDepthFromSide == right.window.revealDepthFromSide
        && left.window.jambDepthHead == right.window.jambDepthHead && left.window.jambDepthSill == right.window.jambDepthSill
        && left.window.jambDepth == right.window.jambDepth && left.window.jambDepth2 == right.window.jambDepth2;
}

bool SameWindowLibraryPartIdentity (const API_LibPart& left, const API_LibPart& right)
{
    return left.index == right.index && left.version == right.version
        && GS::UniString (left.ownUnID) == GS::UniString (right.ownUnID)
        && GS::UniString (left.file_UName) == GS::UniString (right.file_UName);
}

bool WindowNativeTopologySupported (const API_Element& element)
{
    return GetElemTypeId (element.header) == API_WindowID
        && element.window.openingBase.libInd > 0
        && element.window.fixPoint == APIHoleAnchor_CenterFix
        && element.window.openingBase.verticalLink.linkType == API_LinkSillToWallBottom
        && element.window.directionType == API_WDAssociativeToWall;
}

constexpr double CompensatedDoorCenterOffsetForWidthChange (const double centerOffset, const double beforeWidth, const double requestedWidth)
{
    // Empirical hypothesis from E-486: one AC25 width increase (+0.05)
    // shifted objLoc by -0.025 even when the previous center was included in
    // the Change mask. The half-delta compensation still needs fresh live
    // verification on AC25 and AC28, including other opening orientations.
    return centerOffset + (requestedWidth - beforeWidth) * 0.5;
}

constexpr double DoorCenterCompensationTestTolerance = 1.0e-12;
constexpr double DoorCenterCompensationAbsolute (const double value) { return value < 0.0 ? -value : value; }
static_assert (DoorCenterCompensationAbsolute (
    CompensatedDoorCenterOffsetForWidthChange (1.0, 0.9, 0.95) - 1.025) < DoorCenterCompensationTestTolerance,
    "Door width increase must compensate the observed negative half-width center shift.");
static_assert (DoorCenterCompensationAbsolute (
    CompensatedDoorCenterOffsetForWidthChange (1.0, 0.95, 0.9) - 0.975) < DoorCenterCompensationTestTolerance,
    "Door width decrease must apply the inverse half-width compensation.");
static_assert (DoorCenterCompensationAbsolute (
    CompensatedDoorCenterOffsetForWidthChange (2.0, 0.9, 0.95) - 2.025) < DoorCenterCompensationTestTolerance,
    "Door width compensation must start from an explicitly requested center when provided.");

GS::ObjectState ExecuteDoorCreate (const GS::Array<GS::ObjectState>& items, GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    GSErrCode callbackError = NoError;
    GS::UniString failureStage = "not-started";
    if (items.GetSize () != 1) return CreateErrorResponse (APIERR_BADPARS, "Door typed CREATE accepts one exact template case per request.");

    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Create Native Door from Exact Template", [&] () -> GSErrCode {
        const GS::ObjectState& item = items[0];
        const GS::ObjectState* sourceId = item.Get ("cloneSourceElementId");
        const GS::ObjectState* payload = item.Get ("payload");
        if (sourceId == nullptr || payload == nullptr) return APIERR_BADPARS;
        const API_Guid sourceGuid = GetGuidFromObjectState (*sourceId);
        const API_Guid hostGuid = payload->Get ("ownerWallId") == nullptr ? APINULLGuid : GetGuidFromObjectState (*payload->Get ("ownerWallId"));
        Int32 storyIndex = std::numeric_limits<Int32>::min ();
        if (!payload->Get ("floorIndex", storyIndex) || storyIndex < std::numeric_limits<short>::min () || storyIndex > std::numeric_limits<short>::max ())
            return APIERR_BADPARS;

        API_Element source = {};
        source.header.guid = sourceGuid;
        failureStage = "read-exact-door-template";
        GSErrCode error = sourceGuid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&source);
        if (error == NoError && (GetElemTypeId (source.header) != API_DoorID || source.header.guid != sourceGuid)) error = APIERR_BADID;
        const API_Guid sourceMarkerGuid = error == NoError ? source.door.openingBase.markGuid : APINULLGuid;
        const bool sourceMarkerless = sourceMarkerGuid == APINULLGuid;
        if (error == NoError && !sourceMarkerless) error = APIERR_BADPARS;

        API_Element host = {};
        if (error == NoError) {
            host.header.guid = hostGuid;
            failureStage = "read-exact-host-wall";
            error = hostGuid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&host);
            if (error == NoError && (GetElemTypeId (host.header) != API_WallID || host.header.guid != hostGuid)) error = APIERR_BADID;
            if (error == NoError && host.header.floorInd != static_cast<short> (storyIndex)) error = APIERR_BADPARS;
        }

        API_ElementMemo sourceMemo = {};
        NativeMemoGuard sourceMemoGuard (sourceMemo);
        if (error == NoError) {
            failureStage = "read-template-door-memo";
            error = ACAPI_Element_GetMemo (sourceGuid, &sourceMemo, APIMemoMask_All);
        }
        if (error != NoError) {
            callbackError = error;
            elements (CreateFailedExecutionResult (error, "Exact markerless Door template, host story, or complete template memo was unavailable."));
            return error;
        }

        API_Element element = source;
        element.header.guid = APINULLGuid;
        element.header.floorInd = static_cast<short> (storyIndex);
        element.door.openingBase.markGuid = APINULLGuid;
        element.door.owner = hostGuid;
        double value = 0.0;
        if (payload->Get ("centerOffset", value)) element.door.objLoc = value;
        if (payload->Get ("sillHeight", value)) element.door.lower = value;
        if (payload->Get ("width", value)) element.door.openingBase.width = value;
        if (payload->Get ("height", value)) element.door.openingBase.height = value;
        bool flag = false;
        if (payload->Get ("reflected", flag)) element.door.openingBase.reflected = flag;
        if (payload->Get ("refSide", flag)) element.door.openingBase.refSide = flag;
        if (payload->Get ("oSide", flag)) element.door.openingBase.oSide = flag;
        failureStage = "createext-door-without-subelements";
        error = ACAPI_Element_CreateExt (&element, &sourceMemo, 0UL, nullptr);
        if (error == NoError) {
            API_Element created = {};
            created.header.guid = element.header.guid;
            failureStage = "read-created-door";
            error = ACAPI_Element_Get (&created);
            if (error == NoError && (created.header.guid == APINULLGuid || created.header.guid == sourceGuid
                || created.header.floorInd != static_cast<short> (storyIndex) || created.door.owner != hostGuid
                || created.door.openingBase.width != element.door.openingBase.width || created.door.openingBase.height != element.door.openingBase.height
                || created.door.objLoc != element.door.objLoc || created.door.lower != element.door.lower
                || created.door.openingBase.reflected != element.door.openingBase.reflected
                || created.door.openingBase.refSide != element.door.openingBase.refSide || created.door.openingBase.oSide != element.door.openingBase.oSide
                || created.door.openingBase.libInd != source.door.openingBase.libInd
                || created.door.openingBase.markGuid != APINULLGuid)) error = APIERR_COMMANDFAILED;
            API_Element sourceAfter = {};
            if (error == NoError) {
                sourceAfter.header.guid = sourceGuid;
                failureStage = "verify-template-preserved";
                error = ACAPI_Element_Get (&sourceAfter);
                if (error == NoError && (!SameDoorSourceScalarState (source, sourceAfter)
                    || sourceAfter.door.openingBase.markGuid != APINULLGuid)) error = APIERR_COMMANDFAILED;
            }
            if (error == NoError) elements (CreateElementIdObjectState (created.header.guid));
        }
        if (error != NoError) {
            callbackError = error;
            GS::ObjectState failed = CreateFailedExecutionResult (error, "Door typed CREATE failed exact markerless-template, host, or factual preservation postconditions.");
            failed.Add ("failureStage", failureStage);
            elements (failed);
            return error;
        }
        return NoError;
    });
    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
        response.Add ("failureStage", failureStage);
    }
    return response;
}

GS::ObjectState ExecuteDoorUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied, GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    GSErrCode callbackError = NoError;
    GS::UniString failureStage = "not-started";
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    bool firstItemFaultInjectedAfterSuccessfulChange = false;
#else
    (void) faultAfterFirstApplied;
#endif
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Update Native Doors", [&] () -> GSErrCode {
        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState& item = items[index];
            if (callbackError != NoError) {
                results (CreateFailedExecutionResult (callbackError, "Door item was not attempted because the batch is rolling back."));
                continue;
            }
            const GS::ObjectState* id = item.Get ("elementId");
            const API_Guid guid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
            API_Element element = {};
            element.header.guid = guid;
            failureStage = "read-door";
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (error == NoError && (GetElemTypeId (element.header) != API_DoorID || element.header.guid != guid)) error = APIERR_BADID;
            const API_Element before = element;
            // Marker-bearing and unresolved targets remain outside this packet.
            if (error == NoError && before.door.openingBase.markGuid != APINULLGuid) error = APIERR_BADPARS;
            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            double number = 0.0;
            if (error == NoError && item.Get ("centerOffset", number)) { element.door.objLoc = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, objLoc); changed = true; }
            if (error == NoError && item.Get ("sillHeight", number)) { element.door.lower = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, lower); changed = true; }
            if (error == NoError && item.Get ("width", number)) {
                double requestedCenterOffset = before.door.objLoc;
                item.Get ("centerOffset", requestedCenterOffset);
                element.door.objLoc = CompensatedDoorCenterOffsetForWidthChange (
                    requestedCenterOffset, before.door.openingBase.width, number);
                ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, objLoc);
                element.door.openingBase.width = number;
                ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width);
                changed = true;
            }
            if (error == NoError && item.Get ("height", number)) { element.door.openingBase.height = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height); changed = true; }
            bool flag = false;
            if (error == NoError && item.Get ("reflected", flag)) { element.door.openingBase.reflected = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.reflected); changed = true; }
            if (error == NoError && item.Get ("refSide", flag)) { element.door.openingBase.refSide = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.refSide); changed = true; }
            if (error == NoError && item.Get ("oSide", flag)) { element.door.openingBase.oSide = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.oSide); changed = true; }
            if (error == NoError && !changed) error = APIERR_BADPARS;
            if (error == NoError) {
                // This writer admits only markerless Doors. Use the established
                // scalar element-change route; ChangeExt is needed for linked
                // marker sub-elements, which are outside this packet.
                failureStage = "change-door";
                error = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
            }
            if (error == NoError) {
                API_Element after = {};
                after.header.guid = guid;
                failureStage = "read-door-postcondition";
                error = ACAPI_Element_Get (&after);
                GS::UniString failureMessage = "Door postcondition read failed.";
                const bool postconditionReadSucceeded = error == NoError;
                if (error == NoError) {
                    failureStage = "verify-door-preservation";
                    failureMessage = "Door UPDATE changed protected identity, host, library, marker, reveal, or jamb state.";
                }
                if (error == NoError && (after.header.guid != before.header.guid || after.header.floorInd != before.header.floorInd
                    || after.header.layer != before.header.layer || after.door.owner != before.door.owner
                    || after.door.openingBase.libInd != before.door.openingBase.libInd
                    || after.door.openingBase.markGuid != before.door.openingBase.markGuid
                    || after.door.reveal != before.door.reveal || after.door.revealDepthLocation != before.door.revealDepthLocation
                    || after.door.revealDepthOffset != before.door.revealDepthOffset || after.door.revealDepthFromSide != before.door.revealDepthFromSide
                    || after.door.jambDepthHead != before.door.jambDepthHead || after.door.jambDepthSill != before.door.jambDepthSill
                    || after.door.jambDepth != before.door.jambDepth || after.door.jambDepth2 != before.door.jambDepth2)) error = APIERR_COMMANDFAILED;
                const auto expectedDouble = [&] (const char* field, const double oldValue, const double actual) {
                    double requested = oldValue;
                    item.Get (field, requested);
                    return requested == actual;
                };
                const auto expectedBool = [&] (const char* field, const bool oldValue, const bool actual) {
                    bool requested = oldValue;
                    item.Get (field, requested);
                    return requested == actual;
                };
                if (error == NoError) {
                    failureStage = "verify-door-requested-values";
                    failureMessage = "Door UPDATE did not read back its requested writable values.";
                    if (!expectedDouble ("centerOffset", before.door.objLoc, after.door.objLoc)) {
                        failureStage = "verify-door-requested-centerOffset";
                        failureMessage = "Door centerOffset does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedDouble ("sillHeight", before.door.lower, after.door.lower)) {
                        failureStage = "verify-door-requested-sillHeight";
                        failureMessage = "Door sillHeight does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedDouble ("width", before.door.openingBase.width, after.door.openingBase.width)) {
                        failureStage = "verify-door-requested-width";
                        failureMessage = "Door width does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedDouble ("height", before.door.openingBase.height, after.door.openingBase.height)) {
                        failureStage = "verify-door-requested-height";
                        failureMessage = "Door height does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedBool ("reflected", before.door.openingBase.reflected, after.door.openingBase.reflected)) {
                        failureStage = "verify-door-requested-reflected";
                        failureMessage = "Door reflected does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedBool ("refSide", before.door.openingBase.refSide, after.door.openingBase.refSide)) {
                        failureStage = "verify-door-requested-refSide";
                        failureMessage = "Door refSide does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    } else if (!expectedBool ("oSide", before.door.openingBase.oSide, after.door.openingBase.oSide)) {
                        failureStage = "verify-door-requested-oSide";
                        failureMessage = "Door oSide does not match the requested value.";
                        error = APIERR_COMMANDFAILED;
                    }
                }
                if (error != NoError) {
                    callbackError = error;
                    GS::ObjectState failed = CreateFailedExecutionResult (error, failureMessage);
                    failed.Add ("failureStage", failureStage);
                    if (postconditionReadSucceeded
                        && (failureStage == "verify-door-requested-centerOffset" || failureStage == "verify-door-requested-width")) {
                        double requestedCenterOffset = before.door.objLoc;
                        double requestedWidth = before.door.openingBase.width;
                        item.Get ("centerOffset", requestedCenterOffset);
                        item.Get ("width", requestedWidth);
                        failed.Add ("nativeFailureDetails", GS::ObjectState (
                            "requestedCenterOffset", requestedCenterOffset,
                            "actualCenterOffset", after.door.objLoc,
                            "requestedWidth", requestedWidth,
                            "actualWidth", after.door.openingBase.width,
                            "beforeWidth", before.door.openingBase.width));
                    }
                    results (failed);
                    return error;
                }
            }
            if (error != NoError) {
                callbackError = error;
                GS::ObjectState failed = CreateFailedExecutionResult (error, "Door typed UPDATE failed its exact native precondition or preservation postcondition.");
                failed.Add ("failureStage", failureStage);
                results (failed);
                return error;
            }
            results (CreateSuccessfulExecutionResult ());
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
            if (faultAfterFirstApplied && index == 0) {
                firstItemFaultInjectedAfterSuccessfulChange = true;
                callbackError = APIERR_COMMANDFAILED;
                failureStage = "fault-after-first-applied-item";
                results (CreateFailedExecutionResult (callbackError, "Disposable Door candidate fault requested rollback after the first applied item."));
                return callbackError;
            }
#endif
        }
        return callbackError;
    });
    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
        response.Add ("failureStage", failureStage);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (firstItemFaultInjectedAfterSuccessfulChange) response.Add ("testFaultEvidence", GS::ObjectState (
            "schemaVersion", "native-crud-test-fault-evidence/v1", "firstAppliedItemIndex", 0,
            "firstItemChangeSucceeded", true, "hostRollbackReported", true));
#endif
    }
    return response;
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
        "begCoordinate", "endCoordinate", "coordinates", "origin", "floorIndex", "storyIndex", "zCoordinate", "elevation", "level",
        "height", "width", "depth", "thickness", "bottomOffset", "offset", "slantAngle", "arcAngle",
        "verticalCurveHeight", "axisRotationAngle", "profileAngle", "referenceLineLocation", "referencePlaneLocation",
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
                "referenceLineLocation", "structureType", "buildingMaterialId", "compositeId", "profileId"
            });
        }
        return rejectUnsupportedFields ({
                "begCoordinate", "endCoordinate", "storyIndex", "elevation", "arcAngle", "height", "thickness", "bottomOffset", "offset",
                "referenceLineLocation", "structureType", "buildingMaterialId", "compositeId", "profileId"
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

GS::ObjectState ExecuteWindowCreate (const GS::Array<GS::ObjectState>& items, GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& created = response.AddList<GS::ObjectState> ("elements");
    if (items.GetSize () != 1) return CreateErrorResponse (APIERR_BADPARS, "Window typed CREATE accepts one exact same-file template.");
    GS::UniString failureStage = "not-started";
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Create Native Window from Exact Markerless Template", [&] () -> GSErrCode {
        const GS::ObjectState* sourceId = items[0].Get ("cloneSourceElementId");
        const GS::ObjectState* payload = items[0].Get ("payload");
        if (sourceId == nullptr || payload == nullptr) return APIERR_BADPARS;
        const API_Guid sourceGuid = GetGuidFromObjectState (*sourceId);
        const API_Guid hostGuid = payload->Get ("ownerWallId") == nullptr ? APINULLGuid : GetGuidFromObjectState (*payload->Get ("ownerWallId"));
        Int32 storyIndex = std::numeric_limits<Int32>::min ();
        if (!payload->Get ("floorIndex", storyIndex) || storyIndex < std::numeric_limits<short>::min () || storyIndex > std::numeric_limits<short>::max ()) return APIERR_BADPARS;
        API_Element source = {};
        source.header.guid = sourceGuid;
        failureStage = "read-window-template";
        GSErrCode error = sourceGuid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&source);
        if (error == NoError && (GetElemTypeId (source.header) != API_WindowID || source.header.guid != sourceGuid
            || source.window.openingBase.markGuid != APINULLGuid || !WindowNativeTopologySupported (source))) error = APIERR_BADPARS;
        API_LibPart sourcePart = {};
        LibraryPartLocationGuard sourcePartGuard (sourcePart);
        if (error == NoError) {
            sourcePart.index = source.window.openingBase.libInd;
            failureStage = "verify-window-library";
            error = ACAPI_LibraryPart_Get (&sourcePart);
            if (error == NoError && (sourcePart.missingDef || sourcePart.version <= 0 || sourcePart.ownUnID[0] == '\0' || sourcePart.file_UName[0] == '\0')) error = APIERR_BADPARS;
        }
        API_Element host = {};
        if (error == NoError) {
            host.header.guid = hostGuid;
            failureStage = "read-window-host-wall";
            error = hostGuid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&host);
            if (error == NoError && (GetElemTypeId (host.header) != API_WallID || host.header.guid != hostGuid || host.header.floorInd != static_cast<short> (storyIndex))) error = APIERR_BADID;
        }
        API_ElementMemo sourceMemo = {};
        NativeMemoGuard sourceMemoGuard (sourceMemo);
        if (error == NoError) { failureStage = "read-window-template-memo"; error = ACAPI_Element_GetMemo (sourceGuid, &sourceMemo, APIMemoMask_All); }
        if (error != NoError) return error;
        API_Element element = source;
        element.header.guid = APINULLGuid;
        element.header.floorInd = static_cast<short> (storyIndex);
        element.window.owner = hostGuid;
        element.window.openingBase.markGuid = APINULLGuid;
        double number = 0.0;
        if (payload->Get ("centerOffset", number)) element.window.objLoc = number;
        if (payload->Get ("sillHeight", number)) element.window.lower = number;
        if (payload->Get ("width", number)) element.window.openingBase.width = number;
        if (payload->Get ("height", number)) element.window.openingBase.height = number;
        bool flag = false;
        if (payload->Get ("reflected", flag)) element.window.openingBase.reflected = flag;
        if (payload->Get ("refSide", flag)) element.window.openingBase.refSide = flag;
        if (payload->Get ("oSide", flag)) element.window.openingBase.oSide = flag;
        failureStage = "createext-window";
        error = ACAPI_Element_CreateExt (&element, &sourceMemo, 0UL, nullptr);
        if (error == NoError) {
            API_Element after = {};
            after.header.guid = element.header.guid;
            failureStage = "verify-created-window";
            error = ACAPI_Element_Get (&after);
            if (error == NoError && (after.header.guid == APINULLGuid || after.header.guid == sourceGuid || after.header.floorInd != static_cast<short> (storyIndex)
                || after.window.owner != hostGuid || after.window.openingBase.markGuid != APINULLGuid
                || after.window.openingBase.libInd != source.window.openingBase.libInd || !WindowNativeTopologySupported (after)
                || after.window.objLoc != element.window.objLoc || after.window.lower != element.window.lower
                || after.window.openingBase.width != element.window.openingBase.width || after.window.openingBase.height != element.window.openingBase.height
                || after.window.openingBase.reflected != element.window.openingBase.reflected || after.window.openingBase.refSide != element.window.openingBase.refSide
                || after.window.openingBase.oSide != element.window.openingBase.oSide)) error = APIERR_COMMANDFAILED;
            API_LibPart createdPart = {};
            LibraryPartLocationGuard createdPartGuard (createdPart);
            if (error == NoError) {
                createdPart.index = after.window.openingBase.libInd;
                failureStage = "verify-created-window-library";
                error = ACAPI_LibraryPart_Get (&createdPart);
                if (error == NoError && (createdPart.missingDef || !SameWindowLibraryPartIdentity (sourcePart, createdPart))) error = APIERR_COMMANDFAILED;
            }
            API_Element sourceAfter = {};
            if (error == NoError) {
                sourceAfter.header.guid = sourceGuid;
                failureStage = "verify-window-template-preserved";
                error = ACAPI_Element_Get (&sourceAfter);
                if (error == NoError && (!SameWindowSourceScalarState (source, sourceAfter) || sourceAfter.window.openingBase.markGuid != APINULLGuid)) error = APIERR_COMMANDFAILED;
                API_LibPart sourcePartAfter = {};
                LibraryPartLocationGuard sourcePartAfterGuard (sourcePartAfter);
                if (error == NoError) {
                    sourcePartAfter.index = sourceAfter.window.openingBase.libInd;
                    failureStage = "verify-window-template-library-preserved";
                    error = ACAPI_LibraryPart_Get (&sourcePartAfter);
                    if (error == NoError && (sourcePartAfter.missingDef || !SameWindowLibraryPartIdentity (sourcePart, sourcePartAfter))) error = APIERR_COMMANDFAILED;
                }
            }
            if (error == NoError) created (CreateElementIdObjectState (after.header.guid));
        }
        return error;
    });
    if (undoError != NoError) { response.Add ("transactionRolledBack", true); response.Add ("transactionError", undoError); response.Add ("failureStage", failureStage); }
    return response;
}

GS::ObjectState ExecuteWindowUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied, GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    GSErrCode callbackError = NoError;
    GS::UniString failureStage = "not-started";
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    bool firstApplied = false;
#else
    (void) faultAfterFirstApplied;
#endif
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Update Native Windows", [&] () -> GSErrCode {
        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState& item = items[index];
            const GS::ObjectState* id = item.Get ("elementId");
            const API_Guid guid = id == nullptr ? APINULLGuid : GetGuidFromObjectState (*id);
            API_Element element = {};
            element.header.guid = guid;
            failureStage = "read-window";
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (error == NoError && (GetElemTypeId (element.header) != API_WindowID || element.header.guid != guid)) error = APIERR_BADID;
            if (error == NoError) {
                API_Element host = {};
                host.header.guid = element.window.owner;
                failureStage = "verify-window-update-host";
                error = host.header.guid == APINULLGuid ? APIERR_BADID : ACAPI_Element_Get (&host);
                if (error == NoError && (host.header.guid != element.window.owner || GetElemTypeId (host.header) != API_WallID
                    || host.header.floorInd != element.header.floorInd)) error = APIERR_BADID;
            }
            const API_Element before = element;
            if (error == NoError && (before.window.openingBase.markGuid != APINULLGuid || !WindowNativeTopologySupported (before))) error = APIERR_BADPARS;
            API_LibPart libraryBefore = {};
            LibraryPartLocationGuard libraryBeforeGuard (libraryBefore);
            if (error == NoError) {
                libraryBefore.index = before.window.openingBase.libInd;
                failureStage = "read-window-library-before";
                error = ACAPI_LibraryPart_Get (&libraryBefore);
                if (error == NoError && (libraryBefore.missingDef || libraryBefore.version <= 0 || libraryBefore.ownUnID[0] == '\0' || libraryBefore.file_UName[0] == '\0')) error = APIERR_BADPARS;
            }
            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            double number = 0.0;
            if (error == NoError && item.Get ("centerOffset", number)) { element.window.objLoc = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, objLoc); changed = true; }
            if (error == NoError && item.Get ("sillHeight", number)) { element.window.lower = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, lower); changed = true; }
            if (error == NoError && item.Get ("width", number)) { element.window.openingBase.width = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width); changed = true; }
            if (error == NoError && item.Get ("height", number)) { element.window.openingBase.height = number; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height); changed = true; }
            bool flag = false;
            if (error == NoError && item.Get ("reflected", flag)) { element.window.openingBase.reflected = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.reflected); changed = true; }
            if (error == NoError && item.Get ("refSide", flag)) { element.window.openingBase.refSide = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.refSide); changed = true; }
            if (error == NoError && item.Get ("oSide", flag)) { element.window.openingBase.oSide = flag; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.oSide); changed = true; }
            if (error == NoError && !changed) error = APIERR_BADPARS;
            if (error == NoError) { failureStage = "change-window"; error = ACAPI_Element_Change (&element, &mask, nullptr, 0, true); }
            API_Element after = {};
            if (error == NoError) { after.header.guid = guid; failureStage = "read-window-postcondition"; error = ACAPI_Element_Get (&after); }
            if (error == NoError && (after.header.guid != before.header.guid || after.header.floorInd != before.header.floorInd || after.header.layer != before.header.layer
                || after.window.owner != before.window.owner || after.window.openingBase.libInd != before.window.openingBase.libInd
                || after.window.openingBase.markGuid != APINULLGuid || after.window.fixPoint != before.window.fixPoint
                || after.window.openingBase.verticalLink.linkType != before.window.openingBase.verticalLink.linkType
                || after.window.openingBase.verticalLink.linkValue != before.window.openingBase.verticalLink.linkValue
                || after.window.directionType != before.window.directionType || after.window.reveal != before.window.reveal
                || after.window.revealDepthLocation != before.window.revealDepthLocation || after.window.revealDepthOffset != before.window.revealDepthOffset
                || after.window.revealDepthFromSide != before.window.revealDepthFromSide || after.window.jambDepthHead != before.window.jambDepthHead
                || after.window.jambDepthSill != before.window.jambDepthSill || after.window.jambDepth != before.window.jambDepth
                || after.window.jambDepth2 != before.window.jambDepth2 || !WindowNativeTopologySupported (after))) error = APIERR_COMMANDFAILED;
            API_LibPart libraryAfter = {};
            LibraryPartLocationGuard libraryAfterGuard (libraryAfter);
            if (error == NoError) {
                libraryAfter.index = after.window.openingBase.libInd;
                failureStage = "verify-window-library-preserved";
                error = ACAPI_LibraryPart_Get (&libraryAfter);
                if (error == NoError && (libraryAfter.missingDef || !SameWindowLibraryPartIdentity (libraryBefore, libraryAfter))) error = APIERR_COMMANDFAILED;
            }
            const auto expectedDouble = [&] (const char* field, const double oldValue, const double actual) { double requested = oldValue; item.Get (field, requested); return requested == actual; };
            const auto expectedBool = [&] (const char* field, const bool oldValue, const bool actual) { bool requested = oldValue; item.Get (field, requested); return requested == actual; };
            if (error == NoError && (!expectedDouble ("centerOffset", before.window.objLoc, after.window.objLoc)
                || !expectedDouble ("sillHeight", before.window.lower, after.window.lower) || !expectedDouble ("width", before.window.openingBase.width, after.window.openingBase.width)
                || !expectedDouble ("height", before.window.openingBase.height, after.window.openingBase.height)
                || !expectedBool ("reflected", before.window.openingBase.reflected, after.window.openingBase.reflected)
                || !expectedBool ("refSide", before.window.openingBase.refSide, after.window.openingBase.refSide)
                || !expectedBool ("oSide", before.window.openingBase.oSide, after.window.openingBase.oSide))) error = APIERR_COMMANDFAILED;
            if (error != NoError) { callbackError = error; GS::ObjectState failed = CreateFailedExecutionResult (error, "Window typed UPDATE failed exact native precondition or factual requested/protected-value readback."); failed.Add ("failureStage", failureStage); results (failed); return error; }
            results (CreateSuccessfulExecutionResult ());
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
            if (faultAfterFirstApplied && index == 0) { firstApplied = true; callbackError = APIERR_COMMANDFAILED; failureStage = "fault-after-first-applied-item"; results (CreateFailedExecutionResult (callbackError, "Disposable Window candidate fault requested rollback after the first applied item.")); return callbackError; }
#endif
        }
        return callbackError;
    });
    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true); response.Add ("transactionError", undoError); response.Add ("failureStage", failureStage);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (firstApplied) response.Add ("testFaultEvidence", GS::ObjectState ("schemaVersion", "native-crud-test-fault-evidence/v1", "firstAppliedItemIndex", 0, "firstItemChangeSucceeded", true, "hostRollbackReported", true));
#endif
    }
    return response;
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
        case API_LineID:   return CreateLineElementsCommand ().Execute (parameters, processControl);
        case API_ArcID:    return CreateArcsCommand ().Execute (parameters, processControl);
        case API_HatchID:  return CreateHatchesCommand ().Execute (parameters, processControl);
        default:           return CreateErrorResponse (APIERR_BADID, "Unsupported element type for create.");
    }
}

double SignedAreaOfHatchRing (const std::vector<API_Coord>& points)
{
    double twiceArea = 0.0;
    for (GSSize index = 0; index < points.size (); ++index) {
        const API_Coord& point = points[index];
        const API_Coord& next = points[(index + 1) % points.size ()];
        twiceArea += point.x * next.y - next.x * point.y;
    }
    return twiceArea;
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

bool IsSimpleHatchRing (const std::vector<API_Coord>& ring)
{
    if (ring.size () < 3 || ring.size () > 64) return false;
    double twiceArea = 0.0;
    for (GSSize index = 0; index < ring.size (); ++index) {
        const API_Coord& point = ring[index];
        const API_Coord& next = ring[(index + 1) % ring.size ()];
        if (!std::isfinite (point.x) || !std::isfinite (point.y) || IsSame2DCoordinate (point, next)) return false;
        for (GSSize prior = 0; prior < index; ++prior)
            if (IsSame2DCoordinate (point, ring[prior])) return false;
        twiceArea += point.x * next.y - next.x * point.y;
    }
    if (!std::isfinite (twiceArea) || twiceArea == 0.0) return false;
    for (GSSize first = 0; first < ring.size (); ++first) {
        const GSSize firstNext = (first + 1) % ring.size ();
        for (GSSize second = first + 1; second < ring.size (); ++second) {
            const GSSize secondNext = (second + 1) % ring.size ();
            if (firstNext == second || secondNext == first) continue;
            if (HatchSegmentsIntersect (ring[first], ring[firstNext], ring[second], ring[secondNext])) return false;
        }
    }
    return true;
}

bool ReadSimpleHatchMemo (const API_Element& element, API_ElementMemo& memo, std::vector<API_Coord>& ring)
{
    if (element.hatch.poly.nCoords < 4 || element.hatch.poly.nSubPolys != 1 || element.hatch.poly.nArcs != 0
        || memo.coords == nullptr || *memo.coords == nullptr || memo.pends == nullptr || *memo.pends == nullptr
        || memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr)
        return false;
    const GSSize coordinateCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.coords)) / sizeof (API_Coord);
    const GSSize polygonEndCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.pends)) / sizeof (Int32);
    const GSSize vertexCapacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.vertexIDs)) / sizeof (UInt32);
    if (coordinateCapacity <= static_cast<GSSize> (element.hatch.poly.nCoords)
        || polygonEndCapacity <= static_cast<GSSize> (element.hatch.poly.nSubPolys)
        || vertexCapacity <= static_cast<GSSize> (element.hatch.poly.nCoords)
        || (*memo.pends)[1] != element.hatch.poly.nCoords)
        return false;
    const API_Coord& first = (*memo.coords)[1];
    const API_Coord& closing = (*memo.coords)[element.hatch.poly.nCoords];
    if (!IsSame2DCoordinate (first, closing)) return false;
    ring.clear ();
    ring.reserve (static_cast<GSSize> (element.hatch.poly.nCoords - 1));
    for (Int32 index = 1; index < element.hatch.poly.nCoords; ++index)
        ring.push_back ((*memo.coords)[index]);
    return IsSimpleHatchRing (ring);
}

bool ReadHatchMemoVertexIds (const API_Element& element, const API_ElementMemo& memo, std::vector<UInt32>& vertexIds)
{
    if (element.hatch.poly.nCoords <= 0 || memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr)
        return false;
    const GSSize capacity = BMhGetSize (reinterpret_cast<GSHandle> (memo.vertexIDs)) / sizeof (UInt32);
    if (capacity <= static_cast<GSSize> (element.hatch.poly.nCoords))
        return false;
    vertexIds.clear ();
    vertexIds.reserve (static_cast<GSSize> (element.hatch.poly.nCoords));
    for (Int32 index = 1; index <= element.hatch.poly.nCoords; ++index) {
        const UInt32 vertexId = (*memo.vertexIDs)[index];
        if (vertexId > static_cast<UInt32> (std::numeric_limits<Int32>::max ())) return false;
        vertexIds.push_back (vertexId);
    }
    return true;
}

GS::ObjectState ExecuteHatchUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    GSErrCode callbackError = NoError;
    GS::UniString failureStage = "not-started";
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    bool firstItemFaultInjectedAfterSuccessfulChange = false;
#else
    (void) faultAfterFirstApplied;
#endif
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Update Native Hatches", [&] () -> GSErrCode {
        for (GSSize itemIndex = 0; itemIndex < static_cast<GSSize> (items.GetSize ()); ++itemIndex) {
            const GS::ObjectState& item = items[itemIndex];
            if (callbackError != NoError) {
                results (CreateFailedExecutionResult (callbackError, "Hatch item was not attempted because the batch is rolling back."));
                continue;
            }
            const GS::ObjectState* elementId = item.Get ("elementId");
            const API_Guid guid = elementId == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementId);
            API_Element element = {};
            element.header.guid = guid;
            failureStage = "get-element";
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (guid == APINULLGuid) failureStage = "validate-element-guid";
            if (error == NoError && GetElemTypeId (element.header) != API_HatchID) {
                error = APIERR_BADID;
                failureStage = "validate-element-type";
            }
            if (error == NoError && (element.hatch.hatchType != API_FillHatch || element.hatch.determination != APIHatch_DraftingFills)) {
                error = APIERR_BADPARS;
                failureStage = "validate-fill-drafting-profile";
            }
            const API_Element originalElement = element;
            const short originalStoryIndex = element.header.floorInd;
            const bool unsupportedOverrides = error == NoError
                && (element.hatch.contPen.IsOverriden () || element.hatch.fillPen.IsOverriden ()
                    || (element.hatch.hatchFlags & (APIHatch_HasFgRGBColor | APIHatch_HasBkgRGBColor)) != 0);
            if (unsupportedOverrides) {
                error = APIERR_BADPARS;
                failureStage = "validate-unsupported-overrides";
            }

            API_ElementMemo memo = {};
            NativeMemoGuard memoGuard (memo);
            GS::Array<GS::ObjectState> coordinates;
            const bool hasPolygon = item.Get ("polygonOutline", coordinates);
            std::vector<API_Coord> beforeRing;
            std::vector<UInt32> beforeVertexIds;
            std::vector<API_Coord> requestedRing;
            if (error == NoError) {
                error = ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon);
                if (error != NoError) failureStage = "get-before-polygon-memo";
                if (error == NoError && !ReadSimpleHatchMemo (element, memo, beforeRing)) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-before-simple-polygon-memo";
                }
                if (error == NoError && !ReadHatchMemoVertexIds (element, memo, beforeVertexIds)) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-before-vertex-ids";
                }
            }
            if (error == NoError && hasPolygon) {
                if (coordinates.GetSize () != beforeRing.size ()) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-requested-vertex-count";
                }
                if (error == NoError) {
                    requestedRing.reserve (static_cast<GSSize> (coordinates.GetSize ()));
                    for (const GS::ObjectState& point : coordinates) requestedRing.push_back (Get2DCoordinateFromObjectState (point));
                    const double beforeArea = SignedAreaOfHatchRing (beforeRing);
                    const double requestedArea = SignedAreaOfHatchRing (requestedRing);
                    if (!IsSimpleHatchRing (requestedRing) || !std::isfinite (beforeArea)
                        || std::signbit (beforeArea) != std::signbit (requestedArea)) {
                        error = APIERR_BADPARS;
                        failureStage = "validate-requested-simple-ring-and-winding";
                    }
                }
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            Int32 value = 0;
            if (error == NoError && item.Get ("contourPenIndex", value)) {
                if (value <= 0 || value > std::numeric_limits<short>::max ()) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-contour-pen";
                }
                else { element.hatch.contPen.penIndex = static_cast<short> (value); ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, contPen); changed = true; }
            }
            if (error == NoError && item.Get ("fillPenIndex", value)) {
                if (value <= 0 || value > std::numeric_limits<short>::max ()) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-foreground-pen";
                }
                else { element.hatch.fillPen.penIndex = static_cast<short> (value); ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillPen); changed = true; }
            }
            if (error == NoError && item.Get ("fillBackgroundPenIndex", value)) {
                if (value <= 0 || value > std::numeric_limits<short>::max ()) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-background-pen";
                }
                else { element.hatch.fillBGPen = static_cast<short> (value); ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillBGPen); changed = true; }
            }
            if (error == NoError && item.Get ("fillAttributeIndex", value)) {
                if (value <= 0) {
                    error = APIERR_BADPARS;
                    failureStage = "validate-fill-index";
                }
                else { element.hatch.fillInd = ACAPI_CreateAttributeIndex (value); ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, fillInd); changed = true; }
            }
            if (error == NoError && hasPolygon) {
                for (GSSize index = 0; index < requestedRing.size (); ++index) (*memo.coords)[index + 1] = requestedRing[index];
                (*memo.coords)[requestedRing.size () + 1] = requestedRing[0];
                ACAPI_ELEMENT_MASK_SET (mask, API_HatchType, poly);
                changed = true;
            }
            if (error == NoError && !changed) {
                error = APIERR_BADPARS;
                failureStage = "validate-nonempty-change";
            }
            if (error == NoError) {
                failureStage = "acapi-element-change";
                error = ACAPI_Element_Change (&element, &mask, hasPolygon ? &memo : nullptr, hasPolygon ? APIMemoMask_Polygon : 0, true);
            }

            if (error == NoError) {
                API_Element after = {};
                after.header.guid = guid;
                failureStage = "get-after-element";
                error = ACAPI_Element_Get (&after);
                if (error == NoError) {
                    // Keep preservation failures attributable to one factual
                    // field.  A single generic stage made live Hatch UPDATE
                    // failures impossible to distinguish from a pen alias or
                    // profile side effect before the undoable command rolled
                    // the element back.
                    const char* preservationFailureStage = nullptr;
                    // API_ExtendedPenType::IsOverriden() is true only for a
                    // positive index. Archicad may normalize inactive Hatch
                    // override sentinels while changing the base pen.
                    const auto sameHatchPenOverride = [] (Int32 expected, Int32 actual) {
                        return expected == actual || (expected <= 0 && actual <= 0);
                    };
                    if (after.header.floorInd != originalStoryIndex)
                        preservationFailureStage = "verify-preserved-story";
                    else if (after.header.layer != originalElement.header.layer)
                        preservationFailureStage = "verify-preserved-layer";
                    else if (GetElemTypeId (after.header) != API_HatchID)
                        preservationFailureStage = "verify-preserved-element-type";
                    else if (after.hatch.hatchType != originalElement.hatch.hatchType)
                        preservationFailureStage = "verify-preserved-hatch-type";
                    else if (after.hatch.determination != originalElement.hatch.determination)
                        preservationFailureStage = "verify-preserved-determination";
                    else if (after.hatch.hatchFlags != originalElement.hatch.hatchFlags)
                        preservationFailureStage = "verify-preserved-hatch-flags";
                    else if (!sameHatchPenOverride (originalElement.hatch.contPen.colorOverridePenIndex, after.hatch.contPen.colorOverridePenIndex)) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = GS::UniString::Printf (
                            "verify-preserved-contour-pen-override expected=%d actual=%d",
                            static_cast<Int32> (originalElement.hatch.contPen.colorOverridePenIndex),
                            static_cast<Int32> (after.hatch.contPen.colorOverridePenIndex));
                    }
                    else if (!sameHatchPenOverride (originalElement.hatch.fillPen.colorOverridePenIndex, after.hatch.fillPen.colorOverridePenIndex))
                        preservationFailureStage = "verify-preserved-fill-pen-override";
                    else if (GetAttributeIndex (after.hatch.buildingMaterial) != GetAttributeIndex (originalElement.hatch.buildingMaterial))
                        preservationFailureStage = "verify-preserved-building-material";
                    if (preservationFailureStage != nullptr) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = preservationFailureStage;
                    }
                }
                if (error == NoError) {
                    Int32 expectedContourPen = originalElement.hatch.contPen.penIndex;
                    Int32 expectedFillPen = originalElement.hatch.fillPen.penIndex;
                    Int32 expectedBackgroundPen = originalElement.hatch.fillBGPen;
                    Int32 expectedFill = GetAttributeIndex (originalElement.hatch.fillInd);
                    item.Get ("contourPenIndex", expectedContourPen);
                    item.Get ("fillPenIndex", expectedFillPen);
                    item.Get ("fillBackgroundPenIndex", expectedBackgroundPen);
                    item.Get ("fillAttributeIndex", expectedFill);
                    if (after.hatch.contPen.penIndex != expectedContourPen) error = APIERR_COMMANDFAILED;
                    if (after.hatch.fillPen.penIndex != expectedFillPen) error = APIERR_COMMANDFAILED;
                    if (after.hatch.fillBGPen != expectedBackgroundPen) error = APIERR_COMMANDFAILED;
                    if (GetAttributeIndex (after.hatch.fillInd) != expectedFill) error = APIERR_COMMANDFAILED;
                    if (error == APIERR_COMMANDFAILED) failureStage = "verify-element-fields";
                }
                if (error == NoError) {
                    API_ElementMemo afterMemo = {};
                    NativeMemoGuard afterMemoGuard (afterMemo);
                    failureStage = "get-after-polygon-memo";
                    error = ACAPI_Element_GetMemo (guid, &afterMemo, APIMemoMask_Polygon);
                    std::vector<API_Coord> afterRing;
                    std::vector<UInt32> afterVertexIds;
                    if (error == NoError && !ReadSimpleHatchMemo (after, afterMemo, afterRing)) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = "verify-after-simple-polygon-memo";
                    }
                    if (error == NoError && !ReadHatchMemoVertexIds (after, afterMemo, afterVertexIds)) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = "verify-after-vertex-ids";
                    }
                    const std::vector<API_Coord>& expectedRing = hasPolygon ? requestedRing : beforeRing;
                    if (error == NoError && afterRing.size () != expectedRing.size ()) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = "verify-after-vertex-count";
                    }
                    for (GSSize index = 0; error == NoError && index < expectedRing.size (); ++index)
                        if (!IsSame2DCoordinate (afterRing[index], expectedRing[index])) {
                            error = APIERR_COMMANDFAILED;
                            failureStage = "verify-after-coordinates";
                        }
                    if (error == NoError && afterVertexIds != beforeVertexIds) {
                        error = APIERR_COMMANDFAILED;
                        failureStage = "verify-preserved-vertex-ids";
                    }
                }
            }
            if (error != NoError) {
                callbackError = error;
                GS::ObjectState failed = CreateFailedExecutionResult (error, "Hatch typed UPDATE failed its native precondition or factual postcondition.");
                failed.Add ("failureStage", failureStage);
                results (failed);
                return error;
            }
            results (CreateSuccessfulExecutionResult ());
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
            if (faultAfterFirstApplied && itemIndex == 0) {
                firstItemFaultInjectedAfterSuccessfulChange = true;
                callbackError = APIERR_COMMANDFAILED;
                failureStage = "fault-after-first-applied-item";
                results (CreateFailedExecutionResult (callbackError, "Disposable Hatch candidate fault seam requested rollback after the first item was applied."));
                return callbackError;
            }
#endif
        }
        return callbackError;
    });
    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
        response.Add ("failureStage", failureStage);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (firstItemFaultInjectedAfterSuccessfulChange) response.Add ("testFaultEvidence", GS::ObjectState (
            "schemaVersion", "native-crud-test-fault-evidence/v1", "firstAppliedItemIndex", 0,
            "firstItemChangeSucceeded", true, "hostRollbackReported", true));
#endif
    }
    return response;
}

GS::ObjectState ExecuteDraftingUpdate (
    const API_ElemTypeID typeID,
    const GS::Array<GS::ObjectState>& items,
    const bool faultAfterFirstApplied = false)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    GSErrCode callbackError = NoError;
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    bool firstItemFaultInjectedAfterSuccessfulChange = false;
#else
    (void) faultAfterFirstApplied;
#endif
    const GSSize itemCount = static_cast<GSSize> (items.GetSize ());

    const GSErrCode undoError = ACAPI_CallUndoableCommand (typeID == API_LineID ? "Update Native Lines" : "Update Native Arcs", [&]() -> GSErrCode {
        for (GSSize itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            const GS::ObjectState& item = items[itemIndex];
            const GS::ObjectState* elementId = item.Get ("elementId");
            const GS::ObjectState* payload = item.Get ("payload");
            // Native CRUD's adapter carries a nested payload, while the
            // legacy MutateElements normalizer flattens update fields beside
            // elementId.  Accept both shapes without widening the wire
            // contract or accidentally treating the envelope as a payload
            // when a nested object is present.
            if (payload == nullptr)
                payload = &item;
            const API_Guid guid = elementId == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementId);
            API_Element element = {};
            element.header.guid = guid;
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (error == NoError && GetElemTypeId (element.header) != typeID)
                error = APIERR_BADID;
            const short originalStoryIndex = element.header.floorInd;
            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool changed = false;
            bool storyChanged = false;
            if (error == NoError && payload == nullptr)
                error = APIERR_BADPARS;

            if (error == NoError && typeID == API_LineID) {
                const GS::ObjectState* begin = payload->Get ("begCoordinate");
                const GS::ObjectState* end = payload->Get ("endCoordinate");
                if (begin != nullptr) {
                    element.line.begC = Get2DCoordinateFromObjectState (*begin);
                    ACAPI_ELEMENT_MASK_SET (mask, API_LineType, begC);
                    changed = true;
                }
                if (end != nullptr) {
                    element.line.endC = Get2DCoordinateFromObjectState (*end);
                    ACAPI_ELEMENT_MASK_SET (mask, API_LineType, endC);
                    changed = true;
                }
                Int32 pen = 0;
                if (payload->Get ("linePen", pen) || payload->Get ("linePenIndex", pen)) {
                    element.line.linePen.penIndex = static_cast<short> (pen);
                    element.line.linePen.colorOverridePenIndex = 0;
                    ACAPI_ELEMENT_MASK_SET (mask, API_LineType, linePen);
                    changed = true;
                }
                Int32 lineType = 0;
                if (payload->Get ("lineTypeIndex", lineType)) {
                    element.line.ltypeInd = ACAPI_CreateAttributeIndex (lineType);
                    ACAPI_ELEMENT_MASK_SET (mask, API_LineType, ltypeInd);
                    changed = true;
                }
                if (payload->Get ("roomSeparator", element.line.roomSeparator)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_LineType, roomSeparator);
                    changed = true;
                }
            } else if (error == NoError && typeID == API_ArcID) {
                const GS::ObjectState* origin = payload->Get ("origin");
                if (origin != nullptr) {
                    element.arc.origC = Get2DCoordinateFromObjectState (*origin);
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, origC);
                    changed = true;
                }
                if (payload->Get ("radius", element.arc.r)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, r);
                    changed = true;
                }
                if (payload->Get ("axisAngle", element.arc.angle)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, angle);
                    changed = true;
                }
                if (payload->Get ("ratio", element.arc.ratio)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, ratio);
                    changed = true;
                }
                if (payload->Get ("beginAngle", element.arc.begAng) || payload->Get ("begAngle", element.arc.begAng)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, begAng);
                    changed = true;
                }
                if (payload->Get ("endAngle", element.arc.endAng)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, endAng);
                    changed = true;
                }
                if (payload->Get ("reflected", element.arc.reflected)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, reflected);
                    changed = true;
                }
                Int32 pen = 0;
                if (payload->Get ("linePen", pen) || payload->Get ("linePenIndex", pen)) {
                    element.arc.linePen.penIndex = static_cast<short> (pen);
                    element.arc.linePen.colorOverridePenIndex = 0;
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, linePen);
                    changed = true;
                }
                Int32 lineType = 0;
                if (payload->Get ("lineTypeIndex", lineType)) {
                    element.arc.ltypeInd = ACAPI_CreateAttributeIndex (lineType);
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, ltypeInd);
                    changed = true;
                }
                if (payload->Get ("roomSeparator", element.arc.roomSeparator)) {
                    ACAPI_ELEMENT_MASK_SET (mask, API_ArcType, roomSeparator);
                    changed = true;
                }
            }

            // A sparse endpoint patch must be checked against the untouched
            // endpoint as well.  ValidatePayload can only compare two fields
            // that are present in the request; without this post-merge check
            // `{begin: ...}` could turn a valid line into a zero-length line.
            if (error == NoError && typeID == API_LineID &&
                IsSame2DCoordinate (element.line.begC, element.line.endC))
                error = APIERR_BADPARS;

            if (error == NoError && !changed)
                error = APIERR_BADPARS;
            if (error == NoError)
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
            if (error != NoError) {
                callbackError = error;
                results (CreateFailedExecutionResult (error, storyChanged
                    ? "Native CRUD drafting UPDATE changed story; the undoable batch must roll back."
                    : "Failed to update drafting element."));
                return error;
            }
            results (CreateSuccessfulExecutionResult ());
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
            if (faultAfterFirstApplied && itemIndex == 0) {
                firstItemFaultInjectedAfterSuccessfulChange = true;
                results (CreateFailedExecutionResult (APIERR_COMMANDFAILED, "Disposable drafting candidate fault seam requested rollback after the first item was applied."));
                return APIERR_COMMANDFAILED;
            }
#endif
        }
        return callbackError;
    });

    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
    }
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (firstItemFaultInjectedAfterSuccessfulChange && undoError != NoError)
        response.Add ("testFaultEvidence", GS::ObjectState (
            "schemaVersion", "native-crud-test-fault-evidence/v1",
            "firstAppliedItemIndex", 0,
            "firstItemChangeSucceeded", true,
            "hostRollbackReported", true));
#endif
    return response;
}

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
    const bool hasElevation = details.Contains ("elevation");
    if (!hasElevation) return {};
    if (stories.empty ())
        return "Story settings are unavailable; Wall placement cannot be updated.";

    const short currentStoryIndex = element.header.floorInd;
    if (stories.find (currentStoryIndex) == stories.end())
        return "The Wall current story is unavailable; absolute elevation cannot be resolved.";

    const double currentElevation = GetZPos (currentStoryIndex, element.wall.bottomOffset, stories);
    double targetElevation = 0.0;
    if (!details.Get ("elevation", targetElevation) || !std::isfinite (targetElevation))
        return "'elevation' must be a finite real number.";
    const double targetBottomOffset = targetElevation - GetZPos (currentStoryIndex, 0.0, stories);
    if (!std::isfinite (targetBottomOffset))
        return "Wall placement resolved to a non-finite bottom offset.";

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
    return true;
}

GS::ObjectState ExecuteNativeCrudWallUpdate (const GS::Array<GS::ObjectState>& items)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    const Stories stories = GetStories ();
    GSErrCode callbackError = NoError;
    bool testFaultInjectedAfterFirstApplied = false;

    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Update Native Walls", [&]() -> GSErrCode {
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

GS::ObjectState ExecuteUpdate (
    const API_ElemTypeID typeID,
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl)
{
    switch (typeID) {
        // MutateElements is the single project-owned batch boundary.  It opts
        // into the atomic mode of the legacy typed modifiers so a runtime
        // failure in any item causes Archicad to roll back the whole batch.
        case API_WallID: {
            GS::Array<GS::ObjectState> items;
            if (!parameters.Get ("wallsWithDetails", items))
                return CreateErrorResponse (APIERR_BADPARS, "Wall update items are missing.");
            return ExecuteNativeCrudWallUpdate (items);
        }
        case API_SlabID:   return ModifySlabsCommand (true).Execute (parameters, processControl);
        case API_ColumnID: return ModifyColumnsCommand (true).Execute (parameters, processControl);
        case API_BeamID:   return ModifyBeamsCommand (true).Execute (parameters, processControl);
        case API_LineID:
        case API_ArcID: {
            const char* fieldName = typeID == API_LineID ? "linesWithDetails" : "arcsWithDetails";
            GS::Array<GS::ObjectState> items;
            if (!parameters.Get (fieldName, items))
                return CreateErrorResponse (APIERR_BADPARS, "Drafting update items are missing.");
            bool faultAfterFirstApplied = false;
            parameters.Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
            if (faultAfterFirstApplied && typeID != API_LineID && typeID != API_ArcID)
                return CreateErrorResponse (APIERR_BADPARS, "First-applied fault seam is available only for Line or Arc UPDATE in the drafting executor.");
            return ExecuteDraftingUpdate (typeID, items, faultAfterFirstApplied);
        }
        case API_HatchID: {
            GS::Array<GS::ObjectState> items;
            if (!parameters.Get ("hatchesWithDetails", items))
                return CreateErrorResponse (APIERR_BADPARS, "Hatch update items are missing.");
            bool faultAfterFirstApplied = false;
            parameters.Get ("nativeCrudTestFaultAfterFirstApplied", faultAfterFirstApplied);
            return ExecuteHatchUpdate (items, faultAfterFirstApplied);
        }
        default:           return CreateErrorResponse (APIERR_BADID, "Unsupported element type for update.");
    }
}

GS::ObjectState ExecuteDelete (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl)
{
    return DeleteElementsCommand ().Execute (BuildArrayParameters ("elements", items), processControl);
}

constexpr UInt64 TextRichMemoMask = APIMemoMask_TextContentUni | APIMemoMask_ParagraphUni;

void ApplyTextPlacement (
    API_Element& element,
    API_Element& mask,
    const GS::ObjectState& payload,
    const bool deriveFloorFromCoordinate,
    const Stories& stories)
{
    const GS::ObjectState* coordinate = payload.Get ("coordinate");
    if (coordinate != nullptr) {
        const API_Coord3D point = Get3DCoordinateFromObjectState (*coordinate);
        element.text.loc.x = point.x;
        element.text.loc.y = point.y;
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, loc);
        if (deriveFloorFromCoordinate && !payload.Contains ("floorIndex")) {
            element.header.floorInd = GetFloorIndexAndOffset (point.z, stories).first;
            ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
        }
    }
    Int32 floorIndex = 0;
    if (deriveFloorFromCoordinate && payload.Get ("floorIndex", floorIndex)) {
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

// A sparse native-crud Text UPDATE containing only `angle` can use the
// DevKit's edit operation instead of rebuilding the Text element.  The
// native-crud adapter deliberately flattens the nested `patch`/`payload`
// envelope before calling MutateElements, so this predicate receives the
// already-normalized item (`elementId` plus typed fields).
bool IsAngleOnlyTextItem (const GS::ObjectState& item)
{
    if (!item.Contains ("angle"))
        return false;

    for (const char* fieldName : {
        "richText", "coordinate", "floorIndex", "anchor", "width", "height",
        "pen", "fixedSize", "nonBreaking"
    }) {
        if (item.Contains (fieldName))
            return false;
    }
    return true;
}

GSErrCode ValidateTextRotationTarget (const API_Element& freshElement, const double requestedAngle)
{
    if (!std::isfinite (requestedAngle) || !std::isfinite (freshElement.text.angle))
        return APIERR_BADPARS;

    if (freshElement.text.fixedAngle)
        return APIERR_BADPARS;

    // A Text owned by another element (for example a Label) is not an
    // independent placement target.  Rotating it could create collateral
    // changes in its owner, so this bounded lane rejects it explicitly.
    if (freshElement.text.owner != APINULLGuid)
        return APIERR_BADPARS;

    API_Guid groupGuid = APINULLGuid;
    // GetGroup is an authoritative native collateral check.  An unavailable
    // grouping fact is not permission to guess that the target is independent.
    const GSErrCode groupError = ACAPI_Grouping_GetGroup (freshElement.header.guid, &groupGuid);
    if (groupError != NoError)
        return groupError;
    if (groupGuid != APINULLGuid)
        return APIERR_BADPARS;

    return NoError;
}

GSErrCode ValidateAngleOnlyTextItem (const GS::ObjectState& item)
{
    const GS::ObjectState* elementID = item.Get ("elementId");
    const API_Guid guid = elementID == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementID);
    if (guid == APINULLGuid)
        return APIERR_BADPARS;

    double requestedAngle = 0.0;
    if (!item.Get ("angle", requestedAngle))
        return APIERR_BADPARS;

    API_Element freshElement = {};
    freshElement.header.guid = guid;
    GSErrCode error = ACAPI_Element_Get (&freshElement);
    if (error == NoError && GetElemTypeId (freshElement.header) != API_TextID)
        error = APIERR_BADID;
    if (error == NoError)
        error = ValidateTextRotationTarget (freshElement, requestedAngle);
    return error;
}

// Rotate one ungrouped, unlinked Text around its freshly-read anchor.  This
// is intentionally a private native helper rather than the public
// RotateElements command: the native-crud receipt must reflect the exact
// single-item edit and an ACAPI failure must propagate to the undoable lane.
GSErrCode RotateTextByDelta (const API_Element& freshElement, const double requestedAngle)
{
    const GSErrCode validationError = ValidateTextRotationTarget (freshElement, requestedAngle);
    if (validationError != NoError)
        return validationError;

    const double delta = requestedAngle - freshElement.text.angle;
    if (!std::isfinite (delta))
        return APIERR_BADPARS;

    // APIEdit_Rotate consumes a begin/end ray and an origin.  The text's own
    // freshly-read anchor is the origin, so the edit changes only its angle;
    // it does not ask ACAPI_Element_Change to reserialize memo or dimensions.
    const API_Coord origin = freshElement.text.loc;
    const API_Coord3D beginPoint { origin.x + 1.0, origin.y, 0.0 };
    const API_Coord3D endPoint {
        origin.x + std::cos (delta),
        origin.y + std::sin (delta),
        0.0
    };
    GS::Array<API_Neig> elementsToEdit = { API_Neig (freshElement.header.guid) };
    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Rotate;
    editPars.begC = beginPoint;
    editPars.endC = endPoint;
    editPars.origC = origin;
    editPars.withDelete = true;
    return ACAPI_Element_Edit (&elementsToEdit, editPars);
}

GS::ObjectState ExecuteTextCreate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    const Stories stories = GetStories ();
    GSErrCode callbackError = NoError;
    std::vector<API_Elem_Head> createdHeads;
    createdHeads.reserve (items.GetSize ());

    API_NotifyElementType notification = {};
    notification.notifID = APINotifyElement_BeginEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Create Rich Text", [&] () -> GSErrCode {
        bool savedAutoTextFlag = false;
        const GSErrCode autoTextErr = ACAPI_AutoText_GetAutoTextFlag (&savedAutoTextFlag);
        if (autoTextErr != NoError) {
            elements (CreateErrorResponse (autoTextErr, "Failed to read the AutoText flag."));
            callbackError = autoTextErr;
            return callbackError;
        }
        const GS::OnExit autoTextGuard ([&savedAutoTextFlag] () {
            ACAPI_AutoText_ChangeAutoTextFlag (&savedAutoTextFlag);
        });
        bool setAutoTextFlag = false;
        const GSErrCode disableAutoTextErr = ACAPI_AutoText_ChangeAutoTextFlag (&setAutoTextFlag);
        if (disableAutoTextErr != NoError) {
            elements (CreateErrorResponse (disableAutoTextErr, "Failed to disable the AutoText flag."));
            callbackError = disableAutoTextErr;
            return callbackError;
        }

        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState& payload = items[index];
            if (callbackError != NoError) {
                elements (CreateErrorResponse (callbackError, "Text item was not attempted because the batch was rolled back."));
                continue;
            }

            API_Element element = {};
            API_ElementMemo memo = {};
            NativeMemoGuard memoGuard (memo);
#ifdef ServerMainVers_2600
            element.header.type = API_TextID;
#else
            element.header.typeID = API_TextID;
#endif
            GSErrCode error = ACAPI_Element_GetDefaults (&element, nullptr);
            if (error == NoError) {
                const GS::ObjectState* richText = payload.Get ("richText");
                GS::UniString richTextError;
                error = richText == nullptr
                    ? APIERR_BADPARS
                    : NativeRichTextMemo::BuildMemo (*richText, memo, element.text, richTextError);
                if (error != NoError) {
                    elements (CreateErrorResponse (error, richTextError.IsEmpty ()
                        ? "Failed to build Rich Text memo."
                        : richTextError));
                } else {
                    API_Element ignoredMask = {};
                    ApplyTextPlacement (element, ignoredMask, payload, true, stories);
                    error = ACAPI_Element_Create (&element, &memo);
                    if (error == NoError) {
                        // Do not enqueue a New event until the host undo
                        // command has committed.  A callback error rolls the
                        // batch back, and queued notifications must not leak
                        // GUIDs for elements that never survived the batch.
                        createdHeads.push_back (element.header);
                        elements (CreateElementIdObjectState (element.header.guid));
                    } else {
                        elements (CreateErrorResponse (error, "Failed to create Rich Text element."));
                    }
                }
            } else {
                elements (CreateErrorResponse (error, "Failed to get defaults for Rich Text element."));
            }
            if (error != NoError) callbackError = error;
        }
        return callbackError;
    });

    notification = {};
    notification.notifID = APINotifyElement_EndEvents;
    AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);

    if (undoError == NoError && callbackError == NoError) {
        for (const API_Elem_Head& createdHead : createdHeads) {
            notification = {};
            notification.notifID = APINotifyElement_New;
            notification.elemHead = createdHead;
            AddElementNotificationClientCommand::ElementEventHandlerProc (&notification);
        }
    }

    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
    }
    return response;
}

GS::ObjectState ExecuteTextUpdate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& /*processControl*/)
{
    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("executionResults");
    const Stories stories = GetStories ();
    GSErrCode callbackError = NoError;

    // Keep the special lane deliberately narrower than the generic Text
    // update: one item, one mapped `angle` field, and no memo/style/geometry
    // fields.  Mixed batches and content/presentation updates retain the
    // existing ACAPI_Element_Change path.
    const bool angleOnlySingleText = items.GetSize () == 1
        && IsAngleOnlyTextItem (items[0]);

    const GSErrCode undoError = ACAPI_CallUndoableCommand (
        angleOnlySingleText ? "Rotate Rich Text" : "Update Rich Text",
        [&] () -> GSErrCode {
        if (angleOnlySingleText) {
            const GS::ObjectState& item = items[0];
            const GS::ObjectState* elementID = item.Get ("elementId");
            const API_Guid guid = elementID == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementID);
            API_Element element = {};
            element.header.guid = guid;
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            if (error == NoError && GetElemTypeId (element.header) != API_TextID)
                error = APIERR_BADID;
            const short originalStoryIndex = element.header.floorInd;
            double requestedAngle = 0.0;
            if (error == NoError && !item.Get ("angle", requestedAngle))
                error = APIERR_BADPARS;
            if (error == NoError)
                error = RotateTextByDelta (element, requestedAngle);
            if (error == NoError) {
                API_Elem_Head updatedHeader = {};
                updatedHeader.guid = guid;
                error = ACAPI_Element_GetHeader (&updatedHeader);
                if (error == NoError && updatedHeader.floorInd != originalStoryIndex)
                    error = APIERR_COMMANDFAILED;
            }
            if (error == NoError)
                results (CreateSuccessfulExecutionResult ());
            else
                results (CreateFailedExecutionResult (error, "Failed to rotate Text element through the native edit lane."));
            callbackError = error;
            return callbackError;
        }

        for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
            const GS::ObjectState& item = items[index];
            if (callbackError != NoError) {
                results (CreateFailedExecutionResult (callbackError, "Text item was not attempted because the batch was rolled back."));
                continue;
            }

            const GS::ObjectState* elementID = item.Get ("elementId");
            const API_Guid guid = elementID == nullptr ? APINULLGuid : GetGuidFromObjectState (*elementID);
            API_Element element = {};
            API_Element mask = {};
            API_ElementMemo memo = {};
            NativeMemoGuard memoGuard (memo);
            element.header.guid = guid;
            GSErrCode error = guid == APINULLGuid ? APIERR_BADPARS : ACAPI_Element_Get (&element);
            const short originalStoryIndex = element.header.floorInd;
            if (error == NoError) {
                const GS::ObjectState* richText = item.Get ("richText");
                GS::UniString richTextError;
                if (richText != nullptr) {
                    error = NativeRichTextMemo::BuildMemo (*richText, memo, element.text, richTextError);
                    if (error != NoError) {
                        results (CreateFailedExecutionResult (error, richTextError.IsEmpty ()
                            ? "Failed to build Rich Text memo."
                            : richTextError));
                    }
                }
                if (error == NoError) {
                    ApplyTextPlacement (element, mask, item, false, stories);
                    if (richText != nullptr) {
                        error = ACAPI_Element_Change (&element, &mask, &memo, TextRichMemoMask, true);
                    } else {
                        error = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                    }
                    if (error == NoError) {
                        API_Elem_Head updatedHeader = {};
                        updatedHeader.guid = guid;
                        error = ACAPI_Element_GetHeader (&updatedHeader);
                        if (error == NoError && updatedHeader.floorInd != originalStoryIndex)
                            error = APIERR_COMMANDFAILED;
                    }
                    if (error == NoError)
                        results (CreateSuccessfulExecutionResult ());
                    else
                        results (CreateFailedExecutionResult (error, "Failed to update Text element."));
                }
            } else {
                results (CreateFailedExecutionResult (error, "Failed to load Rich Text element."));
            }
            if (error != NoError) callbackError = error;
        }
        return callbackError;
    });

    if (undoError != NoError) {
        response.Add ("transactionRolledBack", true);
        response.Add ("transactionError", undoError);
    }
    return response;
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
    NativeMemoGuard cleanup (memo);

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
        GS::Array<API_Guid> returnedDetailGuids;
        for (const GS::ObjectState& detailRow : detailRows) {
            const GS::ObjectState* detailElementID = detailRow.Get ("elementId");
            const GS::ObjectState* detail = detailRow.Get ("details");
            const API_Guid detailGuid = detailElementID == nullptr
                ? APINULLGuid
                : GetGuidFromObjectState (*detailElementID);
            if (detailGuid == APINULLGuid
                || !ContainsGuid (elementGuids, detailGuid)
                || ContainsGuid (returnedDetailGuids, detailGuid)
                || detail == nullptr
                || detail->Get ("error") != nullptr) {
                verified = false;
                break;
            }
            if (typeID == API_TextID) {
                GS::UniString richTextStatus;
                const GS::ObjectState* richText = detail->Get ("richText");
                if (richText == nullptr || !detail->Get ("richTextStatus", richTextStatus)
                    || richTextStatus != "available") {
                    // A structural detail row is not sufficient evidence for
                    // Text mutations: the Unicode paragraph/run memo must be
                    // present and explicitly marked available.
                    verified = false;
                    break;
                }
            }
            returnedDetailGuids.Push (detailGuid);
        }
        if (static_cast<GSSize> (returnedDetailGuids.GetSize ()) != static_cast<GSSize> (elementGuids.GetSize ())) {
            verified = false;
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
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        const bool stillExists = headerError == NoError;
        // APIERR_BADID is the documented not-found result for a deleted
        // element.  Any other error is an indeterminate readback failure,
        // not proof that the element is absent.
        const bool absent = headerError == APIERR_BADID;
        GS::ObjectState status;
        status.Add ("elementId", CreateGuidObjectState (guid));
        status.Add ("absent", absent);
        if (headerError != NoError && headerError != APIERR_BADID) {
            status.Add ("readbackErrorCode", headerError);
            verified = false;
        }
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

GS::Array<API_Guid> CollectCreatedGuids (const GS::ObjectState& mutationResult)
{
    GS::Array<API_Guid> guids;
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return guids;
    }
    GS::Array<GS::ObjectState> elements;
    if (!mutationResult.Get ("elements", elements)) {
        return guids;
    }
    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementID = element.Get ("elementId");
        if (elementID != nullptr) {
            const API_Guid guid = GetGuidFromObjectState (*elementID);
            if (guid != APINULLGuid && !ContainsGuid (guids, guid)) {
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
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return guids;
    }
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

GS::Array<API_Guid> CollectDeletedGuids (
    const GS::ObjectState& mutationResult,
    const GS::Array<API_Guid>& requestedGuids)
{
    bool success = false;
    if (mutationResult.Get ("success", success) && success) {
        return requestedGuids;
    }
    // DeleteElements returns one aggregate result for the supplied GUID list.
    // A failed aggregate must not be reported as if every requested element
    // had been deleted; the readback then remains an explicit failed/partial
    // postcondition instead of manufacturing positive evidence.
    return {};
}

bool MutationResultIsComplete (
    const GS::UniString& operation,
    const GS::ObjectState& mutationResult,
    const GSSize requestedCount)
{
    if (mutationResult.Get ("error") != nullptr) {
        return false;
    }
    bool transactionRolledBack = false;
    if (mutationResult.Get ("transactionRolledBack", transactionRolledBack) && transactionRolledBack) {
        return false;
    }
    if (mutationResult.Contains ("groupModeRestored")) {
        bool groupModeRestored = false;
        if (!mutationResult.Get ("groupModeRestored", groupModeRestored) || !groupModeRestored)
            return false;
    }
    if (mutationResult.Contains ("groupModeRestoreError")) {
        return false;
    }
    if (operation == "create") {
        GS::Array<GS::ObjectState> elements;
        if (!mutationResult.Get ("elements", elements) || static_cast<GSSize> (elements.GetSize ()) != requestedCount) {
            return false;
        }
        GS::Array<API_Guid> createdGuids;
        for (const GS::ObjectState& element : elements) {
            const GS::ObjectState* elementID = element.Get ("elementId");
            if (elementID == nullptr) {
                return false;
            }
            const API_Guid guid = GetGuidFromObjectState (*elementID);
            if (guid == APINULLGuid || ContainsGuid (createdGuids, guid)) {
                return false;
            }
            createdGuids.Push (guid);
        }
        return static_cast<GSSize> (createdGuids.GetSize ()) == requestedCount;
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
                "enum": ["Wall", "Slab", "Column", "Beam", "Text", "Line", "Arc", "Hatch", "Door"]
            },
            "nativeCandidateColumnTemplateSection": {
                "type": "object",
                "description": "Internal candidate-only factual section readback for the exact same-file Column template."
            },
            "nativeCandidateDoorFields": {
                "type": "array",
                "description": "Internal candidate authorization derived by NativeCrudCommands' exact public-to-native Door field mapping.",
                "items": { "type": "string", "enum": ["ownerWallId", "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide"] },
                "minItems": 1,
                "uniqueItems": true
            },
            "items": {
                "type": "array",
                "minItems": 1,
                "maxItems": 256,
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "cloneSourceElementId": { "$ref": "#/ElementId" },
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
                                "hatchCoordinates": {
                                    "type": "array",
                                    "items": { "$ref": "#/Coordinate2D" },
                                    "minItems": 3,
                                    "maxItems": 64
                                },
                                "floorInd": { "type": "integer" },
                                "contourPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillBackgroundPenIndex": { "type": "integer", "minimum": 1, "maximum": 32767 },
                                "fillAttributeIndex": { "type": "integer", "minimum": 1 },
                                "polygonArcs": {
                                    "type": "array",
                                    "items": { "$ref": "#/PolyArc" }
                                },
                                "holes": { "$ref": "#/Holes2D" }
                                ,"richText": {
                                    "type": "object",
                                    "description": "Versioned native Unicode Rich Text memo with paragraph and run style data.",
                                    "properties": {
                                        "schemaVersion": { "enum": ["native-rich-text-v1"] },
                                        "encoding": { "enum": ["unicode"] },
                                        "offsets": {
                                            "type": "object",
                                            "properties": {
                                                "unit": { "enum": ["archicad-unicode-character"] },
                                                "paragraph": { "enum": ["content"] },
                                                "run": { "enum": ["paragraph"] },
                                                "eol": { "enum": ["paragraph"] }
                                            },
                                            "additionalProperties": false,
                                            "required": ["unit", "paragraph", "run", "eol"]
                                        },
                                        "content": { "type": "string", "maxLength": 1000000 },
                                        "paragraphs": {
                                            "type": "array",
                                            "minItems": 1,
                                            "maxItems": 4096,
                                            "items": {
                                                "type": "object",
                                                "properties": {
                                                    "from": { "type": "integer", "minimum": 0 },
                                                    "range": { "type": "integer", "minimum": 0 },
                                                    "justification": { "type": "integer" },
                                                    "firstIndentMm": { "type": "number" },
                                                    "indentMm": { "type": "number" },
                                                    "rightIndentMm": { "type": "number" },
                                                    "spacing": { "type": "number" },
                                                    "tabs": {
                                                        "type": "array",
                                                        "maxItems": 256,
                                                        "items": {
                                                            "type": "object",
                                                            "properties": {
                                                                "type": { "type": "integer", "minimum": 0 },
                                                                "positionMm": { "type": "number" }
                                                            },
                                                            "additionalProperties": false,
                                                            "required": ["type", "positionMm"]
                                                        }
                                                    },
                                                    "runs": {
                                                        "type": "array",
                                                        "minItems": 1,
                                                        "maxItems": 4096,
                                                        "items": {
                                                            "type": "object",
                                                            "properties": {
                                                                "from": { "type": "integer", "minimum": 0 },
                                                                "range": { "type": "integer", "minimum": 0 },
                                                                "pen": { "type": "integer" },
                                                                "fontIndex": { "type": "integer" },
                                                                "sizeMm": { "type": "number" },
                                                                "faceBits": { "type": "integer", "minimum": 0 },
                                                                "effectBits": { "type": "integer", "minimum": 0 }
                                                            },
                                                            "additionalProperties": false,
                                                            "required": ["from", "range", "pen", "fontIndex", "sizeMm", "faceBits", "effectBits"]
                                                        }
                                                    },
                                                    "eolPositions": {
                                                        "type": "array",
                                                        "maxItems": 4096,
                                                        "items": { "type": "integer", "minimum": 0 }
                                                    },
                                                    "widthMm": { "type": "number", "description": "Output-only paragraph measurement; ignored on write." },
                                                    "heightMm": { "type": "number", "description": "Output-only paragraph measurement; ignored on write." }
                                                },
                                                "additionalProperties": false,
                                                "required": ["from", "range", "justification", "firstIndentMm", "indentMm", "rightIndentMm", "spacing", "tabs", "runs", "eolPositions"]
                                            }
                                        }
                                    },
                                    "additionalProperties": false,
                                    "required": ["schemaVersion", "encoding", "offsets", "content", "paragraphs"]
                                },
                                "coordinate": { "$ref": "#/Coordinate3D" },
                                "angle": { "type": "number" },
                                "anchor": { "type": "integer", "minimum": 0, "maximum": 8 },
                                "fixedSize": { "type": "boolean" },
                                "nonBreaking": { "type": "boolean" },
                                "pen": { "type": "integer" },
                                "linePen": { "type": "integer" },
                                "linePenIndex": { "type": "integer" },
                                "lineTypeIndex": { "type": "integer" },
                                "roomSeparator": { "type": "boolean" },
                                "radius": { "type": "number", "exclusiveMinimum": true },
                                "axisAngle": { "type": "number" },
                                "ratio": { "type": "number", "exclusiveMinimum": true },
                                "begAngle": { "type": "number" },
                                "beginAngle": { "type": "number" },
                                "endAngle": { "type": "number" },
                                "reflected": { "type": "boolean" }
                                ,"ownerWallId": { "$ref": "#/ElementId" }
                                ,"centerOffset": { "type": "number" }
                                ,"sillHeight": { "type": "number" }
                                ,"refSide": { "type": "boolean" }
                                ,"oSide": { "type": "boolean" }
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
            "elementType": { "type": "string", "enum": ["Wall", "Slab", "Column", "Beam", "Text", "Line", "Arc", "Hatch"] },
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
    const bool isText = typeID == API_TextID;
    bool nativeCrudAtomic = false;
    parameters.Get ("nativeCrudAtomic", nativeCrudAtomic);
    bool nativeCrudHatchWriter = false;
    parameters.Get ("nativeCrudHatchWriter", nativeCrudHatchWriter);
    bool nativeCandidateHatchWriter = false;
    const bool hasNativeCandidateHatchWriter = parameters.Get ("nativeCandidateHatchWriter", nativeCandidateHatchWriter);
    if (parameters.Contains ("nativeCandidateHatchWriter") && !hasNativeCandidateHatchWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Hatch diagnostics marker must be a boolean.");
    if (nativeCrudHatchWriter && (typeID != API_HatchID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Hatch typed writer marker is valid only for Hatch CREATE or UPDATE.");
    bool nativeCrudDoorWriter = false;
    const bool hasNativeCrudDoorWriter = parameters.Get ("nativeCrudDoorWriter", nativeCrudDoorWriter);
    if (parameters.Contains ("nativeCrudDoorWriter") && !hasNativeCrudDoorWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Door typed writer marker must be a boolean.");
    bool nativeCandidateDoorWriter = false;
    const bool hasNativeCandidateDoorWriter = parameters.Get ("nativeCandidateDoorWriter", nativeCandidateDoorWriter);
    if (parameters.Contains ("nativeCandidateDoorWriter") && !hasNativeCandidateDoorWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate writer marker must be a boolean.");
    GS::Array<GS::UniString> nativeCandidateDoorFields;
    const bool hasNativeCandidateDoorFields = parameters.Get ("nativeCandidateDoorFields", nativeCandidateDoorFields);
    if (parameters.Contains ("nativeCandidateDoorFields") && !hasNativeCandidateDoorFields)
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate native field authorization must be a string array.");
    if (nativeCrudDoorWriter && (typeID != API_DoorID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Door typed writer marker is valid only for Door CREATE or UPDATE.");
    if (typeID == API_DoorID && (operation == "create" || operation == "update")
        && !nativeCrudDoorWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Door CREATE/UPDATE requires the typed Native CRUD writer route.");
    if (nativeCandidateDoorWriter && (!nativeCrudDoorWriter || typeID != API_DoorID || operation != "create" && operation != "update"))
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Door marker is valid only for Door CREATE or UPDATE through its typed writer.");
    if (nativeCandidateDoorWriter != hasNativeCandidateDoorFields || (nativeCandidateDoorWriter && nativeCandidateDoorFields.IsEmpty ()))
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Door writes require the exact flattened native field authorization derived at the Native CRUD boundary.");
    if (!nativeCandidateDoorWriter && hasNativeCandidateDoorFields)
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate field authorization is invalid without the guarded candidate writer.");
    bool nativeCrudWindowWriter = false;
    const bool hasNativeCrudWindowWriter = parameters.Get ("nativeCrudWindowWriter", nativeCrudWindowWriter);
    if (parameters.Contains ("nativeCrudWindowWriter") && !hasNativeCrudWindowWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Window typed writer marker must be a boolean.");
    bool nativeCandidateWindowWriter = false;
    const bool hasNativeCandidateWindowWriter = parameters.Get ("nativeCandidateWindowWriter", nativeCandidateWindowWriter);
    if (parameters.Contains ("nativeCandidateWindowWriter") && !hasNativeCandidateWindowWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate writer marker must be a boolean.");
    GS::Array<GS::UniString> nativeCandidateWindowFields;
    const bool hasNativeCandidateWindowFields = parameters.Get ("nativeCandidateWindowFields", nativeCandidateWindowFields);
    if (parameters.Contains ("nativeCandidateWindowFields") && !hasNativeCandidateWindowFields)
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate native field authorization must be a string array.");
    if (nativeCrudWindowWriter && (typeID != API_WindowID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Window typed writer marker is valid only for Window CREATE or UPDATE.");
    if (typeID == API_WindowID && (operation == "create" || operation == "update") && !nativeCrudWindowWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Window CREATE/UPDATE requires its typed Native CRUD writer route.");
    if (nativeCandidateWindowWriter && (!nativeCrudWindowWriter || typeID != API_WindowID || (operation != "create" && operation != "update")))
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Window marker is valid only for Window CREATE/UPDATE through its typed writer.");
    if (nativeCandidateWindowWriter != hasNativeCandidateWindowFields || (nativeCandidateWindowWriter && nativeCandidateWindowFields.IsEmpty ()))
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Window writes require exact flattened native field authorization.");
    if (!nativeCandidateWindowWriter && hasNativeCandidateWindowFields)
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate field authorization is invalid without its guarded writer.");
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    if (hasNativeCandidateWindowWriter || hasNativeCandidateWindowFields)
        return CreateErrorResponse (APIERR_BADPARS, "Window candidate writer is unavailable in this ordinary APX build.");
#endif
    if (typeID == API_WindowID && (operation == "create" || operation == "update")) {
#if TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
        if (!nativeCandidateWindowWriter)
            return CreateErrorResponse (APIERR_BADPARS, "Window candidate build requires its exact guarded candidate writer marker.");
#else
        // Ordinary requests have already rejected candidate-only metadata;
        // they use the same typed writer and factual host/story postconditions.
#endif
    }
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    if (hasNativeCandidateDoorWriter || hasNativeCandidateDoorFields)
        return CreateErrorResponse (APIERR_BADPARS, "Door candidate writer is unavailable in this ordinary APX build.");
#endif
    if (nativeCandidateHatchWriter && (!nativeCrudHatchWriter || typeID != API_HatchID || operation != "create"))
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Hatch diagnostics are valid only for Hatch CREATE through its typed writer.");
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    if (nativeCandidateHatchWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Candidate Hatch diagnostics are unavailable in this ordinary APX build.");
#endif
    if (typeID == API_HatchID && (operation == "create" || operation == "update") && !nativeCrudHatchWriter)
        return CreateErrorResponse (APIERR_BADPARS, "Hatch CREATE/UPDATE requires the validated typed Native CRUD writer route.");
    bool nativeCrudTestFaultAfterFirstApplied = false;
    const bool hasNativeCrudTestFaultAfterFirstApplied = parameters.Get ("nativeCrudTestFaultAfterFirstApplied", nativeCrudTestFaultAfterFirstApplied);
    if (parameters.Contains ("nativeCrudTestFaultAfterFirstApplied") && !hasNativeCrudTestFaultAfterFirstApplied)
        return CreateErrorResponse (APIERR_BADPARS, "Native CRUD first-applied fault metadata must be a boolean.");
#if !defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (hasNativeCrudTestFaultAfterFirstApplied)
        return CreateErrorResponse (APIERR_BADPARS, "Native CRUD first-applied fault metadata is unavailable in this ordinary APX build.");
#else
    if (nativeCrudTestFaultAfterFirstApplied && ((typeID != API_ColumnID && typeID != API_LineID && typeID != API_ArcID && typeID != API_HatchID && typeID != API_DoorID && typeID != API_WindowID) || operation != "update"))
        return CreateErrorResponse (APIERR_BADPARS, "First-applied fault seam is valid only for Column, Line, Arc, Hatch, Door or Window UPDATE.");
#endif
    GS::ObjectState candidateColumnTemplateSection;
    const bool hasCandidateColumnTemplateSection = parameters.Get ("nativeCandidateColumnTemplateSection", candidateColumnTemplateSection);
    if (parameters.Contains ("nativeCandidateColumnTemplateSection") && !hasCandidateColumnTemplateSection)
        return CreateErrorResponse (APIERR_BADPARS, "Column candidate template section is malformed.");
    if (hasCandidateColumnTemplateSection && (operation != "create" || typeID != API_ColumnID))
        return CreateErrorResponse (APIERR_BADPARS, "Column candidate template section is valid only for Column CREATE.");
#if !TAPIR_ENABLE_NATIVE_CANDIDATE_WRITERS
    if (hasCandidateColumnTemplateSection)
        return CreateErrorResponse (APIERR_BADPARS, "Column candidate template section is unavailable in this APX build.");
#endif
    const bool isDoor = typeID == API_DoorID;
    const bool isWindow = typeID == API_WindowID;
    const ElementTypeSpec* typeSpec = (isText || isDoor || isWindow) ? nullptr : GetElementTypeSpec (typeID);
    if (!isText && !isDoor && !isWindow && typeSpec == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "'elementType' must be Wall, Slab, Column, Beam, Text, Line, Arc, Hatch or guarded Door/Window.");
    }

    GS::Array<GS::ObjectState> items;
    if (!parameters.Get ("items", items)) {
        return CreateErrorResponse (APIERR_BADPARS, "Missing required array field 'items'.");
    }
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) {
        return CreateErrorResponse (APIERR_BADPARS, "'items' must contain between 1 and 256 entries.");
    }
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
    if (nativeCrudTestFaultAfterFirstApplied && items.GetSize () != 2)
        return CreateErrorResponse (APIERR_BADPARS, "First-applied fault seam requires exactly two UPDATE items.");
#endif

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
    if (nativeCandidateDoorWriter) {
        GS::HashSet<GS::UniString> authorized;
        for (const GS::UniString& field : nativeCandidateDoorFields) {
            if (authorized.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate native field authorization contains duplicates.");
            authorized.Add (field);
        }
        for (const GS::ObjectState& item : normalizedItems) {
            // CREATE keeps its nested payload; UPDATE is flattened by
            // ValidateEnvelopeItem and carries the target GUID at top level.
            // Validate exactly the corresponding normalized writable fields.
            const GS::ObjectState* shape = operation == "create" ? item.Get ("payload") : &item;
            if (shape == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Door candidate normalized item is missing its payload/patch.");
            GS::HashSet<GS::UniString> actual;
            for (const GS::String& name : shape->GetFieldNames ()) {
                const GS::UniString field (name);
                if (operation == "create" && field == "floorIndex") continue;
                if (operation == "update" && field == "elementId") continue;
                actual.Add (field);
            }
            if (actual.GetSize () != authorized.GetSize ())
                return CreateErrorResponse (APIERR_BADPARS, "Door candidate flattened field authorization does not match the normalized native item.");
            for (const GS::UniString& field : actual)
                if (!authorized.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Door candidate flattened field authorization does not match the normalized native item.");
        }
    }
    if (nativeCandidateWindowWriter) {
        GS::HashSet<GS::UniString> authorized;
        const std::initializer_list<const char*> allowedFields = { "ownerWallId", "centerOffset", "sillHeight", "width", "height", "reflected", "refSide", "oSide" };
        for (const GS::UniString& field : nativeCandidateWindowFields) {
            if (authorized.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate native field authorization contains duplicates.");
            bool allowed = false;
            for (const char* allowedField : allowedFields) allowed = allowed || field == allowedField;
            if (!allowed) return CreateErrorResponse (APIERR_BADPARS, "Window candidate native field authorization contains an unsupported field.");
            authorized.Add (field);
        }
        for (const GS::ObjectState& item : normalizedItems) {
            const GS::ObjectState* shape = operation == "create" ? item.Get ("payload") : &item;
            if (shape == nullptr) return CreateErrorResponse (APIERR_BADPARS, "Window candidate normalized item is missing its payload/patch.");
            GS::HashSet<GS::UniString> actual;
            for (const GS::String& name : shape->GetFieldNames ()) {
                const GS::UniString field (name);
                if (operation == "create" && field == "floorIndex") continue;
                if (operation == "update" && field == "elementId") continue;
                actual.Add (field);
            }
            if (actual.GetSize () != authorized.GetSize ()) return CreateErrorResponse (APIERR_BADPARS, "Window candidate flattened field authorization differs from its normalized native item.");
            for (const GS::UniString& field : actual) if (!authorized.Contains (field)) return CreateErrorResponse (APIERR_BADPARS, "Window candidate flattened field authorization differs from its normalized native item.");
        }
    }

    // Validate the exact angle-only target before touching the operator's
    // Suspend Groups mode.  The edit lane repeats this check after entering
    // the undoable command because the native element must still be fresh at
    // the actual edit boundary.
    if (isText && operation == "update" && normalizedItems.GetSize () == 1
        && IsAngleOnlyTextItem (normalizedItems[0])) {
        const GSErrCode rotationPreflightError = ValidateAngleOnlyTextItem (normalizedItems[0]);
        if (rotationPreflightError != NoError)
            return CreateErrorResponse (rotationPreflightError, "Text angle-only update target is fixed, linked, grouped, or unavailable.");
    }

    // Archicad rejects exact changes to members of a group while Suspend
    // Groups is off.  Mutations are already serialized on Archicad's main
    // thread, so temporarily suspend groups for this synchronous command and
    // restore the operator's prior UI mode on every exit path.  This changes
    // neither group membership nor the undo payload.
    bool groupModeNeedsRestore = false;
    if (operation != "create") {
        bool groupsWereSuspended = false;
        GSErrCode groupModeError = TAPIR_View_IsSuspendGroupOn (&groupsWereSuspended);
        if (groupModeError != NoError)
            return CreateErrorResponse (groupModeError, "Failed to read the Suspend Groups mode.");
        groupModeNeedsRestore = !groupsWereSuspended;
        if (groupModeNeedsRestore) {
            groupModeError = TAPIR_Grouping_ChangeSuspendGroup (true);
            if (groupModeError != NoError)
                return CreateErrorResponse (groupModeError, "Failed to suspend groups for exact element mutation.");
        }
    }
    const GS::OnExit groupModeGuard ([&groupModeNeedsRestore] () {
        if (groupModeNeedsRestore)
            TAPIR_Grouping_ChangeSuspendGroup (false);
    });

    GS::ObjectState mutationParameters;
    if ((isDoor || isWindow) && operation == "create") {
        mutationParameters = BuildArrayParameters ("items", normalizedItems);
        if (nativeCandidateDoorWriter) {
            mutationParameters.Add ("nativeCandidateDoorWriter", true);
            mutationParameters.Add ("nativeCandidateDoorFields", nativeCandidateDoorFields);
        }
        if (nativeCandidateWindowWriter) {
            mutationParameters.Add ("nativeCandidateWindowWriter", true);
            mutationParameters.Add ("nativeCandidateWindowFields", nativeCandidateWindowFields);
        }
    } else if (!isText && !isDoor && !isWindow && operation == "create") {
        mutationParameters = BuildArrayParameters (typeSpec->createArrayName, normalizedItems);
        if (hasCandidateColumnTemplateSection)
            mutationParameters.Add ("nativeCandidateColumnTemplateSection", candidateColumnTemplateSection);
        if (nativeCrudHatchWriter)
            mutationParameters.Add ("nativeCrudHatchWriter", true);
        if (nativeCandidateHatchWriter)
            mutationParameters.Add ("nativeCandidateHatchWriter", true);
    } else if ((isDoor || isWindow) && operation == "update") {
        mutationParameters = BuildArrayParameters ("items", normalizedItems);
        mutationParameters.Add ("nativeCrudAtomic", true);
        if (nativeCandidateDoorWriter) {
            mutationParameters.Add ("nativeCandidateDoorWriter", true);
            mutationParameters.Add ("nativeCandidateDoorFields", nativeCandidateDoorFields);
        }
        if (nativeCandidateWindowWriter) {
            mutationParameters.Add ("nativeCandidateWindowWriter", true);
            mutationParameters.Add ("nativeCandidateWindowFields", nativeCandidateWindowFields);
        }
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        if (nativeCrudTestFaultAfterFirstApplied) mutationParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
#endif
    } else if (!isText && !isDoor && !isWindow && operation == "update") {
        mutationParameters = BuildArrayParameters (typeSpec->updateArrayName, normalizedItems);
        if (nativeCrudAtomic)
            mutationParameters.Add ("nativeCrudAtomic", true);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        // This top-level internal key is not part of the MutateElements input
        // schema. NativeCrudCommands adds it only after exact candidate
        // manifest validation; copy it solely into existing atomic typed
        // executors that implement the first-applied rollback seam.
        if ((typeID == API_ColumnID || typeID == API_LineID || typeID == API_ArcID || typeID == API_HatchID) && nativeCrudTestFaultAfterFirstApplied)
            mutationParameters.Add ("nativeCrudTestFaultAfterFirstApplied", true);
#endif
    }

    GS::ObjectState mutationResult;
    if (isText && operation == "create") {
        mutationResult = ExecuteTextCreate (normalizedItems, processControl);
    } else if (isText && operation == "update") {
        mutationResult = ExecuteTextUpdate (normalizedItems, processControl);
    } else if (isWindow && operation == "create") {
        mutationResult = ExecuteWindowCreate (normalizedItems, processControl);
    } else if (isWindow && operation == "update") {
        mutationResult = ExecuteWindowUpdate (normalizedItems, nativeCrudTestFaultAfterFirstApplied, processControl);
    } else if (isDoor && operation == "create") {
        mutationResult = ExecuteDoorCreate (normalizedItems, processControl);
    } else if (isDoor && operation == "update") {
        mutationResult = ExecuteDoorUpdate (normalizedItems, nativeCrudTestFaultAfterFirstApplied, processControl);
    } else if (operation == "create") {
        mutationResult = ExecuteCreate (typeID, mutationParameters, processControl);
    } else if (operation == "update") {
        mutationResult = ExecuteUpdate (typeID, mutationParameters, processControl);
    } else {
        mutationResult = ExecuteDelete (normalizedItems, processControl);
    }

    // Restore the operator's UI mode before producing the response so a
    // failure is factual and observable.  The OnExit guard remains armed as
    // one final best-effort retry, but the element mutation is never retried.
    GSErrCode groupModeRestoreError = NoError;
    if (groupModeNeedsRestore) {
        groupModeRestoreError = TAPIR_Grouping_ChangeSuspendGroup (false);
        if (groupModeRestoreError == NoError)
            groupModeNeedsRestore = false;
    }
    mutationResult.Add ("groupModeRestored", groupModeRestoreError == NoError);
    if (groupModeRestoreError != NoError)
        mutationResult.Add ("groupModeRestoreError", groupModeRestoreError);

    GS::Array<API_Guid> changedGuids;
    if (operation == "create") {
        changedGuids = CollectCreatedGuids (mutationResult);
    } else if (operation == "update") {
        changedGuids = CollectUpdatedGuids (mutationResult, requestedElementGuids);
    } else {
        changedGuids = CollectDeletedGuids (mutationResult, requestedElementGuids);
    }

    const bool mutationComplete = MutationResultIsComplete (operation, mutationResult, items.GetSize ());
    // A partial update must still expose the observed state of every requested
    // element, including items whose delegated executor reported failure.  A
    // readback of successful items only can make a failed scalar/memo update
    // look as if the element had never been part of the operation.
    GS::Array<API_Guid> readbackGuids = changedGuids;
    if ((operation == "update" || operation == "delete") && !mutationComplete) {
        readbackGuids = requestedElementGuids;
    }

    // An empty readback cannot prove a non-empty request.  In particular, a
    // failed create may yield zero changed GUIDs; reporting verified=true in
    // that case would turn a missing postcondition into green evidence.
    bool readbackVerified = items.IsEmpty () || !readbackGuids.IsEmpty ();
    GSSize appliedCount = static_cast<GSSize> (changedGuids.GetSize ());
    GS::ObjectState readback;
    if (operation == "delete") {
        GSSize absentCount = 0;
        readback = BuildDeleteReadback (readbackGuids, typeID, readbackVerified, absentCount);
        // DeleteElements exposes one aggregate host result, so a failed
        // aggregate cannot identify applied items on its own.  The immediate
        // per-GUID readback is the authoritative evidence: count only GUIDs
        // whose absence was explicitly confirmed with APIERR_BADID.
        appliedCount = absentCount;
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
