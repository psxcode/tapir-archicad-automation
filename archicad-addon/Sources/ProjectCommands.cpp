#include "ProjectCommands.hpp"
#include "MigrationHelper.hpp"
#include "File.hpp"
#include "NativeOwnership.hpp"

#include <cmath>

GetProjectInfoCommand::GetProjectInfoCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String GetProjectInfoCommand::GetName () const
{
    return "GetProjectInfo";
}

GS::Optional<GS::UniString> GetProjectInfoCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "isUntitled": {
                "type": "boolean",
                "description": "True, if the project is not saved yet."
            },
            "isTeamwork": {
                "type": "boolean",
                "description": "True, if the project is a Teamwork (BIMcloud) project."
            },
            "projectLocation": {
                "type": "string",
                "description": "The location of the project in the filesystem or a BIMcloud project reference.",
                "minLength": 1
            },
            "projectPath": {
                "type": "string",
                "description": "The path of the project. A filesystem path or a BIMcloud server relative path.",
                "minLength": 1
            },
            "projectName": {
                "type": "string",
                "description": "The name of the project.",
                "minLength": 1
            }
        },
        "additionalProperties": false,
        "required": [
            "isUntitled",
            "isTeamwork"
        ]
    })";
}

GS::ObjectState GetProjectInfoCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_ProjectInfo projectInfo = {};
    GSErrCode err = ACAPI_ProjectOperation_Project (&projectInfo);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to retrieve project information. Check the opened project!");
    }

    GS::ObjectState response;
    response.Add ("isUntitled", projectInfo.untitled);
    response.Add ("isTeamwork", projectInfo.teamwork);
    if (!projectInfo.untitled) {
        if (projectInfo.location) {
            response.Add ("projectLocation", projectInfo.location->ToDisplayText ());
        }
        if (projectInfo.projectPath) {
            response.Add ("projectPath", *projectInfo.projectPath);
        }
        if (projectInfo.projectName) {
            response.Add ("projectName", *projectInfo.projectName);
        }
    }

    return response;
}

GetProjectInfoFieldsCommand::GetProjectInfoFieldsCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String GetProjectInfoFieldsCommand::GetName () const
{
    return "GetProjectInfoFields";
}

GS::Optional<GS::UniString> GetProjectInfoFieldsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "fields": {
                "$ref": "#/ProjectInfoFields"
            }
        },
        "additionalProperties": false,
        "required": [
            "fields"
        ]
    })";
}

GS::ObjectState GetProjectInfoFieldsCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ArrayFB<GS::UniString, 3>> autoTexts;
    API_AutotextType type = APIAutoText_All;

    GSErrCode err = ACAPI_AutoText_GetAutoTexts (&autoTexts, type);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to retrieve project information fields.");
    }

    static const GS::Array<GS::UniString> validPrefixes = {
        "PROJECT", "KEYWORD", "NOTES", "SITE", "BUILDING", "CONTACT", "CAD_TECHNICIAN", "CLIENT"
    };

    GS::ObjectState response;
    const auto& listAdder = response.AddList<GS::ObjectState> ("fields");

    for (const auto& autoText : autoTexts) {
        const GS::UniString& autoTextName = autoText[0];
        const GS::UniString& autoTextId = autoText[1];
        const GS::UniString& autoTextValue = autoText[2];

        bool isValidPrefix = false;
        for (const GS::UniString& validPrefix : validPrefixes) {
            if (autoTextId.BeginsWith (validPrefix) || autoTextId.BeginsWith ("autotext-" + validPrefix)) {
                isValidPrefix = true;
                break;
            }
        }
        if (!isValidPrefix) {
            continue;
        }

        GS::ObjectState projectInfoData;
        projectInfoData.Add ("projectInfoId", autoTextId);
        projectInfoData.Add ("projectInfoName", autoTextName);
        projectInfoData.Add ("projectInfoValue", autoTextValue);
        listAdder (projectInfoData);
    }

    return response;
}

SetProjectInfoFieldCommand::SetProjectInfoFieldCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String SetProjectInfoFieldCommand::GetName () const
{
    return "SetProjectInfoField";
}

GS::Optional<GS::UniString> SetProjectInfoFieldCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectInfoId": {
                "type": "string",
                "description": "The id of the project info field.",
                "minLength": 1
            },
            "projectInfoValue": {
                "type": "string",
                "description": "The new value of the project info field.",
                "minLength": 1
            }
        },
        "additionalProperties": false,
        "required": [
            "projectInfoId",
            "projectInfoValue"
        ]
    })";
}

GS::ObjectState SetProjectInfoFieldCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString projectInfoId;
    GS::UniString projectInfoValue;
    if (!parameters.Get ("projectInfoId", projectInfoId) || !parameters.Get ("projectInfoValue", projectInfoValue)) {
        return CreateErrorResponse (Error, "Invalid input parameters.");
    }

    GSErrCode err = ACAPI_AutoText_SetAnAutoText (&projectInfoId, &projectInfoValue);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to set project information field.");
    }

    return {};
}

CreateProjectInfoFieldsCommand::CreateProjectInfoFieldsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String CreateProjectInfoFieldsCommand::GetName () const
{
    return "CreateProjectInfoFields";
}

GS::Optional<GS::UniString> CreateProjectInfoFieldsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectInfoFields": {
                "type": "array",
                "description": "Array of custom project info fields to create.",
                "items": {
                    "type": "object",
                    "properties": {
                        "projectInfoName": {
                            "type": "string",
                            "description": "Display name of the project info field.",
                            "minLength": 1
                        },
                        "projectInfoValue": {
                            "type": "string",
                            "description": "Initial value of the project info field."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "projectInfoName"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "projectInfoFields"
        ]
    })";
}

GS::Optional<GS::UniString> CreateProjectInfoFieldsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "fields": {
                "$ref": "#/ProjectInfoFields"
            }
        },
        "additionalProperties": false,
        "required": [
            "fields"
        ]
    })";
}

GS::ObjectState CreateProjectInfoFieldsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> projectInfoFields;
    if (!parameters.Get ("projectInfoFields", projectInfoFields) || projectInfoFields.IsEmpty ()) {
        return CreateErrorResponse (APIERR_BADPARS, "projectInfoFields is missing or empty.");
    }

    GS::ObjectState response;
    const auto& fieldsAdder = response.AddList<GS::ObjectState> ("fields");

    ACAPI_CallUndoableCommand ("CreateProjectInfoFields", [&]() -> GSErrCode {
        for (const GS::ObjectState& projectInfoField : projectInfoFields) {
            GS::UniString projectInfoName;
            if (!projectInfoField.Get ("projectInfoName", projectInfoName) || projectInfoName.IsEmpty ()) {
                fieldsAdder (CreateErrorResponse (APIERR_BADPARS, "projectInfoName is missing or empty."));
                continue;
            }

            GS::UniString projectInfoValue;
            projectInfoField.Get ("projectInfoValue", projectInfoValue);

            GS::Guid guid;
            guid.Generate ();
            API_Guid dbKey = GSGuid2APIGuid (guid);

            GSErrCode err = ACAPI_AutoText_CreateAnAutoText (&dbKey, projectInfoName.ToCStr ());
            if (err != NoError) {
                fieldsAdder (CreateErrorResponse (err, "Failed to create project information field."));
                continue;
            }

            GS::UniString projectInfoId ("autotext-");
            projectInfoId.Append (guid.ToUniString ());

            err = ACAPI_AutoText_SetAnAutoText (&projectInfoId, &projectInfoValue);
            if (err != NoError) {
                fieldsAdder (CreateErrorResponse (err, "Failed to set the initial value of the project information field."));
                continue;
            }

            GS::ObjectState createdField;
            createdField.Add ("projectInfoId", projectInfoId);
            createdField.Add ("projectInfoName", projectInfoName);
            createdField.Add ("projectInfoValue", projectInfoValue);
            fieldsAdder (createdField);
        }

        return NoError;
    });

    return response;
}

DeleteProjectInfoFieldsCommand::DeleteProjectInfoFieldsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String DeleteProjectInfoFieldsCommand::GetName () const
{
    return "DeleteProjectInfoFields";
}

GS::Optional<GS::UniString> DeleteProjectInfoFieldsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectInfoIds": {
                "type": "array",
                "description": "List of project info field ids to delete. Only custom fields (ids starting with 'autotext-') can be deleted.",
                "items": {
                    "type": "string"
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "projectInfoIds"
        ]
    })";
}

GS::Optional<GS::UniString> DeleteProjectInfoFieldsCommand::GetResponseSchema () const
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

GS::ObjectState DeleteProjectInfoFieldsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::UniString> projectInfoIds;
    if (!parameters.Get ("projectInfoIds", projectInfoIds) || projectInfoIds.IsEmpty ()) {
        return CreateErrorResponse (APIERR_BADPARS, "projectInfoIds is missing or empty.");
    }

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("DeleteProjectInfoFields", [&]() -> GSErrCode {
        for (const GS::UniString& projectInfoId : projectInfoIds) {
            if (!projectInfoId.BeginsWith ("autotext-")) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS,
                    "Only custom project info fields (ids starting with 'autotext-') can be deleted."));
                continue;
            }

            GSErrCode err = ACAPI_AutoText_DeleteAnAutoText (projectInfoId.ToCStr ());
            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (err, "Failed to delete project info field."));
            } else {
                executionResults (CreateSuccessfulExecutionResult ());
            }
        }
        return NoError;
    });

    return response;
}

GetHotlinksCommand::GetHotlinksCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetHotlinksCommand::GetName () const
{
    return "GetHotlinks";
}

GS::Optional<GS::UniString> GetHotlinksCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "hotlinks": {
                "$ref": "#/Hotlinks"
            }
        },
        "additionalProperties": false,
        "required": [
            "hotlinks"
        ]
    })";
}

static GS::Optional<GS::UniString> GetLocationOfHotlink (const API_Guid& hotlinkGuid)
{
    API_HotlinkNode hotlinkNode = {};
    hotlinkNode.guid = hotlinkGuid;

    ACAPI_Hotlink_GetHotlinkNode (&hotlinkNode);
    if (hotlinkNode.sourceLocation == nullptr) {
        return GS::NoValue;
    }

    return hotlinkNode.sourceLocation->ToDisplayText ();
}

static GS::ObjectState DumpHotlinkWithChildren (const API_Guid& hotlinkGuid,
    GS::HashTable<API_Guid, GS::Array<API_Guid>>& hotlinkTree)
{
    GS::ObjectState hotlinkNodeOS;

    const auto& location = GetLocationOfHotlink (hotlinkGuid);
    if (location.HasValue ()) {
        hotlinkNodeOS.Add ("location", location.Get ());
    }

    const auto& children = hotlinkTree.Retrieve (hotlinkGuid);
    if (!children.IsEmpty ()) {
        const auto& listAdder = hotlinkNodeOS.AddList<GS::ObjectState> ("children");
        for (const API_Guid& childNodeGuid : hotlinkTree.Retrieve (hotlinkGuid)) {
            listAdder (DumpHotlinkWithChildren (childNodeGuid, hotlinkTree));
        }
    }

    return hotlinkNodeOS;
}

GS::ObjectState GetHotlinksCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GS::ObjectState response;
    const auto& listAdder = response.AddList<GS::ObjectState> ("hotlinks");

    for (API_HotlinkTypeID type : {APIHotlink_Module, APIHotlink_XRef}) {
        API_Guid hotlinkRootNodeGuid = APINULLGuid;
        if (ACAPI_Hotlink_GetHotlinkRootNodeGuid (&type, &hotlinkRootNodeGuid) == NoError) {
            GS::HashTable<API_Guid, GS::Array<API_Guid>> hotlinkTree;
            if (ACAPI_Hotlink_GetHotlinkNodeTree (&hotlinkRootNodeGuid, &hotlinkTree) == NoError) {
                for (const API_Guid& childNodeGuid : hotlinkTree.Retrieve (hotlinkRootNodeGuid)) {
                    listAdder (DumpHotlinkWithChildren (childNodeGuid, hotlinkTree));
                }
            }
        }
    }

    return response;
}

GetStoriesCommand::GetStoriesCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String GetStoriesCommand::GetName () const
{
    return "GetStories";
}

GS::Optional<GS::UniString> GetStoriesCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "firstStory": {
                "type": "integer",
                "description": "First story index."
            },
            "lastStory": {
                "type": "integer",
                "description": "Last story index."
            },
            "actStory": {
                "type": "integer",
                "description": "Actual (currently visible in 2D) story index."
            },
            "skipNullFloor": {
                "type": "boolean",
                "description": "Floor indices above ground-floor level may start with 1 instead of 0."
            },
            "stories": {
                "$ref": "#/StoriesParameters"
            }
        },
        "additionalProperties": false,
        "required": [
            "firstStory",
            "lastStory",
            "actStory",
            "skipNullFloor",
            "stories"
        ]
    })";
}


GS::ObjectState GetStoriesCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_StoryInfo storyInfo = {};
    const GS::OnExit storyInfoGuard ([&storyInfo] () {
        if (storyInfo.data != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
        storyInfo.data = nullptr;
    });
    GSErrCode err = ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to retrive stories info.");
    }
    if (storyInfo.data == nullptr || *storyInfo.data == nullptr || storyInfo.lastStory < storyInfo.firstStory) {
        return CreateErrorResponse (APIERR_BADPARS, "Archicad returned invalid story data.");
    }

    const Int32 firstStory = static_cast<Int32> (storyInfo.firstStory);
    const Int32 lastStory = static_cast<Int32> (storyInfo.lastStory);
    const GS::USize storyCount = static_cast<GS::USize> (lastStory - firstStory + 1);
    const GSSize storyDataCount = BMhGetSize (reinterpret_cast<GSHandle> (storyInfo.data)) / sizeof (API_StoryType);
    if (storyCount == 0 || static_cast<GSSize> (storyCount) > storyDataCount) {
        return CreateErrorResponse (APIERR_BADPARS, "Archicad returned an undersized story data handle.");
    }

    GS::ObjectState response;
    response.Add ("firstStory", storyInfo.firstStory);
    response.Add ("lastStory", storyInfo.lastStory);
    response.Add ("actStory", storyInfo.actStory);
    response.Add ("skipNullFloor", storyInfo.skipNullFloor);

    const auto& listAdder = response.AddList<GS::ObjectState> ("stories");

    for (GS::UIndex i = 0; i < storyCount; i++) {
        const API_StoryType& story = (*storyInfo.data)[i];
        GS::ObjectState storyData;
        GS::UniString uName = story.uName;

        storyData.Add ("index", story.index);
        storyData.Add ("floorId", story.floorId);
        storyData.Add ("dispOnSections", story.dispOnSections);
        storyData.Add ("level", story.level);
        if (i + 1 < storyCount) {
            storyData.Add ("height", (*storyInfo.data)[i + 1].level - story.level);
        }
        storyData.Add ("name", uName);

        listAdder (storyData);
    }

    return response;
}

SetStoriesCommand::SetStoriesCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String SetStoriesCommand::GetName () const
{
    return "SetStories";
}

GS::Optional<GS::UniString> SetStoriesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "stories": {
                "$ref": "#/StoriesSettings"
            }
        },
        "additionalProperties": false,
        "required": [
            "stories"
        ]
    })";
}

GS::Optional<GS::UniString> SetStoriesCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}


