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
#include <vector>

namespace {

constexpr UInt64 TextRichMemoMask = APIMemoMask_TextContentUni | APIMemoMask_ParagraphUni;

// A Rich Text element draws with its run pens: API_TextType::pen alone is
// overridden by the paragraph memo, so a pen-only UPDATE used to report
// success while the visible pen stayed unchanged (V1-021).  Retarget every
// run of the exact current memo; content, fonts and sizes stay as they are
// (D-283 style-only rule).  Returns false when the memo has no paragraph
// handle, in which case the element pen is the only pen to change.
static bool SetAllRunPens (API_ElementMemo& memo, const short pen)
{
    if (memo.paragraphs == nullptr || *memo.paragraphs == nullptr)
        return false;
    const GSSize paragraphCount = BMhGetSize (reinterpret_cast<GSHandle> (memo.paragraphs)) / sizeof (API_ParagraphType);
    bool changed = false;
    for (GSSize paragraphIndex = 0; paragraphIndex < paragraphCount; ++paragraphIndex) {
        API_ParagraphType& paragraph = (*memo.paragraphs)[paragraphIndex];
        if (paragraph.run == nullptr) continue;
        const GSSize runCount = BMGetPtrSize (reinterpret_cast<GSPtr> (paragraph.run)) / sizeof (API_RunType);
        for (GSSize runIndex = 0; runIndex < runCount; ++runIndex) {
            paragraph.run[runIndex].pen = pen;
            changed = true;
        }
    }
    return changed;
}

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

} // namespace

namespace ElementMutationInternal {

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

} // namespace ElementMutationInternal

namespace {

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

} // namespace

namespace ElementMutationInternal {

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

} // namespace ElementMutationInternal

namespace {

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

} // namespace

namespace ElementMutationInternal {

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
                Int32 requestedPen = 0;
                bool penThroughMemo = false;
                if (error == NoError && richText == nullptr && item.Get ("pen", requestedPen)) {
                    error = ACAPI_Element_GetMemo (guid, &memo, TextRichMemoMask);
                    if (error == NoError)
                        penThroughMemo = SetAllRunPens (memo, static_cast<short> (requestedPen));
                    else
                        results (CreateFailedExecutionResult (error, "Failed to read the Rich Text memo for a pen UPDATE."));
                }
                if (error == NoError) {
                    ApplyTextPlacement (element, mask, item, false, stories);
                    if (richText != nullptr || penThroughMemo) {
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

} // namespace ElementMutationInternal
