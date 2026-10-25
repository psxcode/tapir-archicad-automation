#include "ProjectCommands.hpp"
#include "NativeOwnership.hpp"
#include "MigrationHelper.hpp"

#include <cmath>

namespace {

static GS::UniString SelectionTypeToString (API_SelTypeID type)
{
    switch (type) {
        case API_SelElems:       return "elements";
        case API_MarqueePoly:    return "marquee_polygon";
        case API_MarqueeHorBox:  return "marquee_horizontal_box";
        case API_MarqueeRotBox:  return "marquee_rotated_box";
        default:                 return "empty";
    }
}

static bool IsMarqueeSelection (API_SelTypeID type)
{
    return type == API_MarqueePoly || type == API_MarqueeHorBox || type == API_MarqueeRotBox;
}

static void ReleaseSelectionInfo (API_SelectionInfo& selection)
{
    ReleaseSelectionInfoHandles (selection);
}

static GS::ObjectState BoundsObject (double xMin, double yMin, double xMax, double yMax)
{
    return GS::ObjectState (
        "xMin", xMin,
        "yMin", yMin,
        "xMax", xMax,
        "yMax", yMax);
}

static GS::ObjectState FocusObject (const API_SelectionInfo& selection, GSErrCode selectionErr)
{
    const API_SelTypeID type = selectionErr == APIERR_NOSEL ? API_SelEmpty : selection.typeID;
    GS::ObjectState focus;
    focus.Add ("selectionType", SelectionTypeToString (type));
    focus.Add ("selectedElementCount", selectionErr == APIERR_NOSEL ? 0 : selection.sel_nElem);
    if (IsMarqueeSelection (type)) {
        GS::ObjectState marquee;
        marquee.Add ("shape", SelectionTypeToString (type));
        marquee.Add ("multiStory", selection.multiStory);
        marquee.Add ("rotation", selection.marquee.boxRotAngle);
        marquee.Add ("bounds", BoundsObject (
            selection.marquee.box.xMin,
            selection.marquee.box.yMin,
            selection.marquee.box.xMax,
            selection.marquee.box.yMax));
        focus.Add ("marquee", marquee);
    }
    return focus;
}

} // namespace

SetMarqueeCommand::SetMarqueeCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SetMarqueeCommand::GetName () const
{
    return "SetMarquee";
}

GS::Optional<GS::UniString> SetMarqueeCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "bounds": {
                "type": "object",
                "properties": {
                    "xMin": { "type": "number" },
                    "yMin": { "type": "number" },
                    "xMax": { "type": "number" },
                    "yMax": { "type": "number" }
                },
                "additionalProperties": false,
                "required": ["xMin", "yMin", "xMax", "yMax"]
            },
            "multiStory": {
                "type": "boolean",
                "description": "Keep the marquee membership across stories. This does not change the current story."
            }
        },
        "additionalProperties": false,
        "required": ["bounds"]
    })";
}

GS::Optional<GS::UniString> SetMarqueeCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "status": { "type": "string", "enum": ["set", "set_unverified"] },
            "verificationStatus": { "type": "string", "enum": ["verified", "unavailable", "mismatch"] },
            "requested": {
                "type": "object",
                "properties": {
                    "multiStory": { "type": "boolean" },
                    "bounds": {
                        "type": "object",
                        "properties": {
                            "xMin": { "type": "number" },
                            "yMin": { "type": "number" },
                            "xMax": { "type": "number" },
                            "yMax": { "type": "number" }
                        },
                        "additionalProperties": false,
                        "required": ["xMin", "yMin", "xMax", "yMax"]
                    }
                },
                "additionalProperties": false,
                "required": ["multiStory", "bounds"]
            },
            "previousFocus": { "type": "object" },
            "currentFocus": { "type": "object" }
        },
        "additionalProperties": false,
        "required": ["status", "verificationStatus", "requested", "previousFocus", "currentFocus"]
    })";
}

