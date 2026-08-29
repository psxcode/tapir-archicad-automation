#pragma once

#include "CommandBase.hpp"

// Project-owned orchestration boundary for typed native CRUD.  The command
// deliberately delegates the type-specific work to the existing Tapir
// executors; it does not expose API_Element or reproduce Tapir's full JSON
// surface.
class MutateElementsCommand : public CommandBase
{
public:
    MutateElementsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::Optional<GS::UniString> GetRawResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
