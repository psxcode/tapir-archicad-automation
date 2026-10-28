#include "HistorySnapshotCommands.hpp"
#include "MigrationHelper.hpp"
#include "File.hpp"

#include <chrono>

namespace {

double ElapsedMs (const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
}

// Module files are written only to a new local path; an existing file is never replaced.
GS::Optional<GS::ObjectState> CheckNewModuleLocation (const GS::UniString& path, IO::Location& location, bool mustExist)
{
    location = IO::Location (path);
    if (path.IsEmpty () || location.GetStatus () != NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "path is not a valid filesystem location");
    }
    IO::Name lastLocalName;
    if (location.GetLastLocalName (&lastLocalName) != NoError || lastLocalName.GetExtension ().Compare ("mod", CaseInsensitive) != GS::UniString::Equal) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "path must be a local .mod file");
    }
    const bool exists = IO::File (location, IO::File::OnNotFound::Fail).GetStatus () == NoError;
    if (exists != mustExist) {
        return CreateFailedExecutionResult (APIERR_BADPARS, mustExist ? "module file does not exist" : "Refusing to overwrite an existing module file");
    }
    return GS::NoValue;
}

}

SaveElementsAsModuleCommand::SaveElementsAsModuleCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SaveElementsAsModuleCommand::GetName () const
{
    return "SaveElementsAsModule";
}

GS::Optional<GS::UniString> SaveElementsAsModuleCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": { "$ref": "#/Elements" },
            "outputPath": { "type": "string", "minLength": 1, "description": "A new local .mod path; existing files are never overwritten." },
            "storyIndex": { "type": "integer", "description": "Story shown while saving (2D drafting is saved only from the shown story); the previous story is shown again afterwards." }
        },
        "additionalProperties": false,
        "required": ["elements", "outputPath"]
    })";
}

GS::Optional<GS::UniString> SaveElementsAsModuleCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "success": { "type": "boolean" },
            "savedCount": { "type": "integer" },
            "missing": { "type": "array" },
            "databaseSwitched": { "type": "boolean" },
            "storyBefore": { "type": "integer" },
            "userOrigin": { "type": "object" },
            "elapsedMs": { "type": "number" }
        }
    })";
}

GS::ObjectState SaveElementsAsModuleCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    GS::UniString outputPath;
    parameters.Get ("elements", elements);
    parameters.Get ("outputPath", outputPath);

    IO::Location location;
    const GS::Optional<GS::ObjectState> refusal = CheckNewModuleLocation (outputPath, location, false);
    if (refusal.HasValue ()) {
        return refusal.Get ();
    }

    GS::Array<API_Elem_Head> heads;
    GS::ObjectState response = CreateSuccessfulExecutionResult ();
    const auto& missing = response.AddList<GS::ObjectState> ("missing");
    for (const GS::ObjectState& item : elements) {
        const API_Guid guid = GetGuidFromElementsArrayItem (item);
        API_Elem_Head head = {};
        if (LoadElementHeaderByGuid (guid, head)) {
            heads.Push (head);
        } else {
            missing (CreateGuidObjectState (guid));
        }
    }
    if (heads.IsEmpty ()) {
        return CreateFailedExecutionResult (APIERR_BADINDEX, "None of the elements exist");
    }

    // The module is written from the floor plan database (elements of every story live there).
    API_DatabaseInfo startingDatabase = {};
    GSErrCode err = ACAPI_Database_GetCurrentDatabase (&startingDatabase);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to read the current database");
    }
    const bool switchDatabase = startingDatabase.typeID != APIWind_FloorPlanID;
    if (switchDatabase) {
        API_DatabaseInfo floorPlan = {};
        floorPlan.typeID = APIWind_FloorPlanID;
        err = ACAPI_Database_ChangeCurrentDatabase (&floorPlan);
        if (err != NoError) {
            return CreateFailedExecutionResult (err, "Failed to switch to the floor plan database");
        }
    }

    // 2D drafting goes into the module only from the story on screen.
    Int32 storyIndex = 0;
    const bool storyGiven = parameters.Get ("storyIndex", storyIndex);
    API_StoryInfo storyInfo = {};
    short storyBefore = 0;
    if (ACAPI_ProjectSetting_GetStorySettings (&storyInfo) == NoError) {
        storyBefore = storyInfo.actStory;
        BMKillHandle (reinterpret_cast<GSHandle*> (&storyInfo.data));
    }
    const bool switchStory = storyGiven && storyIndex != storyBefore;
    if (switchStory) {
        API_StoryCmdType goTo = {};
        goTo.action = APIStory_GoTo;
        goTo.index = static_cast<short> (storyIndex);
        err = ACAPI_ProjectSetting_ChangeStorySettings (&goTo);
        if (err != NoError) {
            if (switchDatabase) ACAPI_Database_ChangeCurrentDatabase (&startingDatabase);
            return CreateFailedExecutionResult (err, "Failed to show the story of the elements");
        }
    }
    API_Coord3D userOrigin = {};
