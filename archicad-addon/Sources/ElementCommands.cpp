#include "ElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "PerfTrace.hpp"
#include "GSUnID.hpp"
#include "Plane.hpp"
#include "CoordTypedef.hpp"
#include "ModelEdge.hpp"
#include "ModelMeshBody.hpp"
#include "NativeImage.hpp"
#include "MemoryOChannel32.hpp"
#include "Base64Converter.hpp"
#include "NativeOwnership.hpp"
#ifdef ServerMainVers_2800
#include "ACAPI/ZoneBoundaryQuery.hpp"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr GSSize MaxNativeTextContentBytes = 8 * 1024 * 1024;

static bool IsFiniteBounds (const API_Box3D& bounds)
{
    return std::isfinite (bounds.xMin) && std::isfinite (bounds.yMin) &&
        std::isfinite (bounds.xMax) && std::isfinite (bounds.yMax);
}

static bool IsFiniteBounds (const API_Box& bounds)
{
    return std::isfinite (bounds.xMin) && std::isfinite (bounds.yMin) &&
        std::isfinite (bounds.xMax) && std::isfinite (bounds.yMax);
}

static bool AddBoundedTextContent (GS::ObjectState& target, const API_ElementMemo& memo, GSErrCode memoError)
{
#ifdef ServerMainVers_2800
    if (memoError == NoError && memo.textContent != nullptr &&
        memo.textContent->GetLength () <= static_cast<USize> (MaxNativeTextContentBytes / sizeof (GS::uchar_t))) {
        target.Add ("content", *memo.textContent);
        return true;
    }
#else
    if (memoError == NoError && memo.textContent != nullptr && *memo.textContent != nullptr) {
        const GSSize textBytes = BMhGetSize (reinterpret_cast<GSHandle> (memo.textContent));
        if (textBytes > 0 && textBytes <= MaxNativeTextContentBytes) {
            const char* text = *memo.textContent;
            GSSize textLength = 0;
            while (textLength < textBytes && text[textLength] != '\0')
                ++textLength;
            if (textLength < textBytes) {
                target.Add ("content", GS::UniString (reinterpret_cast<const GS::uchar_t*> (text)));
                return true;
            }
        }
    }
#endif
    return false;
}

}

static API_ElemFilterFlags ConvertFilterStringToFlag (const GS::UniString& filter)
{
    if (filter == "IsEditable")
        return APIFilt_IsEditable;
    if (filter == "IsVisibleByLayer")
        return APIFilt_OnVisLayer;
    if (filter == "IsVisibleByRenovation")
        return APIFilt_IsVisibleByRenovation;
    if (filter == "IsVisibleByStructureDisplay")
        return APIFilt_IsInStructureDisplay;
    if (filter == "IsVisibleIn3D")
        return APIFilt_In3D;
    if (filter == "OnActualFloor")
        return APIFilt_OnActFloor;
    if (filter == "OnActualLayout")
        return APIFilt_OnActLayout;
    if (filter == "InMyWorkspace")
        return APIFilt_InMyWorkspace;
    if (filter == "IsIndependent")
        return APIFilt_IsIndependent;
    if (filter == "InCroppedView")
        return APIFilt_InCroppedView;
    if (filter == "HasAccessRight")
        return APIFilt_HasAccessRight;
    if (filter == "IsOverriddenByRenovation")
        return APIFilt_IsOverridden;
    return APIFilt_None;
}

static API_Guid GetParentElemOfSectElem (const API_Guid& elemGuid)
{
    API_Element element = {};
    element.header.guid = elemGuid;
    if (ACAPI_Element_GetHeader (&element.header) != NoError ||
        GetElemTypeId (element.header) != API_SectElemID ||
        ACAPI_Element_Get (&element) != NoError) {
        return elemGuid;
    }
    return element.sectElem.parentGuid;
}

static GS::UniString StructureTypeToString (API_ModelElemStructureType structureType)
{
    switch (structureType) {
        case API_BasicStructure:
            return "Basic";
        case API_CompositeStructure:
            return "Composite";
        case API_ProfileStructure:
            return "Profile";
        default:
            return "Basic";
    }
}

static GS::UniString WallReferenceLineLocationToString (API_WallReferenceLineLocationID location)
{
    switch (location) {
        case APIWallRefLine_Outside:
            return "Outside";
        case APIWallRefLine_Center:
            return "Center";
        case APIWallRefLine_Inside:
            return "Inside";
        case APIWallRefLine_CoreOutside:
            return "CoreOutside";
        case APIWallRefLine_CoreCenter:
            return "CoreCenter";
        case APIWallRefLine_CoreInside:
            return "CoreInside";
        default:
            return "Unknown";
    }
}

static GS::UniString WallZoneRelationToString (API_ZoneRelID relation)
{
    switch (relation) {
        case APIZRel_Boundary:
            return "Boundary";
        case APIZRel_ReduceArea:
            return "ReduceArea";
        case APIZRel_None:
            return "None";
#ifdef ServerMainVers_2700
        case APIZRel_SubtractFromZone:
            return "SubtractFromZone";
#endif
        default:
            return "Unknown";
    }
}

static GS::UniString HatchTypeToString (API_HatchSubType hatchType)
{
    switch (hatchType) {
        case API_FillHatch:
            return "Fill";
        case API_BuildingMaterialHatch:
            return "BuildingMaterial";
        default:
            return "Fill";
    }
}

static GS::UniString HatchDeterminationToString (short determination)
{
    switch (determination) {
        case APIHatch_CutFills:
            return "Cut";
        case APIHatch_CoverFills:
            return "Cover";
        case APIHatch_DraftingFills:
        default:
            return "Drafting";
    }
}

static GS::UniString OpeningBasePolygonTypeToString (API_OpeningBasePolygonTypeTypeID type)
{
    switch (type) {
        case API_OpeningBasePolygonCircular:
            return "Circular";
        case API_OpeningBasePolygonCustom:
            return "Custom";
        case API_OpeningBasePolygonRectangular:
        default:
            return "Rectangular";
    }
}

static GS::UniString OpeningConstraintToString (API_OpeningConstraintTypeID constraint)
{
    switch (constraint) {
        case API_OpeningForcedHorizontal:
            return "ForcedHorizontal";
        case API_OpeningAligned:
            return "Aligned";
        case API_OpeningFree:
            return "Free";
        case API_OpeningForcedVertical:
        default:
            return "ForcedVertical";
    }
}

static GS::UniString OpeningLimitTypeToString (API_OpeningLimitTypeTypeID limitType)
{
    switch (limitType) {
        case API_OpeningLimitFinite:
            return "Finite";
        case API_OpeningLimitHalfInfinite:
            return "HalfInfinite";
        case API_OpeningLimitInfinite:
        default:
            return "Infinite";
    }
}

static GS::UniString ArrowTypeToString (API_ArrowID arrowType)
{
    switch (arrowType) {
        case APIArr_EmptyCirc:       return "EmptyCirc";
        case APIArr_CrossCircIs:     return "CrossCircIs";
        case APIArr_FullCirc:        return "FullCirc";
        case APIArr_SlashLine15:     return "SlashLine15";
        case APIArr_OpenArrow15:     return "OpenArrow15";
        case APIArr_ClosArrow15:     return "ClosArrow15";
        case APIArr_FullArrow15:     return "FullArrow15";
        case APIArr_SlashLine30:     return "SlashLine30";
        case APIArr_OpenArrow30:     return "OpenArrow30";
        case APIArr_ClosArrow30:     return "ClosArrow30";
        case APIArr_FullArrow30:     return "FullArrow30";
        case APIArr_SlashLine45:     return "SlashLine45";
        case APIArr_OpenArrow45:     return "OpenArrow45";
        case APIArr_ClosArrow45:     return "ClosArrow45";
        case APIArr_FullArrow45:     return "FullArrow45";
        case APIArr_SlashLine60:     return "SlashLine60";
        case APIArr_OpenArrow60:     return "OpenArrow60";
        case APIArr_ClosArrow60:     return "ClosArrow60";
        case APIArr_FullArrow60:     return "FullArrow60";
        case APIArr_SlashLine90:     return "SlashLine90";
        case APIArr_PepitaCirc:      return "PepitaCirc";
        case APIArr_BandArrow:       return "BandArrow";
        case APIArr_HalfArrowCcw15:  return "HalfArrowCcw15";
        case APIArr_HalfArrowCw15:   return "HalfArrowCw15";
        case APIArr_HalfArrowCcw30:  return "HalfArrowCcw30";
        case APIArr_HalfArrowCw30:   return "HalfArrowCw30";
        case APIArr_HalfArrowCcw45:  return "HalfArrowCcw45";
        case APIArr_HalfArrowCw45:   return "HalfArrowCw45";
        case APIArr_HalfArrowCcw60:  return "HalfArrowCcw60";
        case APIArr_HalfArrowCw60:   return "HalfArrowCw60";
        case APIArr_SlashLine75:     return "SlashLine75";
        default:                     return "EmptyCirc";
    }
}

static GS::ObjectState CreateArrowDataObjectState (const API_ArrowData& arrowData)
{
    return GS::ObjectState (
        "arrowType", ArrowTypeToString (arrowData.arrowType),
        "begArrow", arrowData.begArrow,
        "endArrow", arrowData.endArrow,
        "arrowPen", static_cast<Int32> (arrowData.arrowPen),
        "arrowSize", arrowData.arrowSize
    );
}

template <typename ListProxyType>
static GSErrCode GetElementsFromCurrentDatabase (
    const GS::ObjectState& parameters,
    ListProxyType& elementsListProxy,
    Int32 pageSize,
    Int32& enumeratedRows,
    Int32& emittedRows)
{
    API_ElemTypeID elemType = API_ZombieElemID;
    GS::UniString elementTypeStr;
    if (parameters.Get ("elementType", elementTypeStr)) {
        elemType = GetElementTypeFromNonLocalizedName (elementTypeStr);
    }

    bool includeSubElemObjects = false;
    API_ElemFilterFlags filterFlags = APIFilt_None;
    GS::Array<GS::UniString> filters;
    if (parameters.Get ("filters", filters)) {
        for (const GS::UniString& filter : filters) {
            if (filter == "IncludeSubElemObjects") {
                includeSubElemObjects = true;
            } else {
                filterFlags |= ConvertFilterStringToFlag (filter);
            }
        }
    }

    GS::Array<API_Guid> elemList;
    GSErrCode err;
    {
        PerfTrace::ScopeTimer timer (PerfTrace::Phase::ElemList);
        err = ACAPI_Element_GetElemList (elemType, &elemList, filterFlags);
    }
    if (err != NoError) {
        return err;
    }
    PerfTrace::ScopeTimer parentTimer (PerfTrace::Phase::ParentResolve);
    if (elemType == API_ObjectID && !includeSubElemObjects) {
        for (const API_Guid& elemGuid : elemList) {
            if (enumeratedRows < std::numeric_limits<Int32>::max ())
                ++enumeratedRows;
            if (emittedRows >= pageSize)
                return APIERR_MEMFULL;
            const API_Guid parentGuid = GetParentElemOfSectElem (elemGuid);
            API_Element elem = {};
            elem.header.guid = parentGuid;
            if (parentGuid != APINULLGuid && ACAPI_Element_Get (&elem) == NoError && elem.object.owner == APINULLGuid) {
                elementsListProxy (CreateElementIdObjectState (parentGuid));
                ++emittedRows;
            }
        }
    } else {
        for (const API_Guid& elemGuid : elemList) {
            if (enumeratedRows < std::numeric_limits<Int32>::max ())
                ++enumeratedRows;
            if (emittedRows >= pageSize)
                return APIERR_MEMFULL;
            const API_Guid parentGuid = GetParentElemOfSectElem (elemGuid);
            if (parentGuid == APINULLGuid)
                continue;
            elementsListProxy (CreateElementIdObjectState (parentGuid));
            ++emittedRows;
        }
    }
    return NoError;
}

GetElementsByTypeCommand::GetElementsByTypeCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetElementsByTypeCommand::GetName () const
{
    return "GetElementsByType";
}

GS::Optional<GS::UniString> GetElementsByTypeCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementType": {
                "$ref": "#/ElementType"
            },
            "filters": {
                "type": "array",
                "items": {
                    "$ref": "#/ElementFilter"
                },
                "minItems": 1
            },
            "databases": {
                "$ref": "#/Databases"
            },
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": [
            "elementType"
        ]
    })";
}

GS::Optional<GS::UniString> GetElementsByTypeCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ElementsWithExecutionResultsOrError"
    })";
}

GS::ObjectState GetElementsByTypeCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    PerfTrace::ResetAccumulator ();
    std::chrono::steady_clock::time_point perfTraceStart = std::chrono::steady_clock::now ();

    GS::UniString elementTypeStr;
    if (parameters.Get ("elementType", elementTypeStr)) {
        if (GetElementTypeFromNonLocalizedName (elementTypeStr) == API_ZombieElemID) {
            return CreateErrorResponse (APIERR_BADPARS,
                GS::UniString::Printf ("Invalid elementType '%T'.", elementTypeStr.ToPrintf ()));
        }
    }

    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");
    Int32 enumeratedRows = 0;
    Int32 emittedRows = 0;

    GS::Array<GS::ObjectState> databases;
    bool databasesParameterExists = parameters.Get ("databases", databases);
    if (!databasesParameterExists || databases.IsEmpty ()) {
        const GSErrCode err = GetElementsFromCurrentDatabase (parameters, elements, pageSize, enumeratedRows, emittedRows);
        if (err == APIERR_MEMFULL)
            return CreateNativePageRequiredResponse ("The element enumeration requires another page; no partial rows were returned.", enumeratedRows, 0);
        if (err != NoError)
            return CreateErrorResponse (err, "Failed to retrieve elements from the current database.");
    }
    else {
        const auto& executionResultForDatabases = response.AddList<GS::ObjectState> ("executionResultForDatabases");

        const GS::Array<API_Guid> databaseIds = databases.Transform<API_Guid> (GetGuidFromDatabaseArrayItem);

        auto action = [&]() -> GSErrCode {
            return GetElementsFromCurrentDatabase (parameters, elements, pageSize, enumeratedRows, emittedRows);
        };
        auto actionSuccess = [&]() -> void {
            executionResultForDatabases (CreateSuccessfulExecutionResult ());
        };
        auto actionFailure = [&](GSErrCode err, const GS::UniString& errMsg) -> void {
            executionResultForDatabases (CreateFailedExecutionResult (err, errMsg));
        };

        GSErrCode err = ExecuteActionForEachDatabase (databaseIds, action,  actionSuccess, actionFailure);
        if (err != NoError) {
            if (err == APIERR_MEMFULL) {
                return CreateNativePageRequiredResponse ("The element enumeration requires another page; no partial rows were returned.", enumeratedRows, 0);
            }
            return CreateErrorResponse (err, "Failed to retrieve the starting database or to switch back to it after execution.");
        }
    }

    PerfTrace::SetRequestedReturned (enumeratedRows, emittedRows);
    const double totalMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - perfTraceStart).count ();
    PerfTrace::WriteLine (GS::UniString (GetName ()), PerfTrace::NextSeq (), totalMs);

    return response;
}

GS::String GetAllElementsCommand::GetName () const
{
    return "GetAllElements";
}

GS::Optional<GS::UniString> GetAllElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "filters": {
                "type": "array",
                "items": {
                    "$ref": "#/ElementFilter"
                },
                "minItems": 1
            },
            "databases": {
                "$ref": "#/Databases"
            },
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": []
    })";
}

GetDetailsOfElementsCommand::GetDetailsOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetDetailsOfElementsCommand::GetName () const
{
    return "GetDetailsOfElements";
}

GS::Optional<GS::UniString> GetDetailsOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "detailProfile": {
                "type": "string",
                "enum": ["full", "drafting"]
            },
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": [
            "elements"
        ]
    })";
}

GS::Optional<GS::UniString> GetDetailsOfElementsCommand::GetResponseSchema () const
{
    // EXPERIMENT: response schema validation of multi-megabyte detail batches
    // dominated the framework-side cost (E-026 validator crash history). No
    // response schema => the framework skips response validation.
    return {};
}

// ── Floor plan polygon infrastructure (used by GetDetailsOfElements) ─────────

struct FloorPlanCollectorContext {
    GS::Array<GS::Array<API_Coord>> polys;
    GSSize coordinateCount = 0;
    bool invalid = false;
    bool inCutFill = false;
};

static thread_local FloorPlanCollectorContext* tl_floorPlanCtx = nullptr;

constexpr GSSize MaxCutFillPolygons = 10'000;
constexpr GSSize MaxCutFillCoordinates = 200'000;

static GSErrCode CollectCutFillPolygons (const API_PrimElement* primElem,
                                          const void* par1, const void* par2, const void* /*par3*/)
{
    if (tl_floorPlanCtx == nullptr || primElem == nullptr)
        return NoError;

    try {
    switch (primElem->header.typeID) {
        case API_PrimCtrl_HatchBorderBegID: {
            const auto* border = static_cast<const API_PrimHatchBorder*> (par1);
            tl_floorPlanCtx->inCutFill = (border != nullptr && border->determination == APIHatch_CutFills);
            break;
        }
        case API_PrimCtrl_HatchBorderEndID:
            tl_floorPlanCtx->inCutFill = false;
            break;
        case API_PrimPolyID: {
            if (!tl_floorPlanCtx->inCutFill)
                break;
            const Int32 nCoords = primElem->poly.nCoords;
            if (nCoords < 3 || static_cast<GSSize> (nCoords) > MaxCutFillCoordinates ||
                tl_floorPlanCtx->coordinateCount > MaxCutFillCoordinates - static_cast<GSSize> (nCoords)) {
                tl_floorPlanCtx->invalid = true;
                return APIERR_MEMFULL;
            }
            const auto* coords = static_cast<const API_Coord*> (par1);
            const auto* pends = static_cast<const Int32*> (par2);
            const Int32 nSubPolys = primElem->poly.nSubPolys;
            if (nSubPolys < 1 || nSubPolys > nCoords || (pends == nullptr && nSubPolys != 1)) {
                tl_floorPlanCtx->invalid = true;
                return APIERR_BADPARS;
            }
            if (static_cast<GSSize> (nSubPolys) > MaxCutFillPolygons ||
                static_cast<GSSize> (tl_floorPlanCtx->polys.GetSize ()) > MaxCutFillPolygons - static_cast<GSSize> (nSubPolys)) {
                tl_floorPlanCtx->invalid = true;
                return APIERR_MEMFULL;
            }
            if (coords == nullptr) {
                tl_floorPlanCtx->invalid = true;
                return APIERR_BADPARS;
            }
            Int32 start = 1;
            for (Int32 sub = 1; sub <= nSubPolys; ++sub) {
                const Int32 end = (pends != nullptr) ? pends[sub] : nCoords;
                if (end < start || end > nCoords) {
                    tl_floorPlanCtx->invalid = true;
                    return APIERR_BADPARS;
                }
                GS::Array<API_Coord> ring;
                for (Int32 i = start; i <= end; ++i) {
                    if (!std::isfinite (coords[i].x) || !std::isfinite (coords[i].y)) {
                        tl_floorPlanCtx->invalid = true;
                        return APIERR_BADPARS;
                    }
                    ring.Push (coords[i]);
                }
                if (ring.GetSize () >= 3)
                    tl_floorPlanCtx->polys.Push (ring);
                start = end + 1;
            }
            tl_floorPlanCtx->coordinateCount += static_cast<GSSize> (nCoords);
            break;
        }
        default:
            break;
    }
    return NoError;
    } catch (const std::bad_alloc&) {
        tl_floorPlanCtx->invalid = true;
        return APIERR_MEMFULL;
    } catch (...) {
        tl_floorPlanCtx->invalid = true;
        return APIERR_BADPARS;
    }
}

static bool IsPureDraftingType (API_ElemTypeID typeID)
{
    switch (typeID) {
        case API_LineID:
        case API_PolyLineID:
        case API_ArcID:
        case API_CircleID:
        case API_SplineID:
        case API_TextID:
        case API_LabelID:
        case API_HatchID:
        case API_HotspotID:
        case API_DrawingID:
        case API_PictureID:
        case API_DetailID:
        case API_WorksheetID:
        case API_GroupID:
        case API_CameraID:
        case API_CamSetID:
        case API_CutPlaneID:
        case API_ChangeMarkerID:
        case API_SectElemID:
        case API_ElevationID:
        case API_InteriorElevationID:
        case API_DimensionID:
        case API_RadialDimensionID:
        case API_LevelDimensionID:
        case API_AngleDimensionID:
            return true;
        default:
            return false;
    }
}

