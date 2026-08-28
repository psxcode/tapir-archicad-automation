#include "SchemaDefinitions.hpp"
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "RS.hpp"
#include "ResourceIds.hpp"
#include "OnExit.hpp"

static GS::UniString GetResourceFileContent (short resId)
{
    GSHandle gsHandle = RSLoadResource ('FILE', ACAPI_GetOwnResModule (), resId);
    if (gsHandle == nullptr || *gsHandle == nullptr)
        return {};
    const GS::OnExit guard ([&gsHandle] () {
        if (gsHandle != nullptr)
            BMKillHandle (&gsHandle);
    });
    GS::UniString fileContent (*gsHandle, BMGetHandleSize (gsHandle));
    return fileContent;
}

GS::Optional<GS::UniString> GetCommonSchemaDefinitions ()
{
    static const GS::Optional<GS::UniString> commonSchemaDefinitions = [] () {
        // Schema getters are called by Archicad after ownership of the
        // command object has transferred to the host.  Never let resource or
        // allocation failures escape that virtual callback boundary.
        try {
            const GS::UniString content = GetResourceFileContent (ID_COMMON_SCHEMA_DEFINITIONS_FILE);
            if (content.IsEmpty ())
                return GS::Optional<GS::UniString> ();
            return GS::Optional<GS::UniString> (content);
        } catch (...) {
            return GS::Optional<GS::UniString> ();
        }
    } ();
    return commonSchemaDefinitions;
}
