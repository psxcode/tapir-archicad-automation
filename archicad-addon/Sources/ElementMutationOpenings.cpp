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
#include <limits>

namespace {

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

// A created opening's lengths are recomputed by the host (live AC28: a
// requested 0.2 m sill read back as 0.20000000000000018).  CREATE postconditions
// compare them within the 1e-9 m Tapir readback resolution, far below the 1 mm
// drafting defect (D-293), instead of bit-exact (V1-022).
constexpr double OpeningCreateLengthTolerance = 1.0e-9;

bool SameOpeningCreateLength (const double actual, const double requested)
{
    return std::fabs (actual - requested) <= OpeningCreateLengthTolerance;
}

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

} // namespace

namespace ElementMutationInternal {

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
            if (error == NoError) {
                // Name the first field that did not hold, with the created
                // value, so the factual history row says why (V1-022).
                GS::UniString mismatch;
                const auto differs = [&] (const char* field, double actual, double requested = 0.0) { if (mismatch.IsEmpty ()) mismatch = GS::UniString::Printf (":%s=%.17g/requested=%.17g", field, actual, requested); };
                if (created.header.guid == APINULLGuid || created.header.guid == sourceGuid) differs ("guid", 0.0);
                if (created.header.floorInd != static_cast<short> (storyIndex)) differs ("floorIndex", created.header.floorInd);
                if (created.door.owner != hostGuid) differs ("owner", 0.0);
                if (!SameOpeningCreateLength (created.door.openingBase.width, element.door.openingBase.width)) differs ("width", created.door.openingBase.width, element.door.openingBase.width);
                if (!SameOpeningCreateLength (created.door.openingBase.height, element.door.openingBase.height)) differs ("height", created.door.openingBase.height, element.door.openingBase.height);
                if (!SameOpeningCreateLength (created.door.objLoc, element.door.objLoc)) differs ("centerOffset", created.door.objLoc, element.door.objLoc);
                if (!SameOpeningCreateLength (created.door.lower, element.door.lower)) differs ("sillHeight", created.door.lower, element.door.lower);
                if (created.door.openingBase.reflected != element.door.openingBase.reflected) differs ("reflected", created.door.openingBase.reflected);
                if (created.door.openingBase.refSide != element.door.openingBase.refSide) differs ("refSide", created.door.openingBase.refSide);
                if (created.door.openingBase.oSide != element.door.openingBase.oSide) differs ("oSide", created.door.openingBase.oSide);
                if (created.door.openingBase.libInd != source.door.openingBase.libInd) differs ("libInd", created.door.openingBase.libInd);
                if (created.door.openingBase.markGuid != APINULLGuid) differs ("markGuid", 0.0);
                if (!mismatch.IsEmpty ()) {
                    failureStage += mismatch;
                    error = APIERR_COMMANDFAILED;
                }
            }
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
            if (error == NoError) {
                // Name the first field that did not hold, as for Door (V1-022).
                GS::UniString mismatch;
                const auto differs = [&] (const char* field, double actual, double requested = 0.0) { if (mismatch.IsEmpty ()) mismatch = GS::UniString::Printf (":%s=%.17g/requested=%.17g", field, actual, requested); };
                if (after.header.guid == APINULLGuid || after.header.guid == sourceGuid) differs ("guid", 0.0);
                if (after.header.floorInd != static_cast<short> (storyIndex)) differs ("floorIndex", after.header.floorInd);
                if (after.window.owner != hostGuid) differs ("owner", 0.0);
                if (after.window.openingBase.markGuid != APINULLGuid) differs ("markGuid", 0.0);
                if (after.window.openingBase.libInd != source.window.openingBase.libInd) differs ("libInd", after.window.openingBase.libInd);
                if (!WindowNativeTopologySupported (after)) differs ("topology", 0.0);
                if (!SameOpeningCreateLength (after.window.objLoc, element.window.objLoc)) differs ("centerOffset", after.window.objLoc, element.window.objLoc);
                if (!SameOpeningCreateLength (after.window.lower, element.window.lower)) differs ("sillHeight", after.window.lower, element.window.lower);
                if (!SameOpeningCreateLength (after.window.openingBase.width, element.window.openingBase.width)) differs ("width", after.window.openingBase.width, element.window.openingBase.width);
                if (!SameOpeningCreateLength (after.window.openingBase.height, element.window.openingBase.height)) differs ("height", after.window.openingBase.height, element.window.openingBase.height);
                if (after.window.openingBase.reflected != element.window.openingBase.reflected) differs ("reflected", after.window.openingBase.reflected);
                if (after.window.openingBase.refSide != element.window.openingBase.refSide) differs ("refSide", after.window.openingBase.refSide);
                if (after.window.openingBase.oSide != element.window.openingBase.oSide) differs ("oSide", after.window.openingBase.oSide);
                if (!mismatch.IsEmpty ()) {
                    failureStage += mismatch;
                    error = APIERR_COMMANDFAILED;
                }
            }
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

} // namespace ElementMutationInternal
