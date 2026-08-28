#include "ElementGDLParameterCommands.hpp"
#include "MigrationHelper.hpp"

#include <cstddef>
#include <limits>

constexpr const char* ParameterValueFieldName = "value";
constexpr GSSize MaxGdlArrayItems = 100'000;
constexpr GSSize MaxGdlParameterCount = 10'000;
constexpr GSSize MaxGdlPossibleValueCount = 100'000;
constexpr GSSize MaxGdlPackedStringBytes = 16 * 1024 * 1024;

static bool GetArrayItemCount (const API_AddParType& parameter, GSSize& itemCount)
{
    if (parameter.typeMod == API_ParSimple) {
        itemCount = 1;
        return true;
    }
    if (parameter.typeMod != API_ParArray || parameter.dim1 <= 0 || parameter.dim2 <= 0)
        return false;

    const GSSize dim1 = static_cast<GSSize> (parameter.dim1);
    const GSSize dim2 = static_cast<GSSize> (parameter.dim2);
    if (dim1 > std::numeric_limits<GSSize>::max () / dim2)
        return false;
    itemCount = dim1 * dim2;
    return itemCount <= MaxGdlArrayItems;
}

static bool HasNumericArrayStorage (const API_AddParType& parameter, const GSSize itemCount)
{
    if (parameter.value.array == nullptr || *parameter.value.array == nullptr)
        return false;
    if (itemCount > std::numeric_limits<GSSize>::max () / static_cast<GSSize> (sizeof (double)))
        return false;
    const GSSize bytes = BMhGetSize (parameter.value.array);
    const GSSize requiredBytes = itemCount * static_cast<GSSize> (sizeof (double));
    return bytes >= requiredBytes && bytes <= MaxGdlArrayItems * static_cast<GSSize> (sizeof (double));
}

static bool ReadBoundedUString (const GS::uchar_t* value, const GSSize availableUnits, GSSize& length)
{
    if (value == nullptr)
        return false;
    for (GSSize index = 0; index < availableUnits; ++index) {
        if (value[index] == 0) {
            length = index;
            return true;
        }
    }
    return false;
}

template <std::size_t Capacity>
static bool ReadFixedUString (const GS::uchar_t (&value)[Capacity], GS::UniString& result)
{
    GSSize length = 0;
    if (!ReadBoundedUString (value, static_cast<GSSize> (Capacity), length))
        return false;
    result = GS::UniString (value, static_cast<USize> (length));
    return true;
}

static bool GetPackedStringUnits (const API_AddParType& parameter, const GSSize itemCount, GSSize& units)
{
    if (parameter.value.array == nullptr || *parameter.value.array == nullptr)
        return false;
    const GSSize bytes = BMhGetSize (parameter.value.array);
    if (bytes <= 0 || bytes % static_cast<GSSize> (sizeof (GS::uchar_t)) != 0 || bytes > MaxGdlPackedStringBytes)
        return false;
    units = bytes / static_cast<GSSize> (sizeof (GS::uchar_t));
    const auto* values = reinterpret_cast<const GS::uchar_t*> (*parameter.value.array);
    GSSize offset = 0;
    for (GSSize index = 0; index < itemCount; ++index) {
        if (offset >= units)
            return false;
        GSSize length = 0;
        if (!ReadBoundedUString (values + offset, units - offset, length))
            return false;
        if (length == std::numeric_limits<GSSize>::max () - offset)
            return false;
        offset += length + 1;
    }
    return true;
}

static bool GetPossiblePackedStringUnits (const API_GetParamValuesType& values, GSSize& units)
{
    if (values.uStrValues == nullptr || *values.uStrValues == nullptr || values.nVals <= 0 ||
        static_cast<GSSize> (values.nVals) > MaxGdlPossibleValueCount)
        return false;
    const GSSize bytes = BMhGetSize (reinterpret_cast<GSHandle> (values.uStrValues));
    if (bytes <= 0 || bytes % static_cast<GSSize> (sizeof (GS::uchar_t)) != 0 || bytes > MaxGdlPackedStringBytes)
        return false;
    units = bytes / static_cast<GSSize> (sizeof (GS::uchar_t));
    const auto* data = *values.uStrValues;
    GSSize offset = 0;
    for (GSSize index = 0; index < static_cast<GSSize> (values.nVals); ++index) {
        if (offset >= units)
            return false;
        GSSize length = 0;
        if (!ReadBoundedUString (data + offset, units - offset, length))
            return false;
        if (length == std::numeric_limits<GSSize>::max () - offset)
            return false;
        offset += length + 1;
    }
    return true;
}

