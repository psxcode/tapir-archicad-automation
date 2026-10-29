#pragma once

#include "CommandBase.hpp"
#include "NativeCrudSchema.generated.hpp"

#include <initializer_list>
#include <string>

#ifndef TAPIR_AC_VERSION
#define TAPIR_AC_VERSION 0
#endif
#ifndef TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE
#define TAPIR_ENABLE_NATIVE_DIMENSION_CREATE_CANDIDATE 0
#endif

// Internal boundary of the native CRUD commands.  Each NativeCrud*.cpp translation unit owns one
// responsibility and shares only the constants, the struct and the functions declared here;
// everything else stays in that unit's anonymous namespace.  Nothing here is part of the MCP or
// GraphQL surface.
namespace NativeCrudInternal {

constexpr const char* ContractVersion = "native-crud-v2";

constexpr const char* SnapshotVersion = "native-element-snapshot-v1";

constexpr const char* ReceiptVersion = "native-mutation-receipt-v1";

constexpr const char* HistoryVersion = "native-mutation-history-v1";

constexpr const char* ImpactEvidenceVersion = "native-impact-evidence-v1";

constexpr const char* ImpactMetadataKey = "nativeImpact";

// The limits are generated from NATIVE_SCHEMA_LIMITS (src/native/schema/capability.ts).
constexpr GSSize MaxMutationItems = NativeCrudSchema::MaxItems;

// D-357: the Native write path (MutateNativeElements and every operation of a
// MutateNativeTransaction) admits up to 500 items per operation.  Read
// commands keep the generated MaxMutationItems cap.
constexpr GSSize MaxNativeWriteItems = 500;

constexpr GSSize MaxDiscoveryReferences = NativeCrudSchema::MaxDiscoverReferences;

constexpr GSSize MaxHistoryEntries = NativeCrudSchema::MaxHistoryEntries;

constexpr GSSize MaxImpactElements = NativeCrudSchema::ImpactClosure;

struct NativeImpactEnvelope {
    GS::ObjectState envelope;
    GS::Array<GS::ObjectState> collateralIds;
    GS::Array<GS::ObjectState> impactIds;
    GS::Array<GS::UniString> collateralGuids;
    GS::Array<GS::UniString> collateralElementTypes;
};

// NativeCrudCommon.cpp: scope, contract-version and type-name helpers shared by every native CRUD command
bool IsSupportedNativeType (const API_ElemTypeID typeID);
GS::UniString CanonicalElementTypeName (const GS::UniString& typeName);
GS::Optional<GS::UniString> CurrentSoloFileScope ();
GS::Optional<GS::UniString> ValidateScope (const GS::ObjectState& parameters);
GS::ObjectState ScopeError (const GS::Optional<GS::UniString>& error);
GS::ObjectState NativeContractError (const char* code, const GS::UniString& message);
GS::Optional<GS::UniString> ValidateContractVersion (const GS::ObjectState& parameters);
API_Guid GetRequestedElementGuid (const GS::ObjectState& value);
bool IsHex64 (const GS::UniString& value);
std::string LowerAscii (const GS::UniString& value);

// NativeCrudSnapshots.cpp: canonical writable state, exact snapshots and hosted-opening reads
void CopyStringField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
void CopyIntField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
void CopyDoubleField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
void CopyBoolField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
void CopyObjectField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
void CopyArrayField (const GS::ObjectState& source, const char* sourceName, GS::ObjectState& target, const char* targetName);
GS::ObjectState ExactSnapshots (
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::UniString& scope,
    GS::ProcessControl& processControl,
    bool& complete);

// NativeCrudReceipts.cpp: factual receipts, the process-local receipt cache and delete readback
GS::Optional<GS::ObjectState> FindCachedReceipt (const GS::UniString& operationId, const GS::UniString& expectedScope);
void RememberCachedReceipt (const GS::ObjectState& receipt);
Int64 NextCachedReceiptSequence ();
std::string UtcTimestamp ();
bool ReceiptRequestIdentityMatches (
    const GS::ObjectState& receipt,
    const GS::UniString& expectedScope,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::UniString& requestHash,
    const GS::UniString& description,
    const GS::UniString& operatorNote,
    const GS::UniString& origin,
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::ObjectState* requestedSemanticMetadata);
GS::ObjectState BuildDeleteReadback (
    const GS::Array<GS::ObjectState>& requestedIds,
    const API_ElemTypeID typeID,
    bool& verified,
    GSSize& absentCount);
GS::ObjectState BuildReceipt (
    const GS::UniString& scope,
    const GS::UniString& operationId,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::UniString& description,
    const GS::UniString& origin,
    const GS::UniString& requestHash,
    const GS::ObjectState& beforeResponse,
    const GS::ObjectState& afterResponse,
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::Array<GS::ObjectState>& impactIds,
    const bool complete,
    const bool dispatchComplete,
    const GS::ObjectState* semanticMetadata,
    const GS::UniString& operatorNote,
    const GS::ObjectState* mutationDiagnostics = nullptr,
    const char* statusOverride = nullptr,
    const API_Guid* normalizeSurvivor = nullptr,
    const NativeImpactEnvelope* impact = nullptr);

// NativeCrudImpact.cpp: Wall/Zone impact envelope parsing and snapshot-type validation
GS::Optional<GS::UniString> ParseNativeImpactEnvelope (
    const GS::ObjectState* semanticMetadata,
    const GS::UniString& scope,
    const GS::UniString& operationId,
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& requestedIds,
    NativeImpactEnvelope& output);
GS::Optional<GS::UniString> ValidateNativeImpactSnapshotTypes (
    const GS::Array<GS::ObjectState>& beforeRows,
    const GS::Array<GS::ObjectState>& impactIds,
    const GS::Array<GS::ObjectState>& requestedIds,
    const GS::UniString& typeName,
    const NativeImpactEnvelope* impact);

// NativeCrudWire.cpp: mutation item to MutateElements wire mapping
GS::ObjectState BuildNativeMutationParameters (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items);

// NativeCrudValidation.cpp: typed payload validators, writer gates and mutation item validation
bool HasNativeCrudTestFaultMetadata (const GS::ObjectState* semanticMetadata);
bool HasOnlyObjectStateFields (const GS::ObjectState& value, const std::initializer_list<const char*>& allowedFields);

bool HasOnlyObjectStateFields (const GS::ObjectState& value, const NativeCrudSchema::FieldList& allowedFields);
GS::Optional<GS::UniString> ValidateZoneTypedWriter (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items);
bool HasAutomaticZonePolygonProbeGate (const GS::ObjectState* semanticMetadata);
bool HasAnyZoneMemoWriterGate (const GS::ObjectState* semanticMetadata);
GS::Optional<GS::UniString> ValidateArcTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateHatchTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateZoneMemoWriterCandidate (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata);
GS::Optional<GS::UniString> ValidateDoorTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateWindowTypedPayloads (const GS::UniString& operation, const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateDoorCloneSource (const API_Guid& sourceGuid);
GS::Optional<GS::UniString> ValidateWindowCloneSource (const API_Guid& sourceGuid);
GS::Optional<GS::UniString> ValidateNativeCreatePlacement (
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateMutationItems (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items);

// D-347 stage 3b (D-350): a CREATE whose items restore deleted elements from one checkpoint module.
bool IsModuleRestoreRequest (const GS::Array<GS::ObjectState>& items);
GS::Optional<GS::UniString> ValidateModuleRestoreItem (
    const GS::UniString& typeName,
    const GS::ObjectState& item,
    const GS::ObjectState& first);
struct ModuleRestoreOutcome {
    /** Refused before anything was placed: the error response, nothing to record. */
    GS::Optional<GS::ObjectState> refusal;
    /** The command ran: created elements in item order, completion, rollback and moduleRestore evidence. */
    GS::ObjectState mutationResult;
    /** Archicad rolled the command back and the project's GUIDs and module nodes are as before it (R1). */
    bool rollbackProven = false;
};
ModuleRestoreOutcome ExecuteModuleRestore (
    const GS::UniString& operationId,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items);

} // namespace NativeCrudInternal
