#pragma once

#include "CommandBase.hpp"

// Developer-only history snapshot probes (D-345): save exact elements into a module file and
// bring such a module back as ordinary elements.  Not exposed through Production MCP.

class SaveElementsAsModuleCommand : public CommandBase
{
public:
    SaveElementsAsModuleCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};

class PlaceModuleAsElementsCommand : public CommandBase
{
public:
    PlaceModuleAsElementsCommand ();
    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl& processControl) const override;
};