static GS::UniString ConvertAddParIDToString (API_AddParID addParID)
{
    switch (addParID) {
        case APIParT_Integer:			return "Integer";
        case APIParT_Length:			return "Length";
        case APIParT_Angle:				return "Angle";
        case APIParT_RealNum:			return "RealNumber";
        case APIParT_LightSw:			return "LightSwitch";
        case APIParT_ColRGB:			return "RGBColor";
        case APIParT_Intens:			return "Intensity";
        case APIParT_LineTyp:			return "LineType";
        case APIParT_Mater:				return "Material";
        case APIParT_FillPat:			return "FillPattern";
        case APIParT_PenCol:			return "PenColor";
        case APIParT_CString:			return "String";
        case APIParT_Boolean:			return "Boolean";
        case APIParT_Separator:			return "Separator";
        case APIParT_Title:				return "Title";
        case APIParT_BuildingMaterial:	return "BuildingMaterial";
        case APIParT_Profile:			return "Profile";
        case APIParT_Dictionary:		return "Dictionary";
        default:						return "UNKNOWN";
    }
}

static void AddValueInteger (GS::ObjectState& gdlParameterDetails,
                             const API_AddParType& actParam,
                             const GSSize itemCount)
{
    if (actParam.typeMod == API_ParSimple) {
        gdlParameterDetails.Add (ParameterValueFieldName, static_cast<Int32> (actParam.value.real));
    } else {
        const auto& arrayValueItemAdder = gdlParameterDetails.AddList<Int32> (ParameterValueFieldName);
        const auto* values = reinterpret_cast<const double*> (*actParam.value.array);
        for (GSSize index = 0; index < itemCount; ++index)
            arrayValueItemAdder (static_cast<Int32> (values[index]));
    }
}

static void AddValueDouble (GS::ObjectState& gdlParameterDetails,
                            const API_AddParType& actParam,
                            const GSSize itemCount)
{
    if (actParam.typeMod == API_ParSimple) {
        gdlParameterDetails.Add (ParameterValueFieldName, actParam.value.real);
    } else {
        const auto& arrayValueItemAdder = gdlParameterDetails.AddList<double> (ParameterValueFieldName);
        const auto* values = reinterpret_cast<const double*> (*actParam.value.array);
        for (GSSize index = 0; index < itemCount; ++index)
            arrayValueItemAdder (values[index]);
    }
}

template<typename T>
static void AddValueTrueFalseOptions (GS::ObjectState& gdlParameterDetails,
                                      const API_AddParType& actParam,
                                      const GSSize itemCount,
                                      T optionTrue,
                                      T optionFalse)
{
    if (actParam.typeMod == API_ParSimple) {
        gdlParameterDetails.Add (ParameterValueFieldName, static_cast<Int32> (actParam.value.real) == 0 ? optionFalse : optionTrue);
    } else {
        const auto& arrayValueItemAdder = gdlParameterDetails.AddList<T> (ParameterValueFieldName);
        const auto* values = reinterpret_cast<const double*> (*actParam.value.array);
        for (GSSize index = 0; index < itemCount; ++index)
            arrayValueItemAdder (static_cast<Int32> (values[index]) == 0 ? optionFalse : optionTrue);
    }
}

static void AddValueOnOff (GS::ObjectState& gdlParameterDetails,
                           const API_AddParType& actParam,
                           const GSSize itemCount)
{
    AddValueTrueFalseOptions (gdlParameterDetails, actParam, itemCount, GS::String ("On"), GS::String ("Off"));
}

static void AddValueBool (GS::ObjectState& gdlParameterDetails,
                          const API_AddParType& actParam,
                          const GSSize itemCount)
{
    AddValueTrueFalseOptions (gdlParameterDetails, actParam, itemCount, true, false);
}

static void AddValueString (GS::ObjectState& gdlParameterDetails,
                            const API_AddParType& actParam,
                            const GSSize itemCount,
                            const GSSize packedStringUnits)
{
    if (actParam.typeMod == API_ParSimple) {
        GS::UniString value;
        if (ReadFixedUString (actParam.value.uStr, value))
            gdlParameterDetails.Add (ParameterValueFieldName, value);
    } else {
        const auto& arrayValueItemAdder = gdlParameterDetails.AddList<GS::UniString> (ParameterValueFieldName);
        const GSSize units = packedStringUnits;
        const auto* values = reinterpret_cast<const GS::uchar_t*> (*actParam.value.array);
        GSSize offset = 0;
        for (GSSize index = 0; index < itemCount; ++index) {
            if (offset >= units)
                return;
            GSSize length = 0;
            if (!ReadBoundedUString (values + offset, units - offset, length))
                return;
            arrayValueItemAdder (GS::UniString (values + offset, static_cast<USize> (length)));
            if (length >= units - offset)
                return;
            offset += length + 1;
        }
    }
}