static void AddFloorPlanPolygonsIfAvailable (const API_Guid& guid, GS::ObjectState& os)
{
    FloorPlanCollectorContext ctx;
    tl_floorPlanCtx = &ctx;
    const GS::OnExit ctxGuard ([&] () { tl_floorPlanCtx = nullptr; });

    API_Elem_Head elemHead = {};
    elemHead.guid = guid;

    API_ShapePrimsParams params = {};
    params.dontClip   = true;
    params.allStories = true;
    params.polygon    = nullptr;

#if defined(ServerMainVers_2700)
    const GSErrCode fpErr = ACAPI_DrawingPrimitive_ShapePrimsExt (elemHead, CollectCutFillPolygons, &params);
#else
    const GSErrCode fpErr = ACAPI_Element_ShapePrimsExt (elemHead, CollectCutFillPolygons, &params);
#endif
    if (fpErr != NoError || ctx.invalid)
        return;
    if (ctx.polys.IsEmpty ())
        return;

    const auto& polygonsOS = os.AddList<GS::ObjectState> ("floorPlanPolygons");
    for (const GS::Array<API_Coord>& poly : ctx.polys) {
        GS::ObjectState polyOS;
        const auto& coordsOS = polyOS.AddList<GS::ObjectState> ("coordinates");
        for (const API_Coord& c : poly)
            coordsOS (Create2DCoordinateObjectState (c));
        polygonsOS (polyOS);
    }
}

static void AddLibPartBasedElementDetails (GS::ObjectState& os, const Int32 libInd, const API_Guid& owner, API_ElemTypeID ownerType = API_ZombieElemID)
{
    os.Add ("libPartIndex", libInd);
    API_LibPart	lp = {};
    LibraryPartLocationGuard lpLocationGuard (lp);
    lp.index = libInd;
    const GSErrCode libPartErr = ACAPI_LibraryPart_Get (&lp);
    if (libPartErr == NoError) {
        os.Add ("libPart", GS::ObjectState (
            "name", GS::UniString (lp.docu_UName),
            "parentUnID", CreateGuidObjectState (GS::UnID (lp.parentUnID).GetMainGuid ()),
            "ownUnID", CreateGuidObjectState (GS::UnID (lp.ownUnID).GetMainGuid ())));
    } else {
        // Do not expose zeroed API_LibPart fields as if they were valid
        // metadata when the library lookup failed.
        os.Add ("libPart", GS::ObjectState (
            "name", GS::EmptyUniString,
            "parentUnID", CreateGuidObjectState (APINULLGuid),
            "ownUnID", CreateGuidObjectState (APINULLGuid)));
    }

    if (owner != APINULLGuid) {
        os.Add ("ownerElementId", CreateGuidObjectState (owner));
        if (ownerType == API_ZombieElemID) {
            API_Elem_Head elemHead = {};
            elemHead.guid = owner;
            ACAPI_Element_GetHeader (&elemHead);
            ownerType = GetElemTypeId (elemHead);
        }
        os.Add ("ownerElementType", GetElementTypeNonLocalizedName (ownerType));
    }
}

static std::vector<API_Coord> GetCWPanelSurfaceCoords (const API_Guid& cwPanelGuid)
{
    std::vector<PolygonData> polygonData = GetPolygonsFromMemoCoords (cwPanelGuid);
    if (polygonData.empty ()) {
        return {};
    }

    return polygonData[0].coords;
}

static Geometry::Point2d GridMeshVertexToPoint2d (const API_GridMeshVertex& vertex)
{
    return Geometry::Point2d (vertex.surfaceParam.x, vertex.surfaceParam.y);
}

static const API_GridMeshEdge* GetGridMeshEdge (const API_GridMesh& gridMesh, const API_GridEdgeInfo& edgeInfo)
{
    const auto& edges = edgeInfo.mainAxis ? gridMesh.meshEdgesMainAxis : gridMesh.meshEdgesSecondaryAxis;
    if (!edges.ContainsKey (edgeInfo.id))
        return nullptr;
    return &edges[edgeInfo.id];
}

static bool GetFrameSurfaceParamCoords (
    const API_CWFrameType& cwFrame,
    const API_GridMesh& ownerGridMesh,
    API_Coord& begCoord,
    API_Coord& endCoord)
{
    const API_Coord& begRel = cwFrame.begRel;
    const API_Coord& endRel = cwFrame.endRel;
    if (!ownerGridMesh.meshPolygons.ContainsKey (cwFrame.cellID))
        return false;
    const API_GridMeshPolygon& gridMeshPolygon = ownerGridMesh.meshPolygons[cwFrame.cellID];
    const API_GridMeshEdge* edgeX = GetGridMeshEdge (ownerGridMesh, gridMeshPolygon.edges[0]);
    const API_GridMeshEdge* edgeY = GetGridMeshEdge (ownerGridMesh, gridMeshPolygon.edges[1]);
    if (edgeX == nullptr || edgeY == nullptr ||
        !ownerGridMesh.meshVertices.ContainsKey (edgeX->begID) ||
        !ownerGridMesh.meshVertices.ContainsKey (edgeX->endID) ||
        !ownerGridMesh.meshVertices.ContainsKey (edgeY->begID) ||
        !ownerGridMesh.meshVertices.ContainsKey (edgeY->endID)) {
        return false;
    }
    const Geometry::Point2d vX1 = GridMeshVertexToPoint2d (ownerGridMesh.meshVertices[edgeX->begID]);
    const Geometry::Point2d vX2 = GridMeshVertexToPoint2d (ownerGridMesh.meshVertices[edgeX->endID]);
    const Geometry::Point2d vY1 = GridMeshVertexToPoint2d (ownerGridMesh.meshVertices[edgeY->begID]);
    const Geometry::Point2d vY2 = GridMeshVertexToPoint2d (ownerGridMesh.meshVertices[edgeY->endID]);
    const Geometry::Point2d& cellOrigo = vX1;
    const Geometry::Vector2d vX = vX2 - vX1;
    const Geometry::Vector2d vY = vY2 - vY1;
    begCoord = API_Coord {cellOrigo.x + vX.x * begRel.x + vY.x * begRel.y, cellOrigo.y + vX.y * begRel.x + vY.y * begRel.y};
    endCoord = API_Coord {cellOrigo.x + vX.x * endRel.x + vY.x * endRel.y, cellOrigo.y + vX.y * endRel.x + vY.y * endRel.y};
    return true;
}

static const API_CWFrameType* FindNextFrameOfPanel (std::vector<const API_CWFrameType*>& framePtrs, std::vector<API_Coord3D>& polygonCoords)
{
    if (polygonCoords.empty ())
        return nullptr;
    for (auto it = framePtrs.begin (); it != framePtrs.end (); ++it) {
        const API_CWFrameType* framePtr = *it;
        if (IsSame3DCoordinate (polygonCoords.back (), framePtr->begC)) {
            polygonCoords.push_back (framePtr->endC);
            framePtrs.erase (it);
            return framePtr;
        } else if (IsSame3DCoordinate (polygonCoords.back (), framePtr->endC)) {
            polygonCoords.push_back (framePtr->begC);
            framePtrs.erase (it);
            return framePtr;
        }
    }
    return nullptr;
}

using CWSegmentGridCellID = std::pair<UInt32, API_GridElemID>;

// Do not emit CreateErrorResponse / ErrorItem here: the response schema for
// detailsOfElements items is the success detail object only. Top-level ErrorItem
// previously failed validation; allowing it via oneOf crashed Archicad's schema
// validator on large marquee batches (E-026). Keep positional slots with a
// schema-valid stub whose TypeSpecificDetails carries a string error instead.
static GS::ObjectState CreateUnavailableElementDetails (const GS::ObjectState* elementId, const GS::UniString& message)
{
    GS::ObjectState detailsOfElement;
    detailsOfElement.Add ("type", "Object");
    detailsOfElement.Add ("elementId", elementId != nullptr
        ? *elementId
        : CreateGuidObjectState (APINULLGuid));
    detailsOfElement.Add ("id", GS::EmptyUniString);
    detailsOfElement.Add ("floorIndex", static_cast<Int32> (0));
    detailsOfElement.Add ("layerIndex", static_cast<Int32> (0));
    detailsOfElement.Add ("drawIndex", static_cast<short> (0));
    detailsOfElement.Add ("details", GS::ObjectState ("error", message));
    return detailsOfElement;
}

