#include "NativeDimensionFacts.hpp"
#include "NativeDimensionAuditPolicy.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <document.h>
#include <pointer.h>
#include <stringbuffer.h>
#include <writer.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

bool Require (bool condition, const char* message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

GS::ObjectState Parse (const std::string& json)
{
    GS::ObjectState state;
    const GS::UniString text (json.c_str (), CC_UTF8);
    if (JSON::ConvertToObjectState (text, state) != NoError) throw std::runtime_error ("JSON fixture did not parse.");
    return state;
}

GS::UniString Fingerprint (const GS::ObjectState& state)
{
    const auto result = NativeDimensionFacts::NativeDimensionFactualFingerprint (state);
    if (!result.HasValue ()) throw std::runtime_error ("Native factual fingerprint rejected a finite fixture row.");
    return result.Get ();
}

std::string Serialize (const rapidjson::Value& value)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer (buffer);
    if (!value.Accept (writer)) throw std::runtime_error ("Could not serialize test JSON value.");
    return std::string (buffer.GetString (), buffer.GetSize ());
}

void CollectScalarPaths (const rapidjson::Value& value, const std::string& path, std::vector<std::string>& paths)
{
    if (value.IsObject ()) {
        for (auto member = value.MemberBegin (); member != value.MemberEnd (); ++member) {
            const std::string key (member->name.GetString (), member->name.GetStringLength ());
            const std::string pointerSegment = [&] {
                std::string result;
                for (const char character : key) result += character == '~' ? "~0" : character == '/' ? "~1" : std::string (1, character);
                return result;
            } ();
            CollectScalarPaths (member->value, path + "/" + pointerSegment, paths);
        }
    } else if (value.IsArray ()) {
        for (rapidjson::SizeType index = 0; index < value.Size (); ++index) CollectScalarPaths (value[index], path + "/" + std::to_string (index), paths);
    } else if (!value.IsNull ()) paths.push_back (path);
}

bool MutateScalar (rapidjson::Value& value, const std::string& path, rapidjson::Document::AllocatorType& allocator)
{
    if (value.IsBool ()) { value.SetBool (!value.GetBool ()); return true; }
    if (value.IsString ()) {
        const std::string current (value.GetString (), value.GetStringLength ());
        std::string replacement;
        if (path.size () >= 5 && path.compare (path.size () - 5, 5, "/guid") == 0) {
            replacement = current == "abcdef12-1234-1234-1234-abcdef123456" ? "abcdef12-1234-1234-1234-abcdef123457" : "abcdef12-1234-1234-1234-abcdef123456";
        } else if (path.size () >= 7 && path.compare (path.size () - 7, 7, "/status") == 0) {
            replacement = current == "unavailable" ? "available" : current == "error" ? "success" : "unavailable";
        } else if (path.size () >= 12 && path.compare (path.size () - 12, 12, "/contentMode") == 0) {
            replacement = current == "custom" ? "measured" : "custom";
        } else if (path.size () >= 17 && path.compare (path.size () - 17, 17, "/associationStatus") == 0) {
            replacement = current == "no-element-guid" ? "element-guid-present-type-not-independently-resolved" : "no-element-guid";
        } else if (path.size () >= 13 && path.compare (path.size () - 13, 13, "/applicability") == 0) {
            replacement = current == "always" ? "not-applicable" : "always";
        } else if (path == "/units/coordinates" || path.find ("Unit") != std::string::npos || path.find ("unit") != std::string::npos) {
            replacement = current == "project-meter" ? "millimeter-paper-size" : "project-meter";
        } else replacement = current + "#";
        value.SetString (replacement.c_str (), static_cast<rapidjson::SizeType> (replacement.size ()), allocator);
        return true;
    }
    if (value.IsInt64 ()) { value.SetInt64 (value.GetInt64 () + 1); return true; }
    if (value.IsUint64 ()) { value.SetUint64 (value.GetUint64 () + 1); return true; }
    if (value.IsDouble ()) { value.SetDouble (value.GetDouble () + 1.0); return true; }
    return false;
}

std::string FamilyForType (const rapidjson::Value& row)
{
    const rapidjson::Value* type = rapidjson::Pointer ("/elementType").Get (row);
    if (type == nullptr || !type->IsString ()) return {};
    const std::string value (type->GetString (), type->GetStringLength ());
    return value == "Dimension" ? "linear-chain" : value == "RadialDimension" ? "radial" : value == "AngleDimension" ? "angle" : value == "LevelDimension" ? "level" : "";
}

}