static bool SetParamValueInteger (API_ChangeParamType& changeParam,
                                  const GS::ObjectState& parameterDetails)
{
    Int32 value;
    if (parameterDetails.Get (ParameterValueFieldName, value)) {
        changeParam.realValue = value;
        return true;
    }
    return false;
}

static bool SetParamValueDouble (API_ChangeParamType& changeParam,
                                 const GS::ObjectState& parameterDetails)
{
    double value;
    if (parameterDetails.Get (ParameterValueFieldName, value)) {
        changeParam.realValue = value;
        return true;
    }
    return false;
}

static bool SetParamValueOnOff (API_ChangeParamType& changeParam,
                                const GS::ObjectState& parameterDetails)
{
    GS::String value;
    if (parameterDetails.Get (ParameterValueFieldName, value)) {
        changeParam.realValue = (value == "Off" ? 0 : 1);
        return true;
    }
    return false;
}

static bool SetParamValueBool (API_ChangeParamType& changeParam,
                               const GS::ObjectState& parameterDetails)
{
    bool value;
    if (parameterDetails.Get (ParameterValueFieldName, value)) {
        changeParam.realValue = (value ? 1 : 0);
        return true;
    }
    return false;
}

constexpr USize MaxStrValueLength = 512;

static bool SetParamValueString (API_ChangeParamType& changeParam,
                                 const GS::ObjectState& parameterDetails,
                                 GS::uchar_t (&strValueStorage)[MaxStrValueLength])
{
    GS::UniString value;
    if (parameterDetails.Get (ParameterValueFieldName, value)) {
        const auto ustrObject = value.ToUStr ();
        GS::ucsncpy (strValueStorage, ustrObject, MaxStrValueLength);
        strValueStorage[MaxStrValueLength - 1] = 0;

        changeParam.uStrValue = strValueStorage;
        return true;
    }
    return false;
}

GetGDLParametersOfElementsCommand::GetGDLParametersOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetGDLParametersOfElementsCommand::GetName () const
{
    return "GetGDLParametersOfElements";
}

GS::Optional<GS::UniString> GetGDLParametersOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "elements": {
            "$ref": "#/Elements"
        }
    },
    "additionalProperties": false,
    "required": [
        "elements"
    ]
})";
}

GS::Optional<GS::UniString> GetGDLParametersOfElementsCommand::GetResponseSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "gdlParametersOfElements": {
            "type": "array",
            "description": "The GDL parameters of elements.",
            "items": {
                "$ref": "#/GDLParameterList"
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "gdlParametersOfElements"
    ]
})";
}

