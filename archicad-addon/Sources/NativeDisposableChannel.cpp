#include "NativeDisposableChannel.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_CHANNEL_PROBE)

#include "channel.hpp"

// The prior skeleton's literal "not-configured" outcome is retained only as
// a source-compatibility marker; this implementation returns unavailable or
// the explicit root-binding outcome when inherited handles are absent.

namespace {

constexpr const char* EvidenceVersion = "native-disposable-channel-evidence-v1";

} // namespace

NativeDisposableChannelProbeCommand::NativeDisposableChannelProbeCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableChannelProbeCommand::GetName () const
{
    return "NativeDisposableChannelProbe";
}

GS::Optional<GS::UniString> NativeDisposableChannelProbeCommand::GetInputParametersSchema () const
{
    // The schema is deliberately a strict locator/attestation envelope.  The
    // `authority` value is the literal `none`; no JSON member can arm the
    // command.  Actual inherited handle state, when a future Windows adapter
    // exists, must be supplied outside this JSON contract.
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "channelVersion": { "enum": ["native-disposable-channel-v1"] },
            "command": { "enum": ["read-only-binding-probe-v1"] },
            "runId": { "type": "string", "minLength": 1, "maxLength": 256 },
            "runNonce": { "type": "string", "minLength": 1, "maxLength": 256 },
            "sessionId": { "type": "string", "minLength": 1, "maxLength": 256 },
            "sessionNonce": { "type": "string", "minLength": 1, "maxLength": 256 },
            "sequence": { "type": "integer", "minimum": 1 },
            "challenge": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "issuedAt": { "type": "string", "minLength": 1 },
            "ttlMs": { "type": "integer", "minimum": 1, "maximum": 30000 },
            "deadline": { "type": "string", "minLength": 1 },
            "previousTranscriptHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "transcriptHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "authority": { "enum": ["none"] },
            "writePolicy": { "enum": ["zero-write"] },
            "bindingId": { "type": "string", "minLength": 1, "maxLength": 256 },
            "archicadMajor": { "type": "integer", "enum": [25, 28] },
            "runner": {
                "type": "object", "additionalProperties": false,
                "properties": {
                    "identity": { "type": "string", "minLength": 1 },
                    "version": { "type": "string", "minLength": 1 }
                },
                "required": ["identity", "version"]
            },
            "process": {
                "type": "object", "additionalProperties": false,
                "properties": {
                    "pid": { "type": "integer", "minimum": 1 },
                    "creationToken": { "type": "string", "minLength": 1 },
                    "executable": { "type": "string", "minLength": 1 },
                    "identity": { "type": "string", "minLength": 1 }
                },
                "required": ["pid", "creationToken", "executable", "identity"]
            },
            "apx": {
                "type": "object", "additionalProperties": false,
                "properties": {
                    "path": { "type": "string", "minLength": 1 },
                    "loadedPath": { "type": "string", "minLength": 1 },
                    "fileHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
                    "loadedFileHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
                    "buildId": { "type": "string", "minLength": 1 }
                },
                "required": ["path", "loadedPath", "fileHash", "loadedFileHash", "buildId"]
            },
            "file": {
                "type": "object", "additionalProperties": false,
                "properties": {
                    "path": { "type": "string", "minLength": 1 },
                    "identity": { "type": "string", "minLength": 1 },
                    "hash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
                    "scopeKey": { "type": "string", "minLength": 1 },
                    "generation": { "type": "integer", "minimum": 1 }
                },
                "required": ["path", "identity", "hash", "scopeKey", "generation"]
            },
            "projectGeneration": { "type": "integer", "minimum": 1 },
            "manifestHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "caseHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "scopeHash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
            "inheritedHandles": {
                "type": "object", "additionalProperties": false,
                "properties": {
                    "version": { "enum": ["native-disposable-handle-policy-v1"] },
                    "mode": { "enum": ["explicit-allowlist"] },
                    "source": { "enum": ["test-stub", "windows-inherited", "unavailable"] },
                    "required": {
                        "type": "array", "minItems": 4, "maxItems": 4,
                        "items": { "enum": ["channel-read", "channel-write", "process-lifecycle", "disposable-file-read"] }
                    },
                    "inherited": {
                        "type": "array", "maxItems": 4,
                        "items": { "enum": ["channel-read", "channel-write", "process-lifecycle", "disposable-file-read"] }
                    },
                    "liveVerified": { "enum": [false] }
                },
                "required": ["version", "mode", "source", "required", "inherited", "liveVerified"]
            }
        },
        "required": [
            "channelVersion", "command", "runId", "runNonce", "sessionId", "sessionNonce", "sequence", "challenge", "issuedAt", "ttlMs", "deadline",
            "previousTranscriptHash", "transcriptHash", "authority", "writePolicy", "bindingId",
            "archicadMajor", "runner", "process", "apx", "file", "projectGeneration", "manifestHash", "caseHash", "scopeHash", "inheritedHandles"
        ]
    })";
}

GS::Optional<GS::UniString> NativeDisposableChannelProbeCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["attested", "unavailable", "rejected"] },
            "evidenceVersion": { "enum": ["native-disposable-channel-evidence-v1"] },
            "source": { "enum": ["non-live"] },
            "liveVerified": { "enum": [false] },
            "writesPerformed": { "enum": [false] },
            "nativeMutationDispatched": { "enum": [false] },
            "fileMutationObserved": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": [
            "status", "evidenceVersion", "source", "liveVerified", "writesPerformed", "nativeMutationDispatched", "fileMutationObserved", "authority", "reason"
        ]
    })";
}

GS::ObjectState NativeDisposableChannelProbeCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // The only authority source is the real inherited bootstrap/channel
    // handle set owned by the Packet 8B broker.  JSON, argv, environment,
    // ports, build IDs and copied DTOs are deliberately ignored.  The shared
    // implementation performs only observation/attestation and the fixed
    // zero-write probe; it contains no DevKit mutation or save call.
    const auto probe = tapir::disposable_channel::RunApxZeroWriteProbe (TAPIR_AC_VERSION);
    GS::ObjectState result;
    const char* status = "unavailable";
    if (probe.receipt.status == tapir::disposable_channel::ProbeStatus::Attested)
        status = "attested";
    else if (probe.receipt.status == tapir::disposable_channel::ProbeStatus::Rejected)
        status = "rejected";
    result.Add ("status", status);
    result.Add ("evidenceVersion", EvidenceVersion);
    result.Add ("source", "non-live");
    result.Add ("liveVerified", false);
    result.Add ("writesPerformed", false);
    result.Add ("nativeMutationDispatched", false);
    result.Add ("fileMutationObserved", false);
    result.Add ("authority", "none");
    result.Add ("reason", GS::UniString (probe.receipt.reason.c_str (), CC_UTF8));
    return result;
}

#endif