#ifdef ServerMainVers_2800
    ACAPI_Database_GetLocOrigo (&userOrigin);
#else
    ACAPI_Database (APIDb_GetLocOrigoID, &userOrigin);
#endif

    const auto start = std::chrono::steady_clock::now ();
#ifdef ServerMainVers_2800
    err = ACAPI_ProjectOperation_SaveAsModuleFile (&location, &heads);
#else
    API_Elem_Head** headHandle = reinterpret_cast<API_Elem_Head**> (BMhAllClear (heads.GetSize () * sizeof (API_Elem_Head)));
    if (headHandle == nullptr) {
        err = APIERR_MEMFULL;
    } else {
        for (UIndex i = 0; i < heads.GetSize (); ++i) {
            (*headHandle)[i] = heads[i];
        }
        err = ACAPI_Automate (APIDo_SaveAsModuleFileID, &location, headHandle);
        BMhKill (reinterpret_cast<GSHandle*> (&headHandle));
    }
#endif
    const double elapsedMs = ElapsedMs (start);

    if (switchStory) {
        API_StoryCmdType goBack = {};
        goBack.action = APIStory_GoTo;
        goBack.index = storyBefore;
        ACAPI_ProjectSetting_ChangeStorySettings (&goBack);
    }
    if (switchDatabase) {
        ACAPI_Database_ChangeCurrentDatabase (&startingDatabase);
    }
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to save the elements as a module file");
    }
    response.Add ("savedCount", static_cast<Int32> (heads.GetSize ()));
    response.Add ("databaseSwitched", switchDatabase);
    response.Add ("storyBefore", static_cast<Int32> (storyBefore));
    response.Add ("userOrigin", GS::ObjectState ("x", userOrigin.x, "y", userOrigin.y, "z", userOrigin.z));
    response.Add ("elapsedMs", elapsedMs);
    return response;
}

PlaceModuleAsElementsCommand::PlaceModuleAsElementsCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String PlaceModuleAsElementsCommand::GetName () const
{
    return "PlaceModuleAsElements";
}

GS::Optional<GS::UniString> PlaceModuleAsElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "modulePath": { "type": "string", "minLength": 1 },
            "floorInd": { "type": "integer", "description": "Story of the placed hotlink instance." },
            "refFloorInd": { "type": "integer", "description": "Source story of the module that lands on floorInd." },
            "allStories": { "type": "boolean", "description": "Multi-story (true, default) or single-story hotlink." },
            "breakLink": { "type": "boolean", "description": "Break the hotlink node so the content becomes ordinary elements (default true)." },
            "offset": { "type": "object", "description": "Translation of the placed instance {x, y, z} (default 0)." }
        },
        "additionalProperties": false,
        "required": ["modulePath", "floorInd", "refFloorInd"]
    })";
}

GS::Optional<GS::UniString> PlaceModuleAsElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "success": { "type": "boolean" },
            "hotlinkNodeGuid": { "type": "string" },
            "hotlinkElementGuid": { "type": "string" },
            "proxies": { "type": "array" },
            "broken": { "type": "boolean" },
            "elapsedMs": { "type": "number" }
        }
    })";
}