int main (int argc, char** argv)
{
    if (argc != 2) { std::cerr << "usage: native-dimension-facts-tests <captured-reader-response.json>\n"; return 2; }
    try {
        std::ifstream stream (argv[1], std::ios::binary);
        if (!stream) throw std::runtime_error ("Could not open the captured reader response.");
        const std::string source ((std::istreambuf_iterator<char> (stream)), std::istreambuf_iterator<char> ());
        GS::ObjectState response = Parse (source);
        GS::Array<GS::ObjectState> rows;
        if (!response.Get ("elements", rows) || rows.IsEmpty ()) throw std::runtime_error ("Captured response contains no element rows.");
        rapidjson::Document responseDocument;
        responseDocument.Parse (source.c_str ());
        if (responseDocument.HasParseError () || !responseDocument.IsObject () || !responseDocument.HasMember ("elements") || !responseDocument["elements"].IsArray ()) {
            throw std::runtime_error ("RapidJSON could not parse captured root for full-row mutation matrix.");
        }
        const rapidjson::Value& jsonRows = responseDocument["elements"];
        if (jsonRows.Size () != rows.GetSize ()) throw std::runtime_error ("JSON and ObjectState row counts differ.");
        const GS::ObjectState& captured = rows[0];
        const GS::UniString baseline = Fingerprint (captured);
        UInt32 capturedIndex = 0;
        for (const GS::ObjectState& row : rows) {
            try { (void) Fingerprint (row); }
            catch (...) { std::cerr << "Captured row index failed: " << capturedIndex << '\n'; throw; }
            ++capturedIndex;
        }
        bool passed = true;
        std::size_t rootMutationCount = 0;
        static const std::set<std::string> excludedFactPaths = {
            "canonicalSnapshotKind", "requestedGuid", "modificationStamp", "teamworkOwnerId", "teamworkLockId", "memo",
            "unicodeProbeStatus", "unicodeProbeConsistency", "fingerprint", "fingerprintAlgorithm", "fingerprintCoverage",
            "error", "probeInstrumentation", "pointMemoHandleBytes", "pointMemoStatus", "byteLength", "contentByteLength",
            "bufferCapacity", "bytesHex", "reason", "sectionNodeStatus", "sectionNodeType", "sectionNodeFactsStatus",
            "initialModificationStamp", "probeModificationStamp", "finalHeaderErrorCode", "finalModificationStamp"
        };
        const auto exerciseRootMatrix = [&] (const rapidjson::Value& jsonRow) -> std::size_t {
            const std::string family = FamilyForType (jsonRow);
            if (family.empty ()) throw std::runtime_error ("Full-row matrix found an unsupported captured family.");
            const std::string serializedRow = Serialize (jsonRow);
            GS::ObjectState baseState = Parse (serializedRow);
            std::string baseFacts;
            if (!NativeDimensionFacts::AppendDimensionFacts (baseState, baseFacts, "root", family)) throw std::runtime_error ("Full-row matrix could not encode captured root facts.");
            const GS::UniString baseFingerprint = Fingerprint (baseState);
            std::vector<std::string> paths;
            CollectScalarPaths (jsonRow, "", paths);
            const rapidjson::Value* longPoints = rapidjson::Pointer ("/orderedPoints").Get (jsonRow);
            const bool captured45PointChain = longPoints != nullptr && longPoints->IsArray () && longPoints->Size () == 45;
            std::size_t rowMutationCount = 0;
            for (const std::string& path : paths) {
                std::string leaf;
                const std::size_t slash = path.find_last_of ('/');
                leaf = slash == std::string::npos ? path : path.substr (slash + 1);
                if (leaf == "reason" || path == "/elementType" || path == "/identity/family" || path == "/identity/contextStatus"
                    || path == "/memo/status" || leaf == "slot" || excludedFactPaths.find (leaf) != excludedFactPaths.end ()) continue;
                if (captured45PointChain && path.rfind ("/orderedPoints/", 0) == 0
                    && path.find ("/note/content") == std::string::npos
                    && path.find ("/anchor/location/x") == std::string::npos) continue;
                rapidjson::Document changed;
                changed.CopyFrom (jsonRow, changed.GetAllocator ());
                rapidjson::Value* target = rapidjson::Pointer (path.c_str ()).Get (changed);
                if (target == nullptr) throw std::runtime_error ("Full-row mutation pointer did not resolve: " + path);
                if (!MutateScalar (*target, path, changed.GetAllocator ())) throw std::runtime_error ("Full-row mutation found an unsupported scalar type: " + path);
                const std::string mutatedRow = Serialize (changed);
                GS::ObjectState changedState = Parse (mutatedRow);
                std::string changedFacts;
                const bool encoded = NativeDimensionFacts::AppendDimensionFacts (changedState, changedFacts, "root", family);
                const bool fingerprintChanged = Fingerprint (changedState) != baseFingerprint;
                if (!encoded || changedFacts == baseFacts || !fingerprintChanged) {
                    std::cerr << "Meaningful captured root field did not change projection and fingerprint: family=" << family << " path=" << path << '\n';
                    passed = false;
                } else ++rowMutationCount;
            }
            // The two explicitly diagnostic explanations are intentionally
            // retained in reader output but excluded from factual identity.
            for (const std::string& path : paths) {
                if (path.size () < 7 || path.compare (path.size () - 7, 7, "/reason") != 0) continue;
                rapidjson::Document changed;
                changed.CopyFrom (jsonRow, changed.GetAllocator ());
                rapidjson::Value* target = rapidjson::Pointer (path.c_str ()).Get (changed);
                if (target == nullptr || !target->IsString ()) throw std::runtime_error ("Nested diagnostic reason was not a string.");
                target->SetString ("different diagnostic", changed.GetAllocator ());
                GS::ObjectState changedState = Parse (Serialize (changed));
                std::string changedFacts;
                if (!NativeDimensionFacts::AppendDimensionFacts (changedState, changedFacts, "root", family)
                    || changedFacts != baseFacts || Fingerprint (changedState) != baseFingerprint) {
                    std::cerr << "Diagnostic prose changed factual identity: family=" << family << " path=" << path << '\n';
                    passed = false;
                }
            }
            return rowMutationCount;
        };
        const auto exerciseUnavailableRootObjects = [&] (const rapidjson::Value& jsonRow) -> std::size_t {
            const std::string family = FamilyForType (jsonRow);
            if (family.empty ()) throw std::runtime_error ("Unavailable-object variants found an unsupported captured family.");
            const GS::ObjectState originalState = Parse (Serialize (jsonRow));
            std::string originalFacts;
            if (!NativeDimensionFacts::AppendDimensionFacts (originalState, originalFacts, "root", family)) throw std::runtime_error ("Could not encode captured root before unavailable-object variants.");
            const GS::UniString originalFingerprint = Fingerprint (originalState);
            std::size_t exercised = 0;
            for (const char* path : { "/database", "/bounds", "/dimensionPreferences" }) {
                rapidjson::Document unavailableRow;
                unavailableRow.CopyFrom (jsonRow, unavailableRow.GetAllocator ());
                rapidjson::Value* target = rapidjson::Pointer (path).Get (unavailableRow);
                if (target == nullptr || !target->IsObject ()) throw std::runtime_error (std::string ("Captured complete root object is absent: ") + path);
                const rapidjson::Value* status = rapidjson::Pointer ("/status").Get (*target);
                if (status == nullptr || !status->IsString () || std::string (status->GetString (), status->GetStringLength ()) != "available") {
                    throw std::runtime_error (std::string ("Captured successful root object lacks available status: ") + path);
                }

                rapidjson::Value unavailable (rapidjson::kObjectType);
                unavailable.AddMember ("status", rapidjson::Value ("unavailable", unavailableRow.GetAllocator ()), unavailableRow.GetAllocator ());
                if (std::string (path) == "/dimensionPreferences") {
                    unavailable.AddMember ("source", rapidjson::Value ("ACAPI_ProjectSetting_GetPreferences/APIPrefs_DimensionsID", unavailableRow.GetAllocator ()), unavailableRow.GetAllocator ());
                    unavailable.AddMember ("scope", rapidjson::Value ("project-dimension-preferences", unavailableRow.GetAllocator ()), unavailableRow.GetAllocator ());
                    unavailable.AddMember ("savedViewEffectiveFormat", rapidjson::Value ("not-proven-by-project-preferences-read", unavailableRow.GetAllocator ()), unavailableRow.GetAllocator ());
                    unavailable.AddMember ("formatCoverage", rapidjson::Value ("unavailable", unavailableRow.GetAllocator ()), unavailableRow.GetAllocator ());
                }
                unavailable.AddMember ("errorCode", -2130313214, unavailableRow.GetAllocator ());
                target->CopyFrom (unavailable, unavailableRow.GetAllocator ());

                const auto factsAndFingerprint = [&] (const rapidjson::Value& row, std::string& facts) {
                    const GS::ObjectState state = Parse (Serialize (row));
                    if (!NativeDimensionFacts::AppendDimensionFacts (state, facts, "root", family)) return std::pair<bool, GS::UniString> { false, GS::UniString () };
                    return std::pair<bool, GS::UniString> { true, Fingerprint (state) };
                };
                std::string unavailableFacts;
                const auto unavailableValue = factsAndFingerprint (unavailableRow, unavailableFacts);
                if (!unavailableValue.first || unavailableFacts == originalFacts || unavailableValue.second == originalFingerprint) {
                    throw std::runtime_error (std::string ("Complete synthetic unavailable root object did not change factual encoding and fingerprint: ") + path);
                }

                rapidjson::Document changedErrorCode;
                changedErrorCode.CopyFrom (unavailableRow, changedErrorCode.GetAllocator ());
                rapidjson::Value* changedErrorTarget = rapidjson::Pointer (path).Get (changedErrorCode);
                rapidjson::Value* errorCode = rapidjson::Pointer ("/errorCode").Get (*changedErrorTarget);
                if (errorCode == nullptr || !errorCode->IsInt ()) throw std::runtime_error (std::string ("Synthetic unavailable root object lacks integer errorCode: ") + path);
                errorCode->SetInt (errorCode->GetInt () + 1);
                std::string changedErrorFacts;
                const auto changedErrorValue = factsAndFingerprint (changedErrorCode, changedErrorFacts);
                if (!changedErrorValue.first || changedErrorFacts == unavailableFacts || changedErrorValue.second == unavailableValue.second) {
                    throw std::runtime_error (std::string ("Unavailable root errorCode did not change factual encoding and fingerprint: ") + path);
                }

                rapidjson::Document changedStatus;
                changedStatus.CopyFrom (unavailableRow, changedStatus.GetAllocator ());
                const std::string statusPath = std::string (path) + "/status";
                rapidjson::Value* changedStatusValue = rapidjson::Pointer (statusPath.c_str ()).Get (changedStatus);
                if (changedStatusValue == nullptr || !changedStatusValue->IsString ()) throw std::runtime_error (std::string ("Synthetic unavailable root status was not a string: ") + path);
                changedStatusValue->SetString ("available", changedStatus.GetAllocator ());
                std::string changedStatusFacts;
                const auto changedStatusFingerprint = factsAndFingerprint (changedStatus, changedStatusFacts);
                if (!changedStatusFingerprint.first || changedStatusFacts == unavailableFacts || changedStatusFingerprint.second == unavailableValue.second) {
                    throw std::runtime_error (std::string ("Root availability status did not change factual encoding and fingerprint: ") + path);
                }
                ++exercised;
            }
            return exercised;
        };
        for (rapidjson::SizeType rowIndex = 0; rowIndex < jsonRows.Size (); ++rowIndex) {
            const std::string family = FamilyForType (jsonRows[rowIndex]);
            const std::size_t count = exerciseRootMatrix (jsonRows[rowIndex]);
            rootMutationCount += count;
            std::cout << "Captured root matrix family=" << family << " mutationCount=" << count << ".\n";
        }
        std::size_t unavailableRootVariantCount = 0;
        for (rapidjson::SizeType rowIndex = 0; rowIndex < jsonRows.Size (); ++rowIndex) unavailableRootVariantCount += exerciseUnavailableRootObjects (jsonRows[rowIndex]);
        passed &= Require (unavailableRootVariantCount == static_cast<std::size_t> (jsonRows.Size ()) * 3,
            "Synthetic unavailable database/bounds/preferences variants were not exercised for every captured family row.");
        std::cout << "Captured root unavailable-object variants=" << unavailableRootVariantCount << " across " << jsonRows.Size () << " complete rows.\n";
        rapidjson::Document syntheticRadialRow;
        static const char syntheticRadialJson[] = R"json({
          "elementType":"RadialDimension","elementId":{"guid":"abcdef12-1234-1234-1234-abcdef123456"},
          "nativeElementTypeId":12,"elementVariationId":0,"storyIndex":0,"layerIndex":0,"drawingOrderIndex":1,"referenceGroupLayerIndex":0,"hasMemo":false,
          "database":{"status":"available","databaseId":{"guid":"00000000-0000-0000-0000-000000000000"},"databaseType":0,"databaseIndex":0,"linkedSegmentMemoIndex":0},
          "bounds":{"status":"available","xMin":0.0,"yMin":0.0,"xMax":4.0,"yMax":4.0,"zMin":0.0,"zMax":0.0,"unit":"project-meter"},
          "units":{"coordinates":"project-meter","linearMeasurements":"project-length-values-with-project-preferences-attached","angles":"radian","noteHeights":"millimeter-paper-size","markerSizes":"millimeter-paper-size"},
          "dimensionPreferences":{"status":"available","source":"project-preferences","scope":"project","formatCoverage":"complete-project-context","linear":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"radial":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"level":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"elevation":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"doorWindow":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"parapet":{"unit":1,"roundInch":0,"decimals":2,"showZeroWhole":false,"showZeroInch":0,"showSmall5":0,"hideZeroDecimals":false},"area":{"unit":1,"decimals":2,"hideZeroDecimals":false},"angle":{"unit":1,"decimals":2,"accuracy":0,"hideZeroDecimals":false},"dimensionStyleAttributeIndex":0},
          "identity":{"family":"radial","contextStatus":"database-story-layer-from-native-element-header"},
          "geometryAndSettings":{"base":{"elementVariationId":0,"elementTypeId":12,"lineReference":true,"specialReference":0,"nodeIndex":0,"nativeNodeId":0,"elementId":{"guid":"00000000-0000-0000-0000-000000000000"},"location":{"x":0.0,"y":0.0,"unit":"project-meter"},"location3D":{"x":0.0,"y":0.0,"z":0.0,"unit":"project-meter"},"associationStatus":"no-element-guid"},"endPosition":{"x":4.0,"y":0.0,"unit":"project-meter"},"measuredRadius":4.0,"measurementSource":"native-radius","display":"unavailable-unless-independently-observed","linePen":1,"textPosition":0,"textDirection":0,"showOrigin":false,"onlyDimensionText":false,"prefix":"R","prefixSource":"fixed-buffer","prefixUnicodeStatus":"ascii-nul-terminated-verified","marker":{"markerType":0,"markerPen":1,"markerSize":2.0,"markerSizeUnit":"millimeter-paper-size"},"staticAnchorEvidence":"radial-base-and-endpoints"},
          "standaloneNotes":[{"slot":"note","applicability":"always","details":{"pen":1,"font":1,"layer":1,"textHeight":2.5,"textHeightUnit":"millimeter-paper-size","angle":0.0,"angleUnit":"radian","position":{"x":1.0,"y":1.0,"unit":"project-meter"},"content":"Synthetic radial annotation","contentStatus":"available","contentSource":"verified-caller-bound-unicode-contentUStr","contentMode":"custom","fixPos":true,"styleBits":0,"opaque":false,"frame":false,"useLeaderLine":false,"anchorAtTextEnd":false,"leaderPen":1,"leaderLineType":0,"leaderShape":0,"leaderArrowType":0,"leaderArrowAtBegin":false,"leaderArrowAtEnd":false,"leaderArrowPen":1,"leaderArrowSize":1.0,"anchorPoint":0,"justification":0,"widthFactor":1.0,"spacingFactor":1.0,"rowAnchor":0,"effectBits":0,"sourceExpression":{"status":"unproven","reason":"diagnostic explanation"},"observedDisplay":{"status":"unavailable","reason":"diagnostic explanation"}}}],
          "status":"success","coverage":"partial","unicodeAccessCoverage":{"status":"complete"},"sourceFidelityCoverage":{"status":"unproven"},"readConsistency":{"status":"same-guid-type-and-modification-stamp-through-memo-read"}
        })json";
        syntheticRadialRow.Parse (syntheticRadialJson);
        if (syntheticRadialRow.HasParseError ()) throw std::runtime_error ("Synthetic radial root row did not parse.");
        const std::size_t syntheticRadialMutations = exerciseRootMatrix (syntheticRadialRow);
        rootMutationCount += syntheticRadialMutations;
        std::cout << "Synthetic radial root matrix mutationCount=" << syntheticRadialMutations << ".\n";
        std::size_t orderSensitiveLists = 0;
        std::size_t cardinalitySensitiveLists = 0;
        for (rapidjson::SizeType rowIndex = 0; rowIndex < jsonRows.Size (); ++rowIndex) {
            const rapidjson::Value& jsonRow = jsonRows[rowIndex];
            const std::string family = FamilyForType (jsonRow);
            const std::string serializedRow = Serialize (jsonRow);
            const GS::ObjectState baseState = Parse (serializedRow);
            const GS::UniString baseFingerprint = Fingerprint (baseState);
            std::string baseFacts;
            if (!NativeDimensionFacts::AppendDimensionFacts (baseState, baseFacts, "root", family)) throw std::runtime_error ("Could not encode captured root before list mutation.");
            for (const char* path : { "/orderedPoints", "/standaloneNotes", "/geometryAndSettings/anchors" }) {
                const rapidjson::Value* original = rapidjson::Pointer (path).Get (jsonRow);
                if (original == nullptr || !original->IsArray () || original->Size () < 2) continue;
                rapidjson::Document reordered;
                reordered.CopyFrom (jsonRow, reordered.GetAllocator ());
                rapidjson::Value* reorderedArray = rapidjson::Pointer (path).Get (reordered);
                (*reorderedArray)[0].Swap ((*reorderedArray)[1]);
                const GS::ObjectState reorderedState = Parse (Serialize (reordered));
                std::string reorderedFacts;
                if (!NativeDimensionFacts::AppendDimensionFacts (reorderedState, reorderedFacts, "root", family)
                    || reorderedFacts == baseFacts || Fingerprint (reorderedState) == baseFingerprint) {
                    std::cerr << "Ordered child list permutation did not change the full-root projection: family=" << family << " path=" << path << '\n';
                    passed = false;
                } else ++orderSensitiveLists;
                rapidjson::Document cardinality;
                cardinality.CopyFrom (jsonRow, cardinality.GetAllocator ());
                rapidjson::Value* expandedArray = rapidjson::Pointer (path).Get (cardinality);
                rapidjson::Value extra;
                extra.CopyFrom ((*expandedArray)[0], cardinality.GetAllocator ());
                expandedArray->PushBack (extra, cardinality.GetAllocator ());
                const GS::ObjectState cardinalityState = Parse (Serialize (cardinality));
                std::string cardinalityFacts;
                if (!NativeDimensionFacts::AppendDimensionFacts (cardinalityState, cardinalityFacts, "root", family)
                    || cardinalityFacts == baseFacts || Fingerprint (cardinalityState) == baseFingerprint) {
                    std::cerr << "Ordered child list cardinality did not change the full-root projection: family=" << family << " path=" << path << '\n';
                    passed = false;
                } else ++cardinalitySensitiveLists;
            }
        }
        passed &= Require (orderSensitiveLists >= 3 && cardinalitySensitiveLists >= 3, "Full-root mutation matrix did not exercise point, standalone-note and anchor list order/cardinality.");
        passed &= Require (rootMutationCount > 0, "Full-row nested meaningful-field mutation matrix did not execute.");
        passed &= Require (NativeDimensionFacts::NativeDimensionFnv1a64 ("hello") == 0xa430d84680aabd0bULL,
            "Typed factual hashing is not the standard FNV-1a 64-bit algorithm.");

        GS::ObjectState diagnostic = captured;
        diagnostic.Add ("reason", "diagnostic prose changed");
        diagnostic.Add ("requestedGuid", "different-request-spelling");
        diagnostic.Add ("probeInstrumentation", 99);
        passed &= Require (Fingerprint (diagnostic) == baseline, "Diagnostic/request metadata changed the factual fingerprint.");

        const auto makeRow = [] (const char* type, const char* family, const char* guid) {
            return GS::ObjectState ("elementType", type,
                "elementId", GS::ObjectState ("guid", guid),
                "identity", GS::ObjectState ("family", family, "contextStatus", "available"),
                "status", "success");
        };
        GS::ObjectState uppercaseGuid = makeRow ("Dimension", "linear-chain", "ABCDEF12-1234-1234-1234-ABCDEF123456");
        GS::ObjectState lowercaseGuid = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        passed &= Require (Fingerprint (uppercaseGuid) == Fingerprint (lowercaseGuid), "GUID case changed the factual fingerprint.");
        GS::ObjectState changedGuid = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123457");
        passed &= Require (Fingerprint (uppercaseGuid) != Fingerprint (changedGuid), "Changing the exact element GUID did not change the factual fingerprint.");

        GS::ObjectState legacyAnchor = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState canonicalAnchor = legacyAnchor;
        legacyAnchor.Add ("geometryAndSettings", GS::ObjectState ("arcAnchor", GS::ObjectState ("sectionNodeId", static_cast<UInt64> (12))));
        canonicalAnchor.Add ("geometryAndSettings", GS::ObjectState ("arcAnchor", GS::ObjectState ("nativeNodeId", static_cast<UInt64> (12))));
        passed &= Require (Fingerprint (legacyAnchor) == Fingerprint (canonicalAnchor), "Equivalent legacy/canonical node-identity aliases encoded differently.");
        GS::ObjectState conflictingAnchor = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        conflictingAnchor.Add ("geometryAndSettings", GS::ObjectState ("arcAnchor", GS::ObjectState ("sectionNodeId", static_cast<UInt64> (12), "nativeNodeId", static_cast<UInt64> (12))));
        passed &= Require (!NativeDimensionFacts::NativeDimensionFactualFingerprint (conflictingAnchor).HasValue (), "Conflicting anchor identity aliases were accepted.");

        const auto anchorGeometry = [] (UInt32 nodeId) {
            return GS::ObjectState ("arcAnchor", GS::ObjectState ("nativeNodeId", static_cast<UInt64> (nodeId), "elementTypeId", static_cast<Int32> (7), "lineReference", false, "specialReference", static_cast<Int32> (0), "nodeIndex", static_cast<Int32> (0), "elementId", GS::ObjectState ("guid", "00000000-0000-0000-0000-000000000000"), "location", GS::ObjectState ("x", 0.0, "y", 0.0, "unit", "project-meter"), "location3D", GS::ObjectState ("x", 0.0, "y", 0.0, "z", 0.0, "unit", "project-meter"), "associationStatus", "no-element-guid"));
        };
        GS::ObjectState nodeA = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState nodeB = nodeA;
        nodeA.Add ("geometryAndSettings", anchorGeometry (0x80000000U));
        nodeB.Add ("geometryAndSettings", anchorGeometry (0x80000001U));
        passed &= Require (Fingerprint (nodeA) != Fingerprint (nodeB), "A meaningful Base node identity change did not change the fingerprint.");
        GS::ObjectState rowX = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState rowY = rowX;
        rowX.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", 1.0, "y", 0.0, "unit", "project-meter")));
        rowY.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", 2.0, "y", 0.0, "unit", "project-meter")));
        passed &= Require (Fingerprint (rowX) != Fingerprint (rowY), "A meaningful dimension geometry change did not change the fingerprint.");

        GS::ObjectState positiveZero = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState negativeZero = positiveZero;
        positiveZero.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", 0.0, "y", 0.0, "unit", "project-meter")));
        negativeZero.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", -0.0, "y", 0.0, "unit", "project-meter")));
        passed &= Require (Fingerprint (positiveZero) == Fingerprint (negativeZero), "Negative zero was not normalized.");
        GS::ObjectState fullPrecisionA = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState fullPrecisionB = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        fullPrecisionA.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", 1.0, "y", 0.0, "unit", "project-meter")));
        fullPrecisionB.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", std::nextafter (1.0, 2.0), "y", 0.0, "unit", "project-meter")));
        passed &= Require (Fingerprint (fullPrecisionA) != Fingerprint (fullPrecisionB), "Adjacent finite binary64 facts were rounded together.");
        GS::ObjectState nonFinite = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        nonFinite.Add ("geometryAndSettings", GS::ObjectState ("linePosition", GS::ObjectState ("x", std::numeric_limits<double>::infinity (), "y", 0.0, "unit", "project-meter")));
        passed &= Require (!NativeDimensionFacts::NativeDimensionFactualFingerprint (nonFinite).HasValue (), "Non-finite numeric facts were accepted.");

        GS::ObjectState asciiContent = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        asciiContent.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState ("content", "A")));
        const char exactUtf8[] = { 'A', static_cast<char> (0xC2), static_cast<char> (0xA0), static_cast<char> (0xD0), static_cast<char> (0x91), '\0' };
        GS::ObjectState unicodeContent = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        unicodeContent.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState ("content", GS::UniString (exactUtf8, CC_UTF8))));
        passed &= Require (Fingerprint (asciiContent) != Fingerprint (unicodeContent), "Exact Unicode/NBSP content was omitted from factual hashing.");
        GS::ObjectState availabilityComplete = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState availabilityPartial = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        availabilityComplete.Add ("unicodeAccessCoverage", GS::ObjectState ("status", "complete", "reason", "diagnostic"));
        availabilityPartial.Add ("unicodeAccessCoverage", GS::ObjectState ("status", "partial", "reason", "different diagnostic"));
        passed &= Require (Fingerprint (availabilityComplete) != Fingerprint (availabilityPartial), "Typed source-availability status did not change the factual fingerprint.");

        GS::ObjectState diagnosticReasonsA = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState diagnosticReasonsB = diagnosticReasonsA;
        diagnosticReasonsA.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState (
            "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "original explanation"),
            "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "old display explanation"))));
        diagnosticReasonsB.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState (
            "sourceExpression", GS::ObjectState ("status", "unproven", "reason", "changed explanation"),
            "observedDisplay", GS::ObjectState ("status", "unavailable", "reason", "new display explanation"))));
        passed &= Require (Fingerprint (diagnosticReasonsA) == Fingerprint (diagnosticReasonsB), "Explanatory source/display prose changed factual identity.");

        GS::ObjectState fixedObserved = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState fixedUnavailable = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        fixedObserved.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState ("fixedContent", GS::ObjectState ("status", "ascii-nul-terminated-verified", "nulTerminated", true, "value", "A"))));
        fixedUnavailable.Add ("geometryAndSettings", GS::ObjectState ("defaultNote", GS::ObjectState ("fixedContent", GS::ObjectState ("status", "encoding-unproven", "nulTerminated", false, "value", "A"))));
        passed &= Require (Fingerprint (fixedObserved) != Fingerprint (fixedUnavailable), "Fixed-buffer availability status/termination facts did not change the factual fingerprint.");

        GS::ObjectState currentPreferences = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState legacyPreferences = makeRow ("Dimension", "linear-chain", "abcdef12-1234-1234-1234-abcdef123456");
        currentPreferences.Add ("dimensionPreferences", GS::ObjectState ("status", "available", "formatCoverage", "complete-project-context"));
        legacyPreferences.Add ("dimensionPreferences", GS::ObjectState ("status", "available", "formatCoverage", "legacy-partial-coverage"));
        passed &= Require (Fingerprint (currentPreferences) != Fingerprint (legacyPreferences), "Preference format coverage did not change the factual fingerprint.");

        struct ExpectedSchema { const char* path; const char* fields; };
        static const ExpectedSchema schemaManifest[] = {
            {"root", "elementType,elementId,nativeElementTypeId,elementVariationId,storyIndex,layerIndex,groupElementId,hotlinkElementId,drawingOrderIndex,referenceGroupLayerIndex,hasMemo,renovationStatus,renovationFilterId,database,bounds,units,dimensionPreferences,identity,geometryAndSettings,orderedPoints,standaloneNotes,status,coverage,unicodeAccessCoverage,sourceFidelityCoverage,readConsistency"},
            {"guid", "guid"},
            {"database", "status,databaseId,name,title,reference,databaseType,databaseIndex,linkedElementId,linkedDatabaseId,linkedSegmentMemoIndex,errorCode"},
            {"bounds", "status,xMin,yMin,xMax,yMax,zMin,zMax,unit,errorCode"},
            {"units", "coordinates,linearMeasurements,angles,noteHeights,markerSizes"},
            {"preferences", "status,source,scope,savedViewEffectiveFormat,formatCoverage,errorCode,linear,radial,level,elevation,doorWindow,parapet,area,angle,dimensionStyleAttributeIndex"},
            {"lengthPrefs", "unit,roundInch,decimals,showZeroWhole,showZeroInch,showSmall5,hideZeroDecimals"},
            {"anglePrefs", "unit,decimals,accuracy,hideZeroDecimals"}, {"areaPrefs", "unit,decimals,hideZeroDecimals"},
            {"identity", "family,contextStatus"}, {"availability", "status"}, {"readConsistency", "status"},
            {"linearGeometry", "linePosition,direction,directionUnit,threeDimensionalPosition,threeDimensionalDirection,threeDimensionalNormal,arcAnchor,segmentCount,linePen,textPosition,textDirection,appearance,detailedOpeningDimension,nominalOpeningDimension,compositeThicknessMode,defaultStaticSetting,defaultWitnessForm,defaultWitnessValue,defaultNote,defaultNoteUnicodeStatus,marker,elevationMarkerForm,elevationArrowAngleCode,elevationArrowFilled,elevationArrowUp,elevationNeedsPlus,usedIn3D,horizontalText,onlyDimensionText,elevationOrigin,circleLevelFill,layout,pushNeighborEnabled,placeUnderEnabled,advancedLayout,clipOtherSide,textFillOrder,measurement,staticAnchorEvidence,pointCount"},
            {"radialGeometry", "base,endPosition,measuredRadius,measurementSource,display,linePen,textPosition,textDirection,showOrigin,onlyDimensionText,prefix,prefixSource,prefixUnicodeStatus,marker,staticAnchorEvidence"},
            {"angleGeometry", "arcPosition,arcOrigin,measuredAngle,measurementUnit,measurementSource,display,arcRadius,arcRadiusUnit,linePen,smallArc,onlyDimensionText,textPosition,textDirection,witnessForm,witnessValue,marker,anchors"},
            {"levelGeometry", "markerPosition,levelValue,levelMeasurementSource,display,pen,format,needsPlus,staticLevel,markerSize,markerSizeUnit,markerAngle,markerAngleUnit,parentTypeVariationId,parentTypeId,parentElementId,parentAssociationStatus,parentFloorSemantics,origin"},
            {"coord2", "x,y,unit"}, {"coord3", "x,y,z,unit"}, {"vector2", "x,y"}, {"vector3", "x,y,z"},
            {"anchor", "elementVariationId,elementTypeId,lineReference,specialReference,nodeIndex,nativeNodeId,sectionNodeId,elementId,location,location3D,associationStatus"},
            {"point", "index,anchor,dimensionPosition,dimensionPosition3D,measuredValueFromPreviousPoint,measurementSource,witnessForm,witnessValue,fixedPosition,note"},
            {"slot", "slot,applicability,details"}, {"marker", "markerType,markerPen,markerSize,markerSizeUnit"},
            {"measurement", "source,display"}, {"textFillOrder", "status"},
            {"note", "pen,font,layer,textHeight,textHeightUnit,angle,angleUnit,position,content,contentStatus,contentSource,nativeContent,fixedContent,sourceExpression,contentType,contentMode,fixPos,styleBits,opaque,frame,backgroundPen,framePen,useLeaderLine,anchorAtTextEnd,leaderBegin,leaderMiddle,leaderEnd,leaderPen,leaderLineType,leaderShape,leaderArrowType,leaderArrowAtBegin,leaderArrowAtEnd,leaderArrowPen,leaderArrowSize,anchorPoint,justification,widthFactor,spacingFactor,rowAnchor,effectBits,observedDisplay"},
            {"nativeContent", "value,accessStatus,accessSource,meaningStatus"}, {"fixedContent", "status,nulTerminated,value"},
            {"sourceExpression", "status,reason"}, {"observedDisplay", "status,reason,value,contentSource"}
        };
        for (const ExpectedSchema& schema : schemaManifest) {
            std::set<std::string> expected;
            std::stringstream csv (schema.fields);
            std::string item;
            while (std::getline (csv, item, ',')) expected.insert (item);
            passed &= Require (expected == NativeDimensionFacts::FieldsFor (schema.path),
                (std::string ("Closed factual schema changed or lost a path field at ") + schema.path).c_str ());
        }

        // All captured rows are parsed through the same ObjectState helper as
        // the APX. E729 is captured partial evidence, not live qualification.
        // Radial cases are explicitly synthetic, not a live-capture claim.
        GS::ObjectState syntheticRadialA = makeRow ("RadialDimension", "radial", "abcdef12-1234-1234-1234-abcdef123456");
        GS::ObjectState syntheticRadialB = makeRow ("RadialDimension", "radial", "abcdef12-1234-1234-1234-abcdef123456");
        syntheticRadialA.Add ("geometryAndSettings", GS::ObjectState ("measuredRadius", 4.0, "measurementSource", "native-radius"));
        syntheticRadialB.Add ("geometryAndSettings", GS::ObjectState ("measuredRadius", 5.0, "measurementSource", "native-radius"));
        passed &= Require (Fingerprint (syntheticRadialA) != Fingerprint (syntheticRadialB), "Synthetic radial measurement change did not change the factual fingerprint.");

        using NativeDimensionAuditPolicy::Bounds2D;
        const Bounds2D rectangle { 0.0, 0.0, 10.0, 10.0 };
        passed &= Require (NativeDimensionAuditPolicy::IsFiniteOrderedBounds (rectangle), "A finite ordered rectangle was rejected.");
        passed &= Require (NativeDimensionAuditPolicy::IntersectsInclusive (rectangle, Bounds2D { 10.0, 3.0, 12.0, 4.0 }), "Inclusive boundary contact was not selected.");
        passed &= Require (!NativeDimensionAuditPolicy::IntersectsInclusive (rectangle, Bounds2D { 10.0001, 3.0, 12.0, 4.0 }), "A disjoint candidate was selected.");
        passed &= Require (!NativeDimensionAuditPolicy::IntersectsInclusive (rectangle, Bounds2D { 2.0, 2.0, 1.0, 3.0 }), "An inverted candidate bound was accepted.");
        passed &= Require (!NativeDimensionAuditPolicy::IsFiniteOrderedBounds (Bounds2D { 0.0, 0.0, std::numeric_limits<double>::infinity (), 1.0 }), "A non-finite candidate bound was accepted.");
        passed &= Require (NativeDimensionAuditPolicy::WithinCandidateLimit (4096) && !NativeDimensionAuditPolicy::WithinCandidateLimit (4097), "The fixed candidate cap is not exact.");
        passed &= Require (NativeDimensionAuditPolicy::WithinMatchedParentLimit (512) && !NativeDimensionAuditPolicy::WithinMatchedParentLimit (513), "The fixed matched-parent cap is not exact.");
        passed &= Require (NativeDimensionAuditPolicy::WithinStoredNoteLimit (16384) && !NativeDimensionAuditPolicy::WithinStoredNoteLimit (16385), "The fixed stored-note cap is not exact.");
        passed &= Require (NativeDimensionAuditPolicy::WithinSerializedFactLimit (16ULL * 1024ULL * 1024ULL)
            && !NativeDimensionAuditPolicy::WithinSerializedFactLimit (16ULL * 1024ULL * 1024ULL + 1ULL), "The fixed serialized-facts byte cap is not exact.");

        if (!passed) return 1;
        std::cout << "NativeDimensionFacts shared-helper regression passed; full-root nested field mutations=" << rootMutationCount
            << " across " << jsonRows.Size () << " captured rows plus synthetic radial; list-order/cardinality="
            << orderSensitiveLists << "/" << cardinalitySensitiveLists << ".\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