GS::ObjectState	GetGDLParametersOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::ObjectState response;
    const auto& elemGdlParameterListAdder = response.AddList<GS::ObjectState> ("gdlParametersOfElements");

    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            elemGdlParameterListAdder (CreateErrorResponse (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        API_ParamOwnerType paramOwner = {};
        paramOwner.libInd = -1;
        paramOwner.guid = GetGuidFromObjectState (*elementId);

        API_Element apiElement = {};
        apiElement.header.guid = paramOwner.guid;
        GSErrCode err = ACAPI_Element_Get (&apiElement);
        if (err != NoError) {
            const GS::UniString errorMsg = GS::UniString::Printf ("Not found element with guid %T!", APIGuidToString (paramOwner.guid).ToPrintf ());
            elemGdlParameterListAdder (CreateErrorResponse (err, errorMsg));
            continue;
        }

#ifdef ServerMainVers_2600
        paramOwner.type = apiElement.header.type;
#else
        paramOwner.typeID = apiElement.header.typeID;
#endif

        API_GetParamsType getParams = {};
        API_AddParType** addPars = nullptr;
        bool parametersOpened = false;
        const GS::OnExit parameterResourcesGuard ([&] () {
            if (parametersOpened) {
                ACAPI_LibraryPart_CloseParameters ();
            }
            if (getParams.params != nullptr) {
                ACAPI_DisposeAddParHdl (&getParams.params);
            }
            if (addPars != nullptr) {
                ACAPI_DisposeAddParHdl (&addPars);
            }
        });

        err = ACAPI_LibraryPart_OpenParameters (&paramOwner);
        if (err == NoError) {
            parametersOpened = true;
            err = ACAPI_LibraryPart_GetActParameters (&getParams);
        }

        if (err != NoError) {
            const GS::UniString errorMsg = GS::UniString::Printf ("Failed to get parameters of element with guid %T!", APIGuidToString (paramOwner.guid).ToPrintf ());
            elemGdlParameterListAdder (CreateErrorResponse (err, errorMsg));
            continue;
        }

        Int32 libInd = -1;
        if (GetElemTypeId (apiElement.header) == API_ObjectID) {
            libInd = apiElement.object.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_LampID) {
            libInd = apiElement.lamp.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_WindowID) {
            libInd = apiElement.window.openingBase.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_DoorID) {
            libInd = apiElement.door.openingBase.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_SkylightID) {
            libInd = apiElement.skylight.openingBase.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_ZoneID) {
            libInd = apiElement.zone.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_LabelID) {
            libInd = apiElement.label.u.symbol.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_DrawingID) {
            libInd = apiElement.drawing.title.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_CurtainWallFrameID) {
            libInd = apiElement.cwFrame.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_CurtainWallPanelID) {
            libInd = apiElement.cwPanel.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_CurtainWallJunctionID) {
            libInd = apiElement.cwJunction.libInd;
        } else if (GetElemTypeId (apiElement.header) == API_CurtainWallAccessoryID) {
            libInd = apiElement.cwAccessory.libInd;
        }

        double a;
        double b;
        Int32 addParNum;
        const GSErrCode defaultParamsErr = ACAPI_LibraryPart_GetParams (libInd, &a, &b, &addParNum, &addPars);
        if (defaultParamsErr != NoError || addPars == nullptr || *addPars == nullptr) {
            elemGdlParameterListAdder (CreateErrorResponse (
                defaultParamsErr != NoError ? defaultParamsErr : APIERR_BADPARS,
                "Failed to get the default Library Part parameters."));
            continue;
        }
        if (getParams.params == nullptr || *getParams.params == nullptr) {
            elemGdlParameterListAdder (CreateErrorResponse (APIERR_BADPARS, "Archicad returned no active Library Part parameters."));
            continue;
        }

        const GSSize paramsBytes = BMhGetSize (reinterpret_cast<GSHandle> (getParams.params));
        const GSSize libraryParamsBytes = BMhGetSize (reinterpret_cast<GSHandle> (addPars));
        if (paramsBytes == 0 || paramsBytes % sizeof (API_AddParType) != 0 ||
            libraryParamsBytes == 0 || libraryParamsBytes % sizeof (API_AddParType) != 0 ||
            paramsBytes > MaxGdlParameterCount * static_cast<GSSize> (sizeof (API_AddParType)) ||
            libraryParamsBytes > MaxGdlParameterCount * static_cast<GSSize> (sizeof (API_AddParType))) {
            elemGdlParameterListAdder (CreateErrorResponse (APIERR_BADPARS, "Library Part parameter memo sizes are invalid."));
            continue;
        }
        const GSSize nParams = paramsBytes / sizeof (API_AddParType);
        const GSSize libraryParamCapacity = libraryParamsBytes / sizeof (API_AddParType);
        if (addParNum < 0 || static_cast<GSSize> (addParNum) < nParams || libraryParamCapacity < nParams) {
            elemGdlParameterListAdder (CreateErrorResponse (APIERR_BADPARS, "Library Part parameter memo sizes are inconsistent."));
            continue;
        }
        GS::ObjectState gdlParameters;
        const auto& parameterListAdder = gdlParameters.AddList<GS::ObjectState> ("parameters");
        for (GSIndex ii = 0; ii < nParams; ++ii) {
            const API_AddParType& actParam = (*getParams.params)[ii];
            const API_AddParType& actLibPartParam = (*addPars)[ii];

            if (actParam.typeID == APIParT_Separator) {
                continue;
            }

            API_GetParamValuesType getValues = {};
            const GS::OnExit getValuesGuard ([&] () {
                if (getValues.uStrValues != nullptr) {
                    BMhFree ((GSHandle) getValues.uStrValues);
                }
                if (getValues.realValues != nullptr) {
                    BMhFree ((GSHandle) getValues.realValues);
                }
            });
            getValues.index = actParam.index;
            const GSErrCode valuesErr = ACAPI_LibraryPart_GetParamValues (&getValues);

            GS::ObjectState gdlParameterDetails;
            gdlParameterDetails.Add ("name", actParam.name);
            GS::UniString displayName;
            if (!ReadFixedUString (actLibPartParam.uDescname, displayName))
                displayName = GS::EmptyUniString;
            gdlParameterDetails.Add ("displayName", displayName);
            gdlParameterDetails.Add ("index", actParam.index);
            gdlParameterDetails.Add ("type", ConvertAddParIDToString (actParam.typeID));
            if (actParam.typeMod == API_ParArray) {
                gdlParameterDetails.Add ("dimension1", actParam.dim1);
                gdlParameterDetails.Add ("dimension2", actParam.dim2);
            }

            GSSize arrayItemCount = 1;
            bool valueAvailable = GetArrayItemCount (actParam, arrayItemCount);
            if (valueAvailable && actParam.typeMod == API_ParArray) {
                if (actParam.typeID == APIParT_CString || actParam.typeID == APIParT_Title) {
                    GSSize packedStringUnits = 0;
                    valueAvailable = GetPackedStringUnits (actParam, arrayItemCount, packedStringUnits);
                } else {
                    valueAvailable = HasNumericArrayStorage (actParam, arrayItemCount);
                }
            }
            if (!valueAvailable) {
                gdlParameterDetails.Add ("valueStatus", "unavailable");
            } else {
                switch (actParam.typeID) {
                    case APIParT_Integer:
                    case APIParT_PenCol:
                    case APIParT_LineTyp:
                    case APIParT_Mater:
                    case APIParT_FillPat:
                    case APIParT_BuildingMaterial:
                    case APIParT_Profile:
                        AddValueInteger (gdlParameterDetails, actParam, arrayItemCount);
                        break;
                    case APIParT_ColRGB:
                    case APIParT_Intens:
                    case APIParT_Length:
                    case APIParT_RealNum:
                    case APIParT_Angle:
                        AddValueDouble (gdlParameterDetails, actParam, arrayItemCount);
                        break;
                    case APIParT_LightSw:
                        AddValueOnOff (gdlParameterDetails, actParam, arrayItemCount);
                        break;
                    case APIParT_Boolean:
                        AddValueBool (gdlParameterDetails, actParam, arrayItemCount);
                        break;
                    case APIParT_CString:
                    case APIParT_Title:
                        if (actParam.typeMod == API_ParSimple) {
                            GS::UniString stringValue;
                            if (!ReadFixedUString (actParam.value.uStr, stringValue))
                                gdlParameterDetails.Add ("valueStatus", "unavailable");
                            else
                                AddValueString (gdlParameterDetails, actParam, arrayItemCount, 0);
                        } else {
                            GSSize packedStringUnits = 0;
                            if (!GetPackedStringUnits (actParam, arrayItemCount, packedStringUnits)) {
                                gdlParameterDetails.Add ("valueStatus", "unavailable");
                            } else {
                                AddValueString (gdlParameterDetails, actParam, arrayItemCount, packedStringUnits);
                            }
                        }
                        break;
                    default:
                    case APIParT_Dictionary:
                        // Not supported by the Archicad API yet
                        break;
                }
            }

            GS::UniString valueDescription;
            if (!ReadFixedUString (actParam.valueDescription, valueDescription))
                valueDescription = GS::EmptyUniString;
            if (!valueDescription.IsEmpty ()) {
                gdlParameterDetails.Add ("valueDescription", valueDescription);
            }

            gdlParameterDetails.Add ("isLocked", getValues.locked);
            if (valuesErr != NoError) {
                gdlParameterDetails.Add ("possibleValuesStatus", "unavailable");
                getValues.nVals = 0;
            } else if (getValues.nVals > 0) {
                const bool countInvalid = getValues.nVals < 0 ||
                    static_cast<GSSize> (getValues.nVals) > MaxGdlPossibleValueCount;
                const bool stringValuesPresent = getValues.uStrValues != nullptr;
                const bool realValuesPresent = getValues.realValues != nullptr;
                GSSize packedStringUnits = 0;
                const bool stringValuesValid = !stringValuesPresent ||
                    GetPossiblePackedStringUnits (getValues, packedStringUnits);
                const GSSize realValueBytes = realValuesPresent
                    ? BMhGetSize (reinterpret_cast<GSHandle> (getValues.realValues))
                    : 0;
                const bool realValuesValid = !realValuesPresent ||
                    (*getValues.realValues != nullptr && realValueBytes > 0 &&
                     realValueBytes % static_cast<GSSize> (sizeof (API_VLNumType)) == 0 &&
                     realValueBytes / static_cast<GSSize> (sizeof (API_VLNumType)) >= static_cast<GSSize> (getValues.nVals) &&
                     realValueBytes <= MaxGdlPossibleValueCount * static_cast<GSSize> (sizeof (API_VLNumType)));
                if (countInvalid || (!stringValuesPresent && !realValuesPresent) || !stringValuesValid || !realValuesValid) {
                    gdlParameterDetails.Add ("possibleValuesStatus", "invalid");
                    getValues.nVals = 0;
                }
            }

            const auto& flags = gdlParameterDetails.AddList<GS::UniString> ("flags");
            if (actParam.flags & API_ParFlg_Hidden) {
                flags ("Hidden");
            }
            if (actParam.flags & API_ParFlg_SHidden) {
                flags ("HiddenFromScript");
            }
            if (actParam.flags & API_ParFlg_Disabled) {
                flags ("Disabled");
            }
            if (actParam.flags & API_ParFlg_Child) {
                flags ("Child");
            }
            if (actParam.flags & API_ParFlg_Unique) {
                flags ("Unique");
            }
            if (actParam.flags & API_ParFlg_Fixed) {
                flags ("Fixed");
            }

            if (getValues.nVals > 0) {
                if (actParam.typeID == APIParT_PenCol && getValues.nVals == 255) {
                    // The guard releases the values handle after this parameter.
                } else {
                    if (getValues.uStrValues != nullptr && *getValues.uStrValues != nullptr) {
                        const auto& possibleValues = gdlParameterDetails.AddList<GS::UniString> ("possibleValues");
                        GS::uchar_t *strPos = *getValues.uStrValues;
                        GSSize stringUnits = BMhGetSize (reinterpret_cast<GSHandle> (getValues.uStrValues)) /
                            static_cast<GSSize> (sizeof (GS::uchar_t));
                        GSSize stringOffset = 0;
                        for (GSIndex valIdx = 0; valIdx < getValues.nVals; ++valIdx) {
                            GSSize stringLength = 0;
                            if (stringOffset >= stringUnits ||
                                !ReadBoundedUString (strPos + stringOffset, stringUnits - stringOffset, stringLength))
                                break;
                            possibleValues (GS::UniString (strPos + stringOffset, static_cast<USize> (stringLength)));
                            stringOffset += stringLength + 1;
                        }
                    } else if (getValues.realValues != nullptr && *getValues.realValues != nullptr) {
                        const auto& possibleValues = gdlParameterDetails.AddList<GS::ObjectState> ("possibleValues");
                        for (GSIndex valIdx = 0; valIdx < getValues.nVals; ++valIdx) {
                            const auto& possibleValue = (*getValues.realValues)[valIdx];
                             GS::ObjectState os;
                            if (possibleValue.flags) {
                                if (possibleValue.flags == APIVLVal_LowerLimit) {
                                    os = GS::ObjectState ("value", possibleValue.lowerLimit, "flag", "GreaterThan");
                                } else if (possibleValue.flags & APIVLVal_LowerEqual) {
                                    os = GS::ObjectState ("value", possibleValue.lowerLimit, "flag", "GreaterThanOrEqual");
                                } else if (possibleValue.flags == APIVLVal_UpperLimit) {
                                    os = GS::ObjectState ("value", possibleValue.upperLimit, "flag", "LessThan");
                                } else if (possibleValue.flags & APIVLVal_UpperEqual) {
                                    os = GS::ObjectState ("value", possibleValue.upperLimit, "flag", "LessThanOrEqual");
                                } else if (possibleValue.flags == APIVLVal_Step) {
                                    os = GS::ObjectState ("value", possibleValue.stepBeg, "flag", "StepBegin");
                                    os = GS::ObjectState ("value", possibleValue.stepVal, "flag", "StepValue");
                                }
                            } else {
                                os = GS::ObjectState ("value", possibleValue.value);
                            }
                            GS::UniString valueDescription;
                            if (!ReadFixedUString (possibleValue.valueDescription, valueDescription))
                                valueDescription = GS::EmptyUniString;
                            if (!valueDescription.IsEmpty ()) {
                                os.Add ("description", valueDescription);
                            }
                            possibleValues (os);
                        }
                    }
                    gdlParameterDetails.Add ("canHaveCustomValue", getValues.custom);
                }
            }

            parameterListAdder (gdlParameterDetails);
        }
        elemGdlParameterListAdder (gdlParameters);
    }

    return response;
}