GS::ObjectState SetStoriesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> stories;
    parameters.Get ("stories", stories);

    API_StoryInfo storyInfo = {};
    const GS::OnExit storyInfoGuard ([&storyInfo] () {
        if (storyInfo.data != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
        storyInfo.data = nullptr;
    });
    GSErrCode err = ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to retrive stories info.");
    }
    if (storyInfo.data == nullptr || *storyInfo.data == nullptr) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Archicad returned no story data.");
    }

    const Int32 firstStory = static_cast<Int32> (storyInfo.firstStory);
    const Int32 lastStory = static_cast<Int32> (storyInfo.lastStory);
    if (lastStory < firstStory) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Archicad returned an invalid story range.");
    }
    GS::USize storyCount = static_cast<GS::USize> (lastStory - firstStory + 1);
    const GSSize storyDataCount = BMhGetSize (reinterpret_cast<GSHandle> (storyInfo.data)) / sizeof (API_StoryType);
    if (static_cast<GSSize> (storyCount) > storyDataCount) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Archicad returned an undersized story data handle.");
    }

    if (storyCount != stories.GetSize ()) {
        if (storyCount < stories.GetSize ()) {
            for (GS::UIndex i = storyCount; i < stories.GetSize (); ++i) {
                API_StoryCmdType storyCmd = {};
                storyCmd.action = APIStory_InsAbove;
                storyCmd.index  = storyInfo.lastStory;

                stories[i].Get ("dispOnSections", storyCmd.dispOnSections);
                stories[i].Get ("level", storyCmd.elevation);
                if (storyCount > 1) {
                    storyCmd.height = (*storyInfo.data)[i - 1].level - (*storyInfo.data)[i - 2].level;
                }

                GS::UniString name;
                stories[i].Get ("name", name);
                GS::snuprintf (storyCmd.uName, sizeof (storyCmd.uName), name.ToCStr ());
            
                err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
                if (err != NoError) {
                    return CreateFailedExecutionResult (err, "Failed to create new story.");
                }
            }
        } else {
            for (GS::UIndex i = storyCount; i > stories.GetSize ();) {
                --i;
                API_StoryCmdType storyCmd = {};
                storyCmd.action = APIStory_Delete;
                storyCmd.index  = (*storyInfo.data)[i].index;
            
                err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
                if (err != NoError) {
                    return CreateFailedExecutionResult (err, "Failed to delete story.");
                }
            }
        }

        if (storyInfo.data != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
        storyInfo.data = nullptr;
        err = ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
        if (err != NoError) {
            return CreateFailedExecutionResult (err, "Failed to retrive stories info.");
        }
        if (storyInfo.data == nullptr || *storyInfo.data == nullptr || storyInfo.lastStory < storyInfo.firstStory) {
            return CreateFailedExecutionResult (APIERR_BADPARS, "Archicad returned invalid story data after resizing.");
        }
        storyCount = static_cast<GS::USize> (
            static_cast<Int32> (storyInfo.lastStory) - static_cast<Int32> (storyInfo.firstStory) + 1);
        const GSSize refreshedStoryDataCount = BMhGetSize (reinterpret_cast<GSHandle> (storyInfo.data)) / sizeof (API_StoryType);
        if (static_cast<GSSize> (storyCount) > refreshedStoryDataCount || storyCount != static_cast<GS::USize> (stories.GetSize ())) {
            return CreateFailedExecutionResult (APIERR_BADPARS, "Archicad returned inconsistent story data after resizing.");
        }
    }

    GS::USize recursionCount = 0;
    constexpr GS::USize maxRecursion = 3;
    for (GS::UIndex i = 0; i < storyCount;) {
        const API_StoryType& story = (*storyInfo.data)[i];

        API_StoryCmdType storyCmd = {};
        storyCmd.index  = story.index;

        stories[i].Get ("dispOnSections", storyCmd.dispOnSections);

        bool changed = false;

        if (story.dispOnSections != storyCmd.dispOnSections) {
            storyCmd.action = APIStory_SetDispOnSections;
        
            err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
            if (err != NoError) {
                BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
                return CreateFailedExecutionResult (err, "Failed to modify dispOnSections settings.");
            }

            changed = true;
        }

        GS::UniString name;
        stories[i].Get ("name", name);

        if (story.uName != name) {
            GS::snuprintf (storyCmd.uName, sizeof (storyCmd.uName), name.ToCStr ());
            storyCmd.action = APIStory_Rename;
        
            err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
            if (err != NoError) {
                BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
                return CreateFailedExecutionResult (err, "Failed to rename story.");
            }

            changed = true;
        }

        stories[i].Get ("level", storyCmd.elevation);

        if (std::abs (story.level - storyCmd.elevation) >= 0.0001) {
            storyCmd.action = APIStory_SetElevation;
        
            err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
            if (err != NoError) {
                BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
                return CreateFailedExecutionResult (err, "Failed to change story level.");
            }

            changed = true;
        } else {
            const API_StoryType*   actNextStory = i + 1 < storyCount ? &(*storyInfo.data)[i + 1] : nullptr;
            const GS::ObjectState* newNextStory = i + 1 < stories.GetSize () ? &stories[i + 1] : nullptr;

            double newNextLevel = 0;
            if (actNextStory != nullptr && newNextStory != nullptr &&
                newNextStory->Get ("level", newNextLevel) &&
                std::abs ((newNextLevel - storyCmd.elevation) - (actNextStory->level - story.level)) >= 0.0001) {
                storyCmd.height = newNextLevel - storyCmd.elevation;
                storyCmd.action = APIStory_SetHeight;
            
                err = ACAPI_ProjectSetting_ChangeStorySettings (&storyCmd);
                if (err != NoError) {
                    BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
                    return CreateFailedExecutionResult (err, "Failed to change story height.");
                }

                changed = true;
            }
        }

        if (changed) {
            BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
            err = ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
            if (err != NoError) {
                BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));
                return CreateFailedExecutionResult (err, "Failed to retrive stories info.");
            }
        }

        if (!changed || ++recursionCount >= maxRecursion) {
            recursionCount = 0;
            ++i;
            continue;
        }
    }

    BMKillHandle (reinterpret_cast<GSHandle *> (&storyInfo.data));

    return CreateSuccessfulExecutionResult ();
}

OpenProjectCommand::OpenProjectCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String OpenProjectCommand::GetName () const
{
    return "OpenProject";
}

GS::Optional<GS::UniString> OpenProjectCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectFilePath": {
                "type": "string",
                "description": "The target project file to open."
            }
        },
        "additionalProperties": false,
        "required": [
            "projectFilePath"
        ]
    })";
}

GS::Optional<GS::UniString> OpenProjectCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState OpenProjectCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString projectFilePath;
    if (!parameters.Get ("projectFilePath", projectFilePath)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "projectFilePath parameter is missing");
    }

    IO::Location projectLocation (projectFilePath);
    IO::Name lastLocalName;
    if (projectLocation.GetLastLocalName (&lastLocalName) != NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "projectFilePath parameter is invalid");
    }

    const GS::UniString extension = lastLocalName.GetExtension ();
    API_FileOpenPars openPars = {};
    if (extension.Compare ("pln", CaseInsensitive) == GS::UniString::Equal) {
        openPars.fileTypeID = APIFType_PlanFile;
    } else if (extension.Compare ("pla", CaseInsensitive) == GS::UniString::Equal) {
        openPars.fileTypeID = APIFType_A_PlanFile;
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "projectFilePath parameter is invalid, the extension must be pln or pla");
    }

    openPars.libGiven = false;
    openPars.useStoredLib = true;
#ifndef ServerMainVers_2900
    openPars.enableSaveAlert = false;
#endif
    openPars.file = &projectLocation;

    const GSErrCode err = ACAPI_ProjectOperation_Open (&openPars);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to open the given project");
    }

    return CreateSuccessfulExecutionResult ();
}

CloseProjectCommand::CloseProjectCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String CloseProjectCommand::GetName () const
{
    return "CloseProject";
}

GS::Optional<GS::UniString> CloseProjectCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState CloseProjectCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GSErrCode err = ACAPI_ProjectOperation_Close ();
    if (err != NoError) {
        return CreateFailedExecutionResult (APIERR_COMMANDFAILED, "Failed to close the project. There might be none currently open.");
    }
    return CreateSuccessfulExecutionResult ();
}

SaveProjectCommand::SaveProjectCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String SaveProjectCommand::GetName () const
{
    return "SaveProject";
}

GS::Optional<GS::UniString> SaveProjectCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState SaveProjectCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    GSErrCode err = ACAPI_ProjectOperation_Save ();
    if (err != NoError) {
        return CreateFailedExecutionResult (APIERR_COMMANDFAILED, "Failed to save the project.");
    }
    return CreateSuccessfulExecutionResult ();
}

SaveProjectAsVersionCommand::SaveProjectAsVersionCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String SaveProjectAsVersionCommand::GetName () const
{
    return "SaveProjectAsVersion";
}

GS::Optional<GS::UniString> SaveProjectAsVersionCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "targetVersion": {
                "type": "integer",
                "enum": [23, 24, 25, 26, 27],
                "description": "The Archicad major version to write. The current command is intentionally bounded to versions supported by the Archicad 28 save-as-old-version file types."
            },
            "outputPath": {
                "type": "string",
                "minLength": 1,
                "description": "A new local .pln or .pla path. Existing files are never overwritten."
            },
            "archive": {
                "type": "boolean",
                "description": "Write an archive (.pla) and embed library parts when true; write a plan (.pln) when false."
            },
            "includeLibraryParts": {
                "type": "boolean",
                "description": "For archives, include the full library payload so placed windows, doors, and objects have the best chance of surviving the older-version conversion."
            }
        },
        "additionalProperties": false,
        "required": [
            "targetVersion",
            "outputPath"
        ]
    })";
}

GS::Optional<GS::UniString> SaveProjectAsVersionCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "success": { "type": "boolean" },
            "targetVersion": { "type": "integer" },
            "outputPath": { "type": "string" },
            "archive": { "type": "boolean" },
            "includeLibraryParts": { "type": "boolean" },
            "textures": { "type": "boolean" }
        }
    })";
}

