#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <cctype>
#include <string>

using namespace NativeCrudInternal;

namespace NativeCrudInternal {

bool IsSupportedNativeType (const API_ElemTypeID typeID)
{
    switch (typeID) {
        case API_ZoneID:
        case API_WallID:
        case API_ColumnID:
        case API_TextID:
        case API_LineID:
        case API_ArcID:
        // Archicad exposes a full circle as a distinct Circle element in
        // some detail/discovery responses.  The versioned wire contract
        // intentionally normalizes that primitive to Arc, but it must still
        // pass the native type gate before CanonicalElementTypeName can do
        // the normalization.
        case API_CircleID:
        case API_HatchID:
        case API_DoorID:
        case API_WindowID:
            return true;
        default:
            return false;
    }
}

GS::UniString CanonicalElementTypeName (const GS::UniString& typeName)
{
    // Archicad reports a full circle as Circle while the versioned native
    // contract deliberately exposes Arc as the single circular primitive.
    return GetElementTypeFromNonLocalizedName (typeName) == API_CircleID
        ? GS::UniString ("Arc")
        : typeName;
}

GS::Optional<GS::UniString> CurrentSoloFileScope ()
{
    API_ProjectInfo projectInfo = {};
    if (ACAPI_ProjectOperation_Project (&projectInfo) != NoError)
        return {};
    if (projectInfo.untitled || projectInfo.teamwork || projectInfo.projectPath == nullptr)
        return {};

    GS::UniString path (*projectInfo.projectPath);
    path.ReplaceAll ("\\", "/");
    path = path.ToLowerCase ();
    const std::string pathUtf8 (path.ToCStr (CC_UTF8).Get ());
    if (pathUtf8.size () < 4 || pathUtf8.compare (pathUtf8.size () - 4, 4, ".pln") != 0)
        return {};
    GS::UniString scope ("saved:");
    scope.Append (path);
    return scope;
}

GS::Optional<GS::UniString> ValidateScope (const GS::ObjectState& parameters)
{
    GS::UniString expected;
    if (!parameters.Get ("fileScopeKey", expected) || expected.IsEmpty ())
        return GS::UniString ("fileScopeKey is required.");
    const GS::Optional<GS::UniString> current = CurrentSoloFileScope ();
    if (current.IsEmpty ())
        return GS::UniString ("Native CRUD is limited to a saved Solo PLN project.");
    if (expected != current.Get ())
        return GS::UniString::Printf ("fileScopeKey does not match the opened Solo PLN (expected %T).", current.Get ().ToPrintf ());
    return {};
}

GS::ObjectState ScopeError (const GS::Optional<GS::UniString>& error)
{
    return CreateErrorResponse (APIERR_BADPARS, error.Get ());
}

// Some native contract failures need a stable, machine-readable string code
// rather than an Archicad GSErrCode.  Keep the normal error envelope so the
// TypeScript gateway can classify the failure without guessing from a human
// message (notably for an operation-id replay with a different request).
GS::ObjectState NativeContractError (const char* code, const GS::UniString& message)
{
    GS::ObjectState error;
    error.Add ("code", code);
    error.Add ("message", message);
    return GS::ObjectState ("error", error);
}

GS::Optional<GS::UniString> ValidateContractVersion (const GS::ObjectState& parameters)
{
    GS::UniString requested;
    if (!parameters.Get ("contractVersion", requested) || requested.IsEmpty ())
        return GS::UniString::Printf ("contractVersion is required (expected %s).", ContractVersion);
    if (requested != ContractVersion)
        return GS::UniString::Printf ("Unsupported native CRUD contractVersion (expected %s).", ContractVersion);
    return {};
}

// Internal snapshot/readback collections carry the public element-id shape
// (`elementId: { guid }`), while a few lower-level helpers historically
// received a bare `{ guid }` object. Accept both forms at this boundary so
// exact GUID comparisons cannot silently turn into APINULLGuid.
API_Guid GetRequestedElementGuid (const GS::ObjectState& value)
{
    const GS::ObjectState* nested = value.Get ("elementId");
    return nested != nullptr ? GetGuidFromObjectState (*nested) : GetGuidFromObjectState (value);
}

bool IsHex64 (const GS::UniString& value)
{
    const std::string text (value.ToCStr (CC_UTF8).Get ());
    if (text.size () != 64) return false;
    for (const unsigned char character : text) {
        if (std::isxdigit (character) == 0) return false;
    }
    return true;
}

std::string LowerAscii (const GS::UniString& value)
{
    // UniString performs Unicode case mapping; lowercasing the UTF-16 value
    // before converting to UTF-8 avoids corrupting multibyte characters.
    return std::string (value.ToLowerCase ().ToCStr (CC_UTF8).Get ());
}

} // namespace NativeCrudInternal