SetGDLParametersOfElementsCommand::SetGDLParametersOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SetGDLParametersOfElementsCommand::GetName () const
{
    return "SetGDLParametersOfElements";
}

GS::Optional<GS::UniString> SetGDLParametersOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
    "type": "object",
    "properties": {
        "elementsWithGDLParameters": {
            "type": "array",
            "description": "The elements with GDL parameters dictionary pairs.",
            "items": {
                "type": "object",
                "properties": {
                    "elementId": {
                        "$ref": "#/ElementId"
                    },
                    "gdlParameters": {
                        "$ref": "#/SetGDLParameterArray"
                    }
                },
                "additionalProperties": false,
                "required": [
                    "elementId",
                    "gdlParameters"
                ]
            }
        }
    },
    "additionalProperties": false,
    "required": [
        "elementsWithGDLParameters"
    ]
})";
}

GS::Optional<GS::UniString> SetGDLParametersOfElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": [
            "executionResults"
        ]
    })";
}

GS::ObjectState	SetGDLParametersOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsWithGDLParameters;
    parameters.Get ("elementsWithGDLParameters", elementsWithGDLParameters);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Set GDL Parameters of Elements", [&]() -> GSErrCode {
        for (const GS::ObjectState& elementWithGDLParameters : elementsWithGDLParameters) {
            GSErrCode err = NoError;
            GS::UniString errMessage;
            const GS::ObjectState* elementId = elementWithGDLParameters.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            const API_Guid elemGuid = GetGuidFromObjectState (*elementId);
            API_ParamOwnerType paramOwner = {};
            paramOwner.libInd = 0;
#ifdef ServerMainVers_2600
            paramOwner.type = API_ObjectID;
#else
            paramOwner.typeID = API_ObjectID;
#endif
            paramOwner.guid = elemGuid;

            GS::Array<GS::ObjectState> elemGdlParameters;
            elementWithGDLParameters.Get ("gdlParameters", elemGdlParameters);

            API_GetParamsType getParams = {};
            bool parametersOpened = false;
            const GS::OnExit parametersGuard ([&] () {
                if (parametersOpened)
                    ACAPI_LibraryPart_CloseParameters ();
                if (getParams.params != nullptr)
                    ACAPI_DisposeAddParHdl (&getParams.params);
            });

            err = ACAPI_LibraryPart_OpenParameters (&paramOwner);
            if (err == NoError) {
                parametersOpened = true;
                err = ACAPI_LibraryPart_GetActParameters (&getParams);
                if (err == NoError && (getParams.params == nullptr || *getParams.params == nullptr)) {
                    err = APIERR_BADPARS;
                    errMessage = "Archicad returned no active Library Part parameters.";
                }
                if (err == NoError) {
                    const GSSize paramsBytes = BMhGetSize (reinterpret_cast<GSHandle> (getParams.params));
                    if (paramsBytes <= 0 || paramsBytes % sizeof (API_AddParType) != 0 ||
                        paramsBytes > MaxGdlParameterCount * static_cast<GSSize> (sizeof (API_AddParType))) {
                        err = APIERR_BADPARS;
                        errMessage = "Archicad returned an invalid Library Part parameter memo.";
                    }
                    const GSSize nParams = paramsBytes / sizeof (API_AddParType);
                    GS::HashTable<GS::String, API_AddParID> gdlParametersTypeDictionary;
                    GS::HashTable<short, GS::String> gdlParametersIndexNameDictionary;
                    for (GSIndex ii = 0; err == NoError && ii < nParams; ++ii) {
                        const API_AddParType& actParam = (*getParams.params)[ii];
                        if (actParam.typeID != APIParT_Separator) {
                            auto name = GS::String (actParam.name);
                            gdlParametersIndexNameDictionary.Add (actParam.index, name);
                            gdlParametersTypeDictionary.Add (name, actParam.typeID);
                        }
                    }

                    const auto refreshParameters = [&]() -> GSErrCode {
                        ACAPI_DisposeAddParHdl (&getParams.params);
                        const GSErrCode refreshErr = ACAPI_LibraryPart_GetActParameters (&getParams);
                        if (refreshErr != NoError || getParams.params == nullptr || *getParams.params == nullptr)
                            return refreshErr != NoError ? refreshErr : APIERR_BADPARS;
                        const GSSize refreshedBytes = BMhGetSize (reinterpret_cast<GSHandle> (getParams.params));
                        return refreshedBytes <= 0 || refreshedBytes % sizeof (API_AddParType) != 0 ||
                            refreshedBytes > MaxGdlParameterCount * static_cast<GSSize> (sizeof (API_AddParType))
                            ? APIERR_BADPARS
                            : NoError;
                    };

                    for (const GS::ObjectState& elemGdlParametersItem : elemGdlParameters) {
                        if (err != NoError)
                            break;
                        API_ChangeParamType changeParam = {};
                        GS::Array<GS::ObjectState> parameters;
                        if (elemGdlParametersItem.Get ("parameters", parameters)) {
                            // Legacy mode: old schema had nested list for parameters
                            for (const GS::ObjectState& parameter : parameters) {
                                err = SetOneGDLParameter (parameter, elemGuid, changeParam, gdlParametersTypeDictionary, gdlParametersIndexNameDictionary, errMessage);
                                if (err != NoError) {
                                    break;
                                }

                                err = refreshParameters ();
                            }
                        } else {
                            err = SetOneGDLParameter (elemGdlParametersItem, elemGuid, changeParam, gdlParametersTypeDictionary, gdlParametersIndexNameDictionary, errMessage);
                            if (err != NoError) {
                                break;
                            }

                            err = refreshParameters ();
                        }
                    }

                    if (err == NoError) {
                        API_Element	element = {};
                        element.header.guid = elemGuid;

                        err = ACAPI_Element_Get (&element);
                        if (err == NoError) {
                            API_Element 	mask = {};
                            API_ElementMemo memo = {};

                            ACAPI_ELEMENT_MASK_CLEAR (mask);
                            switch (GetElemTypeId (element.header)) {
                                case API_ObjectID:
                                    element.object.xRatio = getParams.a;
                                    element.object.yRatio = getParams.b;
                                    ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, xRatio);
                                    ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, yRatio);
                                    break;
                                case API_WindowID:
                                case API_DoorID:
                                    element.window.openingBase.width = getParams.a;
                                    element.window.openingBase.height = getParams.b;
                                    ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width);
                                    ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height);
                                    break;
                                case API_SkylightID:
                                    element.skylight.openingBase.width = getParams.a;
                                    element.skylight.openingBase.height = getParams.b;
                                    ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.width);
                                    ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.height);
                                    break;
                                default:
                                    // Not supported yet
                                    break;
                            }

                            if (getParams.params == nullptr || *getParams.params == nullptr) {
                                err = APIERR_BADPARS;
                                errMessage = "Archicad returned no refreshed Library Part parameters.";
                            } else {
                                memo.params = getParams.params;
                                err = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_AddPars, true);
                            }
                        }
                    }
                }
            }

            if (err != NoError) {
                if (errMessage.IsEmpty ()) {
                    executionResults (CreateFailedExecutionResult (err, GS::UniString::Printf ("Failed to change parameters of element with guid %T", APIGuidToString (elemGuid).ToPrintf ())));
                } else {
                    executionResults (CreateFailedExecutionResult (err, errMessage));
                }
            } else {
                executionResults (CreateSuccessfulExecutionResult ());
            }
        }

        return NoError;
    });

    return response;
}