GS::ObjectState SaveProjectAsVersionCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 targetVersion = 0;
    GS::UniString outputPath;
    bool archive = true;
    bool includeLibraryParts = true;

    if (!parameters.Get ("targetVersion", targetVersion)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "targetVersion parameter is missing");
    }
    if (!parameters.Get ("outputPath", outputPath) || outputPath.IsEmpty ()) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "outputPath parameter is missing or empty");
    }
    parameters.Get ("archive", archive);
    parameters.Get ("includeLibraryParts", includeLibraryParts);

    IO::Location outputLocation (outputPath);
    if (outputLocation.GetStatus () != NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "outputPath parameter is not a valid filesystem location");
    }

    IO::Name lastLocalName;
    if (outputLocation.GetLastLocalName (&lastLocalName) != NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "outputPath parameter is not a valid local file path");
    }

    const GS::UniString extension = lastLocalName.GetExtension ();
    const GS::UniString expectedExtension = archive ? "pla" : "pln";
    if (extension.Compare (expectedExtension, CaseInsensitive) != GS::UniString::Equal) {
        return CreateFailedExecutionResult (
            APIERR_BADPARS,
            archive
                ? "outputPath must have a .pla extension when archive is true"
                : "outputPath must have a .pln extension when archive is false"
        );
    }

    // Do not let a conversion accidentally replace the source project or an
    // earlier export.  APIDo_SaveID may otherwise follow normal Save As
    // overwrite behavior, which is too dangerous for an agent-facing write.
    if (IO::File (outputLocation, IO::File::OnNotFound::Fail).GetStatus () == NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Refusing to overwrite an existing output file");
    }

    API_FTypeID fileTypeID;
    switch (targetVersion) {
        case 23:
            fileTypeID = archive ? APIFType_A_PlanFile2300 : APIFType_PlanFile2300;
            break;
        case 24:
            fileTypeID = archive ? APIFType_A_PlanFile2400 : APIFType_PlanFile2400;
            break;
#ifdef ServerMainVers_2600
        case 25:
            fileTypeID = archive ? APIFType_A_PlanFile2500 : APIFType_PlanFile2500;
            break;
        case 26:
            fileTypeID = archive ? APIFType_A_PlanFile2600 : APIFType_PlanFile2600;
            break;
#endif
#ifdef ServerMainVers_2700
        case 27:
            fileTypeID = archive ? APIFType_A_PlanFile2700 : APIFType_PlanFile2700;
            break;
#endif
        default:
            return CreateFailedExecutionResult (APIERR_BADPARS, "targetVersion is not supported by this Archicad build");
    }

    API_FileSavePars savePars = {};
    savePars.fileTypeID = fileTypeID;
    savePars.file = &outputLocation;

    GSErrCode err = NoError;
    if (archive) {
        API_SavePars_Archive archivePars = {};
        archivePars.texturesOn = false;
        archivePars.libraryPartsOn = includeLibraryParts;
#ifdef ServerMainVers_2800
        err = ACAPI_ProjectOperation_Save (&savePars, &archivePars);
#else
        err = ACAPI_Automate (APIDo_SaveID, &savePars, &archivePars);
#endif
    } else {
#ifdef ServerMainVers_2800
        err = ACAPI_ProjectOperation_Save (&savePars);
#else
        err = ACAPI_Automate (APIDo_SaveID, &savePars, nullptr);
#endif
    }

    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to save the project in the requested older Archicad version");
    }

    GS::ObjectState response = CreateSuccessfulExecutionResult ();
    response.Add ("targetVersion", targetVersion);
    response.Add ("outputPath", outputPath);
    response.Add ("archive", archive);
    response.Add ("includeLibraryParts", archive && includeLibraryParts);
    response.Add ("textures", false);
    return response;
}

GetGeoLocationCommand::GetGeoLocationCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetGeoLocationCommand::GetName () const
{
    return "GetGeoLocation";
}

GS::Optional<GS::UniString> GetGeoLocationCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectLocation": {
                "type": "object",
                "properties": {
                    "longitude": {
                        "type": "number",
                        "description": "longitude in degrees"
                    },
                    "latitude": {
                        "type": "number",
                        "description": "latitude in degrees"
                    },
                    "altitude": {
                        "type": "number",
                        "description": "altitude in meters"
                    },
                    "north": {
                        "type": "number",
                        "description": "north direction in radians"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "longitude",
                    "latitude",
                    "altitude",
                    "north"
                ]
            },
            "surveyPoint": {
                "type": "object",
                "properties": {
                    "position": {
                        "type": "object",
                        "properties": {
                            "eastings": {
                                "type": "number",
                                "description": "Location along the easting of the coordinate system of the target map coordinate reference system."
                            },
                            "northings": {
                                "type": "number",
                                "description": "Location along the northing of the coordinate system of the target map coordinate reference system."
                            },
                            "elevation": {
                                "type": "number",
                                "description": "Orthogonal height relative to the vertical datum specified."
                            }
                        },
                        "additionalProperties": false,
                        "required": [
                            "eastings",
                            "northings",
                            "elevation"
                        ]
                    },
                    "geoReferencingParameters": {
                        "type": "object",
                        "properties": {
                            "crsName": {
                                "type": "string",
                                "description": "Name by which the coordinate reference system is identified."
                            },
                            "description": {
                                "type": "string",
                                "description": "Informal description of this coordinate reference system."
                            },
                            "geodeticDatum": {
                                "type": "string",
                                "description": "Name by which this datum is identified."
                            },
                            "verticalDatum": {
                                "type": "string",
                                "description": "Name by which the vertical datum is identified."
                            },
                            "mapProjection": {
                                "type": "string",
                                "description": "Name by which the map projection is identified."
                            },
                            "mapZone": {
                                "type": "string",
                                "description": "Name by which the map zone, relating to the MapProjection, is identified."
                            }
                        },
                        "additionalProperties": false,
                        "required": [
                            "crsName",
                            "description",
                            "geodeticDatum",
                            "verticalDatum",
                            "mapProjection",
                            "mapZone"
                        ]
                    }
                },
                "additionalProperties": false,
                "required": [
                    "position",
                    "geoReferencingParameters"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "projectLocation",
            "surveyPoint"
        ]
    })";
}

GS::ObjectState GetGeoLocationCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_GeoLocation apiGeoLocation = {};
    ACAPI_GeoLocation_GetGeoLocation (&apiGeoLocation);

    return GS::ObjectState (
        "projectLocation", GS::ObjectState (
            "longitude", apiGeoLocation.placeInfo.longitude,
            "latitude", apiGeoLocation.placeInfo.latitude,
            "altitude", apiGeoLocation.placeInfo.altitude,
            "north", apiGeoLocation.placeInfo.north),
        "surveyPoint", GS::ObjectState (
            "position", GS::ObjectState (
                "eastings", apiGeoLocation.geoReferenceData.eastings,
                "northings", apiGeoLocation.geoReferenceData.northings,
                "elevation", apiGeoLocation.geoReferenceData.orthogonalHeight),
            "geoReferencingParameters", GS::ObjectState (
                "crsName", apiGeoLocation.geoReferenceData.name,
                "description", apiGeoLocation.geoReferenceData.description,
                "geodeticDatum", apiGeoLocation.geoReferenceData.geodeticDatum,
                "verticalDatum", apiGeoLocation.geoReferenceData.verticalDatum,
                "mapProjection", apiGeoLocation.geoReferenceData.mapProjection,
                "mapZone", apiGeoLocation.geoReferenceData.mapZone)));
}

SetGeoLocationCommand::SetGeoLocationCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SetGeoLocationCommand::GetName () const
{
    return "SetGeoLocation";
}

GS::Optional<GS::UniString> SetGeoLocationCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "projectLocation": {
                "type": "object",
                "properties": {
                    "longitude": {
                        "type": "number",
                        "description": "longitude in degrees"
                    },
                    "latitude": {
                        "type": "number",
                        "description": "latitude in degrees"
                    },
                    "altitude": {
                        "type": "number",
                        "description": "altitude in meters"
                    },
                    "north": {
                        "type": "number",
                        "description": "north direction in radians"
                    }
                },
                "additionalProperties": false,
                "required": [
                ]
            },
            "surveyPoint": {
                "type": "object",
                "properties": {
                    "position": {
                        "type": "object",
                        "properties": {
                            "eastings": {
                                "type": "number",
                                "description": "Location along the easting of the coordinate system of the target map coordinate reference system."
                            },
                            "northings": {
                                "type": "number",
                                "description": "Location along the northing of the coordinate system of the target map coordinate reference system."
                            },
                            "elevation": {
                                "type": "number",
                                "description": "Orthogonal height relative to the vertical datum specified."
                            }
                        },
                        "additionalProperties": false,
                        "required": [
                        ]
                    },
                    "geoReferencingParameters": {
                        "type": "object",
                        "properties": {
                            "crsName": {
                                "type": "string",
                                "description": "Name by which the coordinate reference system is identified."
                            },
                            "description": {
                                "type": "string",
                                "description": "Informal description of this coordinate reference system."
                            },
                            "geodeticDatum": {
                                "type": "string",
                                "description": "Name by which this datum is identified."
                            },
                            "verticalDatum": {
                                "type": "string",
                                "description": "Name by which the vertical datum is identified."
                            },
                            "mapProjection": {
                                "type": "string",
                                "description": "Name by which the map projection is identified."
                            },
                            "mapZone": {
                                "type": "string",
                                "description": "Name by which the map zone, relating to the MapProjection, is identified."
                            }
                        },
                        "additionalProperties": false,
                        "required": [
                        ]
                    }
                },
                "additionalProperties": false,
                "required": [
                ]
            }
        },
        "additionalProperties": false,
        "required": [
        ]
    })";
}

