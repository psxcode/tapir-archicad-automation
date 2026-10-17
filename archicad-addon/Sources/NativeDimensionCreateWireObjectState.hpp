#pragma once

#include "NativeDimensionCreateWire.hpp"
#include "ObjectState.hpp"
#include "UniString.hpp"

#include <algorithm>
#include <cctype>

namespace NativeDimensionCreateWire {

// Shared producer serializers are deliberately defined next to the strict
// ObjectState readers so the SDK-linked test executable can exercise the same
// producer boundary used by ExtendedElementCommands.
inline GS::UniString EmitUtf8String (const std::string& value)
{
	return GS::UniString (value.c_str (), CC_UTF8);
}

inline GS::ObjectState EmitContextObjectState (const Context& context, const Int32 windowIndex, const Int32 storyIndex)
{
	return GS::ObjectState (
		"kind", EmitUtf8String (context.kind), "schemaVersion", EmitUtf8String (context.schemaVersion), "runId", EmitUtf8String (context.runId),
		"projectPath", EmitUtf8String (context.projectPath), "fileScopeKey", EmitUtf8String (context.fileScopeKey),
		"processId", EmitUtf8String (context.processId), "processStartUtcTicks", EmitUtf8String (context.processStartUtcTicks),
		"processStartEpoch", EmitUtf8String (context.processStartEpoch), "archicadMajor", EmitUtf8String (context.archicadMajor),
		"buildId", EmitUtf8String (context.buildId), "addonLocation", EmitUtf8String (context.addonLocation),
		"windowType", EmitUtf8String (context.windowType), "windowIndex", windowIndex,
		"windowDatabaseGuid", EmitUtf8String (context.windowDatabaseGuid), "currentDatabaseGuid", EmitUtf8String (context.currentDatabaseGuid),
		"storyIndex", storyIndex, "contractVersion", EmitUtf8String (context.contractVersion), "recipe", EmitUtf8String (context.recipe),
		"policyVersion", EmitUtf8String (context.policyVersion),
		"candidateEnabled", context.candidateEnabled == "true",
		"postCreateFaultEnabled", context.postCreateFaultEnabled == "true");
}

inline GS::ObjectState EmitObservedDatabaseIdentityObjectState (const Int32 databaseType, const std::string& databaseGuid)
{
	return GS::ObjectState ("typeId", databaseType, "databaseGuid", EmitUtf8String (databaseGuid));
}

inline void AddCandidateResultCorrelation (GS::ObjectState& result, const Request& request, const Int32 storyIndex)
{
	result.Add ("operationId", EmitUtf8String (request.operationId));
	result.Add ("fileScopeKey", EmitUtf8String (request.fileScopeKey));
	result.Add ("requestHash", EmitUtf8String (request.requestHash));
	result.Add ("recipe", EmitUtf8String (request.recipe));
	result.Add ("storyIndex", storyIndex);
}

inline bool IsCanonicalUnsigned (const std::string& value)
{
	if (value.empty () || (value.size () > 1 && value[0] == '0')) return false;
	for (const unsigned char character : value) if (!std::isdigit (character)) return false;
	return true;
}

inline bool IsGuidText (const std::string& value, const bool allowNull)
{
	if (allowNull && value == "null") return true;
	if (value.size () != 36) return false;
	for (std::size_t index = 0; index < value.size (); ++index) {
		if (index == 8 || index == 13 || index == 18 || index == 23) {
			if (value[index] != '-') return false;
		} else if (!std::isxdigit (static_cast<unsigned char> (value[index]))) return false;
	}
	return true;
}

inline bool ReadExactString (const GS::ObjectState& object, const char* key, std::string& target)
{
	GS::UniString value;
	if (!object.Get (key, value)) return false;
	// ToCStr(...).Get() is NUL-terminated. Reject embedded U+0000 before
	// converting so a suffix cannot disappear from the hashed/validated value.
	for (UIndex index = 0; index < value.GetLength (); ++index) {
		const GS::UniChar::Layout codeUnit = value.GetChar (index).operator GS::UniChar::Layout ();
		if (codeUnit == 0) return false;
		if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
			if (index + 1 >= value.GetLength ()) return false;
			const GS::UniChar::Layout low = value.GetChar (index + 1).operator GS::UniChar::Layout ();
			if (low < 0xdc00 || low > 0xdfff) return false;
			++index;
		} else if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) {
			return false;
		}
	}
	target = value.ToCStr (CC_UTF8).Get ();
	return true;
}