GS::ObjectState SetMarqueeCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* bounds = parameters.Get ("bounds");
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    if (bounds == nullptr
        || !bounds->Get ("xMin", xMin)
        || !bounds->Get ("yMin", yMin)
        || !bounds->Get ("xMax", xMax)
        || !bounds->Get ("yMax", yMax)
        || !std::isfinite (xMin) || !std::isfinite (yMin)
        || !std::isfinite (xMax) || !std::isfinite (yMax)
        || !(xMin < xMax && yMin < yMax)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "bounds must contain finite coordinates with positive area.");
    }

    bool multiStory = false;
    parameters.Get ("multiStory", multiStory);

    API_SelectionInfo previousSelection = {};
    const GSErrCode previousErr = ACAPI_Selection_Get (&previousSelection, nullptr, false);
    const GS::OnExit previousSelectionGuard ([&previousSelection] () { ReleaseSelectionInfo (previousSelection); });
    if (previousErr != NoError && previousErr != APIERR_NOSEL) {
        return CreateErrorResponse (previousErr, "Failed to inspect the operator focus before setting the marquee.");
    }
    if (previousErr == NoError && previousSelection.typeID == API_SelElems) {
        return CreateFailedExecutionResult (
            APIERR_REFUSEDCMD,
            "SetMarquee refuses to replace an active individual element selection. Clear the selection first (DeselectAllElements).");
    }

    API_SelectionInfo requested = {};
    requested.typeID = API_MarqueeHorBox;
    requested.multiStory = multiStory;
    requested.marquee.box.xMin = xMin;
    requested.marquee.box.yMin = yMin;
    requested.marquee.box.xMax = xMax;
    requested.marquee.box.yMax = yMax;
    requested.marquee.boxRotAngle = 0.0;

    const GSErrCode setErr = ACAPI_Selection_SetMarquee (&requested);
    if (setErr != NoError) {
        return CreateErrorResponse (setErr, "Failed to set the persistent marquee.");
    }

    API_SelectionInfo currentSelection = {};
    const GSErrCode currentErr = ACAPI_Selection_Get (&currentSelection, nullptr, false);
    const GS::OnExit currentSelectionGuard ([&currentSelection] () { ReleaseSelectionInfo (currentSelection); });
    const bool verified = currentErr == NoError
        && currentSelection.typeID == API_MarqueeHorBox
        && std::abs (currentSelection.marquee.box.xMin - xMin) <= 1.0e-9
        && std::abs (currentSelection.marquee.box.yMin - yMin) <= 1.0e-9
        && std::abs (currentSelection.marquee.box.xMax - xMax) <= 1.0e-9
        && std::abs (currentSelection.marquee.box.yMax - yMax) <= 1.0e-9
        && currentSelection.multiStory == multiStory;
    const bool verificationUnavailable = currentErr != NoError && currentErr != APIERR_NOSEL;

    GS::ObjectState response;
    response.Add ("status", verified ? "set" : "set_unverified");
    response.Add ("verificationStatus", verified ? "verified" : verificationUnavailable ? "unavailable" : "mismatch");
    response.Add ("requested", GS::ObjectState (
        "multiStory", multiStory,
        "bounds", BoundsObject (xMin, yMin, xMax, yMax)));
    response.Add ("previousFocus", FocusObject (previousSelection, previousErr));
    response.Add ("currentFocus", FocusObject (currentSelection, currentErr));

    return response;
}

namespace {

static const char* FocusChangeResponseSchema = R"({
        "type": "object",
        "properties": {
            "status": { "type": "string", "enum": ["cleared", "already_clear"] },
            "verificationStatus": { "type": "string", "enum": ["verified", "unavailable", "mismatch"] },
            "previousFocus": { "type": "object" },
            "currentFocus": { "type": "object" }
        },
        "additionalProperties": false,
        "required": ["status", "verificationStatus", "previousFocus", "currentFocus"]
    })";

static const char* EmptyInputSchema = R"({
        "type": "object",
        "properties": {},
        "additionalProperties": false,
        "required": []
    })";