GS::ObjectState GetDetailsOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    PerfTrace::ResetAccumulator ();
    std::chrono::steady_clock::time_point perfTraceStart = std::chrono::steady_clock::now ();

    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);
    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");
    if (elements.GetSize () > static_cast<GS::USize> (pageSize)) {
        return CreateNativePageRequiredResponse (
            "The details request requires another page; no partial details were returned.",
            static_cast<Int32> (elements.GetSize ()),
            0);
    }

    GS::UniString detailProfile;
    parameters.Get ("detailProfile", detailProfile);
    const bool draftingProfile = detailProfile == "drafting";

    GS::ObjectState response;
    const auto& detailsOfElements = response.AddList<GS::ObjectState> ("detailsOfElements");

    const Stories stories = GetStories ();

    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            detailsOfElements (CreateUnavailableElementDetails (nullptr, "elementId is missing"));
            continue;
        }

        API_Element elem = {};
        elem.header.guid = GetGuidFromObjectState (*elementId);
        GSErrCode err;
        {
            PerfTrace::ScopeTimer timer (PerfTrace::Phase::ElementGet);
            err = ACAPI_Element_Get (&elem);
        }

        if (err != NoError) {
            detailsOfElements (CreateUnavailableElementDetails (elementId, "Failed to get the details of element"));
            continue;
        }

        GS::ObjectState detailsOfElement;
        const API_ElemTypeID typeID = GetElemTypeId (elem.header);
        const GS::UniString typeName = GetElementTypeNonLocalizedName (typeID);
        PerfTrace::ScopeTimer typeTimer (typeName);     // per-type timer for the rest of the loop body

        detailsOfElement.Add ("type", typeName);
        detailsOfElement.Add ("elementId", CreateGuidObjectState (elem.header.guid));
        detailsOfElement.Add ("floorIndex", elem.header.floorInd);

        if (!draftingProfile) {
            Int32 layerIndex;
            {
                PerfTrace::ScopeTimer timer (PerfTrace::Phase::AttrLookup);
                layerIndex = GetAttributeIndex (elem.header.layer);
            }
            detailsOfElement.Add ("layerIndex", layerIndex);
            detailsOfElement.Add ("drawIndex", static_cast<short> (elem.header.drwIndex));
        }

        {
            API_ElementMemo memo = {};
            const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
            {
                PerfTrace::ScopeTimer timer (PerfTrace::Phase::MemoInfo);
                ACAPI_Element_GetMemo (elem.header.guid, &memo, APIMemoMask_ElemInfoString);
            }

            detailsOfElement.Add ("id", memo.elemInfoString != nullptr
                ? *memo.elemInfoString
                : GS::EmptyUniString);
        }

        {
            API_Elem_Head elemHead = {};
            elemHead.guid = elem.header.guid;
            API_Box3D bounds = {};
            GSErrCode calcBoundsErr;
            {
                PerfTrace::ScopeTimer timer (PerfTrace::Phase::CalcBounds);
                calcBoundsErr = ACAPI_Element_CalcBounds (&elemHead, &bounds);
            }
            if (calcBoundsErr == NoError && IsFiniteBounds (bounds)) {
                detailsOfElement.Add ("bounds", GS::ObjectState (
                    "xMin", bounds.xMin,
                    "yMin", bounds.yMin,
                    "xMax", bounds.xMax,
                    "yMax", bounds.yMax));
            }
        }

        GS::ObjectState typeSpecificDetails;

        switch (typeID) {
            case API_WallID:
                switch (elem.wall.type) {
                    case APIWtyp_Normal:
                        typeSpecificDetails.Add ("geometryType", "Straight");
                        typeSpecificDetails.Add ("arcAngle", elem.wall.angle);
                        break;
                    case APIWtyp_Trapez:
                        typeSpecificDetails.Add ("geometryType", "Trapezoid");
                        break;
                    case APIWtyp_Poly:
                        {
                            typeSpecificDetails.Add ("geometryType", "Polygonal");
                            {
                                PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                                AddPolygonFromMemoCoords (elem.header.guid, typeSpecificDetails, "polygonOutline", "polygonArcs");
                            }
                            break;
                        }
                }
                typeSpecificDetails.Add ("structureType", StructureTypeToString (elem.wall.modelElemStructureType));
                typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, elem.wall.bottomOffset, stories));
                typeSpecificDetails.Add ("begCoordinate", Create2DCoordinateObjectState (elem.wall.begC));
                typeSpecificDetails.Add ("endCoordinate", Create2DCoordinateObjectState (elem.wall.endC));
                typeSpecificDetails.Add ("height", elem.wall.height);
                typeSpecificDetails.Add ("bottomOffset", elem.wall.bottomOffset);
                typeSpecificDetails.Add ("offset", elem.wall.offset);
                typeSpecificDetails.Add ("referenceLineLocation", WallReferenceLineLocationToString (elem.wall.referenceLineLocation));
                typeSpecificDetails.Add ("offsetFromOutside", elem.wall.offsetFromOutside);
                typeSpecificDetails.Add ("sequence", elem.wall.sequence);
                typeSpecificDetails.Add ("zoneRelation", WallZoneRelationToString (elem.wall.zoneRel));
                typeSpecificDetails.Add ("flipped", elem.wall.flipped);
                if (elem.wall.type == APIWtyp_Poly) {
                    typeSpecificDetails.Add ("begThickness", 0);
                    typeSpecificDetails.Add ("endThickness", 0);
                } else {
                    typeSpecificDetails.Add ("begThickness", elem.wall.thickness);
                    typeSpecificDetails.Add ("endThickness", elem.wall.thickness1);
                }
                if (StructureTypeToString (elem.wall.modelElemStructureType) == "Composite") {
                    API_Guid compositeGuid;
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::AttrLookup);
                        compositeGuid = GetAttributeGuidFromIndex (API_CompWallID, elem.wall.composite);
                    }
                    typeSpecificDetails.Add ("compositeId", CreateGuidObjectState (compositeGuid));
                }
                break;

            case API_BeamID:
                typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, elem.beam.level, stories));
                typeSpecificDetails.Add ("begCoordinate", Create2DCoordinateObjectState (elem.beam.begC));
                typeSpecificDetails.Add ("endCoordinate", Create2DCoordinateObjectState (elem.beam.endC));
                typeSpecificDetails.Add ("level", elem.beam.level);
                typeSpecificDetails.Add ("offset", elem.beam.offset);
                typeSpecificDetails.Add ("slantAngle", elem.beam.slantAngle);
                typeSpecificDetails.Add ("isSlanted", elem.beam.isSlanted);
                typeSpecificDetails.Add ("profileAngle", elem.beam.profileAngle);
                typeSpecificDetails.Add ("arcAngle", elem.beam.curveAngle);
                typeSpecificDetails.Add ("verticalCurveHeight", elem.beam.verticalCurveHeight);
                break;

            case API_SlabID:
                typeSpecificDetails.Add ("structureType", StructureTypeToString (elem.slab.modelElemStructureType));
                typeSpecificDetails.Add ("thickness", elem.slab.thickness);
                typeSpecificDetails.Add ("level", elem.slab.level);
                typeSpecificDetails.Add ("offsetFromTop", elem.slab.offsetFromTop);
                typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, elem.slab.level, stories));
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonWithHolesFromMemoCoords (elem.header.guid, typeSpecificDetails, "polygonOutline", "polygonArcs", "holes", "polygonOutline", "polygonArcs");
                }
                break;

            case API_ZoneID:
                typeSpecificDetails.Add ("name", GS::UniString (elem.zone.roomName));
                typeSpecificDetails.Add ("numberStr", GS::UniString (elem.zone.roomNoStr));
                {
                    API_Guid categoryAttributeGuid;
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::AttrLookup);
                        categoryAttributeGuid = GetAttributeGuidFromIndex (API_ZoneCatID, elem.zone.catInd);
                    }
                    typeSpecificDetails.Add ("categoryAttributeId", CreateGuidObjectState (categoryAttributeGuid));
                }
                typeSpecificDetails.Add ("stampPosition", Create2DCoordinateObjectState (elem.zone.pos));
                typeSpecificDetails.Add ("stampAngle", elem.zone.stampAngle);
                typeSpecificDetails.Add ("fixedStampAngle", elem.zone.fixedAngle);
                typeSpecificDetails.Add ("isManual", elem.zone.manual);
                typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, elem.zone.roomBaseLev, stories));
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonWithHolesFromMemoCoords (elem.header.guid, typeSpecificDetails, "polygonOutline", "polygonArcs", "holes", "polygonOutline", "polygonArcs");
                }
                break;

            case API_HatchID:
                // Hand-drafted sheets often use Hatch as their only semantic
                // area primitive (unit diagrams, coloured zones, legends).
                // Keep its geometry and effective display attributes explicit.
                typeSpecificDetails.Add ("hatchType", HatchTypeToString (elem.hatch.hatchType));
                typeSpecificDetails.Add ("determination", HatchDeterminationToString (elem.hatch.determination));
                typeSpecificDetails.Add ("contourPen", (Int32) elem.hatch.contPen.GetEffectiveColorIndex ());
                typeSpecificDetails.Add ("fillPen", (Int32) elem.hatch.fillPen.GetEffectiveColorIndex ());
                typeSpecificDetails.Add ("fillBackgroundPen", (Int32) elem.hatch.fillBGPen);
                typeSpecificDetails.Add ("fillAttributeIndex", GetAttributeIndex (elem.hatch.fillInd));
                typeSpecificDetails.Add ("buildingMaterialIndex", GetAttributeIndex (elem.hatch.buildingMaterial));
                typeSpecificDetails.Add ("hasForegroundRGB", (elem.hatch.hatchFlags & APIHatch_HasFgRGBColor) != 0);
                typeSpecificDetails.Add ("hasBackgroundRGB", (elem.hatch.hatchFlags & APIHatch_HasBkgRGBColor) != 0);
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonWithHolesFromMemoCoords (elem.header.guid, typeSpecificDetails, "polygonOutline", "polygonArcs", "holes", "polygonOutline", "polygonArcs");
                }
                break;

            case API_OpeningID:
                // Generic Opening is the native void used for late shaft and
                // service penetrations. It needs a host and explicit geometry
                // before an agent can audit downstream drawing impact.
                typeSpecificDetails.Add ("ownerElementId", CreateGuidObjectState (elem.opening.owner));
                typeSpecificDetails.Add ("basePolygonType", OpeningBasePolygonTypeToString (elem.opening.extrusionGeometryData.parameters.basePolygonType));
                typeSpecificDetails.Add ("width", elem.opening.extrusionGeometryData.parameters.width);
                typeSpecificDetails.Add ("height", elem.opening.extrusionGeometryData.parameters.height);
                typeSpecificDetails.Add ("constraint", OpeningConstraintToString (elem.opening.extrusionGeometryData.parameters.constraint));
                typeSpecificDetails.Add ("anchorAltitude", elem.opening.extrusionGeometryData.parameters.anchorAltitude);
                typeSpecificDetails.Add ("limitType", OpeningLimitTypeToString (elem.opening.extrusionGeometryData.parameters.limitType));
                typeSpecificDetails.Add ("extrusionStartOffset", elem.opening.extrusionGeometryData.parameters.extrusionStartOffset);
                typeSpecificDetails.Add ("finiteBodyLength", elem.opening.extrusionGeometryData.parameters.finiteBodyLength);
                typeSpecificDetails.Add ("linked", elem.opening.extrusionGeometryData.parameters.linkedStatus == API_OpeningLinked);
                break;

            case API_ColumnID:
                typeSpecificDetails.Add ("origin", Create2DCoordinateObjectState (elem.column.origoPos));
                typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, elem.column.bottomOffset, stories));
                typeSpecificDetails.Add ("height", elem.column.height);
                typeSpecificDetails.Add ("bottomOffset", elem.column.bottomOffset);
                typeSpecificDetails.Add ("slantAngle", elem.column.slantAngle);
                typeSpecificDetails.Add ("isSlanted", elem.column.isSlanted);
                break;

            case API_DoorID:
            case API_WindowID:
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::LibPart);
                    AddLibPartBasedElementDetails (typeSpecificDetails, elem.window.openingBase.libInd, elem.window.owner);
                }
                typeSpecificDetails.Add ("width", elem.window.openingBase.width);
                typeSpecificDetails.Add ("height", elem.window.openingBase.height);
                typeSpecificDetails.Add ("sillHeight", elem.window.lower);
                // `objLoc` is longitudinal placement on the wall. Reveal
                // depth is separate and determines the interior/exterior
                // jamb areas needed for finishes and quantity checks.
                typeSpecificDetails.Add ("hasReveal", elem.window.reveal);
                typeSpecificDetails.Add ("revealDepthReference", elem.window.revealDepthLocation == APIWDRevealDepth_Core ? "Core" : "Side");
                typeSpecificDetails.Add ("revealDepthOffset", elem.window.revealDepthOffset);
                typeSpecificDetails.Add ("revealDepthFromSide", elem.window.revealDepthFromSide);
                typeSpecificDetails.Add ("jambDepthHead", elem.window.jambDepthHead);
                typeSpecificDetails.Add ("jambDepthSill", elem.window.jambDepthSill);
                typeSpecificDetails.Add ("jambDepthLeft", elem.window.jambDepth);
                typeSpecificDetails.Add ("jambDepthRight", elem.window.jambDepth2);
                typeSpecificDetails.Add ("openingStartPoint", Create2DCoordinateObjectState (elem.window.startPoint));
                typeSpecificDetails.Add ("openingDirection", Create2DCoordinateObjectState (elem.window.dirVector));
                typeSpecificDetails.Add ("centerOffset", elem.window.objLoc);
                typeSpecificDetails.Add ("reflected", elem.window.openingBase.reflected);
                typeSpecificDetails.Add ("refSide", elem.window.openingBase.refSide);
                typeSpecificDetails.Add ("oSide", elem.window.openingBase.oSide);
                break;

            case API_LabelID:
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::LibPart);
                    AddLibPartBasedElementDetails (typeSpecificDetails, ((elem.label.labelClass == APILblClass_Symbol) ? elem.label.u.symbol.libInd : -1), elem.label.parent, GetElemTypeId (elem.label.parentType));
                }
                typeSpecificDetails.Add ("begCoordinate", Create2DCoordinateObjectState (elem.label.begC));
                typeSpecificDetails.Add ("midCoordinate", Create2DCoordinateObjectState (elem.label.midC));
                typeSpecificDetails.Add ("endCoordinate", Create2DCoordinateObjectState (elem.label.endC));
                typeSpecificDetails.Add ("hasLeaderLine", elem.label.hasLeaderLine);
                break;

            case API_ObjectID:
            case API_LampID: {
#ifdef ServerMainVers_2600
                auto ownerType = elem.object.ownerType;
#else
                auto ownerType = elem.object.ownerID;
#endif
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::LibPart);
                    AddLibPartBasedElementDetails (typeSpecificDetails, elem.object.libInd, elem.object.owner, GetElemTypeId (ownerType));
                }
                typeSpecificDetails.Add ("origin", Create3DCoordinateObjectState ({elem.object.pos.x, elem.object.pos.y, GetZPos (elem.header.floorInd, elem.object.level, stories)}));
                double zDimension = 0.0;
                API_ElementMemo objectMemo = {};
                const GS::OnExit objectMemoGuard ([&objectMemo] () { ACAPI_DisposeElemMemoHdls (&objectMemo); });
                GSErrCode objectMemoErr;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    objectMemoErr = ACAPI_Element_GetMemo (elem.header.guid, &objectMemo, APIMemoMask_AddPars);
                }
                constexpr GSSize maxObjectParameters = 10'000;
                GSSize nParams = 0;
                if (objectMemoErr == NoError && objectMemo.params != nullptr && *objectMemo.params != nullptr) {
                    const GSSize paramsBytes = BMhGetSize (reinterpret_cast<GSHandle> (objectMemo.params));
                    if (paramsBytes > 0 && paramsBytes % static_cast<GSSize> (sizeof (API_AddParType)) == 0 &&
                        paramsBytes <= maxObjectParameters * static_cast<GSSize> (sizeof (API_AddParType))) {
                        nParams = paramsBytes / static_cast<GSSize> (sizeof (API_AddParType));
                    }
                }
                for (GSIndex ii = 0; ii < nParams && ii < maxObjectParameters; ++ii) {
                    const API_AddParType& actParam = (*objectMemo.params)[ii];

                    const GS::String name(actParam.name);
                    if (name == "ZZYZX" && std::isfinite (actParam.value.real)) {
                        zDimension = actParam.value.real;
                        break;
                    }
                }
                typeSpecificDetails.Add ("dimensions", Create3DCoordinateObjectState ({elem.object.xRatio, elem.object.yRatio, zDimension}));
                typeSpecificDetails.Add ("angle", elem.object.angle);
            } break;

            case API_DetailID:
            case API_WorksheetID: {
                typeSpecificDetails.Add ("basePoint", Create2DCoordinateObjectState (elem.detail.pos));
                typeSpecificDetails.Add ("angle", elem.detail.angle);
                typeSpecificDetails.Add ("markerId", CreateGuidObjectState (elem.detail.markId));
                typeSpecificDetails.Add ("detailName", GS::UniString (elem.detail.detailName));
                typeSpecificDetails.Add ("detailIdStr", GS::UniString (elem.detail.detailIdStr));
                typeSpecificDetails.Add ("isHorizontalMarker", elem.detail.horizontalMarker);
                typeSpecificDetails.Add ("isWindowOpened", elem.detail.windOpened);
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonFromMemoCoords (elem.header.guid, typeSpecificDetails, "clipPolygon");
                }
                GS::ObjectState linkDataOS;
                switch (elem.detail.linkData.referringLevel) {
                    case API_ReferringLevel::ReferredToView:
                        linkDataOS.Add ("referredView", CreateGuidObjectState (elem.detail.linkData.referredView));
                        break;
                    case API_ReferringLevel::ReferredToDrawing:
                        linkDataOS.Add ("referredDrawing", CreateGuidObjectState (elem.detail.linkData.referredDrawing));
                        break;
                    case API_ReferringLevel::ReferredToViewPoint:
                        linkDataOS.Add ("referredPMViewPoint", CreateGuidObjectState (elem.detail.linkData.referredPMViewPoint));
                        break;
                    default:
                        break;
                }
                typeSpecificDetails.Add ("linkData", linkDataOS);
            } break;

            case API_PolyLineID: {
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonFromMemoCoords (elem.header.guid, typeSpecificDetails, "coordinates", "arcs");
                }
                if (!draftingProfile)
                    typeSpecificDetails.Add ("zCoordinate", GetZPos (elem.header.floorInd, 0, stories));
                typeSpecificDetails.Add ("arrowData", CreateArrowDataObjectState (elem.polyLine.arrowData));
            } break;

            case API_CurtainWallID: {
                typeSpecificDetails.Add ("height", elem.curtainWall.height);
                typeSpecificDetails.Add ("angle", elem.curtainWall.angle);
                typeSpecificDetails.Add ("flipped", elem.curtainWall.flipped);
            } break;

            case API_CurtainWallSegmentID: {
                typeSpecificDetails.Add ("begCoordinate", Create3DCoordinateObjectState (elem.cwSegment.begC));
                typeSpecificDetails.Add ("endCoordinate", Create3DCoordinateObjectState (elem.cwSegment.endC));
                typeSpecificDetails.Add ("extrusionVector", Create3DCoordinateObjectState (elem.cwSegment.extrusion));
                typeSpecificDetails.Add ("gridOrigin", Create3DCoordinateObjectState (elem.cwSegment.gridOrigin));
                typeSpecificDetails.Add ("gridAngle", elem.cwSegment.gridAngle);
                if (elem.cwSegment.segmentType == API_CWSegmentTypeID::APICWSeT_Arc) {
                    typeSpecificDetails.Add ("arcOrigin", Create3DCoordinateObjectState (elem.cwSegment.arcOrigin));
                    typeSpecificDetails.Add ("isNegativeArc", elem.cwSegment.negArc);
                }
            } break;

            case API_CurtainWallPanelID: {
                API_Element ownerCW = {};
                ownerCW.header.guid = elem.cwPanel.owner;
                if (ACAPI_Element_Get (&ownerCW) != NoError) {
                    typeSpecificDetails.Add ("error", "Failed to read the Curtain Wall owner.");
                    break;
                }

                API_ElementMemo cwMemo = {};
                const GS::OnExit cwMemoGuard ([&cwMemo] () { ACAPI_DisposeElemMemoHdls (&cwMemo); });
                GSErrCode cwMemoErr;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    cwMemoErr = ACAPI_Element_GetMemo (ownerCW.header.guid, &cwMemo, APIMemoMask_CWallSegments | APIMemoMask_CWallFrames);
                }
                if (cwMemoErr != NoError || cwMemo.cWallSegments == nullptr ||
                    cwMemo.cWallFrames == nullptr ||
                    elem.cwPanel.segmentID >= ownerCW.curtainWall.nSegments) {
                    typeSpecificDetails.Add ("error", "Curtain Wall segment memo data is unavailable or out of range.");
                    break;
                }
                const GSSize segmentBytes = BMGetPtrSize (reinterpret_cast<GSPtr> (cwMemo.cWallSegments));
                const GSSize frameBytes = BMGetPtrSize (reinterpret_cast<GSPtr> (cwMemo.cWallFrames));
                if (segmentBytes % sizeof (*cwMemo.cWallSegments) != 0 || frameBytes % sizeof (*cwMemo.cWallFrames) != 0) {
                    typeSpecificDetails.Add ("error", "Curtain Wall segment/frame memo byte sizes are invalid.");
                    break;
                }
                const GSSize segmentCount = segmentBytes / sizeof (*cwMemo.cWallSegments);
                const GSSize frameCount = frameBytes / sizeof (*cwMemo.cWallFrames);
                if (static_cast<GSSize> (elem.cwPanel.segmentID) >= segmentCount || frameCount == 0) {
                    typeSpecificDetails.Add ("error", "Curtain Wall segment/frame memo bounds are invalid.");
                    break;
                }
                const API_Guid ownerCWSegment = cwMemo.cWallSegments[elem.cwPanel.segmentID].head.guid;

                API_ElementMemo cwSegmentMemo = {};
                const GS::OnExit cwSegmentMemoGuard ([&cwSegmentMemo] () { ACAPI_DisposeElemMemoHdls (&cwSegmentMemo); });
                GSErrCode cwSegmentMemoErr;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    cwSegmentMemoErr = ACAPI_Element_GetMemo (ownerCWSegment, &cwSegmentMemo, APIMemoMask_CWSegGridMesh);
                }
                if (cwSegmentMemoErr != NoError || cwSegmentMemo.cWSegGridMesh == nullptr) {
                    typeSpecificDetails.Add ("error", "Curtain Wall grid mesh memo data is unavailable.");
                    break;
                }
                const API_GridMesh& ownerGridMesh = *cwSegmentMemo.cWSegGridMesh;

                API_ElementMemo cwPanelMemo = {};
                const GS::OnExit cwPanelMemoGuard ([&cwPanelMemo] () { ACAPI_DisposeElemMemoHdls (&cwPanelMemo); });
                GSErrCode cwPanelMemoErr;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    cwPanelMemoErr = ACAPI_Element_GetMemo (elem.header.guid, &cwPanelMemo, APIMemoMask_CWallPanels);
                }
                if (cwPanelMemoErr != NoError || cwPanelMemo.cWallPanelGridIDTable == nullptr ||
                    !cwPanelMemo.cWallPanelGridIDTable->ContainsKey (elem.header.guid)) {
                    typeSpecificDetails.Add ("error", "Curtain Wall panel grid mapping is unavailable.");
                    break;
                }
                const GS::HashTable<API_Guid, GS::Array<API_GridElemID>>& cWallPanelGridIDTable = *cwPanelMemo.cWallPanelGridIDTable;

                const GS::Array<API_GridElemID>& gridIDs = cWallPanelGridIDTable[elem.header.guid];
                GS::Array<API_GridElemID> gridMeshPolygonIDs = gridIDs;
                for (const API_GridElemID& polygonID : gridIDs) {
                    if (!ownerGridMesh.meshPolygons.ContainsKey (polygonID))
                        continue;
                    const API_GridMeshPolygon& gridMeshPolygon = ownerGridMesh.meshPolygons[polygonID];
                    gridMeshPolygonIDs.Append (gridMeshPolygon.neighbourIDs[API_GridMeshDirection::API_GridMeshRight]);
                    gridMeshPolygonIDs.Append (gridMeshPolygon.neighbourIDs[API_GridMeshDirection::API_GridMeshUpper]);
                }

                const std::vector<API_Coord> panelSurfaceCoords = GetCWPanelSurfaceCoords (elem.header.guid);

                std::vector<const API_CWFrameType*> framePtrs;
                {
                    std::vector<const API_CWFrameType*> cornerFramesOfNextSegment;
                    const UIndex availableFrameCount = static_cast<UIndex> (frameCount);
                    const UIndex frameLimit = ownerCW.curtainWall.nFrames < availableFrameCount
                        ? ownerCW.curtainWall.nFrames
                        : availableFrameCount;
                    for (UIndex i = 0; i < frameLimit; ++i) {
                        const API_CWFrameType& frame = cwMemo.cWallFrames[i];
                        if (frame.segmentID == elem.cwPanel.segmentID) {
                            if (gridMeshPolygonIDs.Contains (frame.cellID)) {
                                for (GSIndex i = 0; i + 1 < panelSurfaceCoords.size (); ++i) {
                                    const API_Coord& c1 = panelSurfaceCoords[i];
                                    const API_Coord& c2 = panelSurfaceCoords[i + 1];
                                    API_Coord frameBegCoord = {};
                                    API_Coord frameEndCoord = {};
                                    if (!GetFrameSurfaceParamCoords (frame, ownerGridMesh, frameBegCoord, frameEndCoord))
                                        continue;

                                    if ((IsSame2DCoordinate (frameBegCoord, c1) && IsSame2DCoordinate (frameEndCoord, c2)) ||
                                        (IsSame2DCoordinate (frameBegCoord, c2) && IsSame2DCoordinate (frameEndCoord, c1))) {
                                        framePtrs.push_back (&frame);
                                        break;
                                    }
                                }
                                if (framePtrs.size () >= elem.cwPanel.edgesNum) {
                                    break;
                                }
                            }
                        } else if (frame.classID == APICWFrameClass_Corner && frame.segmentID == elem.cwPanel.segmentID + 1) {
                            cornerFramesOfNextSegment.push_back (&frame);
                        }
                    }
                    if (framePtrs.size () < elem.cwPanel.edgesNum) {
                        framePtrs.insert (framePtrs.end (), cornerFramesOfNextSegment.begin (), cornerFramesOfNextSegment.end ());
                    }
                }

                const auto& polygonCoordinates = typeSpecificDetails.AddList<GS::ObjectState> ("polygonCoordinates");
                const auto& frames = typeSpecificDetails.AddList<GS::ObjectState> ("frames");
                if (!framePtrs.empty ()) {
                    std::vector<API_Coord3D> polygonCoords = { framePtrs[0]->begC, framePtrs[0]->endC };
                    frames (CreateElementIdObjectState (framePtrs[0]->head.guid));
                    framePtrs.erase (framePtrs.begin ());
                    while (polygonCoords.size() != (elem.cwPanel.edgesNum + 1) && !framePtrs.empty ()) {
                        auto* framePtr = FindNextFrameOfPanel (framePtrs, polygonCoords);
                        if (framePtr == nullptr) {
                            break;
                        }
                        frames (CreateElementIdObjectState (framePtr->head.guid));
                    }
                    for (const auto& c : polygonCoords) {
                        polygonCoordinates (Create3DCoordinateObjectState (c));
                    }
                }
                typeSpecificDetails.Add ("isHidden", elem.cwPanel.hidden);
                typeSpecificDetails.Add ("segmentIndex", elem.cwPanel.segmentID);
                typeSpecificDetails.Add ("className", GS::UniString (elem.cwPanel.className));
            } break;

            case API_CurtainWallFrameID: {
                typeSpecificDetails.Add ("begCoordinate", Create3DCoordinateObjectState (elem.cwFrame.begC));
                typeSpecificDetails.Add ("endCoordinate", Create3DCoordinateObjectState (elem.cwFrame.endC));
                typeSpecificDetails.Add ("orientationVector", Create3DCoordinateObjectState (elem.cwFrame.orientation));
                typeSpecificDetails.Add ("panelConnectionHole", GS::ObjectState ("d", elem.cwFrame.d, "w", elem.cwFrame.w));
                typeSpecificDetails.Add ("frameContour", GS::ObjectState ("a1", elem.cwFrame.a1, "a2", elem.cwFrame.a2, "b1", elem.cwFrame.b1, "b2", elem.cwFrame.b2));
                typeSpecificDetails.Add ("segmentIndex", elem.cwFrame.segmentID);
                typeSpecificDetails.Add ("className", GS::UniString (elem.cwFrame.className));
                typeSpecificDetails.Add ("type",
                    elem.cwFrame.classID == APICWFrameClass_Merged ? "Deleted" :
                    elem.cwFrame.classID == APICWFrameClass_Boundary ? "Boundary" :
                    elem.cwFrame.classID == APICWFrameClass_Corner ? "Corner" :
                    elem.cwFrame.classID == APICWFrameClass_Division ? "Division" : "Custom");
            } break;

            case API_MeshID: {
                typeSpecificDetails.Add ("level", elem.mesh.level);
                if (elem.mesh.skirt == 3) {
                    typeSpecificDetails.Add ("skirtType", "SurfaceOnlyWithoutSkirt");
                } else if (elem.mesh.skirt == 2) {
                    typeSpecificDetails.Add ("skirtType", "WithSkirt");
                } else {
                    typeSpecificDetails.Add ("skirtType", "SolidBodyWithSkirt");
                }
                typeSpecificDetails.Add ("skirtLevel", elem.mesh.skirtLevel);
                if (elem.mesh.smoothRidges == APIRidge_AllSharp) {
                    typeSpecificDetails.Add ("ridges", GS::UniString ("AllSharp"));
                } else if (elem.mesh.smoothRidges == APIRidge_AllSmooth) {
                    typeSpecificDetails.Add ("ridges", GS::UniString ("AllSmooth"));
                } else {
                    typeSpecificDetails.Add ("ridges", GS::UniString ("UserDefined"));
                }
                typeSpecificDetails.Add ("showLines",    elem.mesh.showLines != 0);
                typeSpecificDetails.Add ("contourPen",   (Int32)elem.mesh.contPen);
                typeSpecificDetails.Add ("levelPen",     (Int32)elem.mesh.levelPen);
                typeSpecificDetails.Add ("lineTypeIndex", GetAttributeIndex (elem.mesh.ltypeInd));
                constexpr bool includeZCoords = true;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    AddPolygonWithHolesFromMemoCoords (elem.header.guid, typeSpecificDetails, "polygonCoordinates", "polygonArcs", "holes", "polygonCoordinates", "polygonArcs", includeZCoords);
                }
                if (elem.mesh.levelLines.nSubLines > 0) {
                    API_ElementMemo memo = {};
                    const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
                    GSErrCode meshMemoErr;
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                        meshMemoErr = ACAPI_Element_GetMemo (elem.header.guid, &memo, APIMemoMask_MeshLevel);
                    }
                    if (meshMemoErr == NoError && memo.meshLevelCoords != nullptr && *memo.meshLevelCoords != nullptr &&
                        memo.meshLevelEnds != nullptr && *memo.meshLevelEnds != nullptr) {
                        constexpr GSSize maxMeshLevelCoords = 1'000'000;
                        constexpr GSSize maxMeshLevelSublines = 100'000;
                        const GSSize endsBytes = BMhGetSize (reinterpret_cast<GSHandle> (memo.meshLevelEnds));
                        const GSSize coordsBytes = BMhGetSize (reinterpret_cast<GSHandle> (memo.meshLevelCoords));
                        const Int32 requestedSublines = elem.mesh.levelLines.nSubLines;
                        if (endsBytes % sizeof (Int32) != 0 || coordsBytes % sizeof (API_MeshLevelCoord) != 0 ||
                            requestedSublines <= 0 ||
                            static_cast<GSSize> (requestedSublines) > maxMeshLevelSublines ||
                            coordsBytes / sizeof (API_MeshLevelCoord) > maxMeshLevelCoords ||
                            endsBytes / sizeof (Int32) < static_cast<GSSize> (requestedSublines)) {
                            typeSpecificDetails.Add ("error", "Mesh level memo array sizes are invalid.");
                        } else {
                            const auto& sublines = typeSpecificDetails.AddList<GS::ObjectState> ("sublines");
                            const GSSize nSublines = static_cast<GSSize> (requestedSublines);
                            const GSSize nLevelCoords = coordsBytes / sizeof (API_MeshLevelCoord);
                            Int32 iCoord = 0;
                            bool invalidMeshCoordinate = false;
                            for (GSSize i = 0; i < nSublines; ++i) {
                                GS::ObjectState subline;
                                const auto& coordinates = subline.AddList<GS::ObjectState> ("coordinates");

                                const Int32 nCoords = (*memo.meshLevelEnds)[i];
                                if (nCoords < iCoord || static_cast<GSSize> (nCoords) > nLevelCoords) {
                                    typeSpecificDetails.Add ("error", "Mesh level memo contains an invalid subline endpoint.");
                                    break;
                                }
                                for (; iCoord < nCoords; ++iCoord) {
                                    const API_MeshLevelCoord& coord = (*memo.meshLevelCoords)[iCoord];
                                    if (!std::isfinite (coord.c.x) || !std::isfinite (coord.c.y) || !std::isfinite (coord.c.z)) {
                                        typeSpecificDetails.Add ("error", "Mesh level memo contains a non-finite coordinate.");
                                        invalidMeshCoordinate = true;
                                        break;
                                    }
                                    coordinates (Create3DCoordinateObjectState (coord.c));
                                }
                                if (invalidMeshCoordinate)
                                    break;
                                sublines (subline);
                            }
                        }
                    }
                }
            } break;

            case API_DrawingID: {
                // `drawingGuid` is the Navigator item ID of the drawing's
                // source. Keep this native relation so the MCP can resolve a
                // placed drawing through Navigator instead of guessing from
                // its rendered title.
                typeSpecificDetails.Add ("sourceNavigatorItemId", CreateGuidObjectState (elem.drawing.drawingGuid));
                typeSpecificDetails.Add ("pos",           Create2DCoordinateObjectState (elem.drawing.pos));
                typeSpecificDetails.Add ("angle",         elem.drawing.angle);
                typeSpecificDetails.Add ("ratio",         elem.drawing.ratio);
                typeSpecificDetails.Add ("drawingScale",  elem.drawing.drawingScale);
                typeSpecificDetails.Add ("modelOffset",   Create2DCoordinateObjectState (elem.drawing.modelOffset));
                typeSpecificDetails.Add ("isCutWithFrame", elem.drawing.isCutWithFrame);
                if (IsFiniteBounds (elem.drawing.bounds)) {
                    typeSpecificDetails.Add ("bounds", GS::ObjectState (
                        "xMin", elem.drawing.bounds.xMin,
                        "yMin", elem.drawing.bounds.yMin,
                        "xMax", elem.drawing.bounds.xMax,
                        "yMax", elem.drawing.bounds.yMax));
                }
                if (elem.drawing.isCutWithFrame) {
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                        AddPolygonFromMemoCoords (elem.header.guid, typeSpecificDetails, "clipPolygon");
                    }
                }
            } break;

            case API_LineID: {
                typeSpecificDetails.Add ("begCoordinate", Create2DCoordinateObjectState (elem.line.begC));
                typeSpecificDetails.Add ("endCoordinate", Create2DCoordinateObjectState (elem.line.endC));
                if (!draftingProfile) {
                    Int32 lineTypeIndex;
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::AttrLookup);
                        lineTypeIndex = GetAttributeIndex (elem.line.ltypeInd);
                    }
                    typeSpecificDetails.Add ("lineTypeIndex", lineTypeIndex);
                }
                typeSpecificDetails.Add ("linePen", static_cast<Int32> (elem.line.linePen.penIndex));
                if (!draftingProfile)
                    typeSpecificDetails.Add ("roomSeparator", elem.line.roomSeparator);
                typeSpecificDetails.Add ("arrowData", CreateArrowDataObjectState (elem.line.arrowData));
            } break;

            case API_ArcID:
            case API_CircleID: {
                if (!draftingProfile) {
                    typeSpecificDetails.Add ("origin", Create2DCoordinateObjectState (elem.arc.origC));
                    typeSpecificDetails.Add ("radius", elem.arc.r);
                    typeSpecificDetails.Add ("axisAngle", elem.arc.angle);
                    typeSpecificDetails.Add ("ratio", elem.arc.ratio);
                    typeSpecificDetails.Add ("beginAngle", elem.arc.begAng);
                    typeSpecificDetails.Add ("endAngle", elem.arc.endAng);
                    typeSpecificDetails.Add ("reflected", elem.arc.reflected);
                }
                typeSpecificDetails.Add ("whole", elem.arc.whole);
                if (!draftingProfile) {
                    Int32 arcLineTypeIndex;
                    {
                        PerfTrace::ScopeTimer timer (PerfTrace::Phase::AttrLookup);
                        arcLineTypeIndex = GetAttributeIndex (elem.arc.ltypeInd);
                    }
                    typeSpecificDetails.Add ("lineTypeIndex", arcLineTypeIndex);
                }
                typeSpecificDetails.Add ("linePen", static_cast<Int32> (elem.arc.linePen.penIndex));
                if (!draftingProfile)
                    typeSpecificDetails.Add ("roomSeparator", elem.arc.roomSeparator);
                typeSpecificDetails.Add ("arrowData", CreateArrowDataObjectState (elem.arc.arrowData));
            } break;

            case API_TextID: {
                typeSpecificDetails.Add ("angle", elem.text.angle);
                typeSpecificDetails.Add ("size", elem.text.size);
                typeSpecificDetails.Add ("fontIndex", static_cast<Int32> (elem.text.font));
                typeSpecificDetails.Add ("pen", static_cast<Int32> (elem.text.pen));
                if (!draftingProfile) {
                    typeSpecificDetails.Add ("anchorCoordinate", Create2DCoordinateObjectState (elem.text.loc));
                    typeSpecificDetails.Add ("width", elem.text.width);
                    typeSpecificDetails.Add ("height", elem.text.height);
                    typeSpecificDetails.Add ("anchor", static_cast<Int32> (elem.text.anchor));
                    typeSpecificDetails.Add ("justification", static_cast<Int32> (elem.text.just));
                    typeSpecificDetails.Add ("fixedAngle", elem.text.fixedAngle);
                    typeSpecificDetails.Add ("fixedSize", elem.text.fixedSize);
                    typeSpecificDetails.Add ("lineCount", elem.text.nLine);
                }

                API_ElementMemo memo = {};
                const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
                GSErrCode textMemoErr;
                {
                    PerfTrace::ScopeTimer timer (PerfTrace::Phase::TypeMemo);
                    // TextContent is the ANSI/UTF-8 memo. Request the Unicode
                    // variant because the AC28 API exposes textContent as a
                    // GS::UniString*; reading the ANSI buffer as that object
                    // turns UTF-8 bytes into CJK-like UTF-16 mojibake.
                    textMemoErr = ACAPI_Element_GetMemo (elem.header.guid, &memo, APIMemoMask_TextContentUni);
                }
                if (!AddBoundedTextContent (typeSpecificDetails, memo, textMemoErr))
                    typeSpecificDetails.Add ("contentStatus", "unavailable");
            } break;

            default:
                typeSpecificDetails.Add ("error", "Not yet supported element type");
                break;
        }

        detailsOfElement.Add ("details", typeSpecificDetails);
        {
            PerfTrace::ScopeTimer timer (PerfTrace::Phase::ShapePrims);
            if (!IsPureDraftingType (typeID))
                AddFloorPlanPolygonsIfAvailable (elem.header.guid, detailsOfElement);
        }

        detailsOfElements (detailsOfElement);
    }

    PerfTrace::SetRequestedReturned (static_cast<int> (elements.GetSize ()), static_cast<int> (elements.GetSize ()));
    const double totalMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - perfTraceStart).count ();
    PerfTrace::WriteLine (GS::UniString (GetName ()), PerfTrace::NextSeq (), totalMs);

    return response;
}

