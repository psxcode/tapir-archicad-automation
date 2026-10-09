#pragma once

#include "CommandBase.hpp"

// Versioned native CRUD boundary used by the TypeScript Native Layer.  These
// commands are intentionally separate from the internal MutateElements
// executor: that executor remains the type-specific implementation detail,
// while these commands own file-scope validation, canonical snapshots and
// factual terminal receipts.
class GetNativeCrudCapabilitiesCommand : public CommandBase
{
public:
    GetNativeCrudCapabilitiesCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class GetNativeElementSnapshotsCommand : public CommandBase
{
public:
    GetNativeElementSnapshotsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal exact ownership query used to close Wall mutation impact over
// hosted Doors and Windows. This is not registered as an agent-facing MCP
// tool; the typed TypeScript Native gateway consumes its bounded result.
class GetNativeHostedOpeningsForWallsCommand : public CommandBase
{
public:
    GetNativeHostedOpeningsForWallsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class DiscoverNativeElementsCommand : public CommandBase
{
public:
    DiscoverNativeElementsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class GetNativeMutationHistoryCommand : public CommandBase
{
public:
    GetNativeMutationHistoryCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class MutateNativeElementsCommand : public CommandBase
{
public:
    MutateNativeElementsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Specialized atomic Text-cluster transaction used by semantic table
// normalization.  This is intentionally separate from homogeneous CRUD:
// one survivor is updated and the remaining exact Text GUIDs are deleted in
// the same undoable Archicad command.
class NormalizeTextClusterCommand : public CommandBase
{
public:
    NormalizeTextClusterCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Developer-only automatic Zone refresh abstraction.  AC28's
// ZoneBoundaryQuery::Modifier::Update is diagnostic/calculated-boundary
// evidence, so the portable native implementation currently uses one
// undoable create-then-delete transaction.  The command owns the fallback so
// TypeScript never dispatches CreateZones and DeleteElements independently.
class UpdateAutomaticZoneGeometryCommand : public CommandBase
{
public:
    UpdateAutomaticZoneGeometryCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