GSErrCode
SetGDLParametersOfElementsCommand::SetOneGDLParameter (
    const GS::ObjectState& parameter,
    const API_Guid& elemGuid,
    API_ChangeParamType& changeParam,
    const GS::HashTable<GS::String, API_AddParID>& gdlParametersTypeDictionary,
    const GS::HashTable<short, GS::String>& gdlParametersIndexNameDictionary,
    GS::UniString& errMessage)
{
    GS::String name;
    if (parameter.Get ("name", name)) {
        CHTruncate (name.ToCStr (), changeParam.name, API_NameLen);
        if (!gdlParametersTypeDictionary.ContainsKey (changeParam.name)) {
            errMessage = GS::UniString::Printf ("Invalid input: %s is not a GDL parameter of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
            return APIERR_BADPARS;
        }
    } else {
        short index;
        if (parameter.Get ("index", index)) {
            if (gdlParametersIndexNameDictionary.ContainsKey (index)) {
                CHTruncate (gdlParametersIndexNameDictionary[index].ToCStr (), changeParam.name, API_NameLen);
            } else {
                errMessage = GS::UniString::Printf ("Invalid input: no GDL parameter with index %d for element %T", index, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
        } else {
            errMessage = "Invalid input: both name and index are missing, one of them must be set!";
            return APIERR_BADPARS;
        }
    }

    if (!parameter.Contains (ParameterValueFieldName)) {
        errMessage = GS::UniString::Printf ("Invalid input: value is missing for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
        return APIERR_BADPARS;
    }

    GS::uchar_t stringValueStorage[MaxStrValueLength] = {};
    switch (gdlParametersTypeDictionary[changeParam.name]) {
        case APIParT_Integer:
        case APIParT_PenCol:
        case APIParT_LineTyp:
        case APIParT_Mater:
        case APIParT_FillPat:
        case APIParT_BuildingMaterial:
        case APIParT_Profile:
            if (!SetParamValueInteger (changeParam, parameter)) {
                errMessage = GS::UniString::Printf ("Invalid input: the given value is not an integer for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
            break;
        case APIParT_ColRGB:
        case APIParT_Intens:
        case APIParT_Length:
        case APIParT_RealNum:
        case APIParT_Angle:
            if (!SetParamValueDouble (changeParam, parameter)) {
                errMessage = GS::UniString::Printf ("Invalid input: the given value is not a real number for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
            break;
        case APIParT_LightSw:
            if (!SetParamValueOnOff (changeParam, parameter)) {
                errMessage = GS::UniString::Printf ("Invalid input: the given value is not 'On' or 'Off' for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
            break;
        case APIParT_Boolean:
            if (!SetParamValueBool (changeParam, parameter)) {
                errMessage = GS::UniString::Printf ("Invalid input: the given value is not a boolean for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
            break;
        case APIParT_CString:
        case APIParT_Title:
            if (!SetParamValueString (changeParam, parameter, stringValueStorage)) {
                errMessage = GS::UniString::Printf ("Invalid input: the given value is not a string for parameter %s of element %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
                return APIERR_BADPARS;
            }
            break;
        default:
        case APIParT_Dictionary:
            // Not supported by the Archicad API yet
            break;
    }

    GSErrCode err = ACAPI_LibraryPart_ChangeAParameter (&changeParam);
    if (err != NoError) {
        errMessage = GS::UniString::Printf ("Failed to change parameter %s of element with guid %T", changeParam.name, APIGuidToString (elemGuid).ToPrintf ());
        return err;
    }

    return err;
}
