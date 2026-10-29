#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

using namespace NativeCrudInternal;

namespace {

constexpr const char* ImpactEvidenceSource = "native-impact-coordinator-v1";

std::atomic<Int64>& SequenceCounter ()
{
    static std::atomic<Int64> sequence (0);
    return sequence;
}

std::mutex& ReceiptCacheMutex ()
{
    static std::mutex mutex;
    return mutex;
}

// Factual receipts are owned durably by the MCP SQLite store.  The APX only
// keeps a small process-local cache so a response timeout can be reconciled
// while this Archicad process is still alive.  Keeping the cache bounded is
// important: it must never become a second unbounded history database or
// couple durable history to an Archicad instance lifecycle.
struct CachedReceipt {
    std::string operationId;
    std::string fileScopeKey;
    GS::ObjectState receipt;
};

std::vector<CachedReceipt>& ReceiptCache ()
{
    static std::vector<CachedReceipt> cache;
    return cache;
}

} // namespace

namespace NativeCrudInternal {

GS::Optional<GS::ObjectState> FindCachedReceipt (const GS::UniString& operationId, const GS::UniString& expectedScope)
{
    const std::string key (operationId.ToCStr (CC_UTF8).Get ());
    const std::string scope (expectedScope.ToCStr (CC_UTF8).Get ());
    std::lock_guard<std::mutex> lock (ReceiptCacheMutex ());
    auto& cache = ReceiptCache ();
    for (auto it = cache.rbegin (); it != cache.rend (); ++it) {
        if (it->operationId == key && it->fileScopeKey == scope) {
            // Touch the entry on lookup so the bounded process-local cache is
            // an actual LRU rather than FIFO.  A frequently reconciled
            // operation must not evict itself while newer receipts arrive.
            CachedReceipt hit = *it;
            cache.erase (std::next (it).base ());
            cache.push_back (hit);
            return hit.receipt;
        }
    }
    return GS::NoValue;
}

void RememberCachedReceipt (const GS::ObjectState& receipt)
{
    GS::UniString operationId;
    GS::UniString fileScopeKey;
    if (!receipt.Get ("operationId", operationId) || operationId.IsEmpty ()
        || !receipt.Get ("fileScopeKey", fileScopeKey) || fileScopeKey.IsEmpty ())
        return;
    const std::string key (operationId.ToCStr (CC_UTF8).Get ());
    const std::string scope (fileScopeKey.ToCStr (CC_UTF8).Get ());
    std::lock_guard<std::mutex> lock (ReceiptCacheMutex ());
    auto& cache = ReceiptCache ();
    for (CachedReceipt& entry : cache) {
        if (entry.operationId == key && entry.fileScopeKey == scope) {
            entry.receipt = receipt;
            return;
        }
    }
    constexpr GSSize MaxCachedReceipts = 256;
    if (cache.size () >= static_cast<std::size_t> (MaxCachedReceipts))
        cache.erase (cache.begin ());
    cache.push_back ({ key, scope, receipt });
}

Int64 NextCachedReceiptSequence ()
{
    // This sequence is APX-process diagnostic metadata only.  The durable
    // per-file sequence is allocated by MCP SQLite when the prepared row is
    // admitted; the TypeScript store normalizes terminal rows to that
    // sequence.  It is intentionally not persisted by the APX.
    return SequenceCounter ().fetch_add (1) + 1;
}

std::string UtcTimestamp ()
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now ();
    const std::time_t time = std::chrono::system_clock::to_time_t (now);
    std::tm utc = {};
#if defined (_WIN32)
    gmtime_s (&utc, &time);
#else
    gmtime_r (&time, &utc);
#endif
    char value[32] = {};
    std::strftime (value, sizeof (value), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return value;
}

} // namespace NativeCrudInternal