GS::Optional<GS::UniString> SetGeoLocationCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState SetGeoLocationCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    API_GeoLocation apiGeoLocation = {};
    ACAPI_GeoLocation_GetGeoLocation (&apiGeoLocation);

    bool hasAnyInput = false;

    GS::ObjectState projectLocation;
    if (parameters.Get ("projectLocation", projectLocation)) {
        hasAnyInput |= projectLocation.Get ("longitude", apiGeoLocation.placeInfo.longitude);
        hasAnyInput |= projectLocation.Get ("latitude", apiGeoLocation.placeInfo.latitude);
        hasAnyInput |= projectLocation.Get ("altitude", apiGeoLocation.placeInfo.altitude);
        hasAnyInput |= projectLocation.Get ("north", apiGeoLocation.placeInfo.north);
    }
    GS::ObjectState surveyPoint;
    if (parameters.Get ("surveyPoint", surveyPoint)) {
        GS::ObjectState position;
        if (surveyPoint.Get ("position", position)) {
            hasAnyInput |= position.Get ("eastings", apiGeoLocation.geoReferenceData.eastings);
            hasAnyInput |= position.Get ("northings", apiGeoLocation.geoReferenceData.northings);
            hasAnyInput |= position.Get ("elevation", apiGeoLocation.geoReferenceData.orthogonalHeight);
        }
        GS::ObjectState geoReferencingParameters;
        if (surveyPoint.Get ("geoReferencingParameters", geoReferencingParameters)) {
            hasAnyInput |= geoReferencingParameters.Get ("crsName", apiGeoLocation.geoReferenceData.name);
            hasAnyInput |= geoReferencingParameters.Get ("description", apiGeoLocation.geoReferenceData.description);
            hasAnyInput |= geoReferencingParameters.Get ("geodeticDatum", apiGeoLocation.geoReferenceData.geodeticDatum);
            hasAnyInput |= geoReferencingParameters.Get ("verticalDatum", apiGeoLocation.geoReferenceData.verticalDatum);
            hasAnyInput |= geoReferencingParameters.Get ("mapProjection", apiGeoLocation.geoReferenceData.mapProjection);
            hasAnyInput |= geoReferencingParameters.Get ("mapZone", apiGeoLocation.geoReferenceData.mapZone);
        }
    }

    if (!hasAnyInput) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "No valid input parameters provided to update geolocation.");
    }

	GSErrCode err = ACAPI_CallUndoableCommand ("Change GeoLocation", [&] () -> GSErrCode {
        return ACAPI_GeoLocation_SetGeoLocation (&apiGeoLocation);
    });
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to set geolocation.");
    }

    return CreateSuccessfulExecutionResult ();
}

GetCalculationUnitsCommand::GetCalculationUnitsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetCalculationUnitsCommand::GetName () const
{
    return "GetCalculationUnits";
}

GS::Optional<GS::UniString> GetCalculationUnitsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "length": {
                "type": "object",
                "properties": {
                    "unit": {
                        "$ref": "#/LengthType"
                    },
                    "accuracy": {
                        "$ref": "#/AccuracyType"
                    },
                    "decimals": {
                        "type": "integer",
                        "description": "Number of decimals to display for length values."
                    },
                    "roundInch": {
                        "type": "integer",
                        "description": "Fractional inches."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "unit",
                    "accuracy",
                    "decimals"
                ]
            },
            "area": {
                "type": "object",
                "properties": {
                    "unit": {
                        "$ref": "#/AreaType"
                    },
                    "accuracy": {
                        "$ref": "#/AccuracyType"
                    },
                    "decimals": {
                        "type": "integer",
                        "description": "Number of decimals to display for area values."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "unit",
                    "accuracy",
                    "decimals"
                ]
            },
            "volume": {
                "type": "object",
                "properties": {
                    "unit": {
                        "$ref": "#/VolumeType"
                    },
                    "accuracy": {
                        "$ref": "#/AccuracyType"
                    },
                    "decimals": {
                        "type": "integer",
                        "description": "Number of decimals to display for volume values."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "unit",
                    "accuracy",
                    "decimals"
                ]
            },
            "angle": {
                "type": "object",
                "properties": {
                    "unit": {
                        "$ref": "#/AngleType"
                    },
                    "decimals": {
                        "type": "integer",
                        "description": "Number of decimals to display for angle values."
                    },
                    "accuracy": {
                        "type": "integer",
                        "description": "Accuracy for angle values."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "unit",
                    "decimals",
                    "accuracy"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "length",
            "area",
            "volume",
            "angle"
        ]
    })";
}

static GS::UniString ConvertAPILengthType (API_LengthTypeID apiLengthUnit)
{
    switch (apiLengthUnit) {
        case API_LengthTypeID::Meter: return "Meter";
        case API_LengthTypeID::Decimeter: return "Decimeter";
        case API_LengthTypeID::Centimeter: return "Centimeter";
        case API_LengthTypeID::Millimeter: return "Millimeter";
        case API_LengthTypeID::FootFracInch: return "FootFracInch";
        case API_LengthTypeID::FootDecInch: return "FootDecInch";
        case API_LengthTypeID::DecFoot: return "DecFoot";
        case API_LengthTypeID::FracInch: return "FracInch";
        case API_LengthTypeID::DecInch: return "DecInch";
        default: return "Unknown";
    }
}

static GS::UniString ConvertAPIAreaType (API_AreaTypeID apiAreaUnit)
{
    switch (apiAreaUnit) {
        case API_AreaTypeID::SquareMeter: return "SquareMeter";
        case API_AreaTypeID::SquareCentimeter: return "SquareCentimeter";
        case API_AreaTypeID::SquareMillimeter: return "SquareMillimeter";
        case API_AreaTypeID::SquareFoot: return "SquareFoot";
        case API_AreaTypeID::SquareInch: return "SquareInch";
        default: return "Unknown";
    }
}

static GS::UniString ConvertAPIVolumeType (API_VolumeTypeID apiVolumeUnit)
{
    switch (apiVolumeUnit) {
        case API_VolumeTypeID::CubicMeter: return "CubicMeter";
        case API_VolumeTypeID::Liter: return "Liter";
        case API_VolumeTypeID::CubicCentimeter: return "CubicCentimeter";
        case API_VolumeTypeID::CubicMillimeter: return "CubicMillimeter";
        case API_VolumeTypeID::CubicFoot: return "CubicFoot";
        case API_VolumeTypeID::CubicInch: return "CubicInch";
        case API_VolumeTypeID::CubicYard: return "CubicYard";
        case API_VolumeTypeID::Gallon: return "Gallon";
        default: return "Unknown";
    }
}

static GS::UniString ConvertAPIAngleType (API_AngleTypeID apiAngleUnit)
{
    switch (apiAngleUnit) {
        case API_AngleTypeID::DecimalDegree: return "DecimalDegree";
        case API_AngleTypeID::DegreeMinSec: return "DegreeMinSec";
        case API_AngleTypeID::Grad: return "Grad";
        case API_AngleTypeID::Radian: return "Radian";
        case API_AngleTypeID::Surveyors: return "Surveyors";
        default: return "Unknown";
    }
}

static GS::UniString ConvertAPIExtraAccuracyType (API_ExtraAccuracyID apiExtraAccuracy)
{
    switch (apiExtraAccuracy) {
        case API_ExtraAccuracyID::APIExtAc_Off: return "Off";
        case API_ExtraAccuracyID::APIExtAc_Small5: return "ShowSmall5";
        case API_ExtraAccuracyID::APIExtAc_Small25: return "ShowSmall25";
        case API_ExtraAccuracyID::APIExtAc_Small1: return "ShowSmall1";
        case API_ExtraAccuracyID::APIExtAc_Small01: return "ShowSmall01";
        case API_ExtraAccuracyID::APIExtAc_Fractions: return "InchCaseFractions";
        default: return "Unknown";
    }
}

GS::ObjectState GetCalculationUnitsCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    API_CalcUnitPrefs unitPrefs;
    ACAPI_ProjectSetting_GetPreferences (&unitPrefs, APIPrefs_CalcUnitsID);

    return GS::ObjectState (
        "length", GS::ObjectState (
            "unit", ConvertAPILengthType (unitPrefs.length.unit),
            "accuracy", ConvertAPIExtraAccuracyType (unitPrefs.length.accuracy),
            "decimals", unitPrefs.length.decimals),
        "area", GS::ObjectState (
            "unit", ConvertAPIAreaType (unitPrefs.area.unit),
            "accuracy", ConvertAPIExtraAccuracyType (unitPrefs.area.accuracy),
            "decimals", unitPrefs.area.decimals),
        "volume", GS::ObjectState (
            "unit", ConvertAPIVolumeType (unitPrefs.volume.unit),
            "accuracy", ConvertAPIExtraAccuracyType (unitPrefs.volume.accuracy),
            "decimals", unitPrefs.volume.decimals),
        "angle", GS::ObjectState (
            "unit", ConvertAPIAngleType (unitPrefs.angle.unit),
            "decimals", unitPrefs.angle.decimals,
            "accuracy", unitPrefs.angle.accuracy));
}

IFCFileOperationCommand::IFCFileOperationCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String IFCFileOperationCommand::GetName () const
{
    return "IFCFileOperation";
}

GS::Optional<GS::UniString> IFCFileOperationCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "method": {
                "type": "string",
                "description": "The file operation method to use.",
                "enum": ["save", "merge", "open"]
            },
            "ifcFilePath": {
                "type": "string",
                "description": "The target IFC file to use."
            },
            "fileType": {
                "type": "string",
                "description": "The type of the IFC file. The default is 'ifc'.",
                "enum": ["ifc", "ifcxml", "ifczip", "ifcxmlzip"]
            }
        },
        "additionalProperties": false,
        "required": [
            "method",
            "ifcFilePath"
        ]
    })";
}

