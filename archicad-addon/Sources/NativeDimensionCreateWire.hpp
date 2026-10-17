#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>
#include <cctype>

// Cross-language contract: native-dimension-create-wire-v1 uses SHA-256 over
// a domain-separated sequence of UTF-8 fields. Each field is preceded by its
// unsigned 32-bit big-endian byte length. Fields appear in the exact order in
// CanonicalWireBytes; nullable window GUID is encoded as the literal "null".
// Integers use canonical base-10 ASCII with no leading zeroes (except "0").
namespace NativeDimensionCreateWire {

constexpr const char* SchemaVersion = "native-dimension-create-wire/v1";
constexpr const char* HashAlgorithm = "sha256-utf8-lp32be-v1";
constexpr const char* RequestKind = "dimensionCreateCandidateRequest";
constexpr const char* InputKind = "dimensionCreateCandidate";
constexpr const char* ReceiptKind = "dimensionCreateCandidateReceipt";
constexpr const char* Recipe = "linear-static-automatic-measured-two-point-v1";

struct Request {
	std::string kind;
	std::string schemaVersion;
	std::string operation;
	std::string elementType;
	std::string operationId;
	std::string fileScopeKey;
	std::string requestHash;
	std::string bindingId;
	std::string instanceId;
	std::string runtimeKey;
	std::string projectFingerprint;
	std::string projectGeneration;
	std::string archicadMajor;
	std::string description;
	std::string recipe;
	std::string storyIndex;
};

struct Context {
	std::string kind;
	std::string schemaVersion;
	std::string runId;
	std::string projectPath;
	std::string fileScopeKey;
	std::string processId;
	std::string processStartUtcTicks;
	std::string processStartEpoch;
	std::string archicadMajor;
	std::string buildId;
	std::string addonLocation;
	std::string windowType;
	std::string windowIndex;
	std::string windowDatabaseGuid;
	std::string currentDatabaseGuid;
	std::string storyIndex;
	std::string contractVersion;
	std::string recipe;
	std::string policyVersion;
	std::string candidateEnabled;
	std::string postCreateFaultEnabled;
};

#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE 0
#endif
#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT 0
#endif

inline bool CompiledCandidateProfileEnabled ()
{
	return TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE == 1
		&& TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT <= 1
		&& (!TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT || TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE);
}

inline bool CompiledPostCreateFaultEnabled ()
{
	return CompiledCandidateProfileEnabled () && TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_POST_CREATE_FAULT == 1;
}

inline bool CompiledProfileMatches (const Context& context)
{
	return CompiledCandidateProfileEnabled ()
		&& context.candidateEnabled == "true"
		&& (context.postCreateFaultEnabled == "true") == CompiledPostCreateFaultEnabled ();
}

// This producer intentionally supports a narrow saved Windows solo-file
// identity. Non-ASCII paths are refused rather than compared with a
// platform-dependent Unicode case-folding rule.
inline bool CanonicalSavedProjectScope (const std::string& value, std::string& result)
{
	std::string path = value;
	if (path.compare (0, 6, "saved:") == 0) path.erase (0, 6);
	if (path.size () < 7 || !std::isalpha (static_cast<unsigned char> (path[0])) || path[1] != ':'
		|| (path[2] != '/' && path[2] != '\\')) return false;
	for (char& character : path) {
		const unsigned char byte = static_cast<unsigned char> (character);
		if (byte < 0x20 || byte > 0x7e) return false;
		if (character == '\\') character = '/';
		else character = static_cast<char> (std::tolower (byte));
	}
	std::vector<std::string> segments;
	std::size_t start = 3;
	while (start <= path.size ()) {
		const std::size_t end = path.find ('/', start);
		const std::string segment = path.substr (start, end == std::string::npos ? std::string::npos : end - start);
		if (segment.empty () || segment == ".") {
			// Repeated separators and current-directory components normalize away.
		} else if (segment == "..") {
			if (segments.empty ()) return false;
			segments.pop_back ();
		} else {
			segments.push_back (segment);
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	if (segments.empty ()) return false;
	std::string normalized;
	normalized.push_back (path[0]);
	normalized += ":/";
	for (std::size_t index = 0; index < segments.size (); ++index) {
		if (index != 0) normalized.push_back ('/');
		normalized += segments[index];
	}
	if (normalized.size () < 5 || normalized.compare (normalized.size () - 4, 4, ".pln") != 0) return false;
	result = "saved:" + normalized;
	return true;
}

inline bool AppendLengthPrefixed (std::vector<std::uint8_t>& bytes, const std::string& value)
{
	if (value.size () > std::numeric_limits<std::uint32_t>::max ()) return false;
	const auto length = static_cast<std::uint32_t> (value.size ());
	bytes.push_back (static_cast<std::uint8_t> (length >> 24));
	bytes.push_back (static_cast<std::uint8_t> (length >> 16));
	bytes.push_back (static_cast<std::uint8_t> (length >> 8));
	bytes.push_back (static_cast<std::uint8_t> (length));
	bytes.insert (bytes.end (), value.begin (), value.end ());
	return true;
}

inline bool CanonicalWireBytes (const Request& request, const Context& context, std::vector<std::uint8_t>& bytes)
{
	bytes.clear ();
	static const std::string domain = "Tapir.NativeDimensionCreateWire.v1";
	const std::string fields[] = {
		domain,
		request.kind, request.schemaVersion, request.operation, request.elementType,
		request.operationId, request.fileScopeKey, request.requestHash, request.bindingId,
		request.instanceId, request.runtimeKey, request.projectFingerprint, request.projectGeneration,
		request.archicadMajor, request.description, request.recipe, request.storyIndex,
		context.kind, context.schemaVersion, context.runId, context.projectPath, context.fileScopeKey,
		context.processId, context.processStartUtcTicks, context.processStartEpoch, context.archicadMajor,
		context.buildId, context.addonLocation, context.windowType, context.windowIndex,
		context.windowDatabaseGuid, context.currentDatabaseGuid, context.storyIndex,
		context.contractVersion, context.recipe, context.policyVersion,
		context.candidateEnabled, context.postCreateFaultEnabled
	};
	for (const std::string& field : fields) if (!AppendLengthPrefixed (bytes, field)) return false;
	return true;
}

inline std::array<std::uint8_t, 32> Sha256 (const std::uint8_t* input, const std::size_t length)
{
	static constexpr std::uint32_t K[64] = {
		0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
		0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
		0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
		0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
		0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
		0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
		0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
		0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
	};
	auto rotr = [] (const std::uint32_t x, const unsigned n) { return (x >> n) | (x << (32 - n)); };
	std::array<std::uint32_t, 8> h = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
	const std::uint64_t bitLength = static_cast<std::uint64_t> (length) * 8;
	const std::size_t padded = ((length + 9 + 63) / 64) * 64;
	std::vector<std::uint8_t> data (padded, 0);
	for (std::size_t i = 0; i < length; ++i) data[i] = input[i];
	data[length] = 0x80;
	for (unsigned i = 0; i < 8; ++i) data[padded - 1 - i] = static_cast<std::uint8_t> (bitLength >> (i * 8));
	for (std::size_t offset = 0; offset < padded; offset += 64) {
		std::uint32_t w[64] = {};
		for (unsigned i = 0; i < 16; ++i) w[i] = (static_cast<std::uint32_t> (data[offset + i * 4]) << 24)
			| (static_cast<std::uint32_t> (data[offset + i * 4 + 1]) << 16)
			| (static_cast<std::uint32_t> (data[offset + i * 4 + 2]) << 8) | data[offset + i * 4 + 3];
		for (unsigned i = 16; i < 64; ++i) {
			const std::uint32_t s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
			const std::uint32_t s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		auto v = h;
		for (unsigned i = 0; i < 64; ++i) {
			const std::uint32_t s1 = rotr (v[4], 6) ^ rotr (v[4], 11) ^ rotr (v[4], 25);
			const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
			const std::uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
			const std::uint32_t s0 = rotr (v[0], 2) ^ rotr (v[0], 13) ^ rotr (v[0], 22);
			const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
			const std::uint32_t t2 = s0 + maj;
			v = { t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6] };
		}
		for (unsigned i = 0; i < 8; ++i) h[i] += v[i];
	}
	std::array<std::uint8_t, 32> digest = {};
	for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j)
		digest[i * 4 + j] = static_cast<std::uint8_t> (h[i] >> (24 - j * 8));
	return digest;
}

inline std::string Hex (const std::array<std::uint8_t, 32>& digest)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve (64);
	for (const std::uint8_t value : digest) {
		result.push_back (digits[value >> 4]);
		result.push_back (digits[value & 0x0f]);
	}
	return result;
}

inline std::string WireHash (const Request& request, const Context& context)
{
	std::vector<std::uint8_t> bytes;
	if (!CanonicalWireBytes (request, context, bytes)) return {};
	return Hex (Sha256 (bytes.data (), bytes.size ()));
}

inline std::string JsonQuoteUtf8 (const std::string& value)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result = "\"";
	for (const unsigned char c : value) {
		if (c == '"' || c == '\\') { result.push_back ('\\'); result.push_back (static_cast<char> (c)); }
		else if (c == '\b') result += "\\b";
		else if (c == '\t') result += "\\t";
		else if (c == '\n') result += "\\n";
		else if (c == '\f') result += "\\f";
		else if (c == '\r') result += "\\r";
		else if (c < 0x20) {
			result += "\\u00";
			result.push_back (hex[c >> 4]);
			result.push_back (hex[c & 0x0f]);
		} else result.push_back (static_cast<char> (c));
	}
	result.push_back ('"');
	return result;
}

