// D-347 stage 3b (D-350): deleted elements come back from the `.mod` checkpoint their deletion saved.
//
// One undoable command places a private copy of the module at offset zero on the item story and breaks the
// link at once (an unbroken placement cannot be deleted, histrs-ac28-r2), finds the placement's per-file
// translation and each item's element (NativeModuleRestoreMatch), and drags exactly those elements back by
// the translation. The footprint is the whole-project GUID difference around the command: it must be exactly
// the restored elements. A failure is a known refusal only when Archicad rolled the command back and the GUID
// set and the module hotlink nodes are equal to the ones before it (R1); anything else is an unknown outcome.

#include "NativeCrudInternal.hpp"

#include "MigrationHelper.hpp"
#include "NativeDimensionCreateWire.hpp"
#include "NativeModuleRestoreMatch.hpp"
#include "File.hpp"
#include "FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

GSErrCode AllGuids (GS::HashSet<API_Guid>& result)
{
    GS::Array<API_Guid> guids;
    const GSErrCode error = ACAPI_Element_GetElemList (API_ZombieElemID, &guids);
    if (error != NoError) return error;
    for (const API_Guid& guid : guids) result.Add (guid);
    return NoError;
}

// Every module hotlink node of the project; an error means the list is unreadable (never an empty proof).
GSErrCode ModuleNodes (GS::HashSet<API_Guid>& result)
{
    API_HotlinkTypeID type = APIHotlink_Module;
    API_Guid root = APINULLGuid;
    GSErrCode error = ACAPI_Hotlink_GetHotlinkRootNodeGuid (&type, &root);
    if (error != NoError) return error;
    GS::HashTable<API_Guid, GS::Array<API_Guid>> tree;
    error = ACAPI_Hotlink_GetHotlinkNodeTree (&root, &tree);
    if (error != NoError) return error;
    for (const API_Guid& node : tree.Keys ()) {
        result.Add (node);
        for (const API_Guid& child : tree[node]) result.Add (child);
    }
    return NoError;
}

bool SameSet (const GS::HashSet<API_Guid>& left, const GS::HashSet<API_Guid>& right)
{
    if (left.GetSize () != right.GetSize ()) return false;
    for (const API_Guid& guid : left)
        if (!right.Contains (guid)) return false;
    return true;
}

GSErrCode BoxOf (const API_Guid& guid, API_Elem_Head& header, API_Box3D& bounds)
{
    header = {};
    header.guid = guid;
    const GSErrCode error = ACAPI_Element_GetHeader (&header);
    return error != NoError ? error : ACAPI_Element_CalcBounds (&header, &bounds);
}

GS::UniString Utf8 (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// The SHA-256 of a whole file, lowercase hex; empty when it cannot be read.
std::string FileSha256 (const IO::Location& location)
{
    IO::File file (location, IO::File::OnNotFound::Fail);
    if (file.GetStatus () != NoError || file.Open (IO::File::ReadMode) != NoError) return {};
    UInt64 length = 0;
    std::vector<char> bytes;
    bool ok = file.GetDataLength (&length) == NoError && length < (UInt64 (1) << 31);
    if (ok) {
        bytes.resize (static_cast<std::size_t> (length));
        USize read = 0;
        ok = length == 0 || (file.ReadBin (bytes.data (), static_cast<USize> (length), &read) == NoError && read == length);
    }
    file.Close ();
    if (!ok) return {};
    return NativeDimensionCreateWire::Hex (NativeDimensionCreateWire::Sha256 (reinterpret_cast<const std::uint8_t*> (bytes.data ()), bytes.size ()));
}

struct Item {
    API_Guid absent = APINULLGuid;
    NativeModuleRestoreMatch::Recorded recorded;
};

} // namespace