SetDetailsOfElementsCommand::SetDetailsOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String SetDetailsOfElementsCommand::GetName () const
{
    return "SetDetailsOfElements";
}

GS::Optional<GS::UniString> SetDetailsOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementsWithDetails": {
                "type": "array",
                "description": "The elements with parameters.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId"
                        },
                        "details": {
                            "type": "object",
                            "description": "Details of an element.",
                            "properties": {
                                "floorIndex": {
                                    "type": "number"
                                },
                                "layerIndex": {
                                    "type": "number"
                                },
                                "drawIndex": {
                                    "type": "number"
                                },
                                "typeSpecificDetails": {
                                    "$ref": "#/TypeSpecificSettings"
                                }
                            },
                            "additionalProperties": false,
                            "required": []
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId",
                        "details"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "elementsWithDetails"
        ]
    })";
}

GS::Optional<GS::UniString> SetDetailsOfElementsCommand::GetResponseSchema () const
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

// ============================================================
// drawIndex repositioning — implementation notes
// ============================================================
//
// WHY NOT ACAPI_Element_Change WITH drwIndex MASK
// ------------------------------------------------
// ACAPI_Element_Change silently ignores the drwIndex field even when the
// mask bit is set (confirmed Graphisoft bug, AC27). The only working API
// is ACAPI_Grouping_Tool with the following commands:
//   APITool_BringForward  (+1, reliable)
//   APITool_SendBackward  (-1, reliable)
//   APITool_ResetOrder    (→ class default, unreliable for some types — see below)
//
// DEAD ENDS (do not reintroduce)
// --------------------------------
//   APITool_BringToFront : unreliable on Wall and its sub-elements; causes
//                          unexpected level jumps. Removed from all strategies.
//   APITool_SendToBack   : does NOT reliably reach level 1. In sparse files
//                          it stops at 2 or 3. Removed from all strategies.
//
// STRATEGIES FOR NORMAL ELEMENTS
// --------------------------------
// Target clamped to [1, 14]. Two strategies, cheapest wins:
//   Direct  : |target - current| × BringForward or SendBackward
//   ViaReset: 1 ResetOrder + |target - classDefault| steps
//             (disabled for Morph, Railing, PolyLine — ResetOrder is a no-op
//              for these types; a simulated default from GetDefaultDrwIndex is
//              used instead if they need to reach their default level)
//
// CLASS DEFAULTS (per ArchiCAD documentation, levels 1-4 and 11-14 are void)
//   5  : Drawing
//   6  : Hatch, Zone
//   7  : Wall, Slab, Roof, Shell, Morph, Mesh, Column, CurtainWall, Stair,
//         Railing, Door, Window, Skylight, Opening
//   8  : Lamp, Object
//   9  : Line, Arc, Spline, PolyLine, Beam, Hotspot, ChangeMarker, Detail,
//         Worksheet
//   10 : Dimension (all types), Label, Text, CutPlane, Elevation,
//         InteriorElevation
//
// OPENINGS (Door, Window, Skylight) — special case
// --------------------------------------------------
// ArchiCAD enforces a hard minimum of 7 for elements embedded in a host
// (wall or roof/shell). Levels 1-6 are unreachable regardless of strategy.
//
// Host types by opening type:
//   Door   → Wall
//   Window → Wall
//   Skylight → Roof or Shell
//
// Levels 8-14 require indirect host manipulation:
//   1. Step host UP  (target - hostOriginal) × BringForward  → opening follows
//   2. Step host DOWN the same number of times × SendBackward → host returns,
//      opening stays elevated (the two are decoupled once host passes the
//      opening's natural level)
//
// Reset (drawIndex = 0) on an elevated opening is a no-op via direct
// ResetOrder. It must be applied to the HOST instead, which resets all its
// openings to level 7.
//
// Reverse host lookup (opening → host) uses the owner guid stored directly on
// the opening element (API_WindowType::owner for door/window, API_SkylightType::owner
// for skylight) via ACAPI_Element_Get — no need to scan hosts.
//
// BATCH ORDERING CONSTRAINT
// --------------------------
// Openings are processed before all other elements within the same batch.
// This ensures the host manipulation reads the host's pre-batch position, not
// a position already shifted by another element in the same call.
//
// KNOWN LIMITATIONS
// -----------------
// • Door/Window/Skylight: levels 1-6 are unreachable (ArchiCAD constraint).
// • An opening cannot be below its host's current level. If the host wall/roof
//   is at level 9, the opening cannot reach levels 7 or 8.
// • Multiple openings in the same host wall/roof with different targets in a
//   single batch: all will end up at the same level (the last one processed),
//   because moving the host affects all its openings simultaneously.
// • Opening (API_OpeningID, generic void): completely immovable. All
//   Grouping_Tool calls are no-ops. ArchiCAD displays level 0 for these
//   elements (outside the 1-14 ordering system); the API may report 7
//   (class default) but the actual display order cannot be changed. Likely
//   due to multi-host nature (can span Wall, Slab, Roof, Shell, Beam, Column).
// • Morph, Railing, PolyLine: ResetOrder is a no-op; reset (drawIndex = 0)
//   is simulated by stepping from current position to the class default.
// ============================================================

static bool IsOpeningType (const API_Elem_Head& head)
{
    switch (GetElemTypeId (head)) {
        case API_DoorID:
        case API_WindowID:
        case API_SkylightID: return true;
        default:             return false;
    }
}

static bool IsResetOrderReliable (const API_Elem_Head& head)
{
    switch (GetElemTypeId (head)) {
        case API_MorphID:
        case API_RailingID:
        case API_PolyLineID: return false;
        default:             return true;
    }
}

static Int32 GetDefaultDrwIndex (const API_Elem_Head& head)
{
    switch (GetElemTypeId (head)) {
        case API_DrawingID:                                                      return 5;
        case API_HatchID:     case API_ZoneID:                                   return 6;
        case API_WallID:      case API_SlabID:      case API_RoofID:
        case API_ShellID:     case API_MorphID:     case API_MeshID:
        case API_ColumnID:    case API_CurtainWallID: case API_StairID:
        case API_RailingID:   case API_DoorID:      case API_WindowID:
        case API_SkylightID:  case API_OpeningID:                                return 7;
        case API_LampID:      case API_ObjectID:                                 return 8;
        case API_LineID:      case API_ArcID:       case API_SplineID:
        case API_PolyLineID:  case API_BeamID:      case API_HotspotID:
        case API_ChangeMarkerID: case API_DetailID: case API_WorksheetID:        return 9;
        case API_DimensionID: case API_RadialDimensionID:
        case API_LevelDimensionID: case API_AngleDimensionID:
        case API_LabelID:     case API_TextID:      case API_CutPlaneID:
        case API_ElevationID: case API_InteriorElevationID:                      return 10;
        default:                                                                  return 7;
    }
}

static void ApplyDrawIndexStrategy (GS::Array<API_Guid>& guids, const API_Elem_Head& head, Int32 target, Int32 current)
{
    target = GS::Max (1, GS::Min (target, 14));
    const Int32 defaultLevel = GetDefaultDrwIndex (head);
    const Int32 directCost   = GS::Abs (target - current);
    const Int32 viaResetCost = IsResetOrderReliable (head)
                             ? 1 + GS::Abs (target - defaultLevel)
                             : INT_MAX;

    if (viaResetCost < directCost) {
        ACAPI_Grouping_Tool (guids, APITool_ResetOrder, nullptr);
        const API_ToolCmdID step = (target > defaultLevel) ? APITool_BringForward : APITool_SendBackward;
        for (Int32 i = 0; i < GS::Abs (target - defaultLevel); ++i)
            ACAPI_Grouping_Tool (guids, step, nullptr);
    } else {
        const API_ToolCmdID step = (target > current) ? APITool_BringForward : APITool_SendBackward;
        for (Int32 i = 0; i < directCost; ++i)
            ACAPI_Grouping_Tool (guids, step, nullptr);
    }
}

static GS::HashTable<API_Guid, API_Guid> BuildOpeningToHostMap (const GS::HashSet<API_Guid>& targetGuids)
{
    GS::HashTable<API_Guid, API_Guid> result;
    for (const API_Guid& guid : targetGuids) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get (&elem) != NoError)
            continue;

        switch (GetElemTypeId (elem.header)) {
            case API_DoorID:
            case API_WindowID:   result.Add (guid, elem.window.owner);   break;
            case API_SkylightID: result.Add (guid, elem.skylight.owner); break;
            default: break;
        }
    }
    return result;
}

GS::ObjectState SetDetailsOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsWithDetails;
    parameters.Get ("elementsWithDetails", elementsWithDetails);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    // Pre-scan: identify openings (Door/Window/Skylight) that need host manipulation:
    //   - target > 7  : elevate via host (step up, step back)
    //   - target == 0 : reset via host ResetOrder (direct ResetOrder on opening doesn't work when elevated)
    GS::HashSet<API_Guid> openingsNeedingHost;
    for (const GS::ObjectState& ewd : elementsWithDetails) {
        const GS::ObjectState* eid = ewd.Get ("elementId");
        const GS::ObjectState* det = ewd.Get ("details");
        if (eid == nullptr || det == nullptr) continue;
        short tgt = -1;
        det->Get ("drawIndex", tgt);
        if (tgt > 0 && tgt <= 7) continue;   // 1-7: no host needed
        API_Element hdr = {};
        hdr.header.guid = GetGuidFromObjectState (*eid);
        if (ACAPI_Element_GetHeader (&hdr.header) != NoError) continue;
        if (IsOpeningType (hdr.header))
            openingsNeedingHost.Add (hdr.header.guid);
    }
    const GS::HashTable<API_Guid, API_Guid> openingToHost = BuildOpeningToHostMap (openingsNeedingHost);

    // Process openings first so host manipulation starts from the host's natural position,
    // not a position already shifted by another element in the same batch.
    GS::Array<GS::ObjectState> sortedElements;
    for (const GS::ObjectState& ewd : elementsWithDetails) {
        const GS::ObjectState* eid = ewd.Get ("elementId");
        if (eid != nullptr && openingsNeedingHost.Contains (GetGuidFromObjectState (*eid)))
            sortedElements.Push (ewd);
    }
    for (const GS::ObjectState& ewd : elementsWithDetails) {
        const GS::ObjectState* eid = ewd.Get ("elementId");
        if (eid == nullptr || !openingsNeedingHost.Contains (GetGuidFromObjectState (*eid)))
            sortedElements.Push (ewd);
    }

    ACAPI_CallUndoableCommand ("SetDetailsOfElementsCommand", [&]() {
        for (const GS::ObjectState& elementWithDetails : sortedElements) {
            const GS::ObjectState* elementId = elementWithDetails.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            API_Element elem = {};
            elem.header.guid = GetGuidFromObjectState (*elementId);
            GSErrCode err = ACAPI_Element_Get (&elem);

            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (err, "Failed to find the element"));
                continue;
            }

            const GS::ObjectState* details = elementWithDetails.Get ("details");
            if (details == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "details field is missing"));
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool hasElementChanges = false;
            if (details->Get ("floorIndex", elem.header.floorInd)) {
                ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
                hasElementChanges = true;
            }
            Int32 layerIndex;
            if (details->Get ("layerIndex", layerIndex)) {
                elem.header.layer = ACAPI_CreateAttributeIndex (layerIndex);
                ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
                hasElementChanges = true;
            }
            short drwIndexTarget = -1;
            details->Get ("drawIndex", drwIndexTarget);

            const GS::ObjectState* typeSpecificDetails = details->Get ("typeSpecificDetails");
            if (typeSpecificDetails != nullptr) {
                switch (GetElemTypeId (elem.header)) {
                    case API_WallID: {
                        const GS::ObjectState* begCoordinate = typeSpecificDetails->Get ("begCoordinate");
                        if (begCoordinate != nullptr) {
                            elem.wall.begC = Get2DCoordinateFromObjectState (*begCoordinate);
                            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, begC);
                        }
                        const GS::ObjectState* endCoordinate = typeSpecificDetails->Get ("endCoordinate");
                        if (endCoordinate != nullptr) {
                            elem.wall.endC = Get2DCoordinateFromObjectState (*endCoordinate);
                            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, endC);
                        }
                        if (typeSpecificDetails->Get ("height", elem.wall.height)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, height);
                        }
                        if (typeSpecificDetails->Get ("offset", elem.wall.offset)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_WallType, offset);
                        }

                        switch (elem.wall.type) {
                            case APIWtyp_Trapez: {
                                if (typeSpecificDetails->Get ("begThickness", elem.wall.thickness)) {
                                    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness);
                                }
                                if (typeSpecificDetails->Get ("endThickness", elem.wall.thickness1)) {
                                    ACAPI_ELEMENT_MASK_SET (mask, API_WallType, thickness1);
                                }
                            } break;
                            default:
                            break;
                        }
                    } break;
                    case API_ZoneID: {
                        const GS::ObjectState* stampPosition = typeSpecificDetails->Get ("stampPosition");
                        if (stampPosition != nullptr) {
                            elem.zone.pos = Get2DCoordinateFromObjectState (*stampPosition);
                            ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, pos);
                        }
                        if (typeSpecificDetails->Get ("stampAngle", elem.zone.stampAngle)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, stampAngle);
                        }
                        if (typeSpecificDetails->Get ("fixedStampAngle", elem.zone.fixedAngle)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, fixedAngle);
                        }
                    } break;
                    case API_DrawingID: {
                        GS::Array<GS::ObjectState> clipCoords;
                        if (typeSpecificDetails->Get ("clipPolygon", clipCoords) && clipCoords.GetSize () >= 3) {
                            Int32 nUnique = (Int32) clipCoords.GetSize ();
                            if (nUnique > 1) {
                                API_Coord first = Get2DCoordinateFromObjectState (clipCoords.GetFirst ());
                                API_Coord last  = Get2DCoordinateFromObjectState (clipCoords.GetLast ());
                                if (first.x == last.x && first.y == last.y)
                                    --nUnique;
                            }
                            elem.drawing.isCutWithFrame    = true;
                            elem.drawing.poly.nSubPolys    = 1;
                            elem.drawing.poly.nCoords      = nUnique + 1; // +1 for closing vertex
                            elem.drawing.poly.nArcs        = 0;
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, isCutWithFrame);
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, poly);
                        }
                        if (typeSpecificDetails->Get ("drawingScale", elem.drawing.drawingScale)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, drawingScale);
                        }
                        if (typeSpecificDetails->Get ("ratio", elem.drawing.ratio)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, ratio);
                        }
                        if (typeSpecificDetails->Get ("angle", elem.drawing.angle)) {
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, angle);
                        }
                        const GS::ObjectState* posState = typeSpecificDetails->Get ("pos");
                        if (posState != nullptr) {
                            elem.drawing.pos = Get2DCoordinateFromObjectState (*posState);
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, pos);
                        }
                        const GS::ObjectState* modelOffsetState = typeSpecificDetails->Get ("modelOffset");
                        if (modelOffsetState != nullptr) {
                            elem.drawing.modelOffset = Get2DCoordinateFromObjectState (*modelOffsetState);
                            ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, modelOffset);
                        }
                    } break;
                    default:
                    break;
                }
                hasElementChanges = true;
            }

            API_ElementMemo clipMemo = {};
            const GS::OnExit clipMemoGuard ([&clipMemo] () { ACAPI_DisposeElemMemoHdls (&clipMemo); });
            UInt64 memoMask = 0;
            bool hasMemoChanges = false;

            if (typeSpecificDetails != nullptr && GetElemTypeId (elem.header) == API_DrawingID) {
                GS::Array<GS::ObjectState> clipCoords;
                if (typeSpecificDetails->Get ("clipPolygon", clipCoords) && clipCoords.GetSize () >= 3) {
                    // Drop explicit closing vertex if caller already duplicated first point at the end
                    if (clipCoords.GetSize () > 1) {
                        API_Coord first = Get2DCoordinateFromObjectState (clipCoords.GetFirst ());
                        API_Coord last  = Get2DCoordinateFromObjectState (clipCoords.GetLast ());
                        if (first.x == last.x && first.y == last.y)
                            clipCoords.Pop ();
                    }
                    if (static_cast<GSSize> (clipCoords.GetSize ()) > static_cast<GSSize> (std::numeric_limits<Int32>::max () - 1)) {
                        executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "Drawing clip polygon is too large."));
                        continue;
                    }
                    const Int32 nUnique = static_cast<Int32> (clipCoords.GetSize ());
                    // AC polygon convention: coords[1..nUnique] = vertices, coords[nUnique+1] = closing duplicate of coords[1]
                    // Handle size = nUnique+2 (index 0 unused, 1..nUnique vertices, nUnique+1 closing)
                    const GSSize coordCount = static_cast<GSSize> (nUnique) + 2;
                    if (coordCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord)) {
                        executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "Drawing clip polygon is too large."));
                        continue;
                    }
                    clipMemo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle (coordCount * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
                    clipMemo.pends  = reinterpret_cast<Int32**>     (BMAllocateHandle (static_cast<GSSize> (2) * sizeof (Int32), ALLOCATE_CLEAR, 0));
                    if (clipMemo.coords != nullptr && *clipMemo.coords != nullptr &&
                        clipMemo.pends != nullptr && *clipMemo.pends != nullptr) {
                        for (Int32 i = 0; i < nUnique; ++i)
                            (*clipMemo.coords)[i + 1] = Get2DCoordinateFromObjectState (clipCoords[i]);
                        (*clipMemo.coords)[nUnique + 1] = (*clipMemo.coords)[1]; // closing vertex
                        (*clipMemo.pends)[1] = nUnique + 1;
                        memoMask = APIMemoMask_Polygon;
                        hasMemoChanges = true;
                    } else {
                        executionResults (CreateFailedExecutionResult (APIERR_MEMFULL, "Failed to allocate drawing clip polygon memo data."));
                        continue;
                    }
                }
            }

            constexpr bool withDel = true;
            if (hasElementChanges || hasMemoChanges) {
                err = ACAPI_Element_Change (&elem, &mask,
                                            hasMemoChanges ? &clipMemo : nullptr,
                                            memoMask, withDel);
                if (err != NoError) {
                    executionResults (CreateFailedExecutionResult (err, "Failed to change element"));
                    continue;
                }
            }

            if (drwIndexTarget >= 0) {
                GS::Array<API_Guid> guids = { elem.header.guid };

                if (IsOpeningType (elem.header) && drwIndexTarget > 7) {
                    // Openings (Door/Window/Skylight) can only reach levels above 7 by
                    // temporarily elevating their host wall/roof step by step, then
                    // bringing it back the same way (BringToFront is unreliable on walls).
                    const API_Guid* hostGuidPtr = openingToHost.GetPtr (elem.header.guid);
                    if (hostGuidPtr != nullptr) {
                        API_Element hostElem = {};
                        hostElem.header.guid = *hostGuidPtr;
                        if (ACAPI_Element_Get (&hostElem) == NoError) {
                            const Int32 hostOriginal = (Int32) hostElem.header.drwIndex;
                            const Int32 target       = GS::Max (8, GS::Min ((Int32) drwIndexTarget, 14));
                            const Int32 steps        = target - hostOriginal;
                            GS::Array<API_Guid> hostGuids = { *hostGuidPtr };
                            if (steps > 0) {
                                // Step host up — opening follows
                                for (Int32 i = 0; i < steps; ++i)
                                    ACAPI_Grouping_Tool (hostGuids, APITool_BringForward, nullptr);
                                // Step host back down — opening stays elevated
                                for (Int32 i = 0; i < steps; ++i)
                                    ACAPI_Grouping_Tool (hostGuids, APITool_SendBackward, nullptr);
                            }
                        }
                    }
                } else if (drwIndexTarget == 0 && IsOpeningType (elem.header)) {
                    // Reset elevated opening via its host (direct ResetOrder on opening doesn't work)
                    const API_Guid* hostGuidPtr = openingToHost.GetPtr (elem.header.guid);
                    if (hostGuidPtr != nullptr) {
                        GS::Array<API_Guid> hostGuids = { *hostGuidPtr };
                        ACAPI_Grouping_Tool (hostGuids, APITool_ResetOrder, nullptr);
                    }
                } else if (drwIndexTarget == 0 && IsResetOrderReliable (elem.header)) {
                    ACAPI_Grouping_Tool (guids, APITool_ResetOrder, nullptr);
                } else if (drwIndexTarget != 0 || !IsResetOrderReliable (elem.header)) {
                    const short resolvedTarget = (drwIndexTarget == 0) ? (short) GetDefaultDrwIndex (elem.header) : drwIndexTarget;
                    ApplyDrawIndexStrategy (guids, elem.header, (Int32) resolvedTarget, (Int32) elem.header.drwIndex);
                }
            }

            executionResults (CreateSuccessfulExecutionResult ());
        }

        return NoError;
    });

    return response;
}

