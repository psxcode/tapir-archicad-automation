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
#include <vector>

namespace {

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

} // namespace

namespace ElementMutationInternal {

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

} // namespace ElementMutationInternal