namespace NativeCrudInternal {

ModuleRestoreOutcome ExecuteModuleRestore (
    const GS::UniString& operationId,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    ModuleRestoreOutcome outcome;
    const auto refuse = [&outcome] (const char* code, const GS::UniString& message) {
        outcome.refusal = NativeContractError (code, message + " Nothing was placed.");
        return outcome;
    };

    GS::UniString modulePath;
    GS::UniString sha256;
    Int32 storyIndex = 0;
    items[0].Get ("moduleRestore")->Get ("modulePath", modulePath);
    items[0].Get ("moduleRestore")->Get ("sha256", sha256);
    items[0].Get ("placement")->Get ("storyIndex", storyIndex);
    std::vector<Item> restored;
    std::vector<NativeModuleRestoreMatch::Recorded> recorded;
    for (const GS::ObjectState& item : items) {
        Item row;
        row.absent = GetGuidFromObjectState (*item.Get ("absentElementId"));
        row.recorded.type = Utf8 (typeName);
        const GS::ObjectState* bounds = item.Get ("moduleRestore")->Get ("bounds");
        bounds->Get ("minX", row.recorded.bounds.xMin);
        bounds->Get ("minY", row.recorded.bounds.yMin);
        bounds->Get ("maxX", row.recorded.bounds.xMax);
        bounds->Get ("maxY", row.recorded.bounds.yMax);
        restored.push_back (row);
        recorded.push_back (row.recorded);
    }

    API_DatabaseInfo database = {};
    if (ACAPI_Database_GetCurrentDatabase (&database) != NoError || database.typeID != APIWind_FloorPlanID)
        return refuse ("MODULE_RESTORE_DATABASE", "A module restore runs on the floor plan only.");

    // R3/R8: a private copy per attempt (AC25 refuses a module file it already placed in this session); the
    // copy is placed only when its own digest is the request's.
    const IO::Location source (modulePath);
    const std::string operationBytes = Utf8 (operationId);
    const std::string suffix = NativeDimensionCreateWire::Hex (NativeDimensionCreateWire::Sha256 (
        reinterpret_cast<const std::uint8_t*> (operationBytes.data ()), operationBytes.size ())).substr (0, 16);
    const IO::Location copy (modulePath + GS::UniString (".") + Utf8 (suffix) + GS::UniString (".place.mod"));
    bool copyExists = false;
    if (IO::fileSystem.Contains (copy, &copyExists) != NoError || copyExists)
        return refuse ("MODULE_RESTORE_COPY_FAILED", "The private copy of the module already exists or cannot be checked.");
    if (IO::fileSystem.Copy (source, copy) != NoError)
        return refuse ("MODULE_RESTORE_COPY_FAILED", "The module could not be copied.");
    bool copyRemoved = false;
    const GS::OnExit removeCopy ([&copy, &copyRemoved] () {
        if (!copyRemoved) IO::fileSystem.Delete (copy);
    });
    if (FileSha256 (copy) != Utf8 (sha256))
        return refuse ("MODULE_RESTORE_DIGEST_MISMATCH", "The module file is not the one its checkpoint recorded.");

    GS::HashSet<API_Guid> before;
    GS::HashSet<API_Guid> nodesBefore;
    if (AllGuids (before) != NoError)
        return refuse ("MODULE_RESTORE_INVENTORY_FAILED", "The project's elements could not be listed before the restore.");
    // R4: without a readable node list nothing could prove that no hotlink node remains; refuse before placing.
    if (ModuleNodes (nodesBefore) != NoError)
        return refuse ("MODULE_RESTORE_INVENTORY_FAILED", "The project's module hotlinks could not be listed before the restore.");

    NativeModuleRestoreMatch::Result match;
    GS::Array<API_Guid> created;
    double maxDeviation = 0.0;
    GS::UniString failureCode;
    GS::UniString failureMessage;
    const auto fail = [&failureCode, &failureMessage] (const char* code, const GS::UniString& message, const GSErrCode error) {
        failureCode = code;
        failureMessage = message;
        return error != NoError ? error : APIERR_GENERAL;
    };
    const GSErrCode undoError = ACAPI_CallUndoableCommand ("Restore From Checkpoint", [&] () -> GSErrCode {
        API_HotlinkNode node = {};
        node.type = APIHotlink_Module;
        node.storyRangeType = APIHotlink_AllStories;
        node.sourceLocation = new IO::Location (copy);
        node.refFloorInd = static_cast<short> (storyIndex);
        GS::ucsncpy (node.name, L("Checkpoint restore"), API_UniLongNameLen - 1);
        GSErrCode error = NoError;
#ifdef ServerMainVers_2800
        error = ACAPI_Hotlink_GetHotlinkStoryInfo (&node);
        if (error != NoError) return fail ("MODULE_RESTORE_PLACE_FAILED", "GetHotlinkStoryInfo failed.", error);
        error = ACAPI_Hotlink_CreateHotlinkNode (&node);
#else
        error = ACAPI_Database (APIDb_CreateHotlinkNodeID, &node);
#endif
        if (error != NoError) return fail ("MODULE_RESTORE_PLACE_FAILED", "CreateHotlinkNode failed.", error);
        API_Element hotlink = {};
#ifdef ServerMainVers_2600
        hotlink.header.type = API_HotlinkID;
#else
        hotlink.header.typeID = API_HotlinkID;
#endif
        hotlink.header.floorInd = static_cast<short> (storyIndex);
        hotlink.header.layer = ACAPI_CreateAttributeIndex (1);
        hotlink.hotlink.type = APIHotlink_Module;
        hotlink.hotlink.hotlinkNodeGuid = node.guid;
        hotlink.hotlink.transformation.tmx[0] = 1.0;
        hotlink.hotlink.transformation.tmx[5] = 1.0;
        hotlink.hotlink.transformation.tmx[10] = 1.0;
        hotlink.hotlink.ignoreTopFloorLinks = false;
        hotlink.hotlink.relinkWallOpenings = false;
        hotlink.hotlink.adjustLevelDiffs = false;
        error = ACAPI_Element_Create (&hotlink, nullptr);
        if (error != NoError) return fail ("MODULE_RESTORE_PLACE_FAILED", "The module instance could not be placed.", error);
#ifdef ServerMainVers_2800
        error = ACAPI_Hotlink_BreakHotlinkNode (&node.guid);
#else
        error = ACAPI_Database (APIDb_BreakHotlinkNodeID, &node.guid);
#endif
        if (error != NoError) return fail ("MODULE_RESTORE_PLACE_FAILED", "The module link could not be broken.", error);

        GS::HashSet<API_Guid> placedSet;
        if (AllGuids (placedSet) != NoError) return fail ("MODULE_RESTORE_INVENTORY_FAILED", "The placed elements could not be listed.", NoError);
        std::vector<NativeModuleRestoreMatch::Placed> placed;
        GS::Array<API_Guid> placedGuids;
        for (const API_Guid& guid : placedSet) {
            if (before.Contains (guid)) continue;
            API_Elem_Head header = {};
            API_Box3D box = {};
            NativeModuleRestoreMatch::Placed row;
            row.guid = Utf8 (APIGuidToString (guid));
            if (BoxOf (guid, header, box) == NoError) {
                row.type = Utf8 (CanonicalElementTypeName (GetElementTypeNonLocalizedName (GetElemTypeId (header))));
                row.bounds = { box.xMin, box.yMin, box.xMax, box.yMax };
            } else {
                // Unreadable placed element: no type and non-finite bounds, so the match refuses the footprint.
                row.type = "Unknown";
                row.bounds = { NAN, NAN, NAN, NAN };
            }
            placed.push_back (row);
            placedGuids.Push (guid);
        }
        match = NativeModuleRestoreMatch::Match (recorded, placed);
        if (!match.ok) return fail (match.code.c_str (), Utf8 (match.message), NoError);

        GS::Array<API_Neig> neigs;
        for (const std::size_t index : match.placedIndex) neigs.Push (API_Neig (placedGuids[static_cast<UIndex> (index)]));
        if (match.dx != 0.0 || match.dy != 0.0) {
            API_EditPars editPars = {};
            editPars.typeID = APIEdit_Drag;
            editPars.withDelete = true;
            editPars.begC = { 0.0, 0.0, 0.0 };
            editPars.endC = { -match.dx, -match.dy, 0.0 };
            error = ACAPI_Element_Edit (&neigs, editPars);
            if (error != NoError) return fail ("MODULE_RESTORE_MOVE_FAILED", "The placed elements could not be moved back.", error);
        }
        for (UIndex index = 0; index < neigs.GetSize (); ++index) {
            const API_Guid guid = neigs[index].guid != APINULLGuid ? neigs[index].guid : placedGuids[static_cast<UIndex> (match.placedIndex[index])];
            API_Elem_Head header = {};
            API_Box3D box = {};
            if (BoxOf (guid, header, box) != NoError || header.floorInd != storyIndex
                || CanonicalElementTypeName (GetElementTypeNonLocalizedName (GetElemTypeId (header))) != typeName)
                return fail ("MODULE_RESTORE_MISMATCH", "A restored element is not readable on its story with its type.", NoError);
            const double deviation = NativeModuleRestoreMatch::Deviation ({ box.xMin, box.yMin, box.xMax, box.yMax }, restored[index].recorded.bounds, 0.0, 0.0);
            if (!(deviation <= NativeModuleRestoreMatch::Tolerance))
                return fail ("MODULE_RESTORE_MISMATCH", "A restored element is not at its recorded bounds.", NoError);
            maxDeviation = std::max (maxDeviation, deviation);
            created.Push (guid);
        }
        // The footprint is exactly the restored elements: no Hotlink instance, no other placed element.
        GS::HashSet<API_Guid> afterSet;
        if (AllGuids (afterSet) != NoError) return fail ("MODULE_RESTORE_INVENTORY_FAILED", "The restored elements could not be listed.", NoError);
        GS::HashSet<API_Guid> createdSet;
        for (const API_Guid& guid : created) createdSet.Add (guid);
        GS::HashSet<API_Guid> added;
        for (const API_Guid& guid : afterSet)
            if (!before.Contains (guid)) added.Add (guid);
        if (!SameSet (added, createdSet) || afterSet.GetSize () != before.GetSize () + createdSet.GetSize ())
            return fail ("MODULE_RESTORE_MISMATCH", "The placement left elements besides the restored ones.", NoError);
        return NoError;
    });

    // R1: the independent postcondition after the command.
    GS::HashSet<API_Guid> after;
    GS::HashSet<API_Guid> nodesAfter;
    const GSErrCode afterError = AllGuids (after);
    const GSErrCode nodesAfterError = ModuleNodes (nodesAfter);
    // Both a success and a proven rollback need a readable node list equal to the one before.
    const bool nodesUnchanged = nodesAfterError == NoError && SameSet (nodesAfter, nodesBefore);
    bool complete = false;
    if (undoError == NoError && failureCode.IsEmpty () && afterError == NoError && nodesUnchanged) {
        GS::HashSet<API_Guid> added;
        for (const API_Guid& guid : after)
            if (!before.Contains (guid)) added.Add (guid);
        GS::HashSet<API_Guid> createdSet;
        for (const API_Guid& guid : created) createdSet.Add (guid);
        complete = created.GetSize () == items.GetSize () && SameSet (added, createdSet) && after.GetSize () == before.GetSize () + createdSet.GetSize ();
    }
    outcome.rollbackProven = undoError != NoError && afterError == NoError && SameSet (after, before) && nodesUnchanged;

    GS::ObjectState& result = outcome.mutationResult;
    const auto& elements = result.AddList<GS::ObjectState> ("elements");
    GS::ObjectState evidence;
    evidence.Add ("dx", match.dx);
    evidence.Add ("dy", match.dy);
    evidence.Add ("maxDeviation", maxDeviation);
    evidence.Add ("candidates", static_cast<Int32> (match.candidates));
    evidence.Add ("rollbackProven", outcome.rollbackProven);
    evidence.Add ("nodesReadable", nodesAfterError == NoError);
    if (!failureCode.IsEmpty ()) evidence.Add ("code", failureCode);
    const auto& recreates = evidence.AddList<GS::ObjectState> ("recreates");
    if (complete) {
        for (UIndex index = 0; index < created.GetSize (); ++index) {
            elements (GS::ObjectState ("elementId", CreateGuidObjectState (created[index])));
            recreates (GS::ObjectState ("absent", APIGuidToString (restored[index].absent), "created", APIGuidToString (created[index])));
        }
    }
    copyRemoved = IO::fileSystem.Delete (copy) == NoError;
    evidence.Add ("copyRemoved", copyRemoved);
    result.Add ("moduleRestore", evidence);
    result.Add ("mutationComplete", complete);
    result.Add ("partial", !complete);
    if (undoError != NoError) {
        result.Add ("transactionRolledBack", true);
        result.Add ("transactionError", static_cast<Int32> (undoError));
    }
    if (!complete) {
        result.Add ("failureStage", "module-restore");
        result.Add ("error", GS::ObjectState (
            "code", static_cast<Int32> (undoError != NoError ? undoError : APIERR_GENERAL),
            "message", failureCode.IsEmpty ()
                ? GS::UniString ("MODULE_RESTORE_POSTCONDITION: the restore could not be proven from the project's elements.")
                : failureCode + ": " + failureMessage));
    }
    return outcome;
}

} // namespace NativeCrudInternal