// This is the exact stableCandidateJson projection used for requestHash in
// native-dimension-create-candidate.ts: recursively sorted object keys,
// JSON strings, and canonical JSON numbers for the three integer fields.
inline std::string CanonicalRequestJson (const Request& request, const bool includeHash)
{
	std::string result = "{";
	result += "\"binding\":{";
	result += "\"archicadMajor\":" + request.archicadMajor;
	result += ",\"bindingId\":" + JsonQuoteUtf8 (request.bindingId);
	result += ",\"instanceId\":" + JsonQuoteUtf8 (request.instanceId);
	result += ",\"projectFingerprint\":" + JsonQuoteUtf8 (request.projectFingerprint);
	result += ",\"projectGeneration\":" + request.projectGeneration;
	result += ",\"runtimeKey\":" + JsonQuoteUtf8 (request.runtimeKey) + "}";
	result += ",\"description\":" + JsonQuoteUtf8 (request.description);
	result += ",\"elementType\":" + JsonQuoteUtf8 (request.elementType);
	result += ",\"fileScopeKey\":" + JsonQuoteUtf8 (request.fileScopeKey);
	result += ",\"kind\":" + JsonQuoteUtf8 (request.kind);
	result += ",\"operation\":" + JsonQuoteUtf8 (request.operation);
	result += ",\"operationId\":" + JsonQuoteUtf8 (request.operationId);
	result += ",\"placement\":{\"storyIndex\":" + request.storyIndex + "}";
	result += ",\"recipe\":" + JsonQuoteUtf8 (request.recipe);
	result += ",\"schemaVersion\":" + JsonQuoteUtf8 (request.schemaVersion);
	if (includeHash) result += ",\"requestHash\":" + JsonQuoteUtf8 (request.requestHash);
	result.push_back ('}');
	return result;
}

