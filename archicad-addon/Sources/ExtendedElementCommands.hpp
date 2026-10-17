#pragma once

#include "CommandBase.hpp"
#include "ElementCreationCommands.hpp"
#include "ObjectState.hpp"

GS::Optional<GS::UniString> BuildMeshPolyMemoFromGeometry (
    API_Element& elem,
    API_ElementMemo& memo,
    GS::Array<GS::ObjectState>& polygonCoordinates,
    const GS::Array<GS::ObjectState>& polygonArcs,
    const GS::Array<GS::ObjectState>& holes);

GSErrCode BuildMeshSublinesMemoFromGeometry (
    API_Element& elem,
    API_ElementMemo& memo,
    const GS::Array<GS::ObjectState>& sublines);

class CreateWallsCommand : public CreateElementsCommandBase
{
public:
    CreateWallsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateBeamsCommand : public CreateElementsCommandBase
{
public:
    CreateBeamsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateStairsCommand : public CreateElementsCommandBase
{
public:
    CreateStairsCommand ();
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::ObjectState> SetTypeSpecificParameters (API_Element& element, API_ElementMemo& memo, const Stories& stories, const GS::ObjectState& parameters) const override;
};

class CreateWindowsCommand : public CommandBase
{
public:
    CreateWindowsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateDoorsCommand : public CommandBase
{
public:
    CreateDoorsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateOpeningsCommand : public CommandBase
{
public:
    CreateOpeningsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateMorphsCommand : public CommandBase
{
public:
    CreateMorphsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateRoofsCommand : public CommandBase
{
public:
    CreateRoofsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateAssociativeDimensionsCommand : public CommandBase
{
public:
    CreateAssociativeDimensionsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateAssociativeDimensionsOnSectionCommand : public CommandBase
{
public:
    CreateAssociativeDimensionsOnSectionCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateWallThicknessDimensionsCommand : public CommandBase
{
public:
    CreateWallThicknessDimensionsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ModifyWallsCommand : public CommandBase
{
public:
    explicit ModifyWallsCommand (bool atomic = false);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

private:
    bool atomic;
};

class ModifyBeamsCommand : public CommandBase
{
public:
    explicit ModifyBeamsCommand (bool atomic = false);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

private:
    bool atomic;
};

class ModifySlabsCommand : public CommandBase
{
public:
    explicit ModifySlabsCommand (bool atomic = false);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

private:
    bool atomic;
};

class ModifyColumnsCommand : public CommandBase
{
public:
    explicit ModifyColumnsCommand (bool atomic = false);
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;

private:
    bool atomic;
};

class ModifyWindowsCommand : public CommandBase
{
public:
    ModifyWindowsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ModifyMorphsCommand : public CommandBase
{
public:
    ModifyMorphsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ModifyRoofsCommand : public CommandBase
{
public:
    ModifyRoofsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ModifyDoorsCommand : public CommandBase
{
public:
    ModifyDoorsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class CreateSectionsCommand : public CommandBase
{
public:
    CreateSectionsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class GetDimensionDataCommand : public CommandBase
{
public:
    GetDimensionDataCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Complete, exact-GUID, read-only dimension-family snapshot. Kept additive to
// the historical GetDimensionData response used by older Tapir clients.
class GetNativeDimensionDetailsCommand : public CommandBase
{
public:
    GetNativeDimensionDetailsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Pure fixed-input UTF-8 construction proof shared with the internal fixture
// creator; accepts no request values and performs no project/API mutation.
class GetNativeDimensionFixtureInputProofCommand : public CommandBase
{
public:
    GetNativeDimensionFixtureInputProofCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Private read-only context attestation for the disabled fixed Dimension
// candidate producer. It never activates a window, story, or database.
class GetNativeDimensionCreateCandidateContextCommand : public CommandBase
{
public:
	GetNativeDimensionCreateCandidateContextCommand ();
	virtual GS::String GetName () const override;
	virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
	virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
	virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal fixed-recipe branch called only by MutateNativeElements in an
// isolated native candidate profile. It is not registered as an MCP tool.
GS::ObjectState ExecuteNativeDimensionCreateCandidate (const GS::ObjectState& parameters, GS::ProcessControl& processControl);

// Internal, exact-scope read-only probe of radial dimension SDK defaults.
// The result is explicitly tool defaults, not an existing element snapshot.
class GetNativeDimensionRadialDefaultsCommand : public CommandBase
{
public:
    GetNativeDimensionRadialDefaultsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal, exact-scope READ-only probe of hotspots returned for one exact Arc.
// It reports SDK neigs and conversions without admitting a dimension anchor.
class GetNativeDimensionArcHotspotsCommand : public CommandBase
{
public:
    GetNativeDimensionArcHotspotsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal, exact-scope read-only observations of dimension-parent drawing
// text primitives and version-gated autotext candidate metadata.
class GetNativeDimensionParentDrawingRunsCommand : public CommandBase
{
public:
    GetNativeDimensionParentDrawingRunsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Private bounded selected-rectangle inventory and full-parent dimension
// audit capture. This is not the public complete-audit capability.
class GetNativeDimensionAuditCaptureCommand : public CommandBase
{
public:
	GetNativeDimensionAuditCaptureCommand ();
	virtual GS::String GetName () const override;
	virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
	virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
	virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal, process-local staged layer visibility/lock inventory control for
// disposable dimension fixture qualification. Not a public mutation surface.
class NativeDimensionLayerControlCommand : public CommandBase
{
public:
	NativeDimensionLayerControlCommand ();
	virtual GS::String GetName () const override;
	virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
	virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
	virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

// Internal, fixed-preset fixture preparation for disposable dimension reads.
// Deliberately not wired into any public CRUD or MCP capability surface.
class CreateNativeDimensionReadFixturesCommand : public CommandBase
{
public:
    CreateNativeDimensionReadFixturesCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class ModifyMeshesCommand : public CommandBase
{
public:
    ModifyMeshesCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