GS::ObjectState PlaceModuleAsElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString modulePath;
    Int32 floorInd = 0;
    Int32 refFloorInd = 0;
    bool allStories = true;
    bool breakLink = true;
    parameters.Get ("modulePath", modulePath);
    parameters.Get ("floorInd", floorInd);
    parameters.Get ("refFloorInd", refFloorInd);
    parameters.Get ("allStories", allStories);
    parameters.Get ("breakLink", breakLink);
    API_Coord3D offset = {};
    GS::ObjectState offsetState;
    if (parameters.Get ("offset", offsetState)) {
        offsetState.Get ("x", offset.x);
        offsetState.Get ("y", offset.y);
        offsetState.Get ("z", offset.z);
    }

    IO::Location location;
    const GS::Optional<GS::ObjectState> refusal = CheckNewModuleLocation (modulePath, location, true);
    if (refusal.HasValue ()) {
        return refusal.Get ();
    }

    API_HotlinkNode node = {};
    API_Element hotlink = {};
    GS::HashTable<API_Guid, API_Guid> proxies;
    GS::UniString failedStep;
    const auto start = std::chrono::steady_clock::now ();
    // One undo step: node, instance and the broken link come and go together.
    const GSErrCode err = ACAPI_CallUndoableCommand ("History - Place Module As Elements", [&] () -> GSErrCode {
        node.type = APIHotlink_Module;
        node.storyRangeType = allStories ? APIHotlink_AllStories : APIHotlink_SingleStory;
        node.sourceLocation = new IO::Location (location);
        node.refFloorInd = static_cast<short> (refFloorInd);
        GS::ucsncpy (node.name, L("History snapshot"), API_UniLongNameLen - 1);
        GSErrCode stepErr = NoError;
#ifdef ServerMainVers_2800
        stepErr = ACAPI_Hotlink_GetHotlinkStoryInfo (&node);
        if (stepErr != NoError) { failedStep = "GetHotlinkStoryInfo"; return stepErr; }
        stepErr = ACAPI_Hotlink_CreateHotlinkNode (&node);
#else
        stepErr = ACAPI_Database (APIDb_CreateHotlinkNodeID, &node);
#endif
        if (stepErr != NoError) { failedStep = "CreateHotlinkNode"; return stepErr; }

#ifdef ServerMainVers_2600
        hotlink.header.type = API_HotlinkID;
#else
        hotlink.header.typeID = API_HotlinkID;
#endif
        hotlink.header.floorInd = static_cast<short> (floorInd);
        hotlink.header.layer = ACAPI_CreateAttributeIndex (1);
        hotlink.hotlink.type = APIHotlink_Module;
        hotlink.hotlink.hotlinkNodeGuid = node.guid;
        hotlink.hotlink.transformation.tmx[0] = 1.0;
        hotlink.hotlink.transformation.tmx[5] = 1.0;
        hotlink.hotlink.transformation.tmx[10] = 1.0;
        hotlink.hotlink.transformation.tmx[3] = offset.x;
        hotlink.hotlink.transformation.tmx[7] = offset.y;
        hotlink.hotlink.transformation.tmx[11] = offset.z;
        hotlink.hotlink.ignoreTopFloorLinks = false;
        hotlink.hotlink.relinkWallOpenings = false;
        hotlink.hotlink.adjustLevelDiffs = false;
        stepErr = ACAPI_Element_Create (&hotlink, nullptr);
        if (stepErr != NoError) { failedStep = "CreateHotlinkElement"; return stepErr; }

#ifdef ServerMainVers_2800
        stepErr = ACAPI_Hotlink_GetHotlinkProxyElementTable (&hotlink.header.guid, &proxies);
#else
        stepErr = ACAPI_Database (APIDb_GetHotlinkProxyElementTableID, &hotlink.header.guid, &proxies);
#endif
        if (stepErr != NoError) { failedStep = "GetHotlinkProxyElementTable"; return stepErr; }

        if (breakLink) {
#ifdef ServerMainVers_2800
            stepErr = ACAPI_Hotlink_BreakHotlinkNode (&node.guid);
#else
            stepErr = ACAPI_Database (APIDb_BreakHotlinkNodeID, &node.guid);
#endif
            if (stepErr != NoError) { failedStep = "BreakHotlinkNode"; return stepErr; }
        }
        return NoError;
    });
    const double elapsedMs = ElapsedMs (start);

    if (err != NoError) {
        return CreateFailedExecutionResult (err, GS::UniString ("Failed to place the module: ") + failedStep);
    }
    GS::ObjectState response = CreateSuccessfulExecutionResult ();
    response.Add ("hotlinkNodeGuid", APIGuidToString (node.guid));
    response.Add ("hotlinkElementGuid", APIGuidToString (hotlink.header.guid));
    response.Add ("broken", breakLink);
    response.Add ("elapsedMs", elapsedMs);
    const auto& proxyList = response.AddList<GS::ObjectState> ("proxies");
    for (const API_Guid& source : proxies.Keys ()) {
        proxyList (GS::ObjectState ("source", APIGuidToString (source), "placed", APIGuidToString (proxies[source])));
    }
    return response;
}