GS::Optional<GS::UniString> IFCFileOperationCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState IFCFileOperationCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString ifcFilePath;
    if (!parameters.Get ("ifcFilePath", ifcFilePath)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "ifcFilePath parameter is missing");
    }

    GS::UniString methodStr;
    if (!parameters.Get ("method", methodStr)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "method parameter is missing");
    }

    API_IOParams ioParams = {};
    ioParams.fileTypeID = APIFType_IfcFile;
    if (methodStr == "open") {
        ioParams.method = IO_OPEN;
    } else if (methodStr == "merge") {
        ioParams.method = IO_MERGE;
    } else if (methodStr == "save") {
        ioParams.method = IO_SAVEAS;
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "method parameter is invalid");
    }

    GS::UniString fileTypeStr;
    if (!parameters.Get ("fileType", fileTypeStr)) {
        ioParams.refCon = 1;
    } else {
        if (fileTypeStr == "ifc") {
            ioParams.refCon = 1;
        } else if (fileTypeStr == "ifcxml") {
            ioParams.refCon = 2;
        } else if (fileTypeStr == "ifczip") {
            ioParams.refCon = 3;
        } else if (fileTypeStr == "ifcxmlzip") {
            ioParams.refCon = 4;
        } else {
            return CreateFailedExecutionResult (APIERR_BADPARS, "fileType parameter is invalid");
        }
    }

    IO::Location ifcFileLocation (ifcFilePath);
    IO::Name lastLocalName;
    if (ifcFileLocation.GetLastLocalName (&lastLocalName) != NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "ifcFilePath parameter is invalid");
    }
    ioParams.fileLoc = &ifcFileLocation;
    ioParams.saveFileIOName = &lastLocalName;
    ioParams.noDialog = true;
    ioParams.fromDragDrop = false;

    API_ModulID moduleID = { 1198731108, 138575850 };
    const GSErrCode err = ACAPI_AddOnAddOnCommunication_Call (&moduleID, 'IFCI', 1, reinterpret_cast<GSHandle>(&ioParams), nullptr, ioParams.noDialog);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to execute the IFC operation");
    }

    return CreateSuccessfulExecutionResult ();
}

PrintViewCommand::PrintViewCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String PrintViewCommand::GetName () const
{
    return "PrintView";
}

GS::Optional<GS::UniString> PrintViewCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "grid": {
                "type": "boolean",
                "description": "Print the grid. The default is false."
            },
            "fixText": {
                "type": "boolean",
                "description": "Use fixed text size. The default is false."
            },
            "scale": {
                "type": "integer",
                "description": "Print scale. The default is 100."
            },
            "scaleToPaper": { "type": "boolean" },
            "scaleFitToPage": { "type": "boolean" },
            "allColorsToBlack": { "type": "boolean" },
            "ditherOnPrinter": { "type": "boolean" },
            "usePrinterRes": { "type": "boolean" },
            "printGhost": { "type": "boolean" },
            "newSheet": { "type": "boolean" },
            "printAlignment": {
                "type": "string",
                "enum": ["leftTop", "middleTop", "rightTop", "leftMiddle", "center", "rightMiddle", "leftBottom", "middleBottom", "rightBottom"]
            },
            "printArea": {
                "type": "string",
                "description": "The area to print. The default is 'currentView'.",
                "enum": ["currentView", "entireDrawing", "marquee"]
            }
        },
        "additionalProperties": false,
        "required": [
        ]
    })";
}

GS::Optional<GS::UniString> PrintViewCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState PrintViewCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    API_PrintPars pi = {};

    GS::UniString printAreaStr;
    if (!parameters.Get ("printArea", printAreaStr) || printAreaStr == "currentView") {
        pi.printArea = PrintArea_CurrentView;
    } else if (printAreaStr == "entireDrawing") {
        pi.printArea = PrintArea_EntireDrawing;
    } else if (printAreaStr == "marquee") {
        pi.printArea = PrintArea_Marquee;
    } else {
        return CreateFailedExecutionResult (APIERR_BADPARS, "printArea parameter is invalid");
    }

    if (!parameters.Get ("grid", pi.grid)) {
        pi.grid = false;
    }
    if (!parameters.Get ("fixText", pi.fixText)) {
        pi.fixText = false;
    }
    if (!parameters.Get ("scale", pi.scale)) {
        pi.scale = 100;
    }
    parameters.Get ("scaleToPaper", pi.scaleToPaper);
    parameters.Get ("scaleFitToPage", pi.scaleFitToPage);
    parameters.Get ("allColorsToBlack", pi.allColorsToBlack);
    parameters.Get ("ditherOnPrinter", pi.ditherOnPrinter);
    parameters.Get ("usePrinterRes", pi.usePrinterRes);
    parameters.Get ("printGhost", pi.printGhost);
    parameters.Get ("newSheet", pi.newSheet);

    GS::UniString alignment;
    if (parameters.Get ("printAlignment", alignment)) {
        static const GS::UniString names[] = {
            "leftTop", "middleTop", "rightTop", "leftMiddle", "center",
            "rightMiddle", "leftBottom", "middleBottom", "rightBottom"
        };
        pi.printAlignment = APIAnc_MM;
        for (UInt32 i = 0; i < sizeof (names) / sizeof (names[0]); ++i) {
            if (alignment == names[i]) {
                pi.printAlignment = static_cast<API_AnchorID> (i);
                break;
            }
        }
    }

    const GSErrCode err = ACAPI_ProjectOperation_Print (&pi);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to print the current view.");
    }

    return CreateSuccessfulExecutionResult ();
}

RenderMarqueePdfProbeCommand::RenderMarqueePdfProbeCommand () :
    CommandBase (CommonSchema::Used)
{
}

RenderMarqueePrintCommand::RenderMarqueePrintCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String RenderMarqueePrintCommand::GetName () const
{
    return "RenderMarqueePrint";
}

GS::Optional<GS::UniString> RenderMarqueePrintCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "storyIndex": { "type": "integer", "description": "Optional floor-plan storage story to render in the background." },
            "xMin": { "type": "number" },
            "yMin": { "type": "number" },
            "xMax": { "type": "number" },
            "yMax": { "type": "number" },
            "multiStory": { "type": "boolean" },
            "scale": { "type": "integer" },
            "grid": { "type": "boolean" },
            "fixText": { "type": "boolean" },
            "allColorsToBlack": { "type": "boolean" },
            "ditherOnPrinter": { "type": "boolean" },
            "usePrinterRes": { "type": "boolean" },
            "printGhost": { "type": "boolean" },
            "newSheet": { "type": "boolean" },
            "printAlignment": { "type": "string" }
        },
        "additionalProperties": false,
        "required": ["xMin", "yMin", "xMax", "yMax"]
    })";
}