GetSelectedElementsCommand::GetSelectedElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetSelectedElementsCommand::GetName () const
{
    return "GetSelectedElements";
}

GS::Optional<GS::UniString> GetSelectedElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": []
    })";
}

GS::Optional<GS::UniString> GetSelectedElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "focusElements": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "elementType": { "type": "string" },
                        "bounds": {
                            "type": "object",
                            "properties": {
                                "xMin": { "type": "number" }, "yMin": { "type": "number" },
                                "xMax": { "type": "number" }, "yMax": { "type": "number" }
                            },
                            "additionalProperties": false,
                            "required": ["xMin", "yMin", "xMax", "yMax"]
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId", "elementType"]
                }
            },
            "selectionType": {
                "type": "string",
                "enum": ["empty", "elements", "marquee_polygon", "marquee_horizontal_box", "marquee_rotated_box"]
            },
            "elementRelationship": {
                "type": "string",
                "enum": ["none", "individually_selected", "inside_or_intersecting_marquee"]
            },
            "selectedElementCount": { "type": "integer" },
            "editableElementCount": { "type": "integer" },
            "marquee": {
                "type": "object",
                "properties": {
                    "shape": { "type": "string" },
                    "source": { "type": "string", "enum": ["live_query", "event_tracker"] },
                    "multiStory": { "type": "boolean" },
                    "rotation": { "type": "number" },
                    "bounds": {
                        "type": "object",
                        "properties": {
                            "xMin": { "type": "number" }, "yMin": { "type": "number" },
                            "xMax": { "type": "number" }, "yMax": { "type": "number" }
                        },
                        "additionalProperties": false,
                        "required": ["xMin", "yMin", "xMax", "yMax"]
                    },
                    "points": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": { "x": { "type": "number" }, "y": { "type": "number" } },
                            "additionalProperties": false,
                            "required": ["x", "y"]
                        }
                    }
                },
                "additionalProperties": false,
                "required": ["shape", "source", "multiStory", "rotation", "bounds", "points"]
            },
            "error": { "$ref": "#/Error" },
            "pageRequired": { "type": "boolean", "enum": [true] },
            "resourceLimit": { "type": "boolean", "enum": [true] },
            "code": { "type": "string" },
            "message": { "type": "string" },
            "enumerated": { "type": "integer" },
            "emitted": { "type": "integer" }
        },
        "additionalProperties": false
    })";
}

static GS::UniString SelectionTypeToString (API_SelTypeID type)
{
    switch (type) {
        case API_SelElems:       return "elements";
        case API_MarqueePoly:    return "marquee_polygon";
        case API_MarqueeHorBox:  return "marquee_horizontal_box";
        case API_MarqueeRotBox:  return "marquee_rotated_box";
        default:                 return "empty";
    }
}

static bool IsMarqueeSelection (API_SelTypeID type)
{
    return type == API_MarqueePoly || type == API_MarqueeHorBox || type == API_MarqueeRotBox;
}

static bool IsSameHorizontalMarquee (const API_SelectionInfo& selection, double xMin, double yMin, double xMax, double yMax, bool multiStory)
{
    return selection.typeID == API_MarqueeHorBox &&
        selection.marquee.box.xMin == xMin && selection.marquee.box.yMin == yMin &&
        selection.marquee.box.xMax == xMax && selection.marquee.box.yMax == yMax &&
        selection.multiStory == multiStory;
}

static void ReleaseSelectionInfo (API_SelectionInfo& selection)
{
    ReleaseSelectionInfoHandles (selection);
}

// ACAPI_Selection_Get returns an owned coordinate handle.  A shallow copy of
// API_SelectionInfo therefore aliases the handle and is unsafe when the
// source and copy are both disposed.  Keep restoration state in an explicit
// deep copy instead.
static bool CloneSelectionInfo (const API_SelectionInfo& source, API_SelectionInfo& destination)
{
    return CloneSelectionInfoHandles (source, destination);
}

static bool AddMarqueeToResponse (GS::ObjectState& response, const API_SelectionInfo& selectionInfo, const GS::UniString& source)
{
    constexpr GSSize maxResponseMarqueePoints = 200'000;
    if (!std::isfinite (selectionInfo.marquee.box.xMin) ||
        !std::isfinite (selectionInfo.marquee.box.yMin) ||
        !std::isfinite (selectionInfo.marquee.box.xMax) ||
        !std::isfinite (selectionInfo.marquee.box.yMax) ||
        !std::isfinite (selectionInfo.marquee.boxRotAngle)) {
        return false;
    }

    GS::ObjectState marquee;
    marquee.Add ("shape", SelectionTypeToString (selectionInfo.typeID));
    marquee.Add ("source", source);
    marquee.Add ("multiStory", selectionInfo.multiStory);
    marquee.Add ("rotation", selectionInfo.marquee.boxRotAngle);
    const auto& points = marquee.AddList<GS::ObjectState> ("points");

    double xMin = selectionInfo.marquee.box.xMin;
    double yMin = selectionInfo.marquee.box.yMin;
    double xMax = selectionInfo.marquee.box.xMax;
    double yMax = selectionInfo.marquee.box.yMax;
    const GSSize marqueeCoordBytes = selectionInfo.marquee.coords != nullptr && *selectionInfo.marquee.coords != nullptr
        ? BMhGetSize (reinterpret_cast<GSHandle> (selectionInfo.marquee.coords))
        : 0;
    const GSSize marqueeCoordCapacity = marqueeCoordBytes / sizeof (API_Coord);
    const bool hasValidPolygonCoords = selectionInfo.typeID == API_MarqueePoly
        && selectionInfo.marquee.coords != nullptr
        && *selectionInfo.marquee.coords != nullptr
        && selectionInfo.marquee.nCoords >= 1
        && static_cast<GSSize> (selectionInfo.marquee.nCoords) <= maxResponseMarqueePoints
        && static_cast<GSSize> (selectionInfo.marquee.nCoords) + 1 <= marqueeCoordCapacity;
    if (hasValidPolygonCoords) {
        xMin = DBL_MAX; yMin = DBL_MAX;
        xMax = -DBL_MAX; yMax = -DBL_MAX;
        for (Int32 index = 1; index <= selectionInfo.marquee.nCoords; ++index) {
            const API_Coord& coord = (*selectionInfo.marquee.coords)[index];
            if (!std::isfinite (coord.x) || !std::isfinite (coord.y))
                return false;
            points (GS::ObjectState ("x", coord.x, "y", coord.y));
            xMin = std::min (xMin, coord.x); yMin = std::min (yMin, coord.y);
            xMax = std::max (xMax, coord.x); yMax = std::max (yMax, coord.y);
        }
    } else {
        const double centerX = (xMin + xMax) / 2.0;
        const double centerY = (yMin + yMax) / 2.0;
        const double cosine = std::cos (selectionInfo.marquee.boxRotAngle);
        const double sine = std::sin (selectionInfo.marquee.boxRotAngle);
        const API_Coord corners[] = { { xMin, yMin }, { xMax, yMin }, { xMax, yMax }, { xMin, yMax } };
        xMin = DBL_MAX; yMin = DBL_MAX;
        xMax = -DBL_MAX; yMax = -DBL_MAX;
        for (const API_Coord& corner : corners) {
            const double dx = corner.x - centerX;
            const double dy = corner.y - centerY;
            const double x = centerX + dx * cosine - dy * sine;
            const double y = centerY + dx * sine + dy * cosine;
            if (!std::isfinite (x) || !std::isfinite (y))
                return false;
            points (GS::ObjectState ("x", x, "y", y));
            xMin = std::min (xMin, x); yMin = std::min (yMin, y);
            xMax = std::max (xMax, x); yMax = std::max (yMax, y);
        }
    }
    marquee.Add ("bounds", GS::ObjectState ("xMin", xMin, "yMin", yMin, "xMax", xMax, "yMax", yMax));
    response.Add ("marquee", marquee);
    return true;
}

GS::ObjectState GetSelectedElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");

    API_SelectionInfo selectionSummary = {};
    const GSErrCode summaryErr = ACAPI_Selection_Get (&selectionSummary, nullptr, false);
    const GS::OnExit selectionSummaryGuard ([&selectionSummary] () { ReleaseSelectionInfo (selectionSummary); });
    if (summaryErr != NoError && summaryErr != APIERR_NOSEL) {
        return CreateErrorResponse (summaryErr, "Failed to retrieve the active selection summary.");
    }
    if (summaryErr == NoError && selectionSummary.sel_nElem > pageSize) {
        return CreateNativePageRequiredResponse (
            "The active selection requires another page; no partial rows were returned.",
            selectionSummary.sel_nElem,
            0);
    }

    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> selectedNeigs;
    GSErrCode err = ACAPI_Selection_Get (&selectionInfo, &selectedNeigs, false);
    const GS::OnExit selectionInfoGuard ([&selectionInfo] () { ReleaseSelectionInfo (selectionInfo); });
    if (err != NoError && err != APIERR_NOSEL) {
        return CreateErrorResponse (err, "Failed to retrieve selected elements.");
    }
    if (err == NoError && selectedNeigs.GetSize () > static_cast<GS::USize> (pageSize)) {
        const GSSize count = selectedNeigs.GetSize ();
        const Int32 enumerated = count > static_cast<GSSize> (std::numeric_limits<Int32>::max ())
            ? std::numeric_limits<Int32>::max ()
            : static_cast<Int32> (count);
        return CreateNativePageRequiredResponse (
            "The active selection requires another page; no partial rows were returned.",
            enumerated,
            0);
    }

    GS::ObjectState response;
    const auto& elementsList = response.AddList<GS::ObjectState> ("elements");
    const auto& focusElements = response.AddList<GS::ObjectState> ("focusElements");
    const API_SelTypeID selectionType = err == APIERR_NOSEL ? API_SelEmpty : selectionInfo.typeID;
    response.Add ("selectionType", SelectionTypeToString (selectionType));
    response.Add ("elementRelationship",
        selectionType == API_SelElems ? "individually_selected" :
        IsMarqueeSelection (selectionType) ? "inside_or_intersecting_marquee" : "none");
    response.Add ("selectedElementCount", err == APIERR_NOSEL ? 0 : selectionInfo.sel_nElem);
    response.Add ("editableElementCount", err == APIERR_NOSEL ? 0 : selectionInfo.sel_nElemEdit);

    if (IsMarqueeSelection (selectionType)) {
        if (!AddMarqueeToResponse (response, selectionInfo, "live_query"))
            return CreateErrorResponse (APIERR_BADPARS, "The active marquee contains invalid or oversized geometry.");
    }

    if (selectionType == API_SelEmpty) return response;

    for (API_Neig& selectedNeig : selectedNeigs) {
        const API_Guid guid = GetParentElemOfSectElem (selectedNeig.guid);
        elementsList (CreateElementIdObjectState (guid));

        API_Elem_Head elemHead = {};
        elemHead.guid = guid;
        if (ACAPI_Element_GetHeader (&elemHead) != NoError) continue;
        GS::ObjectState focusElement (
            "elementId", CreateGuidObjectState (guid),
            "elementType", GetElementTypeNonLocalizedName (GetElemTypeId (elemHead)));
        API_Box3D bounds = {};
            if (ACAPI_Element_CalcBounds (&elemHead, &bounds) == NoError && IsFiniteBounds (bounds)) {
            focusElement.Add ("bounds", GS::ObjectState (
                "xMin", bounds.xMin, "yMin", bounds.yMin,
                "xMax", bounds.xMax, "yMax", bounds.yMax));
        }
        focusElements (focusElement);
    }

    return response;
}

GetElementsInRectCommand::GetElementsInRectCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetElementsInRectCommand::GetName () const
{
    return "GetElementsInRect";
}

GS::Optional<GS::UniString> GetElementsInRectCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "bounds": {
                "type": "object",
                "properties": {
                    "xMin": { "type": "number" },
                    "yMin": { "type": "number" },
                    "xMax": { "type": "number" },
                    "yMax": { "type": "number" }
                },
                "additionalProperties": false,
                "required": ["xMin", "yMin", "xMax", "yMax"]
            },
            "multiStory": { "type": "boolean", "default": false },
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": ["bounds"]
    })";
}

GS::Optional<GS::UniString> GetElementsInRectCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
                    "elements": { "$ref": "#/Elements" },
                    "focusElements": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "elementId": { "$ref": "#/ElementId" },
                                "elementType": { "type": "string" },
                                "bounds": {
                                    "type": "object",
                                    "properties": {
                                        "xMin": { "type": "number" }, "yMin": { "type": "number" },
                                        "xMax": { "type": "number" }, "yMax": { "type": "number" }
                                    },
                                    "additionalProperties": false,
                                    "required": ["xMin", "yMin", "xMax", "yMax"]
                                }
                            },
                            "additionalProperties": false,
                            "required": ["elementId", "elementType"]
                        }
                    },
                    "elementCount": { "type": "integer" },
                    "focusRestored": { "type": "boolean" },
                    "error": { "$ref": "#/Error" },
                    "pageRequired": { "type": "boolean", "enum": [true] },
                    "resourceLimit": { "type": "boolean", "enum": [true] },
                    "code": { "type": "string" },
                    "message": { "type": "string" },
                    "enumerated": { "type": "integer" },
                    "emitted": { "type": "integer" }
        },
        "additionalProperties": false
    })";
}

