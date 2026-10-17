#include "NativeDimensionCreateWire.hpp"
#include "NativeDimensionCreateWireObjectState.hpp"
#include "NativeDimensionCreateCandidateFacts.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <document.h>
#include <pointer.h>
#include <stringbuffer.h>
#include <writer.h>

#include <iostream>
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>

namespace {

bool Require (const bool condition, const char* message)
{
	if (!condition) std::cerr << message << '\n';
	return condition;
}

std::string ReadCandidateFixture ()
{
	std::ifstream input (NATIVE_DIMENSION_CANDIDATE_FIXTURE_PATH, std::ios::binary);
	if (!input) throw std::runtime_error ("captured candidate reader fixture could not be opened");
	std::ostringstream content;
	content << input.rdbuf ();
	return content.str ();
}

bool ReplaceOnce (std::string& source, const std::string& marker, const std::string& from, const std::string& to)
{
	const std::size_t markerAt = source.find (marker);
	if (markerAt == std::string::npos) return false;
	const std::size_t at = source.find (from, markerAt + marker.size ());
	if (at == std::string::npos) return false;
	source.replace (at, from.size (), to);
	return true;
}

bool RemoveStringFieldOnce (std::string& source, const std::string& marker, const std::string& field)
{
	const std::size_t markerAt = source.find (marker);
	if (markerAt == std::string::npos) return false;
	const std::string key = "\"" + field + "\"";
	const std::size_t keyAt = source.find (key, markerAt + marker.size ());
	if (keyAt == std::string::npos) return false;
	const std::size_t colon = source.find (':', keyAt + key.size ());
	if (colon == std::string::npos) return false;
	std::size_t valueAt = colon + 1;
	while (valueAt < source.size () && (source[valueAt] == ' ' || source[valueAt] == '\t' || source[valueAt] == '\r' || source[valueAt] == '\n')) ++valueAt;
	if (valueAt >= source.size () || source[valueAt] != '"') return false;
	std::size_t end = valueAt + 1;
	bool escaped = false;
	for (; end < source.size (); ++end) {
		if (escaped) { escaped = false; continue; }
		if (source[end] == '\\') { escaped = true; continue; }
		if (source[end] == '"') break;
	}
	if (end >= source.size ()) return false;
	++end;
	while (end < source.size () && (source[end] == ' ' || source[end] == '\t' || source[end] == '\r' || source[end] == '\n')) ++end;
	if (end < source.size () && source[end] == ',') {
		source.erase (keyAt, end + 1 - keyAt);
		return true;
	}
	const std::size_t previousComma = source.rfind (',', keyAt);
	if (previousComma == std::string::npos || previousComma < markerAt) return false;
	source.erase (previousComma, end - previousComma);
	return true;
}

GS::ObjectState Parse (const std::string& json)
{
	GS::ObjectState state;
	if (JSON::ConvertToObjectState (GS::UniString (json.c_str (), CC_UTF8), state) != NoError)
		throw std::runtime_error ("wire JSON fixture did not parse");
	return state;
}

std::string Serialize (const GS::ObjectState& state)
{
	GS::UniString json;
	if (JSON::CreateFromObjectState (state, json, false) != NoError)
		throw std::runtime_error ("producer ObjectState did not serialize");
	return json.ToCStr (CC_UTF8).Get ();
}

bool TestProducerStringEmission (const NativeDimensionCreateWire::Context& templateContext)
{
	using namespace NativeDimensionCreateWire;
	bool ok = true;
	// Keep a direct SDK overload witness beside the production-helper round trip:
	// ObjectState's std::string constructor selects its enumerable/list overload.
	const std::string sdkOverloadValue = "ab";
	const GS::ObjectState directSdkOverload ("value", sdkOverloadValue);
	const std::string directSdkJson = Serialize (directSdkOverload);
	const GS::ObjectState directSdkParsed = Parse (directSdkJson);
	std::string directSdkString;
	if (directSdkJson.find ("\"value\":[97,98]") == std::string::npos)
		std::cerr << "DIRECT_STD_STRING_OBJECTSTATE_JSON=" << directSdkJson << '\n';
	ok &= Require (directSdkJson.find ("\"value\":[97,98]") != std::string::npos
		&& !ReadExactString (directSdkParsed, "value", directSdkString),
		"the compiled SDK std::string ObjectState overload serializes as an integer list, and strict string parsing rejects it");
	Context context = templateContext;
	context.projectPath = "C:/dimension/模型 - floor.pln";
	context.fileScopeKey = "saved:c:/scratch/mutation-r1.pln";
	context.addonLocation = "C:/Archicad/插件.apx";
	context.windowDatabaseGuid = "null";
	const GS::ObjectState emittedContext = EmitContextObjectState (context, 7, 2);
	const std::string contextJson = Serialize (emittedContext);
	const GS::ObjectState parsedContext = Parse (contextJson);
	Context decodedContext;
	const bool contextParsed = ParseContext (parsedContext, decodedContext);
	if (!contextParsed) std::cerr << "PRODUCER_CONTEXT_JSON=" << contextJson << '\n';
	ok &= Require (contextParsed,
		"the shared producer context serializer must round-trip declared strings as strings through the SDK JSON conversion and strict parser");
	ok &= Require (decodedContext.projectPath == context.projectPath && decodedContext.addonLocation == context.addonLocation
		&& decodedContext.windowDatabaseGuid == "null" && decodedContext.currentDatabaseGuid == context.currentDatabaseGuid,
		"context strings preserve exact Unicode and the literal null sentinel");
	Int32 windowIndex = -1, storyIndex = -1;
	bool candidateEnabled = false, faultEnabled = false;
	ok &= Require (parsedContext.Get ("windowIndex", windowIndex) && windowIndex == 7
		&& parsedContext.Get ("storyIndex", storyIndex) && storyIndex == 2
		&& parsedContext.Get ("candidateEnabled", candidateEnabled) && candidateEnabled
		&& parsedContext.Get ("postCreateFaultEnabled", faultEnabled) && !faultEnabled,
		"context indices remain Int32 and profile switches remain Boolean");

	for (const std::string status : { "created", "rejected", "unresolved", "rollbackObserved" }) {
		Request request;
		request.operationId = "операция-1";
		request.fileScopeKey = "";
		request.requestHash = std::string (64, 'a');
		request.recipe = "null";
		request.storyIndex = "2";
		GS::ObjectState result ("kind", "dimensionCreateCandidateResult", "status", GS::UniString (status.c_str (), CC_UTF8));
		AddCandidateResultCorrelation (result, request, 2);
		const std::string resultJson = Serialize (result);
		const GS::ObjectState parsedResult = Parse (resultJson);
		std::string operationId, fileScopeKey, requestHash, recipe, parsedStatus;
		const bool correlationStrings = ReadExactString (parsedResult, "operationId", operationId) && operationId == request.operationId
			&& ReadExactString (parsedResult, "fileScopeKey", fileScopeKey) && fileScopeKey == request.fileScopeKey
			&& ReadExactString (parsedResult, "requestHash", requestHash) && requestHash == request.requestHash
			&& ReadExactString (parsedResult, "recipe", recipe) && recipe == request.recipe
			&& ReadExactString (parsedResult, "status", parsedStatus) && parsedStatus == status;
		if (!correlationStrings) std::cerr << "PRODUCER_RESULT_JSON[" << status << "]=" << resultJson << '\n';
		ok &= Require (correlationStrings,
			"candidate result correlation and every terminal status retain their declared string values");
		Int32 resultStory = -1;
		ok &= Require (parsedResult.Get ("storyIndex", resultStory) && resultStory == 2,
			"candidate result story index remains Int32");
	}

	for (const std::string databaseGuid : { std::string (), std::string ("null"), std::string ("01234567-89ab-cdef-0123-456789abcdef"), std::string ("数据库-guid") }) {
		const GS::ObjectState observed = EmitObservedDatabaseIdentityObjectState (3, databaseGuid);
		const std::string observedJson = Serialize (observed);
		const GS::ObjectState parsedObserved = Parse (observedJson);
		std::string decodedGuid;
		Int32 databaseType = -1;
		const bool databaseGuidString = ReadExactString (parsedObserved, "databaseGuid", decodedGuid) && decodedGuid == databaseGuid;
		if (!databaseGuidString) std::cerr << "PRODUCER_DATABASE_JSON=" << observedJson << '\n';
		ok &= Require (databaseGuidString
			&& parsedObserved.Get ("typeId", databaseType) && databaseType == 3,
			"observed database GUID remains an exact string while the SDK type id remains Int32");
	}
	const GS::ObjectState arrayString = Parse ("{\"operationId\":[\"not-a-string\"]}");
	std::string rejectedArray;
	ok &= Require (!ReadExactString (arrayString, "operationId", rejectedArray),
		"strict string parsing rejects an array instead of coercing it into a declared string");
	return ok;
}

std::string EnvelopeJson (const std::string& wireHash, const bool extraKey = false, const std::string& databaseGuid = "87654321-4321-4321-4321-210fedcba987")
{
	const std::string extra = extraKey ? ",\"unexpected\":true" : "";
	return "{\"schemaVersion\":\"native-dimension-create-wire/v1\",\"hashAlgorithm\":\"sha256-utf8-lp32be-v1\","
		"\"request\":{\"kind\":\"dimensionCreateCandidateRequest\",\"schemaVersion\":\"native-dimension-create-candidate-request-v1\",\"operation\":\"create\",\"elementType\":\"Dimension\",\"operationId\":\"d1\",\"fileScopeKey\":\"saved:c:/scratch/mutation-r1.pln\",\"requestHash\":\"d9b82ea85744f0e4462007ad9b8a4de8c4d16a14489803da08b8cd9d486290fc\",\"binding\":{\"bindingId\":\"binding-ac25\",\"instanceId\":\"ac25-instance\",\"runtimeKey\":\"pid:25\",\"projectFingerprint\":\"saved:c:/scratch/mutation-r1.pln\",\"projectGeneration\":7,\"archicadMajor\":25},\"description\":\"Пример dimension\",\"recipe\":\"linear-static-automatic-measured-two-point-v1\",\"placement\":{\"storyIndex\":2}},"
		"\"expectedContext\":{\"kind\":\"nativeDimensionCreateCandidateContext\",\"schemaVersion\":\"native-dimension-create-context/v1\",\"runId\":\"r1\",\"projectPath\":\"C:/scratch/mutation-r1.pln\",\"fileScopeKey\":\"saved:c:/scratch/mutation-r1.pln\",\"processId\":\"1234\",\"processStartUtcTicks\":\"639273408713980709\",\"processStartEpoch\":\"dotnet-ticks-utc-v1\",\"archicadMajor\":\"25\",\"buildId\":\"build-25\",\"addonLocation\":\"C:/Archicad/addon.apx\",\"windowType\":\"FloorPlan\",\"windowIndex\":2,\"windowDatabaseGuid\":\"null\",\"currentDatabaseGuid\":\""
		+ databaseGuid + "\",\"storyIndex\":2,\"contractVersion\":\"native-crud-v2\",\"recipe\":\"linear-static-automatic-measured-two-point-v1\",\"policyVersion\":\"native-dimension-create-policy/v1\",\"candidateEnabled\":true,\"postCreateFaultEnabled\":false}" + extra + ",\"wireHash\":\"" + wireHash + "\"}";
}

}