inline std::string LogicalRequestHash (const Request& request)
{
	const std::string canonical = CanonicalRequestJson (request, false);
	return Hex (Sha256 (reinterpret_cast<const std::uint8_t*> (canonical.data ()), canonical.size ()));
}

inline bool IsCanonicalSignedInt32 (const std::string& value)
{
	if (value.empty () || value == "-0") return false;
	const std::size_t firstDigit = value[0] == '-' ? 1 : 0;
	if (firstDigit == value.size () || (value[firstDigit] == '0' && value.size () - firstDigit > 1)) return false;
	for (std::size_t index = firstDigit; index < value.size (); ++index)
		if (value[index] < '0' || value[index] > '9') return false;
	try {
		std::size_t consumed = 0;
		const long long parsed = std::stoll (value, &consumed, 10);
		return consumed == value.size () && parsed >= std::numeric_limits<std::int32_t>::min () && parsed <= std::numeric_limits<std::int32_t>::max ();
	} catch (const std::exception&) { return false; }
}

// API_Dimension.floorInd is an Int16 story index, even though the logical
// request schema carries a signed Int32. Keep that transport distinction
// explicit and check it before any SDK write path casts the value.
inline bool IsNativeFloorStoryIndex (const std::string& value)
{
	if (!IsCanonicalSignedInt32 (value)) return false;
	try {
		const long long parsed = std::stoll (value, nullptr, 10);
		return parsed >= std::numeric_limits<std::int16_t>::min ()
			&& parsed <= std::numeric_limits<std::int16_t>::max ();
	} catch (const std::exception&) { return false; }
}

