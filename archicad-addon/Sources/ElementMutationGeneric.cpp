#include "ElementMutationInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ElementGDLParameterCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "NotificationCommands.hpp"

namespace ElementMutationInternal {

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

} // namespace ElementMutationInternal