GS::ObjectState GetElementsInRectCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    const GS::ObjectState* bounds = parameters.Get ("bounds");
    double xMin = 0.0, yMin = 0.0, xMax = 0.0, yMax = 0.0;
    if (bounds == nullptr || !bounds->Get ("xMin", xMin) || !bounds->Get ("yMin", yMin) ||
        !bounds->Get ("xMax", xMax) || !bounds->Get ("yMax", yMax)) {
        return CreateErrorResponse (APIERR_BADPARS, "bounds.xMin, bounds.yMin, bounds.xMax and bounds.yMax are required.");
    }
    if (!std::isfinite (xMin) || !std::isfinite (yMin) || !std::isfinite (xMax) || !std::isfinite (yMax) ||
        xMin >= xMax || yMin >= yMax) {
        return CreateErrorResponse (APIERR_BADPARS, "Rectangle bounds must have positive area.");
    }

    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");

    bool multiStory = false;
    parameters.Get ("multiStory", multiStory);

    API_SelectionInfo selectionBefore = {};
    const GSErrCode selectionBeforeErr = ACAPI_Selection_Get (&selectionBefore, nullptr, false);
    const GS::OnExit selectionBeforeGuard ([&selectionBefore] () { ReleaseSelectionInfo (selectionBefore); });
    if (selectionBeforeErr != NoError && selectionBeforeErr != APIERR_NOSEL) {
        return CreateErrorResponse (selectionBeforeErr, "Failed to inspect the operator focus before the bounded read.");
    }
    const bool hadOriginalMarquee = selectionBeforeErr == NoError && IsMarqueeSelection (selectionBefore.typeID);
    if (selectionBeforeErr == NoError && selectionBefore.typeID == API_SelElems) {
        return CreateErrorResponse (
            APIERR_REFUSEDCMD,
            "Bounded marquee reads require marquee focus. An individual selection is active; use the selection scope or clear the selection before using a marquee/viewport read.");
    }

    API_SelectionInfo restoreSelection = {};
    if (hadOriginalMarquee && !CloneSelectionInfo (selectionBefore, restoreSelection)) {
        return CreateErrorResponse (APIERR_MEMFULL, "Failed to copy the current marquee safely before the bounded read.");
    }
    const GS::OnExit restoreSelectionGuard ([&restoreSelection] () { ReleaseSelectionInfo (restoreSelection); });

    bool temporaryFocusMayBeSet = false;
    bool focusRestoreAttempted = false;
    GSErrCode focusRestoreErr = NoError;
    const auto restoreOperatorFocus = [&]() -> GSErrCode {
        if (!temporaryFocusMayBeSet || focusRestoreAttempted)
            return focusRestoreErr;

        focusRestoreAttempted = true;
        API_SelectionInfo emptySelection = {};
        emptySelection.typeID = API_SelEmpty;
        try {
            focusRestoreErr = ACAPI_Selection_SetMarquee (hadOriginalMarquee ? &restoreSelection : &emptySelection);
        } catch (...) {
            focusRestoreErr = APIERR_GENERAL;
        }
        return focusRestoreErr;
    };
    // GS::OnExit does not catch exceptions from its callback.  The callback
    // above is therefore internally exception-safe: focus cleanup must never
    // turn an otherwise handled native error into an exception at teardown.
    const GS::OnExit focusGuard ([&restoreOperatorFocus] () { (void) restoreOperatorFocus (); });

    API_SelectionInfo temporaryMarquee = {};
    temporaryMarquee.typeID = API_MarqueeHorBox;
    temporaryMarquee.multiStory = multiStory;
    temporaryMarquee.marquee.box.xMin = xMin;
    temporaryMarquee.marquee.box.yMin = yMin;
    temporaryMarquee.marquee.box.xMax = xMax;
    temporaryMarquee.marquee.box.yMax = yMax;
    temporaryMarquee.marquee.boxRotAngle = 0.0;

    temporaryFocusMayBeSet = true;
    const GSErrCode setErr = ACAPI_Selection_SetMarquee (&temporaryMarquee);
    if (setErr != NoError) {
        const GSErrCode restoreErr = restoreOperatorFocus ();
        return CreateErrorResponse (
            restoreErr != NoError ? restoreErr : setErr,
            restoreErr != NoError
                ? "Failed to set the temporary bounded read rectangle and could not restore the operator focus."
                : "Failed to set the temporary bounded read rectangle.");
    }

    API_SelectionInfo boundedSelection = {};
    GS::Array<API_Neig> selectedNeigs;
    const GSErrCode boundedErr = ACAPI_Selection_Get (&boundedSelection, &selectedNeigs, false);
    const GS::OnExit boundedSelectionGuard ([&boundedSelection] () { ReleaseSelectionInfo (boundedSelection); });

    GS::ObjectState response;
    const auto& elements = response.AddList<GS::ObjectState> ("elements");
    const auto& focusElements = response.AddList<GS::ObjectState> ("focusElements");
    Int32 elementCount = 0;
    const GSSize selectedCount = selectedNeigs.GetSize ();
    const bool pageRequired = boundedErr == NoError && selectedCount > static_cast<GSSize> (pageSize);
    if (boundedErr == NoError && !pageRequired) {
        for (const API_Neig& selectedNeig : selectedNeigs) {
            const API_Guid guid = GetParentElemOfSectElem (selectedNeig.guid);
            elements (CreateElementIdObjectState (guid));
            ++elementCount;

            API_Elem_Head elemHead = {};
            elemHead.guid = guid;
            if (ACAPI_Element_GetHeader (&elemHead) != NoError) continue;
            GS::ObjectState focusElement (
                "elementId", CreateGuidObjectState (guid),
                "elementType", GetElementTypeNonLocalizedName (GetElemTypeId (elemHead)));
            API_Box3D elementBounds = {};
            if (ACAPI_Element_CalcBounds (&elemHead, &elementBounds) == NoError && IsFiniteBounds (elementBounds)) {
                focusElement.Add ("bounds", GS::ObjectState (
                    "xMin", elementBounds.xMin, "yMin", elementBounds.yMin,
                    "xMax", elementBounds.xMax, "yMax", elementBounds.yMax));
            }
            focusElements (focusElement);
        }
    } else if (boundedErr != APIERR_NOSEL) {
        // Do not overwrite an unknown/current operator focus after a failed
        // selection read.  A second read is required before it is safe to
        // restore the temporary marquee.
        API_SelectionInfo selectionAfterError = {};
        const GSErrCode selectionAfterErrorErr = ACAPI_Selection_Get (&selectionAfterError, nullptr, false);
        const GS::OnExit selectionAfterErrorGuard ([&selectionAfterError] () { ReleaseSelectionInfo (selectionAfterError); });
        if (selectionAfterErrorErr != NoError || !IsSameHorizontalMarquee (selectionAfterError, xMin, yMin, xMax, yMax, multiStory)) {
            const GSErrCode restoreMarqueeErr = restoreOperatorFocus ();
            return CreateErrorResponse (
                restoreMarqueeErr != NoError
                    ? restoreMarqueeErr
                    : selectionAfterErrorErr != NoError ? selectionAfterErrorErr : APIERR_REFUSEDCMD,
                restoreMarqueeErr != NoError
                    ? "Failed to read the temporary bounded rectangle and could not restore the operator focus."
                    : "Failed to read the temporary bounded rectangle and could not safely verify its focus for restoration.");
        }
        const GSErrCode restoreMarqueeErr = restoreOperatorFocus ();
        return CreateErrorResponse (restoreMarqueeErr != NoError ? restoreMarqueeErr : boundedErr, "Failed to read the temporary bounded rectangle.");
    }

    API_SelectionInfo selectionAfter = {};
    const GSErrCode selectionAfterErr = ACAPI_Selection_Get (&selectionAfter, nullptr, false);
    const GS::OnExit selectionAfterGuard ([&selectionAfter] () { ReleaseSelectionInfo (selectionAfter); });
    const bool focusReadUnavailable = selectionAfterErr != NoError;
    const bool focusChangedDuringRead = selectionAfterErr == NoError &&
        !IsSameHorizontalMarquee (selectionAfter, xMin, yMin, xMax, yMax, multiStory);

    if (focusReadUnavailable || focusChangedDuringRead) {
        const GSErrCode restoreMarqueeErr = restoreOperatorFocus ();
        return CreateErrorResponse (
            restoreMarqueeErr != NoError
                ? restoreMarqueeErr
                : focusReadUnavailable ? selectionAfterErr : APIERR_REFUSEDCMD,
            restoreMarqueeErr != NoError
                ? "Operator focus could not be verified after the bounded read and could not be restored."
                : "Operator focus could not be verified after the bounded read; refusing to overwrite the current focus.");
    }

    const GSErrCode restoreErr = restoreOperatorFocus ();

    if (restoreErr != NoError) {
        return CreateErrorResponse (
            restoreErr,
            "Bounded read succeeded but the operator focus could not be restored.");
    }

    if (pageRequired) {
        const Int32 enumerated = selectedCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ())
            ? std::numeric_limits<Int32>::max ()
            : static_cast<Int32> (selectedCount);
        return CreateNativePageRequiredResponse (
            "The bounded rectangle requires another page; no partial rows were returned.",
            enumerated,
            0);
    }

    response.Add ("elementCount", elementCount);
    response.Add ("focusRestored", true);
    return response;
}

ChangeSelectionOfElementsCommand::ChangeSelectionOfElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String ChangeSelectionOfElementsCommand::GetName () const
{
    return "ChangeSelectionOfElements";
}

GS::Optional<GS::UniString> ChangeSelectionOfElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "addElementsToSelection": {
                "$ref": "#/Elements"
            },
            "removeElementsFromSelection": {
                "$ref": "#/Elements"
            }
        },
        "additionalProperties": false,
        "required": [
        ]
    })";
}

GS::Optional<GS::UniString> ChangeSelectionOfElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResultsOfAddToSelection": {
                "$ref": "#/ExecutionResults"
            },
            "executionResultsOfRemoveFromSelection": {
                "$ref": "#/ExecutionResults"
            }
        },
        "additionalProperties": false,
        "required": [
            "executionResultsOfAddToSelection",
            "executionResultsOfRemoveFromSelection"
        ]
    })";
}

GS::ObjectState ChangeSelectionOfElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> addElementsToSelection;
    parameters.Get ("addElementsToSelection", addElementsToSelection);
    GS::Array<GS::ObjectState> removeElementsFromSelection;
    parameters.Get ("removeElementsFromSelection", removeElementsFromSelection);

    GS::ObjectState response;
    const auto& executionResultsOfAddToSelection = response.AddList<GS::ObjectState> ("executionResultsOfAddToSelection");
    const auto& executionResultsOfRemoveFromSelection = response.AddList<GS::ObjectState> ("executionResultsOfRemoveFromSelection");

    for (const GS::ObjectState& element : addElementsToSelection) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            executionResultsOfAddToSelection (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        const GSErrCode err = ACAPI_Selection_Select ({ API_Neig (GetGuidFromObjectState (*elementId)) }, true);
        if (err != NoError) {
            executionResultsOfAddToSelection (CreateFailedExecutionResult (err, "Failed to add to selection"));
        } else {
            executionResultsOfAddToSelection (CreateSuccessfulExecutionResult ());
        }
    }

    for (const GS::ObjectState& element : removeElementsFromSelection) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            executionResultsOfRemoveFromSelection (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        const GSErrCode err = ACAPI_Selection_Select ({ API_Neig (GetGuidFromObjectState (*elementId)) }, false);
        if (err != NoError) {
            executionResultsOfRemoveFromSelection (CreateFailedExecutionResult (err, "Failed to remove from selection"));
        } else {
            executionResultsOfRemoveFromSelection (CreateSuccessfulExecutionResult ());
        }
    }

    return response;
}

GetSubelementsOfHierarchicalElementsCommand::GetSubelementsOfHierarchicalElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetSubelementsOfHierarchicalElementsCommand::GetName () const
{
    return "GetSubelementsOfHierarchicalElements";
}

GS::Optional<GS::UniString> GetSubelementsOfHierarchicalElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> GetSubelementsOfHierarchicalElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "subelements": {
                "type": "array",
                "items": {
                    "type": "object",
                    "description": "Subelements grouped by type.",
                    "properties": {
                        "cWallSegments": {
                            "$ref": "#/Elements"
                        },
                        "cWallFrames": {
                            "$ref": "#/Elements"
                        },
                        "cWallPanels": {
                            "$ref": "#/Elements"
                        },
                        "cWallJunctions": {
                            "$ref": "#/Elements"
                        },
                        "cWallAccessories": {
                            "$ref": "#/Elements"
                        },
                        "stairRisers": {
                            "$ref": "#/Elements"
                        },
                        "stairTreads": {
                            "$ref": "#/Elements"
                        },
                        "stairStructures": {
                            "$ref": "#/Elements"
                        },
                        "railingNodes": {
                            "$ref": "#/Elements"
                        },
                        "railingSegments": {
                            "$ref": "#/Elements"
                        },
                        "railingPosts": {
                            "$ref": "#/Elements"
                        },
                        "railingRailEnds": {
                            "$ref": "#/Elements"
                        },
                        "railingRailConnections": {
                            "$ref": "#/Elements"
                        },
                        "railingHandrailEnds": {
                            "$ref": "#/Elements"
                        },
                        "railingHandrailConnections": {
                            "$ref": "#/Elements"
                        },
                        "railingToprailEnds": {
                            "$ref": "#/Elements"
                        },
                        "railingToprailConnections": {
                            "$ref": "#/Elements"
                        },
                        "railingRails": {
                            "$ref": "#/Elements"
                        },
                        "railingToprails": {
                            "$ref": "#/Elements"
                        },
                        "railingHandrails": {
                            "$ref": "#/Elements"
                        },
                        "railingPatterns": {
                            "$ref": "#/Elements"
                        },
                        "railingInnerPosts": {
                            "$ref": "#/Elements"
                        },
                        "railingPanels": {
                            "$ref": "#/Elements"
                        },
                        "railingBalusterSets": {
                            "$ref": "#/Elements"
                        },
                        "railingBalusters": {
                            "$ref": "#/Elements"
                        },
                        "beamSegments": {
                            "$ref": "#/Elements"
                        },
                        "columnSegments": {
                            "$ref": "#/Elements"
                        }
                    },
                    "additionalProperties": false,
                    "required": []
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "subelements"
        ]
    })";
}

template<typename APIElemType>
static void AddSubelementsToObjectState (GS::ObjectState& subelements, APIElemType* subelemArray, const char* subelementType)
{
    if (subelemArray == nullptr)
        return;
    const GSSize nSubelementsWithThisType = BMGetPtrSize (reinterpret_cast<GSPtr>(subelemArray)) / sizeof (APIElemType);
    if (nSubelementsWithThisType == 0) {
        return;
    }

    const auto& subelementsWithThisType = subelements.AddList<GS::ObjectState> (subelementType);
    for (GSIndex i = 0; i < nSubelementsWithThisType; ++i) {
        subelementsWithThisType (CreateElementIdObjectState (subelemArray[i].head.guid));
    }
}

GS::ObjectState GetSubelementsOfHierarchicalElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::ObjectState response;
    const auto& subelementsOfHierarchicalElements = response.AddList<GS::ObjectState> ("subelements");

    for (const GS::ObjectState& hierarchicalElement : elements) {
        const GS::ObjectState* elementId = hierarchicalElement.Get ("elementId");
        if (elementId == nullptr) {
            subelementsOfHierarchicalElements (CreateErrorResponse (APIERR_BADPARS, "elementId of hierarchicalElement is missing"));
            continue;
        }

        const API_Guid elemGuid = GetGuidFromObjectState (*elementId);

        API_ElementMemo memo = {};
        const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
        GSErrCode err = ACAPI_Element_GetMemo (elemGuid, &memo, APIMemoMask_All);

        if (err != NoError) {
            subelementsOfHierarchicalElements (CreateErrorResponse (err, "Failed to get the subelements"));
            continue;
        }

        GS::ObjectState subelements;

#define AddSubelements(memoArrayFieldName) AddSubelementsToObjectState(subelements, memo.memoArrayFieldName, #memoArrayFieldName)

        AddSubelements (cWallSegments);
        AddSubelements (cWallFrames);
        AddSubelements (cWallPanels);
        AddSubelements (cWallJunctions);
        AddSubelements (cWallAccessories);

        AddSubelements (stairRisers);
        AddSubelements (stairTreads);
        AddSubelements (stairStructures);

        AddSubelements (railingNodes);
        AddSubelements (railingSegments);
        AddSubelements (railingPosts);
        AddSubelements (railingRailEnds);
        AddSubelements (railingRailConnections);
        AddSubelements (railingHandrailEnds);
        AddSubelements (railingHandrailConnections);
        AddSubelements (railingToprailEnds);
        AddSubelements (railingToprailConnections);
        AddSubelements (railingRails);
        AddSubelements (railingToprails);
        AddSubelements (railingHandrails);
        AddSubelements (railingPatterns);
        AddSubelements (railingInnerPosts);
        AddSubelements (railingPanels);
        AddSubelements (railingBalusterSets);
        AddSubelements (railingBalusters);

        AddSubelements (beamSegments);

        AddSubelements (columnSegments);

#undef AddSubelementsToObjectState

        subelementsOfHierarchicalElements (subelements);
    }

    return response;
}

GetConnectedElementsCommand::GetConnectedElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetConnectedElementsCommand::GetName () const
{
    return "GetConnectedElements";
}

GS::Optional<GS::UniString> GetConnectedElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "connectedElementType": {
                "$ref": "#/ElementType"
            }
        },
        "additionalProperties": false,
        "required": [
            "elements",
            "connectedElementType"
        ]
    })";
}

GS::Optional<GS::UniString> GetConnectedElementsCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ConnectedElementsOrError"
    })";
}

GS::ObjectState GetConnectedElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    API_ElemTypeID elemType = API_ZombieElemID;
    GS::UniString elementTypeStr;
    if (parameters.Get ("connectedElementType", elementTypeStr)) {
        elemType = GetElementTypeFromNonLocalizedName (elementTypeStr);
        if (elemType == API_ZombieElemID) {
            return CreateErrorResponse (APIERR_BADPARS,
                GS::UniString::Printf ("Invalid connectedElementType '%T'.", elementTypeStr.ToPrintf ()));
        }
    }

    GS::ObjectState response;
    const auto& connectedElementsOfInputElements = response.AddList<GS::ObjectState> ("connectedElements");

    for (const GS::ObjectState& ownerElementOS : elements) {
        const GS::ObjectState* elementId = ownerElementOS.Get ("elementId");
        if (elementId == nullptr) {
            connectedElementsOfInputElements (CreateErrorResponse (APIERR_BADPARS, "elementId of owner element is missing"));
            continue;
        }

        const API_Guid ownerElemGuid = GetGuidFromObjectState (*elementId);

        GS::ObjectState elementsOS;
        const auto& elements = elementsOS.AddList<GS::ObjectState> ("elements");
        GS::Array<API_Guid> connectedElements;
        if (ACAPI_Grouping_GetConnectedElements (ownerElemGuid, elemType, &connectedElements) == NoError) {
            for (const API_Guid& elem : connectedElements) {
                elements (CreateElementIdObjectState (elem));
            }
        }

        connectedElementsOfInputElements (elementsOS);
    }

    return response;
}

GetWallRelationsCommand::GetWallRelationsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetWallRelationsCommand::GetName () const
{
    return "GetWallRelations";
}

GS::Optional<GS::UniString> GetWallRelationsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "walls": {
                "$ref": "#/Elements",
                "description": "At most 256 native Wall element IDs."
            }
        },
        "additionalProperties": false,
        "required": ["walls"]
    })";
}

GS::Optional<GS::UniString> GetWallRelationsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "wallRelations": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "status": { "type": "string", "enum": ["success", "error"] },
                        "error": { "type": "string" },
                        "connectedOutline": {
                            "type": "array",
                            "items": { "$ref": "#/Coordinate2D" }
                        },
                        "conBeg": { "$ref": "#/WallRelationItems" },
                        "conEnd": { "$ref": "#/WallRelationItems" },
                        "conRef": { "$ref": "#/WallRelationItems" },
                        "con": { "$ref": "#/WallRelationItems" },
                        "conX": { "$ref": "#/WallRelationItems" }
                    },
                    "additionalProperties": false,
                    "required": ["status"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["wallRelations"]
    })";
}

static bool AddWallRelationItems (GS::ObjectState& target, const char* key, API_ConnectionGuidItem** items, Int32 count)
{
    if (count < 0)
        return false;
    const auto& output = target.AddList<GS::ObjectState> (key);
    if (count == 0)
        return true;
    if (items == nullptr || *items == nullptr)
        return false;
    const GSSize bytes = BMhGetSize (reinterpret_cast<GSHandle> (items));
    if (bytes % sizeof (API_ConnectionGuidItem) != 0 || bytes / sizeof (API_ConnectionGuidItem) < static_cast<GSSize> (count))
        return false;
    for (Int32 index = 0; index < count; ++index) {
        const API_ConnectionGuidItem& item = (*items)[index];
        GS::ObjectState relation;
        relation.Add ("elementId", CreateGuidObjectState (item.guid));
        relation.Add ("connectedWithBeginning", item.conWithBeg);
        output (relation);
    }
    return true;
}

static bool AddConnectedWallOutline (GS::ObjectState& target, const API_WallRelation& relation)
{
    const auto& output = target.AddList<GS::ObjectState> ("connectedOutline");
    if (relation.connPoly.nCoords < 0)
        return false;
    if (relation.connPoly.nCoords == 0)
        return true;
    if (relation.coords == nullptr || *relation.coords == nullptr)
        return false;
    const GSSize bytes = BMhGetSize (reinterpret_cast<GSHandle> (relation.coords));
    const GSSize required = static_cast<GSSize> (relation.connPoly.nCoords) + 1;
    if (bytes % sizeof (API_Coord) != 0 || bytes / sizeof (API_Coord) < required)
        return false;
    for (Int32 index = 1; index <= relation.connPoly.nCoords; ++index)
        output (Create2DCoordinateObjectState ((*relation.coords)[index]));
    return true;
}

GS::ObjectState GetWallRelationsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> walls;
    parameters.Get ("walls", walls);
    if (walls.IsEmpty () || walls.GetSize () > 256)
        return CreateErrorResponse (APIERR_BADPARS, "walls must contain between 1 and 256 Wall element IDs.");

    GS::ObjectState response;
    const auto& results = response.AddList<GS::ObjectState> ("wallRelations");
    for (const GS::ObjectState& wallOS : walls) {
        const GS::ObjectState* elementId = wallOS.Get ("elementId");
        if (elementId == nullptr) {
            GS::ObjectState result;
            result.Add ("status", "error");
            result.Add ("error", "elementId is missing");
            results (result);
            continue;
        }

        const API_Guid guid = GetGuidFromObjectState (*elementId);
        GS::ObjectState result;
        result.Add ("elementId", CreateGuidObjectState (guid));

        API_Element element = {};
        element.header.guid = guid;
        GSErrCode error = ACAPI_Element_Get (&element);
        if (error != NoError) {
            result.Add ("status", "error");
            result.Add ("error", GS::UniString::Printf ("Failed to load Wall (error %d).", error));
            results (result);
            continue;
        }
        if (GetElemTypeId (element.header) != API_WallID) {
            result.Add ("status", "error");
            result.Add ("error", "Element is not a native Wall.");
            results (result);
            continue;
        }

        API_WallRelation relation = {};
        error = ACAPI_Element_GetRelations (guid, API_WallID, &relation, 0);
        const GS::OnExit dispose ([&relation] () { ACAPI_DisposeWallRelationHdls (&relation); });
        if (error != NoError) {
            result.Add ("status", "error");
            result.Add ("error", GS::UniString::Printf ("Failed to get Wall relations (error %d).", error));
            results (result);
            continue;
        }

        const bool relationDataValid =
            AddWallRelationItems (result, "conBeg", relation.conBeg, relation.nConBeg) &&
            AddWallRelationItems (result, "conEnd", relation.conEnd, relation.nConEnd) &&
            AddWallRelationItems (result, "conRef", relation.conRef, relation.nConRef) &&
            AddWallRelationItems (result, "con", relation.con, relation.nCon) &&
            AddWallRelationItems (result, "conX", relation.conX, relation.nConX) &&
            AddConnectedWallOutline (result, relation);
        result.Add ("status", relationDataValid ? "success" : "error");
        if (!relationDataValid)
            result.Add ("error", "ArchiCAD returned invalid Wall relation memo bounds.");
        results (result);
    }
    return response;
}

