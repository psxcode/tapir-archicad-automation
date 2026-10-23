#pragma once

#include "CommandBase.hpp"

#include <initializer_list>

// Internal boundary of the typed native CRUD mutation command.  Each
// ElementMutation*.cpp translation unit owns one responsibility and shares only
// the functions declared here; everything else stays in that unit's anonymous
// namespace.  Nothing here is part of the MCP or GraphQL surface.
namespace ElementMutationInternal {

constexpr GSSize MaxMutationItems = 256;

struct ElementTypeSpec {
    API_ElemTypeID typeID;
    const char* createArrayName;
    const char* updateArrayName;
};

// ElementMutationValidation.cpp: type specs, payload/field validation and envelope item validation
const ElementTypeSpec* GetElementTypeSpec (const API_ElemTypeID typeID);
bool IsOneOf (const GS::UniString& value, const std::initializer_list<const char*>& allowedValues);
GS::Optional<GS::UniString> ValidateEnvelopeItem (
    const GS::ObjectState& item,
    const GS::UniString& operation,
    const API_ElemTypeID typeID,
    GS::Array<GS::ObjectState>& normalizedItems,
    GS::Array<API_Guid>& elementGuids);

// ElementMutationOpenings.cpp: Door and Window typed CREATE/UPDATE executors
GS::ObjectState ExecuteDoorCreate (const GS::Array<GS::ObjectState>& items, GS::ProcessControl& /*processControl*/);
GS::ObjectState ExecuteDoorUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied, GS::ProcessControl& /*processControl*/);
GS::ObjectState ExecuteWindowCreate (const GS::Array<GS::ObjectState>& items, GS::ProcessControl& /*processControl*/);
GS::ObjectState ExecuteWindowUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied, GS::ProcessControl& /*processControl*/);

// ElementMutationDrafting.cpp: Line/Arc/Hatch drafting UPDATE executors and hatch ring geometry
GS::ObjectState ExecuteHatchUpdate (const GS::Array<GS::ObjectState>& items, const bool faultAfterFirstApplied);
GS::ObjectState ExecuteDraftingUpdate (
    const API_ElemTypeID typeID,
    const GS::Array<GS::ObjectState>& items,
    const bool faultAfterFirstApplied);

// ElementMutationBim.cpp: Wall typed UPDATE executor and its detail helpers
GS::ObjectState ExecuteNativeCrudWallUpdate (const GS::Array<GS::ObjectState>& items);

// ElementMutationGeneric.cpp: generic CREATE/UPDATE/DELETE executors delegating to the existing Tapir commands
GS::ObjectState ExecuteCreate (
    const API_ElemTypeID typeID,
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl);
GS::ObjectState ExecuteUpdate (
    const API_ElemTypeID typeID,
    const GS::ObjectState& parameters,
    GS::ProcessControl& processControl);
GS::ObjectState ExecuteDelete (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& processControl);

// ElementMutationText.cpp: Text placement, run pens, rotation and the Text CREATE/UPDATE executors
bool IsAngleOnlyTextItem (const GS::ObjectState& item);
GSErrCode ValidateAngleOnlyTextItem (const GS::ObjectState& item);
GS::ObjectState ExecuteTextCreate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& /*processControl*/);
GS::ObjectState ExecuteTextUpdate (
    const GS::Array<GS::ObjectState>& items,
    GS::ProcessControl& /*processControl*/);

// ElementMutationReadback.cpp: section readback, readback envelopes and the mutation postcondition
bool ContainsGuid (const GS::Array<API_Guid>& guids, const API_Guid& candidate);
GS::ObjectState BuildArrayParameters (
    const char* fieldName,
    const GS::Array<GS::ObjectState>& items);
GS::ObjectState BuildElementReadback (
    const GS::Array<API_Guid>& elementGuids,
    const API_ElemTypeID typeID,
    GS::ProcessControl& processControl,
    bool& verified);
GS::ObjectState BuildDeleteReadback (
    const GS::Array<API_Guid>& elementGuids,
    const API_ElemTypeID typeID,
    bool& verified,
    GSSize& absentCount);
GS::Array<API_Guid> CollectCreatedGuids (const GS::ObjectState& mutationResult);
GS::Array<API_Guid> CollectUpdatedGuids (
    const GS::ObjectState& mutationResult,
    const GS::Array<API_Guid>& requestedGuids);
GS::Array<API_Guid> CollectDeletedGuids (
    const GS::ObjectState& mutationResult,
    const GS::Array<API_Guid>& requestedGuids);
bool MutationResultIsComplete (
    const GS::UniString& operation,
    const GS::ObjectState& mutationResult,
    const GSSize requestedCount);

} // namespace ElementMutationInternal