// Reads the focus again after a clear and reports it next to the focus before.
static GS::ObjectState FocusChangeResponse (
    bool changed,
    const API_SelectionInfo& previousSelection,
    GSErrCode previousErr,
    bool (*isCleared) (const API_SelectionInfo&, GSErrCode))
{
    API_SelectionInfo currentSelection = {};
    const GSErrCode currentErr = ACAPI_Selection_Get (&currentSelection, nullptr, false);
    const GS::OnExit currentSelectionGuard ([&currentSelection] () { ReleaseSelectionInfo (currentSelection); });
    const bool verificationUnavailable = currentErr != NoError && currentErr != APIERR_NOSEL;

    GS::ObjectState response;
    response.Add ("status", changed ? "cleared" : "already_clear");
    response.Add ("verificationStatus",
        verificationUnavailable ? "unavailable" : isCleared (currentSelection, currentErr) ? "verified" : "mismatch");
    response.Add ("previousFocus", FocusObject (previousSelection, previousErr));
    response.Add ("currentFocus", FocusObject (currentSelection, currentErr));
    return response;
}

static bool HasNoMarquee (const API_SelectionInfo& selection, GSErrCode selectionErr)
{
    return selectionErr == APIERR_NOSEL || !IsMarqueeSelection (selection.typeID);
}

static bool HasNoElementSelection (const API_SelectionInfo& selection, GSErrCode selectionErr)
{
    return selectionErr == APIERR_NOSEL || selection.typeID != API_SelElems || selection.sel_nElem == 0;
}

} // namespace

ClearMarqueeCommand::ClearMarqueeCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ClearMarqueeCommand::GetName () const
{
    return "ClearMarquee";
}

GS::Optional<GS::UniString> ClearMarqueeCommand::GetInputParametersSchema () const
{
    return GS::UniString (EmptyInputSchema);
}

GS::Optional<GS::UniString> ClearMarqueeCommand::GetResponseSchema () const
{
    return GS::UniString (FocusChangeResponseSchema);
}

GS::ObjectState ClearMarqueeCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_SelectionInfo previousSelection = {};
    const GSErrCode previousErr = ACAPI_Selection_Get (&previousSelection, nullptr, false);
    const GS::OnExit previousSelectionGuard ([&previousSelection] () { ReleaseSelectionInfo (previousSelection); });
    if (previousErr != NoError && previousErr != APIERR_NOSEL) {
        return CreateErrorResponse (previousErr, "Failed to inspect the operator focus before clearing the marquee.");
    }
    if (HasNoMarquee (previousSelection, previousErr)) {
        return FocusChangeResponse (false, previousSelection, previousErr, HasNoMarquee);
    }

    API_SelectionInfo emptySelection = {};
    emptySelection.typeID = API_SelEmpty;
    const GSErrCode clearErr = ACAPI_Selection_SetMarquee (&emptySelection);
    if (clearErr != NoError) {
        return CreateErrorResponse (clearErr, "Failed to clear the marquee.");
    }
    return FocusChangeResponse (true, previousSelection, previousErr, HasNoMarquee);
}

DeselectAllElementsCommand::DeselectAllElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String DeselectAllElementsCommand::GetName () const
{
    return "DeselectAllElements";
}

GS::Optional<GS::UniString> DeselectAllElementsCommand::GetInputParametersSchema () const
{
    return GS::UniString (EmptyInputSchema);
}

GS::Optional<GS::UniString> DeselectAllElementsCommand::GetResponseSchema () const
{
    return GS::UniString (FocusChangeResponseSchema);
}

GS::ObjectState DeselectAllElementsCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_SelectionInfo previousSelection = {};
    const GSErrCode previousErr = ACAPI_Selection_Get (&previousSelection, nullptr, false);
    const GS::OnExit previousSelectionGuard ([&previousSelection] () { ReleaseSelectionInfo (previousSelection); });
    if (previousErr != NoError && previousErr != APIERR_NOSEL) {
        return CreateErrorResponse (previousErr, "Failed to inspect the operator focus before clearing the selection.");
    }
    // A running marquee is not an individual selection: leave it to ClearMarquee.
    if (HasNoElementSelection (previousSelection, previousErr)) {
        return FocusChangeResponse (false, previousSelection, previousErr, HasNoElementSelection);
    }

    const GSErrCode deselectErr = ACAPI_Selection_DeselectAll ();
    if (deselectErr != NoError) {
        return CreateErrorResponse (deselectErr, "Failed to clear the element selection.");
    }
    return FocusChangeResponse (true, previousSelection, previousErr, HasNoElementSelection);
}