GetRoomRelationsCommand::GetRoomRelationsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetRoomRelationsCommand::GetName () const
{
    return "GetRoomRelations";
}

GS::Optional<GS::UniString> GetRoomRelationsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "zones": {
                "$ref": "#/Elements",
                "description": "At most 256 native Zone element IDs."
            },
            "doors": {
                "$ref": "#/Elements",
                "description": "At most 256 native Door element IDs."
            }
        },
        "additionalProperties": false,
        "minProperties": 1
    })";
}

GS::Optional<GS::UniString> GetRoomRelationsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "zoneRelations": {
                "type": "array",
                "items": { "$ref": "#/ZoneRoomRelation" }
            },
            "doorRelations": {
                "type": "array",
                "items": { "$ref": "#/DoorRoomRelation" }
            }
        },
        "additionalProperties": false,
        "required": ["zoneRelations", "doorRelations"]
    })";
}

static void AddGuidList (GS::ObjectState& target, const char* key, const GS::Array<API_Guid>& guids)
{
    const auto& output = target.AddList<GS::ObjectState> (key);
    for (const API_Guid& guid : guids)
        output (CreateGuidObjectState (guid));
}

static void AddRoomRelationGroups (GS::ObjectState& target, const API_RoomRelation& relation)
{
    const auto& groups = target.AddList<GS::ObjectState> ("relatedElements");
    for (const auto& pair : relation.elementsGroupedByType) {
#ifndef ServerMainVers_2700
        if (pair.key == nullptr || pair.value == nullptr || pair.value->IsEmpty ())
            continue;
        GS::ObjectState group;
        group.Add ("elementType", GetElementTypeNonLocalizedName (*pair.key));
        AddGuidList (group, "elementIds", *pair.value);
#else
        if (pair.value.IsEmpty ())
            continue;
        GS::ObjectState group;
        group.Add ("elementType", GetElementTypeNonLocalizedName (pair.key.typeID));
        AddGuidList (group, "elementIds", pair.value);
#endif
        groups (group);
    }
}

static void AddRoomRelationWallParts (GS::ObjectState& target, const API_RoomRelation& relation)
{
    const auto& parts = target.AddList<GS::ObjectState> ("wallParts");
    for (const API_WallPart& part : relation.wallPart) {
        GS::ObjectState item;
        item.Add ("wallElementId", CreateGuidObjectState (part.guid));
        item.Add ("roomEdge", part.roomEdge);
        item.Add ("tBeg", part.tBeg);
        item.Add ("tEnd", part.tEnd);
        parts (item);
    }
}

static void AddRoomRelationError (GS::ObjectState& target, const API_Guid& guid, const char* message, GSErrCode error)
{
    target.Add ("elementId", CreateGuidObjectState (guid));
    target.Add ("status", "error");
    target.Add ("error", GS::UniString::Printf ("%s (error %d).", message, error));
}

GS::ObjectState GetRoomRelationsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> zones;
    GS::Array<GS::ObjectState> doors;
    parameters.Get ("zones", zones);
    parameters.Get ("doors", doors);
    if (zones.GetSize () > 256 || doors.GetSize () > 256 || zones.IsEmpty () && doors.IsEmpty ())
        return CreateErrorResponse (APIERR_BADPARS, "zones and doors must contain at most 256 IDs each and at least one ID in total.");

    GS::ObjectState response;
    const auto& zoneResults = response.AddList<GS::ObjectState> ("zoneRelations");
    const auto& doorResults = response.AddList<GS::ObjectState> ("doorRelations");

    for (const GS::ObjectState& zoneOS : zones) {
        const GS::ObjectState* elementId = zoneOS.Get ("elementId");
        if (elementId == nullptr) {
            GS::ObjectState result;
            result.Add ("status", "error");
            result.Add ("error", "elementId is missing.");
            zoneResults (result);
            continue;
        }
        const API_Guid guid = GetGuidFromObjectState (*elementId);
        API_Element element = {};
        element.header.guid = guid;
        GSErrCode error = ACAPI_Element_Get (&element);
        if (error != NoError) {
            GS::ObjectState result;
            AddRoomRelationError (result, guid, "Failed to load Zone", error);
            zoneResults (result);
            continue;
        }
        if (GetElemTypeId (element.header) != API_ZoneID) {
            GS::ObjectState result;
            result.Add ("elementId", CreateGuidObjectState (guid));
            result.Add ("status", "error");
            result.Add ("error", "Element is not a native Zone.");
            zoneResults (result);
            continue;
        }

        API_RoomRelation relation = {};
        error = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &relation, 0);
        const GS::OnExit dispose ([&relation] () { ACAPI_DisposeRoomRelationHdls (&relation); });
        GS::ObjectState result;
        result.Add ("elementId", CreateGuidObjectState (guid));
        if (error != NoError) {
            result.Add ("status", "error");
            result.Add ("error", GS::UniString::Printf ("Failed to get Zone room relations (error %d).", error));
            zoneResults (result);
            continue;
        }
        result.Add ("status", "success");
        AddRoomRelationGroups (result, relation);
        AddRoomRelationWallParts (result, relation);
        zoneResults (result);
    }

    for (const GS::ObjectState& doorOS : doors) {
        const GS::ObjectState* elementId = doorOS.Get ("elementId");
        if (elementId == nullptr) {
            GS::ObjectState result;
            result.Add ("status", "error");
            result.Add ("error", "elementId is missing.");
            doorResults (result);
            continue;
        }
        const API_Guid guid = GetGuidFromObjectState (*elementId);
        API_Element element = {};
        element.header.guid = guid;
        GSErrCode error = ACAPI_Element_Get (&element);
        if (error != NoError) {
            GS::ObjectState result;
            AddRoomRelationError (result, guid, "Failed to load Door", error);
            doorResults (result);
            continue;
        }
        if (GetElemTypeId (element.header) != API_DoorID) {
            GS::ObjectState result;
            result.Add ("elementId", CreateGuidObjectState (guid));
            result.Add ("status", "error");
            result.Add ("error", "Element is not a native Door.");
            doorResults (result);
            continue;
        }

        API_DoorRelation relation = {};
        error = ACAPI_Element_GetRelations (guid, API_ZoneID, &relation, 0);
        GS::ObjectState result;
        result.Add ("elementId", CreateGuidObjectState (guid));
        if (error != NoError) {
            result.Add ("status", "error");
            result.Add ("error", GS::UniString::Printf ("Failed to get Door room relations (error %d).", error));
            doorResults (result);
            continue;
        }
        result.Add ("status", "success");
        const auto& roomIds = result.AddList<GS::ObjectState> ("zoneElementIds");
        if (relation.fromRoom != APINULLGuid)
            roomIds (CreateGuidObjectState (relation.fromRoom));
        if (relation.toRoom != APINULLGuid && relation.toRoom != relation.fromRoom)
            roomIds (CreateGuidObjectState (relation.toRoom));
        doorResults (result);
    }

    return response;
}

GetZoneBoundariesCommand::GetZoneBoundariesCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetZoneBoundariesCommand::GetName () const
{
    return "GetZoneBoundaries";
}

GS::Optional<GS::UniString> GetZoneBoundariesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "zoneElementId": {
                "$ref": "#/ElementId"
            }
        },
        "additionalProperties": false,
        "required": [
            "zoneElementId"
        ]
    })";
}

GS::Optional<GS::UniString> GetZoneBoundariesCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ZoneBoundariesOrError"
    })";
}

GS::ObjectState GetZoneBoundariesCommand::Execute (
    const GS::ObjectState& parameters,
#ifdef ServerMainVers_2800
    GS::ProcessControl& processControl) const
#else
    GS::ProcessControl& /*processControl*/) const
#endif
{
    const GS::ObjectState* zoneElementId = parameters.Get ("zoneElementId");
    if (zoneElementId == nullptr) {
        return CreateErrorResponse (APIERR_BADPARS, "zoneElementId is missing");
    }

#ifdef ServerMainVers_2800
    ACAPI::ZoneBoundaryQuery query = ACAPI::CreateZoneBoundaryQuery ();

    ACAPI::Result updateResult = query.Modify (
        [&] (ACAPI::ZoneBoundaryQuery::Modifier& modifier) -> GSErrCode {
            ACAPI::Result<void> result = modifier.Update (processControl);
            return result.IsOk () ? NoError : result.UnwrapErr ().kind;
        }
    );

    if (updateResult.IsErr ()) {
        return CreateErrorResponse (updateResult.UnwrapErr ().kind, "Failed to execute zone boundary query");
    }

    GS::ObjectState response;
    const auto& zoneBoundaries = response.AddList<GS::ObjectState> ("zoneBoundaries");

    const API_Guid zoneGuid = GetGuidFromObjectState (*zoneElementId);
    const ACAPI::Result<std::vector<ACAPI::ZoneBoundary>> boundaries = query.GetZoneBoundaries (zoneGuid);

    if (boundaries.IsErr ()) {
        return CreateErrorResponse (boundaries.UnwrapErr ().kind, "Failed to get zone boundary");
    }

    for (const ACAPI::ZoneBoundary& boundary : boundaries.Unwrap ()) {
        GS::ObjectState boundaryOS;
        boundaryOS.Add ("connectedElementId", CreateGuidObjectState (boundary.GetElemId ()));
        boundaryOS.Add ("isExternal", boundary.IsExternal ());
        boundaryOS.Add ("neighbouringZoneElementId", CreateGuidObjectState (boundary.GetNeighbouringZoneId ()));
        boundaryOS.Add ("area", boundary.GetArea ());

        const auto& polygonOutline = boundaryOS.AddList<GS::ObjectState> ("polygonOutline");
        const ModelerAPI::MeshBody& body = boundary.GetBody ();
        const ModelerAPI::Polygon& poly = boundary.GetPolygon ();
        const Int32 edgeCount = poly.GetEdgeCount ();
        if (edgeCount <= 0)
            return CreateErrorResponse (APIERR_BADPARS, "ArchiCAD returned a zone boundary without edges.");
        {
            ModelerAPI::Edge edge;
            ModelerAPI::Vertex vertex;
            Int32 lastVertexIndex2 = 0;
            bool contourClosed = false;
            bool hasOuterVertex = false;
            const auto addVertex = [&] (const ModelerAPI::Vertex& modelerVertex) {
                const API_Coord3D coordinate { modelerVertex.x, modelerVertex.y, modelerVertex.z };
                polygonOutline (Create3DCoordinateObjectState (coordinate));
                hasOuterVertex = true;
            };
            for (Int32 edgeIdx = 1; edgeIdx <= edgeCount; ++edgeIdx) {

                const Int32 edgeIndex = poly.GetEdgeIndex (edgeIdx);
                // ModelerAPI uses zero as the contour/hole separator.  The
                // public ZoneBoundary response currently exposes only the
                // outer polygon, so close it and stop before serializing hole
                // edges instead of treating the separator as an invalid index.
                if (edgeIndex == 0) {
                    if (lastVertexIndex2 <= 0)
                        return CreateErrorResponse (APIERR_BADPARS, "ArchiCAD returned a zone boundary without an outer contour.");
                    body.GetVertex (lastVertexIndex2, &vertex);
                    addVertex (vertex);
                    contourClosed = true;
                    break;
                }
                // Polygon edge indices are signed: the sign carries the edge
                // orientation, while zero separates the outer contour from
                // hole contours.  Pass the signed index to ModelerAPI; it
                // resolves the oriented edge as documented by the AC28
                // ZoneBoundaryQuery example.
                body.GetEdge (edgeIndex, &edge);
                const Int32 vertexIndex1 = edge.GetVertexIndex1 ();
                const Int32 vertexIndex2 = edge.GetVertexIndex2 ();
                if (vertexIndex1 <= 0 || vertexIndex2 <= 0)
                    return CreateErrorResponse (APIERR_BADPARS, "ArchiCAD returned an invalid zone boundary vertex index.");
                lastVertexIndex2 = vertexIndex2;
                body.GetVertex (vertexIndex1, &vertex);
                addVertex (vertex);

                if (edgeIdx == edgeCount) {
                    body.GetVertex (vertexIndex2, &vertex);
                    addVertex (vertex);
                    contourClosed = true;
                }
            }
            if (!contourClosed || !hasOuterVertex)
                return CreateErrorResponse (APIERR_BADPARS, "ArchiCAD returned an empty zone boundary outer contour.");
        }

        zoneBoundaries (boundaryOS);
    }

    return response;
#else
    return CreateErrorResponse (APIERR_NOTSUPPORTED, "This command is only supported in ArchiCAD 28 or later.");
#endif
}

GetCollisionsCommand::GetCollisionsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetCollisionsCommand::GetName () const
{
    return "GetCollisions";
}

GS::Optional<GS::UniString> GetCollisionsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementsGroup1": {
                "$ref": "#/Elements"
            },
            "elementsGroup2": {
                "$ref": "#/Elements"
            },
            "settings": {
                "type": "object",
                "properties": {
                    "volumeTolerance": {
                        "type": "number",
                        "description": "Intersection body volume greater then this value will be considered as a collision. Default value is 0.001."
                    },
                    "performSurfaceCheck": {
                        "type": "boolean",
                        "description": "Enables surface collision check. If disabled the surfaceTolerance value will be ignored. By default it's false."
                    },
                    "surfaceTolerance": {
                        "type": "number",
                        "description": "Intersection body surface area greater then this value will be considered as a collision. Default value is 0.001."
                    }
                },
                "additionalProperties": false,
                "required": [
                    "volumeTolerance",
                    "performSurfaceCheck",
                    "surfaceTolerance"
                ]
            }
        },
        "additionalProperties": false,
        "required": [
            "elementsGroup1",
            "elementsGroup2"
        ]
    })";
}

GS::Optional<GS::UniString> GetCollisionsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "collisions": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId1": {
                            "$ref": "#/ElementId"
                        },
                        "elementId2": {
                            "$ref": "#/ElementId"
                        },
                        "hasBodyCollision": {
                            "type": "boolean"
                        },
                        "hasClearenceCollision": {
                            "type": "boolean"
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId1",
                        "elementId2",
                        "hasBodyCollision",
                        "hasClearenceCollision"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "collisions"
        ]
    })";
}

GS::ObjectState GetCollisionsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsGroup1;
    parameters.Get ("elementsGroup1", elementsGroup1);
    GS::Array<GS::ObjectState> elementsGroup2;
    parameters.Get ("elementsGroup2", elementsGroup2);

    API_CollisionDetectionSettings collisionSettings = {};
    collisionSettings.volumeTolerance = 0.001;
    collisionSettings.performSurfaceCheck = false;
    collisionSettings.surfaceTolerance = 0.001;
    GS::ObjectState settings;
    if (parameters.Get ("settings", settings)) {
        settings.Get ("volumeTolerance", collisionSettings.volumeTolerance);
        settings.Get ("performSurfaceCheck", collisionSettings.performSurfaceCheck);
        settings.Get ("surfaceTolerance", collisionSettings.surfaceTolerance);
    }

    const GS::Array<API_Guid> elemIds1 = elementsGroup1.Transform<API_Guid> (GetGuidFromElementsArrayItem);
    const GS::Array<API_Guid> elemIds2 = elementsGroup2.Transform<API_Guid> (GetGuidFromElementsArrayItem);
    GS::Array<GS::Pair<API_CollisionElem, API_CollisionElem>> resultArray;
    GSErrCode err = ACAPI_Element_GetCollisions (elemIds1, elemIds2, resultArray, collisionSettings);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to perform collision detection.");
    }

    GS::ObjectState response;
    const auto& collisions = response.AddList<GS::ObjectState> ("collisions");

    for (const auto& collisionElement : resultArray) {
        collisions (GS::ObjectState (
            "elementId1", CreateGuidObjectState (collisionElement.first.collidedElemGuid),
            "elementId2", CreateGuidObjectState (collisionElement.second.collidedElemGuid),
            "hasBodyCollision", collisionElement.first.hasBodyCollision,
            "hasClearenceCollision", collisionElement.second.hasClearenceCollision));
    }

    return response;
}

MoveElementsCommand::MoveElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String MoveElementsCommand::GetName () const
{
    return "MoveElements";
}

GS::Optional<GS::UniString> MoveElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementsWithMoveVectors": {
                "type": "array",
                "description": "The elements with move vector pairs.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId"
                        },
                        "moveVector": {
                            "type": "object",
                            "description" : "Move vector of a 3D point.",
                            "properties" : {
                                "x": {
                                    "type": "number",
                                    "description" : "X value of the vector."
                                },
                                "y" : {
                                    "type": "number",
                                    "description" : "Y value of the vector."
                                },
                                "z" : {
                                    "type": "number",
                                    "description" : "Z value of the vector."
                                }
                            },
                            "additionalProperties": false,
                            "required" : [
                                "x",
                                "y",
                                "z"
                            ]
                        },
                        "copy": {
                            "type": "boolean",
                            "description" : "Optional parameter. If true, then a copy of the element will be moved. By default it's false."
                        }
                    },
                    "additionalProperties": false,
                    "required": [
                        "elementId",
                        "moveVector"
                    ]
                }
            }
        },
        "additionalProperties": false,
        "required": [
            "elementsWithMoveVectors"
        ]
    })";
}

GS::Optional<GS::UniString> MoveElementsCommand::GetResponseSchema () const
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

static GSErrCode MoveElement (const API_Guid& elemGuid, const API_Vector3D& moveVector, bool withCopy)
{
    GS::Array<API_Neig> elementsToEdit = { API_Neig (elemGuid) };

    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Drag;
    editPars.endC = moveVector;
    editPars.withDelete = !withCopy;

    return ACAPI_Element_Edit (&elementsToEdit, editPars);
}

GS::ObjectState	MoveElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsWithMoveVectors;
    parameters.Get ("elementsWithMoveVectors", elementsWithMoveVectors);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Move Elements", [&]() -> GSErrCode {
        for (const GS::ObjectState& elementWithMoveVector : elementsWithMoveVectors) {
            const GS::ObjectState* elementId = elementWithMoveVector.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            const GS::ObjectState* moveVector = elementWithMoveVector.Get ("moveVector");
            if (moveVector == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "moveVector is missing"));
                continue;
            }

            const API_Guid elemGuid = GetGuidFromObjectState (*elementId);

            bool copy = false;
            elementWithMoveVector.Get ("copy", copy);

            const GSErrCode err = MoveElement (elemGuid,
                                               Get3DCoordinateFromObjectState (*moveVector),
                                               copy);
            if (err != NoError) {
                const GS::UniString errorMsg = GS::UniString::Printf ("Failed to move element with guid %T!", APIGuidToString (elemGuid).ToPrintf ());
                executionResults (CreateFailedExecutionResult (err, errorMsg));
            } else {
                executionResults (CreateSuccessfulExecutionResult ());
            }
        }

        return NoError;
    });

    return response;
}

static GSErrCode RotateElement (const API_Guid& elemGuid, const API_Coord& beginPoint, const API_Coord& endPoint, const API_Coord& origin, bool withCopy)
{
    GS::Array<API_Neig> elementsToEdit = { API_Neig (elemGuid) };

    API_EditPars editPars = {};
    editPars.typeID = APIEdit_Rotate;
    editPars.begC = API_Coord3D { beginPoint.x, beginPoint.y, 0.0 };
    editPars.endC = API_Coord3D { endPoint.x, endPoint.y, 0.0 };
    editPars.origC = origin;
    editPars.withDelete = !withCopy;

    return ACAPI_Element_Edit (&elementsToEdit, editPars);
}

RotateElementsCommand::RotateElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String RotateElementsCommand::GetName () const
{
    return "RotateElements";
}

GS::Optional<GS::UniString> RotateElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementsWithRotations": {
                "type": "array",
                "description": "The elements with rotation settings.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": {
                            "$ref": "#/ElementId"
                        },
                        "rotation": {
                            "type": "object",
                            "description": "Rotation parameters for an element.",
                            "properties": {
                                "beginPoint": {
                                    "type": "object",
                                    "description": "Starting point of the rotation arc.",
                                    "properties": {
                                        "x": { "type": "number" },
                                        "y": { "type": "number" }
                                    },
                                    "additionalProperties": false,
                                    "required": ["x", "y"]
                                },
                                "endPoint": {
                                    "type": "object",
                                    "description": "End point of the rotation arc.",
                                    "properties": {
                                        "x": { "type": "number" },
                                        "y": { "type": "number" }
                                    },
                                    "additionalProperties": false,
                                    "required": ["x", "y"]
                                },
                                "origin": {
                                    "type": "object",
                                    "description": "Center of rotation.",
                                    "properties": {
                                        "x": { "type": "number" },
                                        "y": { "type": "number" }
                                    },
                                    "additionalProperties": false,
                                    "required": ["x", "y"]
                                }
                            },
                            "additionalProperties": false,
                            "required": ["beginPoint", "endPoint", "origin"]
                        },
                        "copy": {
                            "type": "boolean",
                            "description": "Optional parameter. If true, a copy of the element is rotated. By default it's false."
                        }
                    },
                    "additionalProperties": false,
                    "required": ["elementId", "rotation"]
                }
            }
        },
        "additionalProperties": false,
        "required": ["elementsWithRotations"]
    })";
}