GS::Optional<GS::UniString> RenderMarqueePrintCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "storyIndex": { "type": "integer" },
            "backgroundDatabaseRestored": { "type": "boolean" },
            "temporaryMarqueeCleared": { "type": "boolean" },
            "focusChangedDuringRender": { "type": "boolean" }
        },
        "required": ["backgroundDatabaseRestored", "temporaryMarqueeCleared", "focusChangedDuringRender"]
    })";
}

GS::ObjectState RenderMarqueePrintCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 requestedStory = -1;
    double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
    bool multiStory = false;
    if (!parameters.Get ("xMin", xMin) || !parameters.Get ("yMin", yMin) ||
        !parameters.Get ("xMax", xMax) || !parameters.Get ("yMax", yMax)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "xMin, yMin, xMax and yMax are required.");
    }
    parameters.Get ("storyIndex", requestedStory);
    parameters.Get ("multiStory", multiStory);
    if (!std::isfinite (xMin) || !std::isfinite (yMin) || !std::isfinite (xMax) || !std::isfinite (yMax) ||
        xMin >= xMax || yMin >= yMax)
        return CreateFailedExecutionResult (APIERR_BADPARS, "Marquee bounds must have positive area.");

    API_SelectionInfo selectionBefore = {};
    const GSErrCode selectionErr = ACAPI_Selection_Get (&selectionBefore, nullptr, false);
    const GS::OnExit selectionBeforeGuard ([&selectionBefore] () { ReleaseSelectionInfoHandles (selectionBefore); });
    if (selectionErr != NoError && selectionErr != APIERR_NOSEL)
        return CreateErrorResponse (selectionErr, "Failed to inspect the operator focus before rendering.");
    if (selectionErr == NoError && selectionBefore.typeID == API_SelElems) {
        return CreateFailedExecutionResult (APIERR_REFUSEDCMD, "Render refuses to change marquee while an element selection is active.");
    }
    const bool hadOriginalMarquee = selectionErr == NoError &&
        (selectionBefore.typeID == API_MarqueePoly || selectionBefore.typeID == API_MarqueeHorBox || selectionBefore.typeID == API_MarqueeRotBox);

    API_DatabaseInfo startingDatabase = {};
    GSErrCode operationErr = ACAPI_Database_GetCurrentDatabase (&startingDatabase);
    if (operationErr != NoError) {
        return CreateFailedExecutionResult (operationErr, "Failed to read the starting database.");
    }

    bool databaseChanged = false;
    bool marqueeSet = false;
    GS::UniString failureMessage;
    if (requestedStory >= 0) {
        API_DatabaseInfo targetDatabase = {};
        targetDatabase.typeID = APIWind_FloorPlanID;
        targetDatabase.index = requestedStory;
        operationErr = ACAPI_Window_GetDatabaseInfo (&targetDatabase);
        if (operationErr == NoError)
            operationErr = ACAPI_Database_ChangeCurrentDatabase (&targetDatabase);
        if (operationErr != NoError)
            failureMessage = "Failed to change the current database in the background.";
        else
            databaseChanged = true;
    }

    API_SelectionInfo temporaryMarquee = {};
    temporaryMarquee.typeID = API_MarqueeHorBox;
    temporaryMarquee.multiStory = multiStory;
    temporaryMarquee.marquee.box.xMin = xMin;
    temporaryMarquee.marquee.box.yMin = yMin;
    temporaryMarquee.marquee.box.xMax = xMax;
    temporaryMarquee.marquee.box.yMax = yMax;
    temporaryMarquee.marquee.boxRotAngle = 0.0;
    if (operationErr == NoError) {
        operationErr = ACAPI_Selection_SetMarquee (&temporaryMarquee);
        if (operationErr == NoError)
            marqueeSet = true;
        else
            failureMessage = "Failed to set the temporary marquee.";
    }

    if (operationErr == NoError) {
        API_PrintPars printPars = {};
        printPars.printArea = PrintArea_Marquee;
        printPars.scale = 100;
        printPars.grid = false;
        printPars.fixText = false;
        printPars.scaleToPaper = true;
        printPars.scaleFitToPage = true;
        printPars.printAlignment = APIAnc_MM;
        parameters.Get ("scale", printPars.scale);
        parameters.Get ("grid", printPars.grid);
        parameters.Get ("fixText", printPars.fixText);
        parameters.Get ("allColorsToBlack", printPars.allColorsToBlack);
        parameters.Get ("ditherOnPrinter", printPars.ditherOnPrinter);
        parameters.Get ("usePrinterRes", printPars.usePrinterRes);
        parameters.Get ("printGhost", printPars.printGhost);
        parameters.Get ("newSheet", printPars.newSheet);
        GS::UniString alignment;
        if (parameters.Get ("printAlignment", alignment)) {
            static const GS::UniString names[] = {
                "leftTop", "middleTop", "rightTop", "leftMiddle", "center",
                "rightMiddle", "leftBottom", "middleBottom", "rightBottom"
            };
            for (UInt32 i = 0; i < sizeof (names) / sizeof (names[0]); ++i) {
                if (alignment == names[i]) {
                    printPars.printAlignment = static_cast<API_AnchorID> (i);
                    break;
                }
            }
        }
        operationErr = ACAPI_ProjectOperation_Print (&printPars);
        if (operationErr != NoError)
            failureMessage = "Failed to print the temporary marquee.";
    }

    const GSErrCode restoreDatabaseErr = databaseChanged
        ? ACAPI_Database_ChangeCurrentDatabase (&startingDatabase)
        : NoError;
    if (operationErr == NoError && restoreDatabaseErr != NoError) {
        operationErr = restoreDatabaseErr;
        failureMessage = "Print succeeded, but the starting background database could not be restored.";
    }

    bool focusChangedDuringRender = false;
    if (marqueeSet) {
        API_SelectionInfo currentSelection = {};
        const GSErrCode currentErr = ACAPI_Selection_Get (&currentSelection, nullptr, false);
        const GS::OnExit currentSelectionGuard ([&currentSelection] () { ReleaseSelectionInfoHandles (currentSelection); });
        if (currentErr != NoError || currentSelection.typeID != API_MarqueeHorBox) {
            focusChangedDuringRender = true;
        } else {
            focusChangedDuringRender = currentSelection.marquee.box.xMin != xMin ||
                currentSelection.marquee.box.yMin != yMin || currentSelection.marquee.box.xMax != xMax ||
                currentSelection.marquee.box.yMax != yMax || currentSelection.multiStory != multiStory;
        }
        if (!focusChangedDuringRender && restoreDatabaseErr == NoError) {
            API_SelectionInfo marqueeAfter = {};
            const GS::OnExit marqueeAfterGuard ([&marqueeAfter] () { ReleaseSelectionInfoHandles (marqueeAfter); });
            bool restoreReady = true;
            if (hadOriginalMarquee) {
                restoreReady = CloneSelectionInfoHandles (selectionBefore, marqueeAfter);
                if (!restoreReady) {
                    operationErr = APIERR_MEMFULL;
                    failureMessage = "Print completed, but the original marquee could not be copied safely for restoration.";
                }
            } else {
                marqueeAfter.typeID = API_SelEmpty;
            }
            if (restoreReady) {
                const GSErrCode marqueeRestoreErr = ACAPI_Selection_SetMarquee (&marqueeAfter);
                if (operationErr == NoError && marqueeRestoreErr != NoError) {
                    operationErr = marqueeRestoreErr;
                    failureMessage = "Print succeeded, but the original marquee could not be restored.";
                }
            }
        }
    }
    if (operationErr != NoError)
        return CreateFailedExecutionResult (operationErr, failureMessage);

    GS::ObjectState response;
    if (requestedStory >= 0)
        response.Add ("storyIndex", requestedStory);
    response.Add ("backgroundDatabaseRestored", true);
    response.Add ("temporaryMarqueeCleared", !focusChangedDuringRender);
    response.Add ("focusChangedDuringRender", focusChangedDuringRender);
    return response;
}

GS::String RenderMarqueePdfProbeCommand::GetName () const
{
    return "RenderMarqueePdfProbe";
}

GS::Optional<GS::UniString> RenderMarqueePdfProbeCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "storyIndex": { "type": "integer", "description": "Floor-plan storage story to render in the background." },
            "xMin": { "type": "number" },
            "yMin": { "type": "number" },
            "xMax": { "type": "number" },
            "yMax": { "type": "number" },
            "pdfPath": { "type": "string", "description": "Absolute output path for the temporary PDF." },
            "pageWidthMm": { "type": "number", "minimum": 1 },
            "pageHeightMm": { "type": "number", "minimum": 1 }
        },
        "additionalProperties": false,
        "required": ["storyIndex", "xMin", "yMin", "xMax", "yMax", "pdfPath"]
    })";
}