namespace {

bool ReceiptPrimaryGuidsMatch (
    const GS::ObjectState& receipt,
    const GS::Array<GS::ObjectState>& requestedIds)
{
    GS::Array<GS::UniString> recordedGuids;
    if (!receipt.Get ("primaryGuids", recordedGuids)
        || recordedGuids.GetSize () != requestedIds.GetSize ())
        return false;

    GS::Array<API_Guid> expected;
    for (const GS::ObjectState& requestedId : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        if (guid == APINULLGuid || expected.Contains (guid))
            return false;
        expected.Push (guid);
    }

    GS::Array<API_Guid> recorded;
    for (const GS::UniString& recordedGuid : recordedGuids) {
        const API_Guid guid = APIGuidFromString (recordedGuid.ToCStr ());
        if (guid == APINULLGuid || recorded.Contains (guid))
            return false;
        recorded.Push (guid);
    }

    for (const API_Guid& guid : expected)
        if (!recorded.Contains (guid)) return false;
    return true;
}

bool ReceiptImpactEnvelopeMatches (
    const GS::ObjectState& receipt,
    const GS::ObjectState* requestedSemanticMetadata)
{
    const GS::ObjectState* requestedImpact = requestedSemanticMetadata == nullptr
        ? nullptr
        : requestedSemanticMetadata->Get (ImpactMetadataKey);
    const GS::ObjectState* receiptSemanticMetadata = receipt.Get ("semanticMetadata");
    const GS::ObjectState* receiptImpact = receiptSemanticMetadata == nullptr
        ? nullptr
        : receiptSemanticMetadata->Get (ImpactMetadataKey);
    if ((requestedImpact == nullptr) != (receiptImpact == nullptr)) return false;
    if (requestedImpact == nullptr) return true;
    GS::UniString requestedJson;
    GS::UniString receiptJson;
    if (JSON::CreateFromObjectState (*requestedImpact, requestedJson) != NoError
        || JSON::CreateFromObjectState (*receiptImpact, receiptJson) != NoError)
        return false;
    // The TypeScript coordinator canonicalizes the complete carrier before
    // requestHash and dispatch.  Comparing the complete persisted carrier,
    // not only primary IDs or a hash-shaped string, binds cache replay to the
    // exact closure/evidence contract.
    return requestedJson == receiptJson;
}

} // namespace

namespace NativeCrudInternal {

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
    const GS::ObjectState* requestedSemanticMetadata)
{
    GS::UniString recordedScope;
    GS::UniString recordedOperation;
    GS::UniString recordedType;
    GS::UniString recordedHash;
    GS::UniString recordedDescription;
    GS::UniString recordedOrigin;
    GS::UniString recordedOperatorNote;
    if (!receipt.Get ("fileScopeKey", recordedScope)
        || !receipt.Get ("operation", recordedOperation)
        || !receipt.Get ("elementType", recordedType)
        || !receipt.Get ("requestHash", recordedHash)
        || !receipt.Get ("description", recordedDescription)
        || !receipt.Get ("origin", recordedOrigin))
        return false;

    const GS::UniString expectedOrigin = origin.IsEmpty () ? GS::UniString ("NATIVE") : origin;
    const bool hasRecordedOperatorNote = receipt.Get ("operatorNote", recordedOperatorNote);
    if (recordedScope != expectedScope
        || recordedOperation != operation
        || recordedType != typeName
        || LowerAscii (recordedHash) != LowerAscii (requestHash)
        || recordedDescription != description
        || recordedOrigin != expectedOrigin
        || hasRecordedOperatorNote != !operatorNote.IsEmpty ()
        || (hasRecordedOperatorNote && recordedOperatorNote != operatorNote))
        return false;
    if (!ReceiptImpactEnvelopeMatches (receipt, requestedSemanticMetadata)) return false;

    // CREATE has no caller-supplied primary GUIDs; its typed payload and
    // clone source are covered by requestHash.  UPDATE/DELETE must also
    // match the exact GUID set so an operation-id typo cannot replay a
    // receipt for a different set of native elements.
    return operation == "create" || ReceiptPrimaryGuidsMatch (receipt, requestedIds);
}

} // namespace NativeCrudInternal

namespace {

GS::ObjectState ChangedField (const GS::ObjectState* before, const GS::ObjectState* after)
{
    GS::ObjectState field;
    field.Add ("path", "writableState");
    if (before != nullptr) field.Add ("before", *before);
    if (after != nullptr) field.Add ("after", *after);
    return field;
}

} // namespace

