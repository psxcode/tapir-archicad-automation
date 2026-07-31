#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"

// Instance info file lifecycle.
//
// The patched Tapir add-on writes a small JSON file per running Archicad
// process so the MCP server can discover instances without HTTP port scanning:
//
//   %LOCALAPPDATA%\ArchicadTapirMcp\ports\{pid}.json
//
// containing { pid, port, startedAt, projectName?, projectPath?,
// projectLocation?, isTeamwork, isUntitled }. The file is refreshed on project
// open/new/save events and deleted when the add-on is unloaded.

namespace InstanceInfoFile {

// Writes (or refreshes) the {pid}.json file with the current pid, JSON server
// port, process start time and project info. Safe to call repeatedly.
GSErrCode WriteInstanceInfo ();

// Deletes the {pid}.json file written by WriteInstanceInfo. Ignores
// not-found so it is safe to call during add-on unload even if the file was
// never written or was already removed by the MCP reader's garbage collection.
GSErrCode Delete ();

} // namespace InstanceInfoFile