int main ()
{
	using namespace NativeDimensionCreateWire;
	bool ok = true;
	const std::string abc = "abc";
	ok &= Require (Hex (Sha256 (reinterpret_cast<const std::uint8_t*> (abc.data ()), abc.size ()))
		== "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 implementation must match the standard abc vector");

	GS::ObjectState blank;
	Request request;
	Context context;
	std::string hash;
	ok &= Require (!ParseEnvelope (blank, request, context, hash), "empty envelope must fail closed");

	const std::string candidateWireHash = "ded2632ff4e7bec89d3750a7e939a5b43752f03e6aa5f5ff350500ec64ec02f3";
	GS::ObjectState valid = Parse (EnvelopeJson (candidateWireHash));
	ok &= Require (ParseEnvelope (valid, request, context, hash), "actual closed ObjectState parser should accept the complete native wire shape");
	if (hash != candidateWireHash) ok &= Require (false, "native parser wire hash vector differs");
	ok &= Require (ValidateRequestRecipe (request).accepted, "fixed request recipe should pass independently parsed request validation");
	ok &= Require (LogicalRequestHash (request) == "d9b82ea85744f0e4462007ad9b8a4de8c4d16a14489803da08b8cd9d486290fc", "logical v1 requestHash must remain byte-compatible with TS stableCandidateJson");
	ok &= Require (ValidateExpectedContext (context, context).accepted, "exact attested context should pass");
	ok &= TestProducerStringEmission (context);
	Request wrongProjectRequest = request;
	wrongProjectRequest.projectFingerprint = "C:/Scratch/another-project.pln";
	ok &= Require (!RequestMatchesContext (wrongProjectRequest, context), "binding projectFingerprint cannot contradict request fileScope and actual context project path");
	wrongProjectRequest.projectFingerprint = "C:/Scratch/Mutation-R1.pln";
	ok &= Require (RequestMatchesContext (wrongProjectRequest, context), "case-normalized bare project path should match the exact saved solo fileScope");
	Request controlRequest = request;
	controlRequest.description = "tabs\tline\ncarriage\rback\bform\f\x01";
	controlRequest.requestHash = LogicalRequestHash (controlRequest);
	ok &= Require (JsonQuoteUtf8 (controlRequest.description) == "\"tabs\\tline\\ncarriage\\rback\\bform\\f\\u0001\"",
		"control escaping must match JavaScript JSON.stringify exactly");
	ok &= Require (LogicalRequestHash (controlRequest) == "c9008578c7e605b6c5b40066db21c053384a20360e8a3ecd6e372172fdbd5811",
		"control characters must match the JS JSON.stringify logical v1 hash vector");
	ok &= Require (WireHash (controlRequest, context) == "124aa6008e6efb14d86d501408eb86e3533a185ba77b28814b10667296dceb0e",
		"control characters must match the TS context-bound wire hash vector");

	const std::string wrongHash = std::string (candidateWireHash).replace (0, 1, "0");
	GS::ObjectState badDigest = Parse (EnvelopeJson (wrongHash));
	ok &= Require (!ParseEnvelope (badDigest, request, context, hash), "wire hash mutation must be rejected before policy");
	GS::ObjectState extra = Parse (EnvelopeJson (candidateWireHash, true));
	ok &= Require (!ParseEnvelope (extra, request, context, hash), "unknown envelope keys must be rejected");
	GS::ObjectState nullDatabase = Parse (EnvelopeJson (candidateWireHash, false, "00000000-0000-0000-0000-000000000000"));
	ok &= Require (!ParseEnvelope (nullDatabase, request, context, hash), "null current database GUID must be rejected");
	GS::ObjectState outerNul = Parse ("{\"operationId\":\"prefix\\u0000suffix\"}");
	std::string exactOuter;
	ok &= Require (!ReadExactString (outerNul, "operationId", exactOuter), "outer identity strings reject embedded NUL before UTF-8 C-string conversion");
	Request truncatedDescriptionRequest = request;
	truncatedDescriptionRequest.description = "prefix";
	truncatedDescriptionRequest.requestHash = LogicalRequestHash (truncatedDescriptionRequest);
	const std::string truncatedDescriptionWireHash = WireHash (truncatedDescriptionRequest, context);
	std::string embeddedNullJson = EnvelopeJson (candidateWireHash);
	const std::string descriptionToken = "\"description\":\"Пример dimension\"";
	const std::size_t descriptionAt = embeddedNullJson.find (descriptionToken);
	if (descriptionAt == std::string::npos) {
		ok &= Require (false, "embedded-NUL vector description field must exist");
	} else {
		embeddedNullJson.replace (descriptionAt, descriptionToken.size (), "\"description\":\"prefix\\u0000suffix\"");
		const std::string oldRequestHash = "d9b82ea85744f0e4462007ad9b8a4de8c4d16a14489803da08b8cd9d486290fc";
		const std::size_t requestHashAt = embeddedNullJson.find (oldRequestHash);
		const std::size_t wireHashAt = embeddedNullJson.rfind (candidateWireHash);
		if (requestHashAt == std::string::npos || wireHashAt == std::string::npos) {
			ok &= Require (false, "embedded-NUL vector hashes must exist");
		} else {
			embeddedNullJson.replace (requestHashAt, oldRequestHash.size (), truncatedDescriptionRequest.requestHash);
			embeddedNullJson.replace (wireHashAt, candidateWireHash.size (), truncatedDescriptionWireHash);
		}
		GS::ObjectState embeddedNull = Parse (embeddedNullJson);
		ok &= Require (!ParseEnvelope (embeddedNull, request, context, hash), "embedded U+0000 must be rejected before NUL-terminated UTF-8 conversion");
	}
	Request replacementDescriptionRequest = request;
	replacementDescriptionRequest.description = "\xEF\xBF\xBD";
	replacementDescriptionRequest.requestHash = LogicalRequestHash (replacementDescriptionRequest);
	const std::string replacementDescriptionWireHash = WireHash (replacementDescriptionRequest, context);
	std::string malformedUnicodeJson = EnvelopeJson (candidateWireHash);
	const std::string malformedRequestHash = "d9b82ea85744f0e4462007ad9b8a4de8c4d16a14489803da08b8cd9d486290fc";
	const std::size_t malformedDescriptionAt = malformedUnicodeJson.find (descriptionToken);
	const std::size_t malformedRequestHashAt = malformedUnicodeJson.find (malformedRequestHash);
	const std::size_t malformedWireHashAt = malformedUnicodeJson.rfind (candidateWireHash);
	if (malformedDescriptionAt == std::string::npos || malformedRequestHashAt == std::string::npos || malformedWireHashAt == std::string::npos) {
		ok &= Require (false, "malformed-surrogate vector fields must exist");
	} else {
		malformedUnicodeJson.replace (malformedDescriptionAt, descriptionToken.size (), "\"description\":\"\\ud800\"");
		malformedUnicodeJson.replace (malformedRequestHashAt, malformedRequestHash.size (), replacementDescriptionRequest.requestHash);
		malformedUnicodeJson.replace (malformedWireHashAt, candidateWireHash.size (), replacementDescriptionWireHash);
		bool malformedAccepted = false;
		try {
			GS::ObjectState malformedUnicode = Parse (malformedUnicodeJson);
			malformedAccepted = ParseEnvelope (malformedUnicode, request, context, hash);
		} catch (const std::exception&) {
			// A parser-level refusal is also a correct fail-closed outcome.
		}
		ok &= Require (!malformedAccepted, "lone UTF-16 surrogate must not alias the replacement-character hash");
	}
	Request basementRequest = request;
	basementRequest.storyIndex = "-2";
	ok &= Require (ValidateRequestRecipe (basementRequest).accepted, "a valid basement story remains representable as canonical signed Int32");
	Request minimumNativeStory = request;
	minimumNativeStory.storyIndex = "-32768";
	ok &= Require (ValidateRequestRecipe (minimumNativeStory).accepted, "the minimum native Int16 story index should be accepted");
	Request maximumNativeStory = request;
	maximumNativeStory.storyIndex = "32767";
	ok &= Require (ValidateRequestRecipe (maximumNativeStory).accepted, "the maximum native Int16 story index should be accepted");
	Request belowNativeStory = request;
	belowNativeStory.storyIndex = "-32769";
	ok &= Require (!ValidateRequestRecipe (belowNativeStory).accepted, "story indices below the SDK Int16 range must be refused before dispatch");
	Request aboveNativeStory = request;
	aboveNativeStory.storyIndex = "32768";
	ok &= Require (!ValidateRequestRecipe (aboveNativeStory).accepted, "story indices above the SDK Int16 range must be refused before dispatch");
	GS::ObjectState changedCurrent = Parse (EnvelopeJson (candidateWireHash));
	Request sameRequest;
	Context expected;
	std::string ignored;
	ok &= Require (ParseEnvelope (changedCurrent, sameRequest, expected, ignored), "baseline request should parse");
	Context actual = expected;
	actual.storyIndex = "-4";
	ok &= Require (!ValidateExpectedContext (expected, actual).accepted, "story drift must refuse exact context policy");
	actual = expected;
	actual.currentDatabaseGuid = "00000000-0000-0000-0000-000000000000";
	ok &= Require (!ValidateExpectedContext (expected, actual).accepted, "null actual current database must refuse");
	actual = expected;
	actual.windowDatabaseGuid = "null";
	ok &= Require (ValidateExpectedContext (expected, actual).accepted, "SDK Floor Plan null window database sentinel is distinct from current database identity");
	actual = expected;
	actual.processStartUtcTicks = "639273408713980710";
	ok &= Require (!ValidateExpectedContext (expected, actual).accepted, "process start tick drift must refuse exact context policy");
	Context unavailable = expected;
	unavailable.windowType = "unavailable";
	unavailable.windowIndex = "-1";
	unavailable.windowDatabaseGuid = "null";
	unavailable.currentDatabaseGuid = "00000000-0000-0000-0000-000000000000";
	unavailable.storyIndex = "-1";
	ok &= Require (!ValidateExpectedContext (expected, unavailable).accepted, "unavailable context remains a diagnostic observation and cannot authorize candidate dispatch");
	const auto parsesContextIndices = [&] (const std::string& windowIndex, const std::string& storyIndex) {
		try {
			std::string json = EnvelopeJson (candidateWireHash);
			const std::string oldWindow = "\"windowIndex\":2";
			const std::string oldStory = "\"storyIndex\":2";
			if (!ReplaceOnce (json, "\"expectedContext\"", oldWindow, "\"windowIndex\":" + windowIndex)
				|| !ReplaceOnce (json, "\"expectedContext\"", oldStory, "\"storyIndex\":" + storyIndex)) return false;
			GS::ObjectState envelope = Parse (json);
			const GS::ObjectState* contextObject = envelope.Get ("expectedContext");
			Context parsed;
			return contextObject != nullptr && ParseContext (*contextObject, parsed)
				&& parsed.windowIndex == windowIndex && parsed.storyIndex == storyIndex;
		} catch (const std::exception&) {
			return false;
		}
	};
	ok &= Require (parsesContextIndices ("-2147483648", "-2147483648"), "native context parser accepts exact minimum Int32 indices");
	ok &= Require (parsesContextIndices ("2147483647", "2147483647"), "native context parser accepts exact maximum Int32 indices");
	ok &= Require (!parsesContextIndices ("-2147483649", "2"), "native context parser refuses a window index below Int32");
	ok &= Require (!parsesContextIndices ("2147483648", "2"), "native context parser refuses a window index above Int32");
	ok &= Require (!parsesContextIndices ("2", "2147483648"), "native context parser refuses a story index above Int32");
	ok &= Require (!parsesContextIndices ("1.5", "2"), "native context parser refuses non-integer context indices");
	ok &= Require (!parsesContextIndices ("\"2\"", "2"), "native context parser refuses numeric strings for Int32 indices");
	std::string diagnosticJson = EnvelopeJson (candidateWireHash);
	const std::string currentDatabaseToken = "\"currentDatabaseGuid\":\"87654321-4321-4321-4321-210fedcba987\"";
	const std::size_t currentDatabaseAt = diagnosticJson.find (currentDatabaseToken);
	if (currentDatabaseAt == std::string::npos) {
		ok &= Require (false, "diagnostic unavailable-context vector contains the actual current-database field");
	} else {
		diagnosticJson.replace (currentDatabaseAt, currentDatabaseToken.size (), "\"currentDatabaseGuid\":\"00000000-0000-0000-0000-000000000000\"");
		diagnosticJson.replace (diagnosticJson.find ("\"windowType\":\"FloorPlan\""), std::string ("\"windowType\":\"FloorPlan\"").size (), "\"windowType\":\"unavailable\"");
		diagnosticJson.replace (diagnosticJson.find ("\"windowIndex\":2"), std::string ("\"windowIndex\":2").size (), "\"windowIndex\":-1");
		diagnosticJson.replace (diagnosticJson.find ("\"storyIndex\":2"), std::string ("\"storyIndex\":2").size (), "\"storyIndex\":-1");
		GS::ObjectState diagnosticEnvelope = Parse (diagnosticJson);
		const GS::ObjectState* diagnosticContext = diagnosticEnvelope.Get ("expectedContext");
		Context observed;
		ok &= Require (diagnosticContext != nullptr && ParseContext (*diagnosticContext, observed)
			&& observed.windowType == "unavailable" && observed.currentDatabaseGuid == "00000000-0000-0000-0000-000000000000",
			"strict native diagnostic parser preserves an emitted unavailable-window and null-current-database observation");
	}

	const std::string capturedJson = ReadCandidateFixture ();
	GS::ObjectState capturedResponse = Parse (capturedJson);
	GS::Array<GS::ObjectState> capturedRows;
	GS::ObjectState capturedRow, capturedElementId, capturedDatabase, capturedDatabaseId;
	GS::UniString capturedGuid, capturedDatabaseGuid;
	Int32 capturedStory = -1;
	ok &= Require (capturedResponse.Get ("elements", capturedRows) && capturedRows.GetSize () == 1,
		"the captured T4a2 reader fixture contains exactly one real Dimension row");
	if (capturedRows.GetSize () == 1) capturedRow = capturedRows[0];
	ok &= Require (capturedRow.Get ("elementId", capturedElementId) && capturedElementId.Get ("guid", capturedGuid)
		&& capturedRow.Get ("database", capturedDatabase) && capturedDatabase.Get ("databaseId", capturedDatabaseId)
		&& capturedDatabaseId.Get ("guid", capturedDatabaseGuid) && capturedRow.Get ("storyIndex", capturedStory),
		"captured row identity and database fields are available for the candidate facts guard");
	ok &= Require (FixedCandidateReaderRowMatches (capturedRow, capturedGuid, capturedDatabaseGuid, capturedStory),
		"the candidate-specific in-transaction guard accepts the exact captured fixed-recipe reader row");
	const auto rejectsCandidateMutation = [&] (const std::string& marker, const std::string& from, const std::string& to, const char* label) {
		std::string mutated = capturedJson;
		if (!ReplaceOnce (mutated, marker, from, to)) return Require (false, label);
		GS::ObjectState changedResponse = Parse (mutated);
		GS::Array<GS::ObjectState> rows;
		if (!changedResponse.Get ("elements", rows) || rows.GetSize () != 1) return Require (false, label);
		return Require (!FixedCandidateReaderRowMatches (rows[0], capturedGuid, capturedDatabaseGuid, capturedStory), label);
	};
	const auto rejectsCandidateFieldRemoval = [&] (const std::string& field, const char* label) {
		std::string mutated = capturedJson;
		if (!RemoveStringFieldOnce (mutated, "\"elements\"", field)) return Require (false, label);
		GS::ObjectState changedResponse = Parse (mutated);
		GS::Array<GS::ObjectState> rows;
		if (!changedResponse.Get ("elements", rows) || rows.GetSize () != 1) return Require (false, label);
		return Require (!FixedCandidateReaderRowMatches (rows[0], capturedGuid, capturedDatabaseGuid, capturedStory), label);
	};
	ok &= rejectsCandidateMutation ("\"geometryAndSettings\"", "\"linePen\": 85", "\"linePen\": 86",
		"an actual nested linePen mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"defaultNote\"", "\"x\": 0", "\"x\": 1",
		"an actual default-note position mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"defaultNote\"", "\"content\": \"\"", "\"content\": \"unexpected\"",
		"an actual default-note content mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"dimensionPreferences\"", "\"formatCoverage\": \"complete-project-context\"", "\"formatCoverage\": \"unavailable\"",
		"missing complete project dimension format coverage fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"witnessValue\": 0.3", "\"witnessValue\": 0.4",
		"an actual nested point witness mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"elements\"", "\"hasMemo\": true", "\"hasMemo\": false",
		"an actual row with hasMemo false fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"database\"", "\"databaseIndex\": 0", "\"databaseIndex\": \"0\"",
		"an actual row with a non-integer database index fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"geometryAndSettings\"", "\"pointCount\": 2", "\"pointCount\": 1",
		"an actual row with an inconsistent geometry point count fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"sourceFidelityCoverage\"", "\"status\": \"unproven\"", "\"status\": \"available\"",
		"an actual row cannot overstate source-fidelity coverage");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"contentStatus\": \"unicode-content-available\"", "\"contentStatus\": \"unavailable\"",
		"an actual point-note Unicode availability mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"contentSource\": \"memo-owned-unicode-contentUStr\"", "\"contentSource\": \"fixed-buffer-content\"",
		"an actual point-note Unicode provenance mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"accessStatus\": \"unicode-content-available\"", "\"accessStatus\": \"unavailable\"",
		"an actual native-content access-status mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"accessSource\": \"memo-owned-unicode-contentUStr\"", "\"accessSource\": \"fixed-buffer-content\"",
		"an actual native-content provenance mutation fails the candidate postcondition");
	ok &= rejectsCandidateMutation ("\"defaultNote\"", "\"status\": \"unproven\"", "\"status\": \"available\"",
		"an actual default-note source-expression claim cannot be promoted");
	ok &= rejectsCandidateMutation ("\"defaultNote\"", "\"status\": \"unavailable\"", "\"status\": \"available\"",
		"an actual default-note display gap cannot be promoted");
	ok &= rejectsCandidateMutation ("\"linePosition\"", "\"x\": 20", "\"x\": 20.000000001",
		"a line-position delta within the former native tolerance is rejected to match TS exact comparison");
	ok &= rejectsCandidateMutation ("\"linePosition\"", "\"x\": 20", "\"x\": 20.0000001",
		"a line-position delta outside the former native tolerance is rejected");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"measuredValueFromPreviousPoint\": 4", "\"measuredValueFromPreviousPoint\": 4.0000001",
		"a measured-value delta within the former native tolerance is rejected to match TS exact comparison");
	ok &= rejectsCandidateMutation ("\"orderedPoints\"", "\"measuredValueFromPreviousPoint\": 4", "\"measuredValueFromPreviousPoint\": 4.000002",
		"a measured-value delta outside the former native tolerance is rejected");
	ok &= rejectsCandidateFieldRemoval ("fingerprint", "deleting the mandatory native fingerprint is rejected before commit");
	std::string capturedFingerprint;
	const std::string fingerprintMarker = "\"fingerprint\": \"";
	const std::size_t fingerprintStart = capturedJson.find (fingerprintMarker);
	if (fingerprintStart != std::string::npos) {
		const std::size_t valueStart = fingerprintStart + fingerprintMarker.size ();
		const std::size_t valueEnd = capturedJson.find ('\"', valueStart);
		if (valueEnd != std::string::npos) capturedFingerprint = capturedJson.substr (valueStart, valueEnd - valueStart);
	}
	ok &= Require (!capturedFingerprint.empty (), "the captured successful reader row contains its actual fingerprint");
	if (!capturedFingerprint.empty ()) {
		ok &= rejectsCandidateMutation ("\"elements\"", "\"fingerprint\": \"" + capturedFingerprint + "\"", "\"fingerprint\": \"not-a-fingerprint\"",
			"a malformed native fingerprint is rejected before commit for the captured SDK major");
	}
	ok &= rejectsCandidateMutation ("\"elements\"", "\"fingerprintAlgorithm\": \"native-dimension-fnv1a-64-typed-facts-v2\"", "\"fingerprintAlgorithm\": \"unsupported\"",
		"a malformed fingerprint algorithm is rejected before commit");
	ok &= rejectsCandidateFieldRemoval ("fingerprintAlgorithm", "deleting the mandatory fingerprint algorithm is rejected before commit");
	ok &= rejectsCandidateFieldRemoval ("fingerprintCoverage", "deleting mandatory fingerprint coverage is rejected before commit");
	ok &= rejectsCandidateMutation ("\"elements\"", "\"fingerprintCoverage\": \"partial\"", "\"fingerprintCoverage\": \"complete\"",
		"unsupported fingerprint coverage is rejected before commit");
	ok &= rejectsCandidateMutation ("\"measurement\"", "\"source\": \"per-point-native-dimVal\"", "\"source\": null",
		"a successful linear reader row missing its mandatory measurement source type is rejected before commit");
	ok &= rejectsCandidateMutation ("\"defaultNote\"", "\"pen\": 20", "\"pen\": null",
		"a successful reader row missing a typed required note fact is rejected before commit");

	Context actualCompiled = expected;
	actualCompiled.candidateEnabled = TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE ? "true" : "false";
	actualCompiled.postCreateFaultEnabled = TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT ? "true" : "false";
	int createSentinel = 0;
	if (CompiledProfileMatches (actualCompiled)) ++createSentinel;
	ok &= Require (createSentinel == (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE ? 1 : 0),
		"the shared compiled candidate policy gate drives the zero-create source-policy sentinel");
	Context spoofed = actualCompiled;
	spoofed.candidateEnabled = actualCompiled.candidateEnabled == "true" ? "false" : "true";
	if (CompiledProfileMatches (spoofed)) ++createSentinel;
	ok &= Require (createSentinel == (TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE ? 1 : 0),
		"caller-supplied candidate flags cannot change compiled producer policy");
	spoofed = actualCompiled;
	spoofed.postCreateFaultEnabled = actualCompiled.postCreateFaultEnabled == "true" ? "false" : "true";
	ok &= Require (!CompiledProfileMatches (spoofed), "caller-supplied post-create fault flags cannot change compiled producer policy");
	Context impossibleFault = actualCompiled;
	impossibleFault.candidateEnabled = "false";
	impossibleFault.postCreateFaultEnabled = "true";
	ok &= Require (!CompiledProfileMatches (impossibleFault), "post-create fault can never authorize without its compiled candidate profile");

	return ok ? 0 : 1;
}
