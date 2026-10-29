#pragma once

#if defined(TAPIR_HISTORY_SAVE_AS_PROBE)

#include "CommandBase.hpp"

// Developer-only Save As probe (D-351); compiled only into the isolated probe APX.
class ProbeSaveProjectAsCommand : public CommandBase
{
public:
    ProbeSaveProjectAsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

#endif