GS::Optional<GS::UniString> RotateElementsCommand::GetResponseSchema () const
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

GS::ObjectState RotateElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsWithRotations;
    parameters.Get ("elementsWithRotations", elementsWithRotations);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("Rotate Elements", [&]() -> GSErrCode {
        for (const GS::ObjectState& elementWithRotation : elementsWithRotations) {
            const GS::ObjectState* elementId = elementWithRotation.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            const GS::ObjectState* rotation = elementWithRotation.Get ("rotation");
            if (rotation == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "rotation is missing"));
                continue;
            }

            const GS::ObjectState* beginPoint = rotation->Get ("beginPoint");
            const GS::ObjectState* endPoint = rotation->Get ("endPoint");
            const GS::ObjectState* origin = rotation->Get ("origin");
            if (beginPoint == nullptr || endPoint == nullptr || origin == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "rotation beginPoint, endPoint, or origin is missing"));
                continue;
            }

            const API_Guid elemGuid = GetGuidFromObjectState (*elementId);

            bool copy = false;
            elementWithRotation.Get ("copy", copy);

            const GSErrCode err = RotateElement (elemGuid,
                                                Get2DCoordinateFromObjectState (*beginPoint),
                                                Get2DCoordinateFromObjectState (*endPoint),
                                                Get2DCoordinateFromObjectState (*origin),
                                                copy);
            if (err != NoError) {
                const GS::UniString errorMsg = GS::UniString::Printf ("Failed to rotate element with guid %T!", APIGuidToString (elemGuid).ToPrintf ());
                executionResults (CreateFailedExecutionResult (err, errorMsg));
            } else {
                executionResults (CreateSuccessfulExecutionResult ());
            }
        }

        return NoError;
    });

    return response;
}

FilterElementsCommand::FilterElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String FilterElementsCommand::GetName () const
{
    return "FilterElements";
}

GS::Optional<GS::UniString> FilterElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "filters": {
                "type": "array",
                "items": {
                    "$ref": "#/ElementFilter"
                },
                "minItems": 1
            }
        },
        "additionalProperties": false,
        "required": [
            "elements"
        ]
    })";
}

GS::Optional<GS::UniString> FilterElementsCommand::GetResponseSchema () const
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

GS::ObjectState FilterElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::Array<GS::UniString> filters;
    parameters.Get ("filters", filters);

    API_ElemFilterFlags filterFlags = APIFilt_None;
    for (const GS::UniString& filter : filters) {
        filterFlags |= ConvertFilterStringToFlag (filter);
    }
    if (filterFlags == APIFilt_None) {
        return CreateErrorResponse (APIERR_BADPARS, "Invalid or missing filters!");
    }

    GS::ObjectState response;
    const auto& filteredElements = response.AddList<GS::ObjectState> ("elements");

    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            continue;
        }

        const API_Guid elemGuid = GetGuidFromObjectState (*elementId);
        if (!ACAPI_Element_Filter (elemGuid, filterFlags)) {
            continue;
        }

        filteredElements (CreateElementIdObjectState (elemGuid));
    }

    return response;
}

HighlightElementsCommand::HighlightElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String HighlightElementsCommand::GetName () const
{
    return "HighlightElements";
}

GS::Optional<GS::UniString> HighlightElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elements": {
                "$ref": "#/Elements"
            },
            "highlightedColors": {
                "type": "array",
                "description": "A list of colors to highlight elements.",
                "items": {
                    "type": "array",
                    "description": "Color of the highlighted element as an [r, g, b, a] array. Each component must be in the 0-255 range.",
                    "items": {
                        "type": "integer"
                    },
                    "minItems": 4,
                    "maxItems": 4
                }
            },
            "wireframe3D": {
                "type": "boolean",
                "description" : "Optional parameter. Switch non highlighted elements in the 3D window to wireframe."
            },
            "nonHighlightedColor": {
                "type": "array",
                "description": "Optional parameter. Color of the non highlighted elements as an [r, g, b, a] array. Each component must be in the 0-255 range.",
                "items": {
                    "type": "integer"
                },
                "minItems": 4,
                "maxItems": 4
            }
        },
        "additionalProperties": false,
        "required": [
            "elements",
            "highlightedColors"
        ]
    })";
}

GS::Optional<GS::UniString> HighlightElementsCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

#ifdef ServerMainVers_2600

static API_RGBAColor GetRGBAColorFromArray (const GS::Array<GS::Int32>& color)
{
    return API_RGBAColor {
        color[0] / 255.0,
        color[1] / 255.0,
        color[2] / 255.0,
        color[3] / 255.0
    };
}

static GS::Optional<API_RGBAColor> GetRGBAColorFromObjectState (const GS::ObjectState& os, const GS::String& name)
{
    GS::Array<GS::Int32> color;
    if (os.Get (name, color)) {
        return GetRGBAColorFromArray (color);
    } else {
        return {};
    }
}

GS::ObjectState HighlightElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    if (elements.IsEmpty ()) {
        ACAPI_UserInput_ClearElementHighlight ();
        // need to call redraw for changes to take effect
        ACAPI_View_Redraw ();
        return CreateSuccessfulExecutionResult ();
    }

    GS::Array<GS::Array<GS::Int32>> highlightedColors;
    parameters.Get ("highlightedColors", highlightedColors);

    if (highlightedColors.GetSize () != elements.GetSize ()) {
        return CreateFailedExecutionResult (APIERR_BADPARS, "The size of 'elements' array and 'highlightedColors' array does not match.");
    }

    GS::HashTable<API_Guid, API_RGBAColor> elementsWithColors;
    for (USize i = 0; i < elements.GetSize (); ++i) {
        GS::ObjectState elementId;
        if (elements[i].Get ("elementId", elementId)) {
            const API_Guid elemGuid = GetGuidFromObjectState (elementId);
            const API_RGBAColor color = GetRGBAColorFromArray (highlightedColors[i]);
            elementsWithColors.Add (elemGuid, color);
        }
    }

    GS::Optional<bool> wireframe3D;
    bool tmp;
    if (parameters.Get ("wireframe3D", tmp)) {
        wireframe3D = tmp;
    }

    const GS::Optional<API_RGBAColor> nonHighlightedColor = GetRGBAColorFromObjectState (parameters, "nonHighlightedColor");

    ACAPI_UserInput_SetElementHighlight (elementsWithColors, wireframe3D, nonHighlightedColor);

    // need to call redraw for changes to take effect
    ACAPI_View_Redraw ();

    return CreateSuccessfulExecutionResult ();
}

#else

GS::ObjectState HighlightElementsCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    return CreateFailedExecutionResult (APIERR_GENERAL, GetName () + " command is not supported for this AC version.");
}

#endif


static API_Coord3D TransformPoint (const API_Coord3D& pt, const API_Tranmat& tm)
{
    API_Coord3D res;
    res.x = (pt.x * tm.tmx[0]) + (pt.y * tm.tmx[1]) + (pt.z * tm.tmx[2]) + tm.tmx[3];
    res.y = (pt.x * tm.tmx[4]) + (pt.y * tm.tmx[5]) + (pt.z * tm.tmx[6]) + tm.tmx[7];
    res.z = (pt.x * tm.tmx[8]) + (pt.y * tm.tmx[9]) + (pt.z * tm.tmx[10]) + tm.tmx[11];
    return res;
}

static void UpdateGlobalBoundsWithPoint (API_Box3D& globalBounds, const API_Coord3D& pt)
{
    if (pt.x < globalBounds.xMin) globalBounds.xMin = pt.x;
    if (pt.x > globalBounds.xMax) globalBounds.xMax = pt.x;
    if (pt.y < globalBounds.yMin) globalBounds.yMin = pt.y;
    if (pt.y > globalBounds.yMax) globalBounds.yMax = pt.y;
    if (pt.z < globalBounds.zMin) globalBounds.zMin = pt.z;
    if (pt.z > globalBounds.zMax) globalBounds.zMax = pt.z;
}

static void GetLocalBodyCorners (const API_BodyType& body, API_Coord3D (&corners)[8])
{
    corners[0] = { body.xmin, body.ymin, body.zmin };
    corners[1] = { body.xmax, body.ymin, body.zmin };
    corners[2] = { body.xmin, body.ymax, body.zmin };
    corners[3] = { body.xmax, body.ymax, body.zmin };
    corners[4] = { body.xmin, body.ymin, body.zmax };
    corners[5] = { body.xmax, body.ymin, body.zmax };
    corners[6] = { body.xmin, body.ymax, body.zmax };
    corners[7] = { body.xmax, body.ymax, body.zmax };
}

static GSErrCode CalculateSolidBodyBounds (const API_Elem_Head& elemHead, API_Box3D& outBounds)
{
    outBounds.xMin = outBounds.yMin = outBounds.zMin = 1e30;
    outBounds.xMax = outBounds.yMax = outBounds.zMax = -1e30;

    API_ElemInfo3D info3D = {};
    GSErrCode err = ACAPI_ModelAccess_Get3DInfo (elemHead, &info3D);
    if (err != NoError) {
        return err;
    }

    bool foundSolidBody = false;

    for (Int32 iBody = info3D.fbody; iBody <= info3D.lbody; ++iBody) {
        API_Component3D bodyComp = {};
        bodyComp.header.typeID = API_BodyID;
        bodyComp.header.index = iBody;

        if (ACAPI_ModelAccess_GetComponent (&bodyComp) != NoError) continue;

        if (bodyComp.body.nPgon == 0) { // Skip non-solid bodies
            continue;
        }

        foundSolidBody = true;

        API_Coord3D corners[8];
        GetLocalBodyCorners (bodyComp.body, corners);

        for (int k = 0; k < 8; ++k) {
            const API_Coord3D globalPt = TransformPoint (corners[k], bodyComp.body.tranmat);
            UpdateGlobalBoundsWithPoint (outBounds, globalPt);
        }
    }

    if (!foundSolidBody) {
        return APIERR_GENERAL;
    }

    return NoError;
}

Get3DBoundingBoxesCommand::Get3DBoundingBoxesCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String Get3DBoundingBoxesCommand::GetName () const
{
    return "Get3DBoundingBoxes";
}

GS::Optional<GS::UniString> Get3DBoundingBoxesCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> Get3DBoundingBoxesCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
            "properties": {
            "boundingBoxes3D": {
                "$ref": "#/BoundingBoxes3D"
            }
        },
        "additionalProperties": false,
        "required": [
            "boundingBoxes3D"
        ]
    })";
}

GS::ObjectState Get3DBoundingBoxesCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GS::ObjectState response;
    const auto& boundingBoxes3D = response.AddList<GS::ObjectState> ("boundingBoxes3D");

    for (const GS::ObjectState& element : elements) {
        const GS::ObjectState* elementId = element.Get ("elementId");
        if (elementId == nullptr) {
            boundingBoxes3D (CreateErrorResponse (APIERR_BADPARS, "elementId is missing"));
            continue;
        }

        API_Elem_Head elemHead = {};
        elemHead.guid = GetGuidFromObjectState (*elementId);
        GSErrCode err = ACAPI_Element_GetHeader (&elemHead);
        if (err != NoError) {
            boundingBoxes3D (CreateErrorResponse (err, "Failed to find element in Archicad"));
            continue;
        }
        const API_ElemTypeID typeID = GetElemTypeId (elemHead);

        API_Box3D box3D = {};
        if (typeID == API_RoofID || typeID == API_ZoneID) {
            err = CalculateSolidBodyBounds (elemHead, box3D);
        } else {
            err = ACAPI_Element_CalcBounds (&elemHead, &box3D);
        }
        if (err != NoError) {
            boundingBoxes3D (CreateErrorResponse (err, "Failed to get the 3D bounding box"));
            continue;
        }
        if (!IsFiniteBounds (box3D)) {
            boundingBoxes3D (CreateErrorResponse (APIERR_GENERAL, "Archicad returned a non-finite 3D bounding box"));
            continue;
        }

        GS::ObjectState boundingBox3D ("xMin", box3D.xMin,
                                       "xMax", box3D.xMax,
                                       "yMin", box3D.yMin,
                                       "yMax", box3D.yMax,
                                       "zMin", box3D.zMin,
                                       "zMax", box3D.zMax);
        boundingBoxes3D (GS::ObjectState ("boundingBox3D", boundingBox3D));
    }

    return response;
}

DeleteElementsCommand::DeleteElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String DeleteElementsCommand::GetName () const
{
    return "DeleteElements";
}

GS::Optional<GS::UniString> DeleteElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> DeleteElementsCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState DeleteElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GSErrCode err = NoError;

    ACAPI_CallUndoableCommand ("DeleteElementsCommand", [&]() {
        err = ACAPI_Element_Delete (elements.Transform<API_Guid> (GetGuidFromElementsArrayItem));

        return err;
    });

    return err == NoError
        ? CreateSuccessfulExecutionResult ()
        : CreateFailedExecutionResult (err, "Failed to delete elements.");
}

LockElementsCommand::LockElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String LockElementsCommand::GetName () const
{
    return "LockElements";
}

GS::Optional<GS::UniString> LockElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> LockElementsCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState LockElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GSErrCode err = NoError;

    ACAPI_CallUndoableCommand ("LockElementsCommand", [&]() {
        err = ACAPI_Grouping_Tool (elements.Transform<API_Guid> (GetGuidFromElementsArrayItem), APITool_Lock, nullptr);
        return err;
    });

    return err == NoError
        ? CreateSuccessfulExecutionResult ()
        : CreateFailedExecutionResult (err, "Failed to lock elements.");
}

UnlockElementsCommand::UnlockElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String UnlockElementsCommand::GetName () const
{
    return "UnlockElements";
}

GS::Optional<GS::UniString> UnlockElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> UnlockElementsCommand::GetResponseSchema () const
{
    return R"({
        "$ref": "#/ExecutionResult"
    })";
}

GS::ObjectState UnlockElementsCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elements;
    parameters.Get ("elements", elements);

    GSErrCode err = NoError;

    ACAPI_CallUndoableCommand ("UnlockElementsCommand", [&]() {
        err = ACAPI_Grouping_Tool (elements.Transform<API_Guid> (GetGuidFromElementsArrayItem), APITool_Unlock, nullptr);
        return err;
    });

    return err == NoError
        ? CreateSuccessfulExecutionResult ()
        : CreateFailedExecutionResult (err, "Failed to unlock elements.");
}

GetElementPreviewImageCommand::GetElementPreviewImageCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetElementPreviewImageCommand::GetName () const
{
    return "GetElementPreviewImage";
}

GS::Optional<GS::UniString> GetElementPreviewImageCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementId": {
                "$ref": "#/ElementId"
            },
            "imageType": {
                "type": "string",
                "description": "The type of the preview image. Default is 3D.",
                "enum": ["2D", "Section", "3D"]
            },
            "format": {
                "type": "string",
                "description": "The image format. Default is png.",
                "enum": ["png", "jpg"]
            },
            "width": {
                "type": "integer",
                "description": "The width of the preview image in pixels. Default is 128."
            },
            "height": {
                "type": "integer",
                "description": "The height of the preview image in pixels. Default is 128."
            }
        },
        "additionalProperties": false,
        "required": [
            "elementId"
        ]
    })";
}

GS::Optional<GS::UniString> GetElementPreviewImageCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "previewImage": {
                "type": "string",
                "description": "The base64 encoded preview image."
            }
        },
        "additionalProperties": false,
        "required": [
            "previewImage"
        ]
    })";
}

GS::ObjectState GetElementPreviewImageCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    API_VisualOverriddenImage image = {};
    image.view = APIImage_Model3D;
    GS::UniString imageTypeStr;
    if (parameters.Get ("imageType", imageTypeStr)) {
        if (imageTypeStr == "2D") {
            image.view = APIImage_Model2D;
        } else if (imageTypeStr == "Section") {
            image.view = APIImage_Section;
        } else if (imageTypeStr == "3D") {
            image.view = APIImage_Model3D;
        } else {
            return CreateErrorResponse (APIERR_BADPARS, "Invalid imageType parameter.");
        }
    }

    NewDisplay::NativeImage::Encoding encoding = NewDisplay::NativeImage::Encoding::PNG;
    GS::UniString formatStr;
    if (parameters.Get ("format", formatStr)) {
        if (formatStr == "png") {
            encoding = NewDisplay::NativeImage::Encoding::PNG;
        } else if (formatStr == "jpg") {
            encoding = NewDisplay::NativeImage::Encoding::JPEG;
        } else {
            return CreateErrorResponse (APIERR_BADPARS, "Invalid format parameter.");
        }
    }

    UInt32 width = 128;
    UInt32 height = 128;
    parameters.Get ("width", width);
    parameters.Get ("height", height);

    NewDisplay::NativeImage nativeImage (width, height, 32, nullptr);
    image.nativeImagePtr = &nativeImage;
    GSErrCode err = ACAPI_GraphicalOverride_GetVisualOverriddenImage (GetGuidFromElementsArrayItem (parameters), &image);
    BMhFree (image.vectorImageHandle);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to get element preview image.");
    }

    GS::MemoryOChannel32 memChannel (GS::MemoryOChannel32::BMAllocation);
    if (!nativeImage.Encode (memChannel, encoding)) {
        return CreateErrorResponse (APIERR_GENERAL, "Failed to encode element preview image.");
    }

    auto str = Base64Converter::Encode (memChannel.GetDestination (), memChannel.GetDataSize ());
    str.DeleteAll (GS::UniChar(char('\n')));
    return GS::ObjectState ("previewImage", str);
}

GetRoomImageCommand::GetRoomImageCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetRoomImageCommand::GetName () const
{
    return "GetRoomImage";
}

GS::Optional<GS::UniString> GetRoomImageCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "zoneId": {
                "$ref": "#/ElementId"
            },
            "format": {
                "type": "string",
                "description": "The image format. Default is png.",
                "enum": ["png", "jpg"]
            },
            "width": {
                "type": "integer",
                "description": "The width of the preview image in pixels. Default is 256."
            },
            "height": {
                "type": "integer",
                "description": "The height of the preview image in pixels. Default is 256."
            },
            "offset": {
                "type": "number",
                "description": "Offset of the clip polygon from the edge of the zone. Default is 0.001."
            },
            "scale": {
                "type": "number",
                "description": "Scale of the view (e.g. 0.005 for 1:200). Default is 0.005."
            },
            "backgroundColor": {
                "$ref": "#/ColorRGB",
                "description": "Background color of the generated image. Default is white (1.0, 1.0, 1.0)."
            }
        },
        "additionalProperties": false,
        "required": [
            "zoneId"
        ]
    })";
}

GS::Optional<GS::UniString> GetRoomImageCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "roomImage": {
                "type": "string",
                "description": "The base64 encoded room image."
            }
        },
        "additionalProperties": false,
        "required": [
            "roomImage"
        ]
    })";
}

GS::ObjectState GetRoomImageCommand::Execute (const GS::ObjectState& parameters, GS::ProcessControl& /*processControl*/) const
{
    API_RoomImage image = {};
    image.roomGuid = GetGuidFromArrayItem ("zoneId", parameters);
    image.viewType = APIImage_Model2D;

    NewDisplay::NativeImage::Encoding encoding = NewDisplay::NativeImage::Encoding::PNG;
    GS::UniString formatStr;
    if (parameters.Get ("format", formatStr)) {
        if (formatStr == "png") {
            encoding = NewDisplay::NativeImage::Encoding::PNG;
        } else if (formatStr == "jpg") {
            encoding = NewDisplay::NativeImage::Encoding::JPEG;
        } else {
            return CreateErrorResponse (APIERR_BADPARS, "Invalid format parameter.");
        }
    }

    UInt32 width = 256;
    UInt32 height = 256;
    parameters.Get ("width", width);
    parameters.Get ("height", height);

    image.offset = 0.001;
    parameters.Get ("offset", image.offset);

    image.scale = 0.005;
    parameters.Get ("scale", image.scale);

    image.backgroundColor = {1.0, 1.0, 1.0};
    GetColor(parameters, "backgroundColor", image.backgroundColor);

    NewDisplay::NativeImage nativeImage (width, height, 32, nullptr);
    image.nativeImagePtr = &nativeImage;
    GSErrCode err = ACAPI_Element_GetRoomImage (&image);
    if (err != NoError) {
        return CreateErrorResponse (err, "Failed to get room image.");
    }

    GS::MemoryOChannel32 memChannel (GS::MemoryOChannel32::BMAllocation);
    if (!nativeImage.Encode (memChannel, encoding)) {
        return CreateErrorResponse (APIERR_GENERAL, "Failed to encode room image.");
    }

    auto str = Base64Converter::Encode (memChannel.GetDestination (), memChannel.GetDataSize ());
    str.DeleteAll (GS::UniChar(char('\n')));
    return GS::ObjectState ("roomImage", str);
}