GS::Optional<GS::UniString> RenderMarqueePdfProbeCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "pdfPath": { "type": "string" },
            "storyIndex": { "type": "integer" },
            "backgroundDatabaseRestored": { "type": "boolean" },
            "temporaryMarqueeCleared": { "type": "boolean" }
        },
        "required": ["pdfPath", "storyIndex", "backgroundDatabaseRestored", "temporaryMarqueeCleared"]
    })";
}

GS::ObjectState RenderMarqueePdfProbeCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 storyIndex = 0;
    double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
    GS::UniString pdfPath;
    if (!parameters.Get ("storyIndex", storyIndex) || !parameters.Get ("xMin", xMin) || !parameters.Get ("yMin", yMin) ||
        !parameters.Get ("xMax", xMax) || !parameters.Get ("yMax", yMax) || !parameters.Get ("pdfPath", pdfPath)) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "storyIndex, bounds and pdfPath are required.");
    }
    if (!std::isfinite (xMin) || !std::isfinite (yMin) || !std::isfinite (xMax) || !std::isfinite (yMax) ||
        xMin >= xMax || yMin >= yMax) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Marquee bounds must have positive area.");
    }

    // A selection can hide an existing marquee from ACAPI_Selection_Get.  This first
    // proof is intentionally conservative: do not disturb a user's element selection.
    // Retain an exposed marquee's coordinate handle until cleanup so a polygonal or
    // rotated user marquee can be restored exactly.
    API_SelectionInfo selectionBefore = {};
    const GSErrCode selectionErr = ACAPI_Selection_Get (&selectionBefore, nullptr, false);
    const GS::OnExit selectionBeforeGuard ([&selectionBefore] () { ReleaseSelectionInfoHandles (selectionBefore); });
    if (selectionErr != NoError && selectionErr != APIERR_NOSEL)
        return CreateErrorResponse (selectionErr, "Failed to inspect the operator focus before the PDF render.");
    if (selectionErr == NoError && selectionBefore.typeID == API_SelElems) {
        return CreateFailedExecutionResult (APIERR_REFUSEDCMD, "Render probe refuses to change marquee while an element selection is active.");
    }
    const bool hadOriginalMarquee = selectionErr == NoError &&
        (selectionBefore.typeID == API_MarqueePoly || selectionBefore.typeID == API_MarqueeHorBox || selectionBefore.typeID == API_MarqueeRotBox);

    API_DatabaseInfo startingDatabase = {};
    GSErrCode err = ACAPI_Database_GetCurrentDatabase (&startingDatabase);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to read the starting database.");
    }

    API_DatabaseInfo targetDatabase = {};
    targetDatabase.typeID = APIWind_FloorPlanID;
    targetDatabase.index = storyIndex;
    err = ACAPI_Window_GetDatabaseInfo (&targetDatabase);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to resolve the requested floor-plan story.");
    }

    bool databaseChanged = false;
    bool marqueeSet = false;
    GSErrCode operationErr = NoError;
    GS::UniString failureMessage;

    err = ACAPI_Database_ChangeCurrentDatabase (&targetDatabase);
    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to change the current database in the background.");
    }
    databaseChanged = true;

    API_SelectionInfo temporaryMarquee = {};
    temporaryMarquee.typeID = API_MarqueeHorBox;
    temporaryMarquee.multiStory = false;
    temporaryMarquee.marquee.box.xMin = xMin;
    temporaryMarquee.marquee.box.yMin = yMin;
    temporaryMarquee.marquee.box.xMax = xMax;
    temporaryMarquee.marquee.box.yMax = yMax;
    temporaryMarquee.marquee.boxRotAngle = 0.0;
    operationErr = ACAPI_Selection_SetMarquee (&temporaryMarquee);
    if (operationErr == NoError)
        marqueeSet = true;
    else
        failureMessage = "Failed to set the temporary marquee.";

    if (operationErr == NoError) {
        float pageWidthMm = 420.0f;
        float pageHeightMm = 297.0f;
        parameters.Get ("pageWidthMm", pageWidthMm);
        parameters.Get ("pageHeightMm", pageHeightMm);
        if (pageWidthMm <= 0.0f || pageHeightMm <= 0.0f) {
            operationErr = APIERR_BADPARS;
            failureMessage = "PDF page dimensions must be positive.";
        } else {
            IO::Location pdfLocation (pdfPath);
            API_FileSavePars savePars = {};
            savePars.fileTypeID = APIFType_PdfFile;
            savePars.file = &pdfLocation;
            API_SavePars_Pdf pdfPars = {};
            pdfPars.sizeX = pageWidthMm;
            pdfPars.sizeY = pageHeightMm;
            operationErr = TAPIR_ProjectOperation_SavePdf (&savePars, &pdfPars);
            if (operationErr != NoError)
                failureMessage = "Failed to export the temporary marquee PDF.";
        }
    }

    // Restore the background database first, then restore the exact original marquee
    // (or clear ours when there was none). Neither operation changes the front window.
    const GSErrCode restoreErr = databaseChanged ? ACAPI_Database_ChangeCurrentDatabase (&startingDatabase) : NoError;
    if (operationErr == NoError && restoreErr != NoError) {
        operationErr = restoreErr;
        failureMessage = "PDF was exported, but the starting background database could not be restored.";
    }
    bool temporaryMarqueeCleared = false;
    if (marqueeSet) {
        API_SelectionInfo currentSelection = {};
        const GSErrCode currentErr = ACAPI_Selection_Get (&currentSelection, nullptr, false);
        const GS::OnExit currentSelectionGuard ([&currentSelection] () { ReleaseSelectionInfoHandles (currentSelection); });
        const bool focusChanged = currentErr != NoError || currentSelection.typeID != API_MarqueeHorBox ||
            currentSelection.marquee.box.xMin != xMin || currentSelection.marquee.box.yMin != yMin ||
            currentSelection.marquee.box.xMax != xMax || currentSelection.marquee.box.yMax != yMax ||
            currentSelection.multiStory != false;
        if (focusChanged) {
            if (operationErr == NoError) {
                operationErr = currentErr != NoError ? currentErr : APIERR_REFUSEDCMD;
                failureMessage = "PDF was exported, but the temporary marquee changed or could not be verified; it was not overwritten.";
            }
        } else {
            API_SelectionInfo marqueeAfter = {};
            const GS::OnExit marqueeAfterGuard ([&marqueeAfter] () { ReleaseSelectionInfoHandles (marqueeAfter); });
            bool restoreReady = true;
            if (hadOriginalMarquee) {
                restoreReady = CloneSelectionInfoHandles (selectionBefore, marqueeAfter);
                if (!restoreReady) {
                    operationErr = APIERR_MEMFULL;
                    failureMessage = "PDF completed, but the original marquee could not be copied safely for restoration.";
                }
            } else {
                marqueeAfter.typeID = API_SelEmpty;
            }
            if (restoreReady) {
                const GSErrCode marqueeRestoreErr = ACAPI_Selection_SetMarquee (&marqueeAfter);
                if (marqueeRestoreErr == NoError) {
                    temporaryMarqueeCleared = true;
                } else if (operationErr == NoError) {
                    operationErr = marqueeRestoreErr;
                    failureMessage = "PDF was exported, but the original marquee could not be restored.";
                }
            }
        }
    }
    if (operationErr != NoError)
        return CreateFailedExecutionResult (operationErr, failureMessage);

    return GS::ObjectState (
        "pdfPath", pdfPath,
        "storyIndex", storyIndex,
        "backgroundDatabaseRestored", true,
        "temporaryMarqueeCleared", temporaryMarqueeCleared
    );
}

RebuildViewCommand::RebuildViewCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String RebuildViewCommand::GetName () const
{
    return "RebuildView";
}

GS::Optional<GS::UniString> RebuildViewCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "regenerate": {
                "type": "boolean",
                "description": "Regenerate the view. The default is false, meaning the view will not be regenerated, but rebuilt."
            }
        },
        "additionalProperties": false,
        "required": [
        ]
    })";
}

GS::Optional<GS::UniString> RebuildViewCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState RebuildViewCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    bool regenerate = false;
    parameters.Get ("regenerate", regenerate);

    GSErrCode err = ACAPI_View_Rebuild (&regenerate);

    if (err != NoError) {
        return CreateFailedExecutionResult (err, "Failed to rebuild the view.");
    }

    return CreateSuccessfulExecutionResult ();
}
