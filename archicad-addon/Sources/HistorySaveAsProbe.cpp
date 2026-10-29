// Developer-only Save As probe (D-351): the isolated probe APX saves the open Solo project under a new
// .pln path the way the engineer's File > Save As does, so a live run can check what the history sees.
// Compiled only with TAPIR_ENABLE_HISTORY_SAVE_AS_PROBE; never part of the ordinary or customer APX.
#if defined(TAPIR_HISTORY_SAVE_AS_PROBE)

#include "HistorySaveAsProbe.hpp"
#include "MigrationHelper.hpp"
#include "NativeCrudInternal.hpp"
#include "File.hpp"

ProbeSaveProjectAsCommand::ProbeSaveProjectAsCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String ProbeSaveProjectAsCommand::GetName () const
{
    return "ProbeSaveProjectAs";
}

GS::Optional<GS::UniString> ProbeSaveProjectAsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "outputPath": { "type": "string", "minLength": 1, "description": "A new local .pln path; existing files are never overwritten." }
        },
        "additionalProperties": false,
        "required": ["outputPath"]
    })";
}

GS::Optional<GS::UniString> ProbeSaveProjectAsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "success": { "type": "boolean" },
            "scopeBefore": { "type": "string" },
            "scopeAfter": { "type": "string" },
            "saveError": { "type": "integer" }
        }
    })";
}

GS::ObjectState ProbeSaveProjectAsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::UniString outputPath;
    parameters.Get ("outputPath", outputPath);
    IO::Location location (outputPath);
    IO::Name lastLocalName;
    if (outputPath.IsEmpty () || location.GetStatus () != NoError || location.GetLastLocalName (&lastLocalName) != NoError
        || lastLocalName.GetExtension ().Compare ("pln", CaseInsensitive) != GS::UniString::Equal) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "outputPath must be a local .pln file");
    }
    if (IO::File (location, IO::File::OnNotFound::Fail).GetStatus () == NoError) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "Refusing to overwrite an existing file");
    }
    const GS::Optional<GS::UniString> before = NativeCrudInternal::CurrentSoloFileScope ();
    if (before.IsEmpty ()) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "The probe needs an open saved Solo .pln project");
    }

    API_FileSavePars savePars = {};
    savePars.fileTypeID = APIFType_PlanFile;
    savePars.file = &location;
#ifdef ServerMainVers_2800
    const GSErrCode error = ACAPI_ProjectOperation_Save (&savePars);
#else
    const GSErrCode error = ACAPI_Automate (APIDo_SaveID, &savePars, nullptr);
#endif

    const GS::Optional<GS::UniString> after = NativeCrudInternal::CurrentSoloFileScope ();
    GS::ObjectState response = CreateSuccessfulExecutionResult ();
    response.Add ("scopeBefore", before.Get ());
    if (after.HasValue ()) response.Add ("scopeAfter", after.Get ());
    response.Add ("saveError", static_cast<Int32> (error));
    return response;
}

#endif