inline bool ParseRequest (const GS::ObjectState& object, Request& result)
{
	const GS::HashSet<GS::String> keys = object.GetFieldNames ();
	if (keys.GetSize () != 11) return false;
	const char* required[] = { "kind", "schemaVersion", "operation", "elementType", "operationId", "fileScopeKey", "requestHash", "binding", "description", "recipe", "placement" };
	for (const char* key : required) if (!object.Contains (key)) return false;
	if (!(ReadExactString (object, "kind", result.kind)
		&& ReadExactString (object, "schemaVersion", result.schemaVersion)
		&& ReadExactString (object, "operation", result.operation)
		&& ReadExactString (object, "elementType", result.elementType)
		&& ReadExactString (object, "operationId", result.operationId)
		&& ReadExactString (object, "fileScopeKey", result.fileScopeKey)
		&& ReadExactString (object, "requestHash", result.requestHash)
		&& ReadExactString (object, "description", result.description)
		&& ReadExactString (object, "recipe", result.recipe))) return false;
	const GS::ObjectState* binding = object.Get ("binding");
	const GS::ObjectState* placement = object.Get ("placement");
	if (binding == nullptr || placement == nullptr || binding->GetFieldNames ().GetSize () != 6 || placement->GetFieldNames ().GetSize () != 1
		|| !binding->Contains ("bindingId") || !binding->Contains ("instanceId") || !binding->Contains ("runtimeKey")
		|| !binding->Contains ("projectFingerprint") || !binding->Contains ("projectGeneration") || !binding->Contains ("archicadMajor")
		|| !placement->Contains ("storyIndex")) return false;
	Int64 projectGeneration = -1;
	Int32 archicadMajor = 0, storyIndex = 0;
	if (!ReadExactString (*binding, "bindingId", result.bindingId)
		|| !ReadExactString (*binding, "instanceId", result.instanceId)
		|| !ReadExactString (*binding, "runtimeKey", result.runtimeKey)
		|| !ReadExactString (*binding, "projectFingerprint", result.projectFingerprint)
		|| !binding->Get ("projectGeneration", projectGeneration)
		|| !binding->Get ("archicadMajor", archicadMajor)
		|| !placement->Get ("storyIndex", storyIndex)) return false;
	result.projectGeneration = std::to_string (projectGeneration);
	result.archicadMajor = std::to_string (archicadMajor);
	result.storyIndex = std::to_string (storyIndex);
	return ValidateRequestRecipe (result).accepted
		&& result.requestHash.size () == 64
		&& std::all_of (result.requestHash.begin (), result.requestHash.end (), [] (const unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
		&& projectGeneration >= 0 && static_cast<std::uint64_t> (projectGeneration) <= 9007199254740991ULL
		&& IsCanonicalUnsigned (result.projectGeneration) && IsCanonicalUnsigned (result.archicadMajor)
		&& IsCanonicalSignedInt32 (result.storyIndex)
		&& LogicalRequestHash (result) == result.requestHash;
}

inline bool ParseContext (const GS::ObjectState& object, Context& result)
{
	const GS::HashSet<GS::String> keys = object.GetFieldNames ();
	if (keys.GetSize () != 21) return false;
	const char* required[] = { "kind", "schemaVersion", "runId", "projectPath", "fileScopeKey", "processId", "processStartUtcTicks", "processStartEpoch", "archicadMajor", "buildId", "addonLocation", "windowType", "windowIndex", "windowDatabaseGuid", "currentDatabaseGuid", "storyIndex", "contractVersion", "recipe", "policyVersion", "candidateEnabled", "postCreateFaultEnabled" };
	for (const char* key : required) if (!object.Contains (key)) return false;
	if (!(ReadExactString (object, "kind", result.kind)
		&& ReadExactString (object, "schemaVersion", result.schemaVersion)
		&& ReadExactString (object, "runId", result.runId)
		&& ReadExactString (object, "projectPath", result.projectPath)
		&& ReadExactString (object, "fileScopeKey", result.fileScopeKey)
		&& ReadExactString (object, "processId", result.processId)
		&& ReadExactString (object, "processStartUtcTicks", result.processStartUtcTicks)
		&& ReadExactString (object, "processStartEpoch", result.processStartEpoch)
		&& ReadExactString (object, "archicadMajor", result.archicadMajor)
		&& ReadExactString (object, "buildId", result.buildId)
		&& ReadExactString (object, "addonLocation", result.addonLocation)
		&& ReadExactString (object, "windowType", result.windowType)
		&& ReadExactString (object, "windowDatabaseGuid", result.windowDatabaseGuid)
		&& ReadExactString (object, "currentDatabaseGuid", result.currentDatabaseGuid)
		&& ReadExactString (object, "contractVersion", result.contractVersion)
		&& ReadExactString (object, "recipe", result.recipe)
		&& ReadExactString (object, "policyVersion", result.policyVersion))) return false;
	bool candidateEnabled = false, postCreateFaultEnabled = false;
	if (!object.Get ("candidateEnabled", candidateEnabled) || !object.Get ("postCreateFaultEnabled", postCreateFaultEnabled)) return false;
	result.candidateEnabled = candidateEnabled ? "true" : "false";
	result.postCreateFaultEnabled = postCreateFaultEnabled ? "true" : "false";
	Int32 windowIndex = 0, storyIndex = 0;
	if (!object.Get ("windowIndex", windowIndex) || !object.Get ("storyIndex", storyIndex)) return false;
	result.windowIndex = std::to_string (windowIndex);
	result.storyIndex = std::to_string (storyIndex);
	return result.kind == "nativeDimensionCreateCandidateContext"
		&& result.schemaVersion == "native-dimension-create-context/v1"
		&& !result.runId.empty () && !result.projectPath.empty () && !result.fileScopeKey.empty ()
		&& IsCanonicalUnsigned (result.processId) && IsCanonicalUnsigned (result.processStartUtcTicks)
		&& result.processStartEpoch == "dotnet-ticks-utc-v1"
		&& (result.archicadMajor == "25" || result.archicadMajor == "28")
		&& !result.buildId.empty () && !result.addonLocation.empty ()
		&& IsGuidText (result.windowDatabaseGuid, true) && IsGuidText (result.currentDatabaseGuid, true)
		&& (result.windowType == "FloorPlan" || (result.windowType == "unavailable" && result.windowIndex == "-1" && result.windowDatabaseGuid == "null"))
		&& result.contractVersion == "native-crud-v2" && result.recipe == Recipe
		&& result.policyVersion == "native-dimension-create-policy/v1";
}

inline bool RequestMatchesContext (const Request& request, const Context& context)
{
	std::string projectScope, requestFingerprint;
	return CanonicalSavedProjectScope (context.projectPath, projectScope)
		&& CanonicalSavedProjectScope (request.projectFingerprint, requestFingerprint)
		&& projectScope == context.fileScopeKey && requestFingerprint == request.fileScopeKey
		&& request.fileScopeKey == context.fileScopeKey && request.archicadMajor == context.archicadMajor
		&& request.storyIndex == context.storyIndex && request.recipe == context.recipe
		&& context.currentDatabaseGuid != "00000000-0000-0000-0000-000000000000";
}

inline bool ParseEnvelope (const GS::ObjectState& object, Request& request, Context& context, std::string& wireHash)
{
	const GS::HashSet<GS::String> keys = object.GetFieldNames ();
	if (keys.GetSize () != 5 || !object.Contains ("schemaVersion") || !object.Contains ("hashAlgorithm")
		|| !object.Contains ("request") || !object.Contains ("expectedContext") || !object.Contains ("wireHash")) return false;
	GS::UniString schema, algorithm;
	const GS::ObjectState* requestObject = object.Get ("request");
	const GS::ObjectState* contextObject = object.Get ("expectedContext");
	if (!object.Get ("schemaVersion", schema) || schema != SchemaVersion
		|| !object.Get ("hashAlgorithm", algorithm) || algorithm != HashAlgorithm
		|| requestObject == nullptr || contextObject == nullptr
		|| !ParseRequest (*requestObject, request) || !ParseContext (*contextObject, context)
		|| !ReadExactString (object, "wireHash", wireHash) || wireHash.size () != 64) return false;
	return std::all_of (wireHash.begin (), wireHash.end (), [] (const unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
		&& WireHash (request, context) == wireHash && RequestMatchesContext (request, context);
}

} // namespace NativeDimensionCreateWire