namespace NativeCrudInternal {

// DeleteElementsCommand owns its implementation-local readback helper.  The
// versioned native envelope needs the same per-GUID absence proof without
// depending on that translation unit, so keep a small contract-local helper
// here.  A non-BADID header error is deliberately not treated as proof of
// deletion.
GS::ObjectState BuildDeleteReadback (
    const GS::Array<GS::ObjectState>& requestedIds,
    const API_ElemTypeID typeID,
    bool& verified,
    GSSize& absentCount)
{
    GS::ObjectState readback;
    const auto& deleted = readback.AddList<GS::ObjectState> ("deleted");
    verified = true;
    absentCount = 0;
    for (const GS::ObjectState& requestedId : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (requestedId);
        API_Elem_Head header = {};
        header.guid = guid;
        const GSErrCode headerError = ACAPI_Element_GetHeader (&header);
        const bool absent = headerError == APIERR_BADID;
        GS::ObjectState status;
        status.Add ("elementId", requestedId);
        status.Add ("absent", absent);
        if (headerError != NoError && headerError != APIERR_BADID) {
            status.Add ("readbackErrorCode", headerError);
            verified = false;
        }
        if (absent) {
            ++absentCount;
        } else {
            verified = false;
            if (headerError == NoError)
                status.Add ("elementType", GetElementTypeNonLocalizedName (GetElemTypeId (header)));
        }
        deleted (status);
    }
    (void) typeID;
    return readback;
}

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
    const GS::ObjectState* mutationDiagnostics,
    const char* statusOverride,
    const API_Guid* normalizeSurvivor,
    const NativeImpactEnvelope* impact)
{
    GS::ObjectState receipt;
    receipt.Add ("schemaVersion", ReceiptVersion);
    receipt.Add ("operationId", operationId);
    receipt.Add ("fileScopeKey", scope);
    receipt.Add ("terminalAt", GS::UniString (UtcTimestamp ().c_str (), CC_UTF8));
    receipt.Add ("operation", operation);
    receipt.Add ("elementType", typeName);
    // A host-side validation/transaction rejection is a known terminal fact;
    // it must not be reported as an evidence gap (which would unnecessarily
    // poison reconciliation).  Only a successfully dispatched operation with
    // missing before/after evidence is `incomplete_evidence`.
    const char* status = statusOverride != nullptr
        ? statusOverride
        : !dispatchComplete ? "failed" : complete ? "applied" : "incomplete_evidence";
    receipt.Add ("status", status);
    receipt.Add ("description", description);
    if (!operatorNote.IsEmpty ()) receipt.Add ("operatorNote", operatorNote);
    receipt.Add ("origin", origin.IsEmpty () ? GS::UniString ("NATIVE") : origin);
    receipt.Add ("requestHash", requestHash);
    if (semanticMetadata != nullptr)
        receipt.Add ("semanticMetadata", *semanticMetadata);
    const auto& primary = receipt.AddList<GS::UniString> ("primaryGuids");
    for (const GS::ObjectState& id : requestedIds) {
        const API_Guid guid = GetRequestedElementGuid (id);
        primary (APIGuidToString (guid));
    }
    const auto& collateral = receipt.AddList<GS::UniString> ("collateralGuids");
    if (impact != nullptr)
        for (const GS::UniString& guid : impact->collateralGuids) collateral (guid);
    const auto& elements = receipt.AddList<GS::ObjectState> ("elements");
    GS::Array<GS::ObjectState> before;
    GS::Array<GS::ObjectState> after;
    beforeResponse.Get ("elements", before);
    afterResponse.Get ("elements", after);
    for (const GS::ObjectState& id : impactIds) {
        const API_Guid guid = GetRequestedElementGuid (id);
        const GS::ObjectState* beforeSnapshot = nullptr;
        const GS::ObjectState* afterSnapshot = nullptr;
        for (const GS::ObjectState& candidate : before) {
            const GS::ObjectState* candidateID = candidate.Get ("elementId");
            if (candidateID != nullptr && GetGuidFromObjectState (*candidateID) == guid) beforeSnapshot = &candidate;
        }
        for (const GS::ObjectState& candidate : after) {
            const GS::ObjectState* candidateID = candidate.Get ("elementId");
            if (candidateID != nullptr && GetGuidFromObjectState (*candidateID) == guid) afterSnapshot = &candidate;
        }
        GS::ObjectState element;
        element.Add ("elementId", id);
        bool primaryElement = false;
        for (const GS::ObjectState& primaryId : requestedIds) {
            if (GetRequestedElementGuid (primaryId) == guid) {
                primaryElement = true;
                break;
            }
        }
        GS::UniString snapshotType;
        if (beforeSnapshot != nullptr && beforeSnapshot->Get ("elementType", snapshotType))
            element.Add ("elementType", snapshotType);
        else if (afterSnapshot != nullptr && afterSnapshot->Get ("elementType", snapshotType))
            element.Add ("elementType", snapshotType);
        else if (impact != nullptr && !primaryElement) {
            GS::UniString collateralType;
            const GSSize collateralIdCount = static_cast<GSSize> (impact->collateralIds.GetSize ());
            const GSSize collateralTypeCount = static_cast<GSSize> (impact->collateralElementTypes.GetSize ());
            for (GSSize index = 0; index < collateralIdCount; ++index) {
                if (GetRequestedElementGuid (impact->collateralIds[index]) == guid
                    && index < collateralTypeCount) {
                    collateralType = impact->collateralElementTypes[index];
                    break;
                }
            }
            element.Add ("elementType", collateralType.IsEmpty () ? typeName : collateralType);
        } else
            element.Add ("elementType", typeName);
        element.Add ("role", primaryElement ? "PRIMARY" : "COLLATERAL");
        const bool isNormalizedDelete = operation == "normalize_multiline"
            && normalizeSurvivor != nullptr
            && guid != *normalizeSurvivor;
        element.Add ("action", isNormalizedDelete ? "delete" : operation == "normalize_multiline" ? "update" : operation);
        if (beforeSnapshot != nullptr) element.Add ("beforeState", *beforeSnapshot);
        if (afterSnapshot != nullptr) element.Add ("afterState", *afterSnapshot);
        if ((operation == "delete" || isNormalizedDelete) && beforeSnapshot != nullptr) {
            const GS::ObjectState* recreate = beforeSnapshot->Get ("recreateState");
            if (recreate != nullptr) element.Add ("recreateState", *recreate);
        }
        const GS::ObjectState* beforeState = beforeSnapshot == nullptr ? nullptr : beforeSnapshot->Get ("writableState");
        const GS::ObjectState* afterState = afterSnapshot == nullptr ? nullptr : afterSnapshot->Get ("writableState");
        const auto& changed = element.AddList<GS::ObjectState> ("changedFields");
        if (beforeState != nullptr || afterState != nullptr) changed (ChangedField (beforeState, afterState));
        elements (element);
    }
    if (impact != nullptr) {
        bool collateralBeforeComplete = true;
        bool collateralAfterComplete = true;
        for (const GS::ObjectState& collateralId : impact->collateralIds) {
            const API_Guid guid = GetRequestedElementGuid (collateralId);
            bool hasBefore = false;
            for (const GS::ObjectState& row : before) {
                const GS::ObjectState* id = row.Get ("elementId");
                if (id != nullptr && GetGuidFromObjectState (*id) == guid) {
                    hasBefore = true;
                    break;
                }
            }
            bool hasAfter = false;
            for (const GS::ObjectState& row : after) {
                const GS::ObjectState* id = row.Get ("elementId");
                if (id != nullptr && GetGuidFromObjectState (*id) == guid) {
                    hasAfter = true;
                    break;
                }
            }
            collateralBeforeComplete = collateralBeforeComplete && hasBefore;
            collateralAfterComplete = collateralAfterComplete && (operation == "delete" ? true : hasAfter);
        }
        const bool impactCoverageComplete = complete && collateralBeforeComplete && collateralAfterComplete;
        GS::ObjectState evidence;
        evidence.Add ("schemaVersion", ImpactEvidenceVersion);
        evidence.Add ("source", ImpactEvidenceSource);
        evidence.Add ("envelope", impact->envelope);
        evidence.Add ("impactComplete", impactCoverageComplete);
        evidence.Add ("predictedImpactComplete", true);
        evidence.Add ("collateralBeforeComplete", collateralBeforeComplete);
        evidence.Add ("collateralAfterComplete", collateralAfterComplete);
        evidence.Add ("coverage", impactCoverageComplete ? "complete" : "incomplete");
        receipt.Add ("impactEvidence", evidence);
        GS::ObjectState dependency = evidence;
        dependency.Add ("status", status);
        receipt.Add ("dependencyEvidence", dependency);
    }
    GS::ObjectState transaction (
        "atomic", true,
        "undoable", true,
        "dispatchAttempted", true,
        "dispatchComplete", dispatchComplete,
        "evidenceComplete", complete,
        "outcome", status);
    if (mutationDiagnostics != nullptr) {
        bool transactionRolledBack = false;
        if (mutationDiagnostics->Get ("transactionRolledBack", transactionRolledBack))
            transaction.Add ("transactionRolledBack", transactionRolledBack);
        Int32 transactionError = 0;
        if (mutationDiagnostics->Get ("transactionError", transactionError))
            transaction.Add ("transactionError", transactionError);
        GS::UniString failureStage;
        if (mutationDiagnostics->Get ("failureStage", failureStage))
            transaction.Add ("failureStage", failureStage);
        bool nativeApiErrorCodeRecorded = false;
        bool nativeFailureMessageRecorded = false;
        bool nativeCommandFailureObserved = false;
        const GS::ObjectState* nativeCommandError = mutationDiagnostics->Get ("error");
        if (nativeCommandError != nullptr) {
            Int32 nativeApiErrorCode = 0;
            if (nativeCommandError->Get ("code", nativeApiErrorCode)) {
                transaction.Add ("nativeApiErrorCode", nativeApiErrorCode);
                nativeApiErrorCodeRecorded = true;
                nativeCommandFailureObserved = true;
            }
            GS::UniString nativeFailureMessage;
            if (nativeCommandError->Get ("message", nativeFailureMessage)) {
                transaction.Add ("nativeFailureMessage", nativeFailureMessage);
                nativeFailureMessageRecorded = true;
                nativeCommandFailureObserved = true;
            }
        }
        // Typed writers keep the API failure beside the failed execution row.
        // Preserve that exact diagnostic in the terminal receipt even when
        // the host did not return an undo-command error; equal snapshots alone
        // do not prove that Archicad rolled back the attempted change.
        GS::Array<GS::ObjectState> executionResults;
        if (mutationDiagnostics->Get ("executionResults", executionResults)) {
            for (const GS::ObjectState& executionResult : executionResults) {
                bool success = false;
                if (!executionResult.Get ("success", success) || success)
                    continue;
                if (failureStage.IsEmpty () && executionResult.Get ("failureStage", failureStage))
                    transaction.Add ("failureStage", failureStage);
                const GS::ObjectState* nativeError = executionResult.Get ("error");
                Int32 nativeApiErrorCode = 0;
                if (!nativeApiErrorCodeRecorded && nativeError != nullptr && nativeError->Get ("code", nativeApiErrorCode))
                    transaction.Add ("nativeApiErrorCode", nativeApiErrorCode);
                GS::UniString nativeFailureMessage;
                if (!nativeFailureMessageRecorded && nativeError != nullptr && nativeError->Get ("message", nativeFailureMessage))
                    transaction.Add ("nativeFailureMessage", nativeFailureMessage);
                const GS::ObjectState* nativeFailureDetails = executionResult.Get ("nativeFailureDetails");
                if (nativeFailureDetails != nullptr)
                    transaction.Add ("nativeFailureDetails", *nativeFailureDetails);
                break;
            }
        }
        if (failureStage.IsEmpty () && nativeCommandFailureObserved)
            transaction.Add ("failureStage", "native-mutation-command");
        bool groupModeRestored = false;
        if (mutationDiagnostics->Get ("groupModeRestored", groupModeRestored))
            transaction.Add ("groupModeRestored", groupModeRestored);
        // D-347 stage 3b: the translation, the old-to-new pairs and the rollback proof of a module restore.
        const GS::ObjectState* moduleRestore = mutationDiagnostics->Get ("moduleRestore");
        if (moduleRestore != nullptr)
            transaction.Add ("moduleRestore", *moduleRestore);
        Int32 groupModeRestoreError = 0;
        if (mutationDiagnostics->Get ("groupModeRestoreError", groupModeRestoreError))
            transaction.Add ("groupModeRestoreError", groupModeRestoreError);
#if defined(TAPIR_NATIVE_CRUD_TEST_FAULT_AFTER_FIRST_APPLIED)
        const GS::ObjectState* testFaultEvidence = mutationDiagnostics->Get ("testFaultEvidence");
        if (testFaultEvidence != nullptr)
            transaction.Add ("testFaultEvidence", *testFaultEvidence);
#endif
    }
    receipt.Add ("transaction", transaction);
    return receipt;
}

} // namespace NativeCrudInternal