struct PolicyResult {
	bool accepted = false;
	const char* reason = "unavailable";
};

inline PolicyResult ValidateRequestRecipe (const Request& request)
{
	if (request.kind != RequestKind || request.schemaVersion != "native-dimension-create-candidate-request-v1"
		|| request.operation != "create" || request.elementType != "Dimension"
		|| request.recipe != Recipe || request.operationId.empty () || request.fileScopeKey.empty ()
		|| request.bindingId.empty () || request.instanceId.empty () || request.runtimeKey.empty ()
		|| request.projectFingerprint.empty () || request.description.empty () || !IsNativeFloorStoryIndex (request.storyIndex)
		|| request.requestHash.size () != 64
		|| !std::all_of (request.requestHash.begin (), request.requestHash.end (), [] (const unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return { false, "request-shape-or-recipe" };
	if (request.projectGeneration.empty () || request.archicadMajor.empty ()) return { false, "request-identity-unavailable" };
	if (request.archicadMajor != "25" && request.archicadMajor != "28") return { false, "unsupported-archicad-major" };
	return { true, "fixed-recipe-request" };
}

inline PolicyResult ValidateExpectedContext (const Context& expected, const Context& actual)
{
	if (expected.kind != "nativeDimensionCreateCandidateContext" || expected.schemaVersion != "native-dimension-create-context/v1"
		|| actual.kind != expected.kind || actual.schemaVersion != expected.schemaVersion) return { false, "context-schema" };
	if (expected.runId.empty () || expected.projectPath.empty () || expected.fileScopeKey.empty ()
		|| expected.processId.empty () || expected.processStartUtcTicks.empty () || expected.processStartEpoch != "dotnet-ticks-utc-v1"
		|| expected.archicadMajor.empty () || expected.buildId.empty () || expected.addonLocation.empty ()
		|| !IsCanonicalSignedInt32 (expected.storyIndex) || !IsCanonicalSignedInt32 (expected.windowIndex)) return { false, "context-required-facts" };
	if (expected.projectPath != actual.projectPath || expected.fileScopeKey != actual.fileScopeKey
		|| expected.processId != actual.processId || expected.processStartUtcTicks != actual.processStartUtcTicks
		|| expected.processStartEpoch != actual.processStartEpoch || expected.archicadMajor != actual.archicadMajor
		|| expected.buildId != actual.buildId || expected.addonLocation != actual.addonLocation
		|| expected.windowType != actual.windowType || expected.windowIndex != actual.windowIndex
		|| expected.windowDatabaseGuid != actual.windowDatabaseGuid || expected.currentDatabaseGuid != actual.currentDatabaseGuid
		|| expected.storyIndex != actual.storyIndex || expected.contractVersion != actual.contractVersion
		|| expected.recipe != actual.recipe || expected.policyVersion != actual.policyVersion
		|| expected.candidateEnabled != actual.candidateEnabled || expected.postCreateFaultEnabled != actual.postCreateFaultEnabled) return { false, "context-mismatch" };
	if (actual.windowType != "FloorPlan" || actual.currentDatabaseGuid.empty () || actual.currentDatabaseGuid == "00000000-0000-0000-0000-000000000000") return { false, "floor-plan-context-unavailable" };
	if (actual.contractVersion != "native-crud-v2" || actual.recipe != Recipe
		|| (actual.candidateEnabled != "true" && actual.candidateEnabled != "false")
		|| (actual.postCreateFaultEnabled != "true" && actual.postCreateFaultEnabled != "false")
		|| actual.candidateEnabled != "true"
		|| (actual.postCreateFaultEnabled == "true" && actual.candidateEnabled != "true")) return { false, "compiled-policy-unavailable" };
	return { true, "context-exact" };
}

} // namespace NativeDimensionCreateWire
