#include "ElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "PerfTrace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <limits>
#include <map>
#include <new>
#include <vector>

namespace {

constexpr double GeometryEpsilon = 1.0e-9;
// Sheet frame strokes in the live School/Kərkicahan/Masazır projects are
// tens of model units long.  Short drafting noise is not useful to the
// frame classifier and dominates the projection payload.  Keep this as a
// native projection threshold, not a sheet semantic decision: Node still
// classifies frames and can fall back to the legacy route on coverage/parity
// problems.
constexpr double MinimumProjectionSegmentLength = 10.0;
constexpr Int32 MaxRectangles = 256;
constexpr GSSize MaxNativeElementList = 1'000'000;
constexpr GSSize MaxNativeDiagonalSegments = 100'000;
constexpr GSSize MaxTextContentBytes = 8 * 1024 * 1024;
// The native pass must stay bounded even for an unusually dense diagonal
// drafting layer.  A partial candidate scan is surfaced explicitly; Node may
// never turn it into a negative "not marked" status.
constexpr Int32 MaxStrikeThroughPairChecks = 500000;
constexpr Int32 MaxStrikeThroughPairs = 4096;
constexpr double MinimumStrikeThroughLengthRatio = 0.45;
constexpr double MinimumStrikeThroughAngleDegrees = 20.0;
constexpr double MaximumStrikeThroughAngleDegrees = 80.0;
constexpr double MinimumIntersectionParameter = 0.05;

struct RectSpec {
    GS::UniString id;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    short storyIndex = 0;
    bool centerInside = false;
};

struct DiagonalSegment {
    API_Guid guid = APINULLGuid;
    short floorIndex = 0;
    Int32 pen = 0;
    API_Coord begin = {};
    API_Coord end = {};
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double length = 0.0;
    double angle = 0.0;
};

struct StrikeThroughCandidate {
    DiagonalSegment left;
    DiagonalSegment right;
    API_Coord intersection = {};
    double angleSeparation = 0.0;
};

struct StrikeThroughScan {
    std::vector<StrikeThroughCandidate> candidates;
    Int32 pairChecks = 0;
    bool limited = false;
};

static GS::ObjectState BoundsObject (double xMin, double yMin, double xMax, double yMax)
{
    return GS::ObjectState (
        "xMin", xMin,
        "yMin", yMin,
        "xMax", xMax,
        "yMax", yMax);
}

static GS::ObjectState BoundsObject (const API_Box3D& bounds)
{
    return BoundsObject (bounds.xMin, bounds.yMin, bounds.xMax, bounds.yMax);
}

static bool IsDegenerate (const API_Coord& begin, const API_Coord& end)
{
    return std::abs (begin.x - end.x) <= GeometryEpsilon
        && std::abs (begin.y - end.y) <= GeometryEpsilon;
}

static bool IsFiniteCoord (const API_Coord& coordinate)
{
    return std::isfinite (coordinate.x) && std::isfinite (coordinate.y);
}

static bool IsFiniteBounds (const API_Box3D& bounds)
{
    return std::isfinite (bounds.xMin) && std::isfinite (bounds.yMin) &&
        std::isfinite (bounds.xMax) && std::isfinite (bounds.yMax);
}

static Int32 SaturatingCount (const GSSize count)
{
    return count > static_cast<GSSize> (std::numeric_limits<Int32>::max ())
        ? std::numeric_limits<Int32>::max ()
        : count < 0 ? 0 : static_cast<Int32> (count);
}

static bool IsHorizontal (const API_Coord& begin, const API_Coord& end)
{
    return std::abs (begin.y - end.y) <= GeometryEpsilon
        && std::abs (begin.x - end.x) > GeometryEpsilon;
}

static bool IsVertical (const API_Coord& begin, const API_Coord& end)
{
    return std::abs (begin.x - end.x) <= GeometryEpsilon
        && std::abs (begin.y - end.y) > GeometryEpsilon;
}

static bool IsOrthogonal (const API_Coord& begin, const API_Coord& end)
{
    return IsHorizontal (begin, end) || IsVertical (begin, end);
}

static bool IsTooShort (const API_Coord& begin, const API_Coord& end)
{
    return std::hypot (end.x - begin.x, end.y - begin.y) < MinimumProjectionSegmentLength;
}

static double AcuteAngleDegrees (double left, double right)
{
    constexpr double Pi = 3.14159265358979323846;
    constexpr double HalfPi = Pi / 2.0;
    constexpr double PiPeriod = Pi;
    double delta = std::fmod (std::abs (left - right), PiPeriod);
    if (delta > HalfPi) delta = PiPeriod - delta;
    return delta * 180.0 / Pi;
}

static bool FiniteSegmentIntersection (
    const DiagonalSegment& left,
    const DiagonalSegment& right,
    API_Coord& intersection)
{
    if (!IsFiniteCoord (left.begin) || !IsFiniteCoord (left.end) ||
        !IsFiniteCoord (right.begin) || !IsFiniteCoord (right.end))
        return false;
    const double x1 = left.begin.x;
    const double y1 = left.begin.y;
    const double x2 = left.end.x;
    const double y2 = left.end.y;
    const double x3 = right.begin.x;
    const double y3 = right.begin.y;
    const double x4 = right.end.x;
    const double y4 = right.end.y;
    const double denominator = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if (std::abs (denominator) <= GeometryEpsilon) return false;
    const double leftNumerator = (x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4);
    const double rightNumerator = (x1 - x3) * (y1 - y2) - (y1 - y3) * (x1 - x2);
    const double leftParameter = leftNumerator / denominator;
    const double rightParameter = rightNumerator / denominator;
    if (leftParameter < MinimumIntersectionParameter
        || leftParameter > 1.0 - MinimumIntersectionParameter
        || rightParameter < MinimumIntersectionParameter
        || rightParameter > 1.0 - MinimumIntersectionParameter) {
        return false;
    }
    intersection.x = x1 + leftParameter * (x2 - x1);
    intersection.y = y1 + leftParameter * (y2 - y1);
    return IsFiniteCoord (intersection);
}

static bool BoundsOverlap (const DiagonalSegment& left, const DiagonalSegment& right)
{
    return left.maxX >= right.minX && right.maxX >= left.minX
        && left.maxY >= right.minY && right.maxY >= left.minY;
}

static StrikeThroughScan FindStrikeThroughCandidates (const std::vector<DiagonalSegment>& segments)
{
    // Story and pen are cheap semantic/style partitions.  The sweep below is
    // the broad phase: active segments overlap in X, then Y/angle/length are
    // checked before the exact finite intersection.  This avoids the raw
    // O(number-of-all-lines^2) pair search while retaining only compact hits.
    std::map<std::pair<short, Int32>, std::vector<DiagonalSegment>> groups;
    for (const DiagonalSegment& segment : segments) {
        groups[{ segment.floorIndex, segment.pen }].push_back (segment);
    }

    StrikeThroughScan result;
    for (auto& groupEntry : groups) {
        auto& group = groupEntry.second;
        std::stable_sort (group.begin (), group.end (), [] (const DiagonalSegment& left, const DiagonalSegment& right) {
            if (left.minX != right.minX) return left.minX < right.minX;
            if (left.minY != right.minY) return left.minY < right.minY;
            return left.length < right.length;
        });
        std::vector<size_t> active;
        active.reserve (group.size ());
        for (size_t currentIndex = 0; currentIndex < group.size (); ++currentIndex) {
            const DiagonalSegment& current = group[currentIndex];
            active.erase (std::remove_if (active.begin (), active.end (), [&group, &current] (size_t index) {
                return group[index].maxX < current.minX - GeometryEpsilon;
            }), active.end ());

            for (const size_t activeIndex : active) {
                if (++result.pairChecks > MaxStrikeThroughPairChecks) {
                    result.limited = true;
                    return result;
                }
                const DiagonalSegment& previous = group[activeIndex];
                if (!BoundsOverlap (previous, current)) continue;
                if (previous.length <= 1.0e-6 || current.length <= 1.0e-6) continue;
                const double lengthRatio = std::min (previous.length, current.length)
                    / std::max (previous.length, current.length);
                if (lengthRatio < MinimumStrikeThroughLengthRatio) continue;
                const double angleSeparation = AcuteAngleDegrees (previous.angle, current.angle);
                if (angleSeparation < MinimumStrikeThroughAngleDegrees
                    || angleSeparation > MaximumStrikeThroughAngleDegrees) continue;
                API_Coord intersection = {};
                if (!FiniteSegmentIntersection (previous, current, intersection)) continue;
                result.candidates.push_back ({ previous, current, intersection, angleSeparation });
                if (static_cast<Int32> (result.candidates.size ()) >= MaxStrikeThroughPairs) {
                    result.limited = true;
                    return result;
                }
            }
            active.push_back (currentIndex);
        }
    }
    return result;
}

static GS::ObjectState StrikeThroughLineObjectState (const DiagonalSegment& line)
{
    return GS::ObjectState (
        "sourceGuid", CreateGuidObjectState (line.guid),
        "sourceType", "Line",
        "floorIndex", line.floorIndex,
        "pen", line.pen,
        "begin", Create2DCoordinateObjectState (line.begin),
        "end", Create2DCoordinateObjectState (line.end),
        "bounds", BoundsObject (line.minX, line.minY, line.maxX, line.maxY),
        "length", line.length,
        "angle", line.angle);
}

static GS::ObjectState StrikeThroughPairObjectState (const StrikeThroughCandidate& candidate)
{
    return GS::ObjectState (
        "left", StrikeThroughLineObjectState (candidate.left),
        "right", StrikeThroughLineObjectState (candidate.right),
        "intersection", Create2DCoordinateObjectState (candidate.intersection),
        "angleSeparation", candidate.angleSeparation);
}

static GS::UniString OrientationOf (const API_Coord& begin, const API_Coord& end)
{
    return IsHorizontal (begin, end) ? "horizontal" : "vertical";
}

static GS::ObjectState SegmentObjectState (
    const API_Guid& guid,
    const GS::UniString& sourceType,
    Int32 sourceSegmentIndex,
    short floorIndex,
    const API_Coord& begin,
    const API_Coord& end)
{
    const double xMin = std::min (begin.x, end.x);
    const double yMin = std::min (begin.y, end.y);
    const double xMax = std::max (begin.x, end.x);
    const double yMax = std::max (begin.y, end.y);
    return GS::ObjectState (
        "sourceGuid", CreateGuidObjectState (guid),
        "sourceType", sourceType,
        "sourceSegmentIndex", sourceSegmentIndex,
        "floorIndex", floorIndex,
        "begin", Create2DCoordinateObjectState (begin),
        "end", Create2DCoordinateObjectState (end),
        "bounds", BoundsObject (xMin, yMin, xMax, yMax),
        "orientation", OrientationOf (begin, end),
        "length", std::hypot (end.x - begin.x, end.y - begin.y));
}

static bool ReadRectSpec (const GS::ObjectState& objectState, RectSpec& rect, GS::UniString& error)
{
    if (!objectState.Get ("rectId", rect.id) || rect.id.IsEmpty ()) {
        error = "Each rectangle requires a non-empty rectId.";
        return false;
    }
    const GS::ObjectState* bounds = objectState.Get ("bounds");
    if (bounds == nullptr
        || !bounds->Get ("xMin", rect.xMin)
        || !bounds->Get ("yMin", rect.yMin)
        || !bounds->Get ("xMax", rect.xMax)
        || !bounds->Get ("yMax", rect.yMax)
        || !std::isfinite (rect.xMin) || !std::isfinite (rect.yMin)
        || !std::isfinite (rect.xMax) || !std::isfinite (rect.yMax)
        || !(rect.xMin < rect.xMax && rect.yMin < rect.yMax)) {
        error = GS::UniString::Printf ("Rectangle '%T' must have positive bounds.", rect.id.ToPrintf ());
        return false;
    }

    Int32 storyIndex = 0;
    if (!objectState.Get ("storyIndex", storyIndex)
        || storyIndex < SHRT_MIN || storyIndex > SHRT_MAX) {
        error = GS::UniString::Printf ("Rectangle '%T' requires a valid storyIndex.", rect.id.ToPrintf ());
        return false;
    }
    rect.storyIndex = static_cast<short> (storyIndex);

    GS::UniString match;
    if (!objectState.Get ("match", match)
        || (match != "center_inside" && match != "bounds_intersect")) {
        error = GS::UniString::Printf ("Rectangle '%T' requires match center_inside or bounds_intersect.", rect.id.ToPrintf ());
        return false;
    }
    rect.centerInside = match == "center_inside";
    return true;
}

static bool MatchesRect (const RectSpec& rect, short floorIndex, const API_Box3D& bounds)
{
    if (rect.storyIndex != floorIndex) return false;
    if (rect.centerInside) {
        const double centerX = (bounds.xMin + bounds.xMax) / 2.0;
        const double centerY = (bounds.yMin + bounds.yMax) / 2.0;
        return centerX >= rect.xMin && centerX <= rect.xMax
            && centerY >= rect.yMin && centerY <= rect.yMax;
    }
    return bounds.xMax >= rect.xMin && bounds.xMin <= rect.xMax
        && bounds.yMax >= rect.yMin && bounds.yMin <= rect.yMax;
}

static bool ResolveFontName (Int32 fontIndex, GS::UniString& name)
{
    if (fontIndex <= 0) return false;
#ifdef ServerMainVers_2700
    API_FontType font = {};
    font.head.index = fontIndex;
    if (ACAPI_Font_GetFont (font) != NoError) return false;
    name = GS::UniString (font.head.name);
    return !name.IsEmpty ();
#else
    API_Attribute font = {};
    font.header.typeID = API_FontID;
    font.header.index = fontIndex;
    if (ACAPI_Attribute_Get (&font) != NoError) return false;
    name = GS::UniString (font.font.head.name);
    return !name.IsEmpty ();
#endif
}

static void AddTextObject (
    GS::ObjectState& target,
    const API_Element& element,
    const API_Box3D& bounds,
    Int32& readErrorCount)
{
    target.Add ("sourceGuid", CreateGuidObjectState (element.header.guid));
    target.Add ("sourceType", "Text");
    target.Add ("floorIndex", element.header.floorInd);
    target.Add ("bounds", BoundsObject (bounds));
    target.Add ("angle", element.text.angle);
    target.Add ("size", element.text.size);
    target.Add ("pen", static_cast<Int32> (element.text.pen));
    target.Add ("fontIndex", static_cast<Int32> (element.text.font));

    GS::UniString fontName;
    if (ResolveFontName (static_cast<Int32> (element.text.font), fontName)) {
        target.Add ("fontName", fontName);
        target.Add ("fontResolutionStatus", "resolved");
    } else {
        // Do not manufacture a font name: Node must keep AzLat evidence
        // unavailable instead of silently guessing an encoding.
        target.Add ("fontResolutionStatus", "unavailable");
    }

    API_ElementMemo memo = {};
    const GS::OnExit guard ([&memo] () { ACAPI_DisposeElemMemoHdls (&memo); });
    const GSErrCode memoError = ACAPI_Element_GetMemo (
        element.header.guid,
        &memo,
        APIMemoMask_TextContentUni);
    bool contentAvailable = false;
#ifdef ServerMainVers_2800
    if (memoError == NoError && memo.textContent != nullptr &&
        memo.textContent->GetLength () <= static_cast<USize> (MaxTextContentBytes)) {
        target.Add ("content", *memo.textContent);
        contentAvailable = true;
    }
#else
    if (memoError == NoError && memo.textContent != nullptr && *memo.textContent != nullptr) {
        const GSSize textBytes = BMhGetSize (reinterpret_cast<GSHandle> (memo.textContent));
        if (textBytes > 0 && textBytes <= MaxTextContentBytes) {
            const char* text = *memo.textContent;
            GSSize textLength = 0;
            while (textLength < textBytes && text[textLength] != '\0') ++textLength;
            if (textLength < textBytes) {
                target.Add ("content", GS::UniString (reinterpret_cast<const GS::uchar_t*> (text)));
                contentAvailable = true;
            }
        }
    }
#endif
    if (contentAvailable) {
        target.Add ("contentStatus", "available");
    } else {
        target.Add ("contentStatus", "unavailable");
        ++readErrorCount;
    }
}

} // namespace

GetOrthogonalDraftingProjectionCommand::GetOrthogonalDraftingProjectionCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetOrthogonalDraftingProjectionCommand::GetName () const
{
    return "GetOrthogonalDraftingProjection";
}

GS::Optional<GS::UniString> GetOrthogonalDraftingProjectionCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        }
    })";
}

GS::Optional<GS::UniString> GetOrthogonalDraftingProjectionCommand::GetResponseSchema () const
{
    // Large projections are intentionally not response-schema validated.
    return {};
}

GS::ObjectState GetOrthogonalDraftingProjectionCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");

    try {
    PerfTrace::ResetAccumulator ();
    const auto started = std::chrono::steady_clock::now ();
    GS::ObjectState response;
    const auto& segments = response.AddList<GS::ObjectState> ("segments");

    Int32 enumeratedLineCount = 0;
    Int32 enumeratedPolyLineCount = 0;
    Int32 emittedSegmentCount = 0;
    Int32 discardedArcCount = 0;
    Int32 discardedDiagonalLineCount = 0;
    Int32 discardedArcPolyLineCount = 0;
    Int32 discardedDiagonalPolyLineCount = 0;
    Int32 discardedDegenerateCount = 0;
    Int32 discardedShortSegmentCount = 0;
    Int32 readErrorCount = 0;
    std::vector<DiagonalSegment> diagonalSegments;
    bool diagonalInputLimited = false;

    // These lists are only used to make the coverage counters explicit. No
    // element payload is read for curved/fill types.
    for (const API_ElemTypeID curvedType : { API_ArcID, API_CircleID, API_HatchID }) {
        GS::Array<API_Guid> curvedElements;
        if (ACAPI_Element_GetElemList (curvedType, &curvedElements, APIFilt_None) == NoError) {
            if (curvedElements.GetSize () > MaxNativeElementList)
                return CreateNativeResourceLimitResponse ("The native curved-element catalog exceeds its host safety limit; no partial rows were returned.", SaturatingCount (curvedElements.GetSize ()), 0);
            discardedArcCount += SaturatingCount (curvedElements.GetSize ());
        } else {
            ++readErrorCount;
        }
    }

    GS::Array<API_Guid> lineElements;
    if (ACAPI_Element_GetElemList (API_LineID, &lineElements, APIFilt_None) != NoError) {
        ++readErrorCount;
    } else {
        if (lineElements.GetSize () > MaxNativeElementList)
            return CreateNativeResourceLimitResponse ("The native Line catalog exceeds its host safety limit; no partial rows were returned.", SaturatingCount (lineElements.GetSize ()), 0);
        enumeratedLineCount = SaturatingCount (lineElements.GetSize ());
        for (const API_Guid& guid : lineElements) {
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) {
                ++readErrorCount;
                continue;
            }
            const API_Coord begin = element.line.begC;
            const API_Coord end = element.line.endC;
            if (!IsFiniteCoord (begin) || !IsFiniteCoord (end)) {
                ++readErrorCount;
                continue;
            }
            if (IsDegenerate (begin, end)) {
                ++discardedDegenerateCount;
            } else if (!IsOrthogonal (begin, end)) {
                ++discardedDiagonalLineCount;
                const double length = std::hypot (end.x - begin.x, end.y - begin.y);
                if (length >= MinimumProjectionSegmentLength) {
                    const GSSize diagonalLimit = std::min (
                        static_cast<GSSize> (pageSize) * 4,
                        MaxNativeDiagonalSegments);
                    if (diagonalSegments.size () < diagonalLimit) {
                        diagonalSegments.push_back ({
                            guid,
                            element.header.floorInd,
                            static_cast<Int32> (element.line.linePen.penIndex),
                            begin,
                            end,
                            std::min (begin.x, end.x),
                            std::min (begin.y, end.y),
                            std::max (begin.x, end.x),
                            std::max (begin.y, end.y),
                            length,
                            std::atan2 (end.y - begin.y, end.x - begin.x),
                        });
                    } else {
                        diagonalInputLimited = true;
                    }
                }
            } else if (IsTooShort (begin, end)) {
                ++discardedShortSegmentCount;
            } else {
                if (emittedSegmentCount >= pageSize)
                    return CreateNativePageRequiredResponse ("The orthogonal projection requires another page; no partial rows were returned.", enumeratedLineCount + enumeratedPolyLineCount, emittedSegmentCount);
                segments (SegmentObjectState (guid, "Line", 0, element.header.floorInd, begin, end));
                ++emittedSegmentCount;
            }
        }
    }

    GS::Array<API_Guid> polyLineElements;
    if (ACAPI_Element_GetElemList (API_PolyLineID, &polyLineElements, APIFilt_None) != NoError) {
        ++readErrorCount;
    } else {
        if (polyLineElements.GetSize () > MaxNativeElementList)
            return CreateNativeResourceLimitResponse ("The native PolyLine catalog exceeds its host safety limit; no partial rows were returned.", SaturatingCount (polyLineElements.GetSize ()), 0);
        enumeratedPolyLineCount = SaturatingCount (polyLineElements.GetSize ());
        for (const API_Guid& guid : polyLineElements) {
            const std::vector<PolygonData> polygons = GetPolygonsFromMemoCoords (guid);
            if (polygons.empty ()) {
                ++readErrorCount;
                continue;
            }
            bool hasArc = false;
            Int32 arcCount = 0;
            bool hasDiagonal = false;
            bool hasDegenerate = false;
            Int32 segmentIndex = 0;
            for (const PolygonData& polygon : polygons) {
                if (!polygon.arcs.empty ()) {
                    hasArc = true;
                    arcCount += static_cast<Int32> (polygon.arcs.size ());
                }
                for (size_t index = 1; index < polygon.coords.size (); ++index) {
                    const API_Coord& begin = polygon.coords[index - 1];
                    const API_Coord& end = polygon.coords[index];
                    if (!IsFiniteCoord (begin) || !IsFiniteCoord (end)) {
                        hasDegenerate = true;
                        continue;
                    }
                    if (IsDegenerate (begin, end)) {
                        hasDegenerate = true;
                    } else if (!IsOrthogonal (begin, end)) {
                        hasDiagonal = true;
                    }
                    ++segmentIndex;
                }
            }
            if (hasArc) {
                discardedArcPolyLineCount++;
                discardedArcCount += arcCount;
                continue;
            }
            if (hasDiagonal) {
                ++discardedDiagonalPolyLineCount;
                continue;
            }
            if (hasDegenerate) ++discardedDegenerateCount;

            API_Elem_Head header = {};
            header.guid = guid;
            short floorIndex = 0;
            if (ACAPI_Element_GetHeader (&header) == NoError) {
                floorIndex = header.floorInd;
            } else {
                ++readErrorCount;
            }
            Int32 emittedIndex = 0;
            for (const PolygonData& polygon : polygons) {
                for (size_t index = 1; index < polygon.coords.size (); ++index) {
                    const API_Coord& begin = polygon.coords[index - 1];
                    const API_Coord& end = polygon.coords[index];
                    if (IsDegenerate (begin, end)) continue;
                    if (IsTooShort (begin, end)) {
                        ++discardedShortSegmentCount;
                        continue;
                    }
                    if (emittedSegmentCount >= pageSize)
                        return CreateNativePageRequiredResponse ("The orthogonal projection requires another page; no partial rows were returned.", enumeratedLineCount + enumeratedPolyLineCount, emittedSegmentCount);
                    segments (SegmentObjectState (guid, "PolyLine", emittedIndex++, floorIndex, begin, end));
                    ++emittedSegmentCount;
                }
            }
        }
    }

    const StrikeThroughScan strikeThroughScan = FindStrikeThroughCandidates (diagonalSegments);
    const auto& strikeThroughPairs = response.AddList<GS::ObjectState> ("strikeThroughPairs");
    for (const StrikeThroughCandidate& candidate : strikeThroughScan.candidates)
        strikeThroughPairs (StrikeThroughPairObjectState (candidate));

    response.Add ("enumeratedLineCount", enumeratedLineCount);
    response.Add ("enumeratedPolyLineCount", enumeratedPolyLineCount);
    response.Add ("emittedSegmentCount", emittedSegmentCount);
    response.Add ("discardedArcCount", discardedArcCount);
    response.Add ("discardedDiagonalLineCount", discardedDiagonalLineCount);
    response.Add ("discardedArcPolyLineCount", discardedArcPolyLineCount);
    response.Add ("discardedDiagonalPolyLineCount", discardedDiagonalPolyLineCount);
    response.Add ("discardedDegenerateCount", discardedDegenerateCount);
    response.Add ("discardedShortSegmentCount", discardedShortSegmentCount);
    response.Add ("readErrorCount", readErrorCount);
    response.Add ("coverageStatus", readErrorCount == 0 ? "complete" : "partial");
    response.Add ("enumeratedDiagonalLineCount", static_cast<Int32> (diagonalSegments.size ()));
    response.Add ("strikeThroughPairChecks", strikeThroughScan.pairChecks);
    response.Add ("strikeThroughCandidateCount", static_cast<Int32> (strikeThroughScan.candidates.size ()));
    response.Add ("strikeThroughCoverageStatus", (strikeThroughScan.limited || diagonalInputLimited || readErrorCount != 0) ? "partial" : "complete");
    response.Add ("strikeThroughCandidateLimit", MaxStrikeThroughPairs);
    PerfTrace::SetRequestedReturned (enumeratedLineCount + enumeratedPolyLineCount, emittedSegmentCount);
    PerfTrace::WriteLine (
        GS::UniString (GetName ()),
        PerfTrace::NextSeq (),
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ());
    return response;
    } catch (const std::bad_alloc&) {
        return CreateNativeResourceLimitResponse (
            "The orthogonal projection could not allocate its required working set; no partial rows were returned.",
            0,
            0);
    }
}

GetElementsInRectsCommand::GetElementsInRectsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String GetElementsInRectsCommand::GetName () const
{
    return "GetElementsInRects";
}

GS::Optional<GS::UniString> GetElementsInRectsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "rects": {
                "type": "array",
                "minItems": 1,
                "maxItems": 256,
                "items": {
                    "type": "object",
                    "properties": {
                        "rectId": { "type": "string", "minLength": 1 },
                        "bounds": {
                            "type": "object",
                            "properties": {
                                "xMin": { "type": "number" }, "yMin": { "type": "number" },
                                "xMax": { "type": "number" }, "yMax": { "type": "number" }
                            },
                            "additionalProperties": false,
                            "required": ["xMin", "yMin", "xMax", "yMax"]
                        },
                        "storyIndex": { "type": "integer" },
                        "match": { "type": "string", "enum": ["center_inside", "bounds_intersect"] }
                    },
                    "additionalProperties": false,
                    "required": ["rectId", "bounds", "storyIndex", "match"]
                }
            },
            "includeTypes": {
                "type": "array",
                "minItems": 1,
                "items": { "type": "string", "enum": ["Text"] }
            },
            "pageSize": {
                "type": "integer",
                "minimum": 1
            }
        },
        "additionalProperties": false,
        "required": ["rects", "includeTypes"]
    })";
}

GS::Optional<GS::UniString> GetElementsInRectsCommand::GetResponseSchema () const
{
    // Text batches can still be large; keep the framework out of the hot
    // path and return explicit coverage/status fields for Node validation.
    return {};
}

GS::ObjectState GetElementsInRectsCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    try {
    GS::Array<GS::ObjectState> rectObjects;
    if (!parameters.Get ("rects", rectObjects) || rectObjects.IsEmpty ())
        return CreateErrorResponse (APIERR_BADPARS, "rects must contain at least one rectangle.");
    if (rectObjects.GetSize () > MaxRectangles)
        return CreateErrorResponse (APIERR_BADPARS, "GetElementsInRects accepts at most 256 rectangles per call.");

    GS::Array<GS::UniString> includeTypes;
    if (!parameters.Get ("includeTypes", includeTypes) || includeTypes.IsEmpty ())
        return CreateErrorResponse (APIERR_BADPARS, "includeTypes must contain Text.");
    for (const GS::UniString& type : includeTypes) {
        if (type != "Text")
            return CreateErrorResponse (APIERR_NOTSUPPORTED, "GetElementsInRects currently supports includeTypes=[Text] only.");
    }

    Int32 pageSize = std::numeric_limits<Int32>::max ();
    if (!ReadNativePageSize (parameters, pageSize, pageSize))
        return CreateErrorResponse (APIERR_BADPARS, "pageSize must be a positive integer.");

    std::vector<RectSpec> rects;
    rects.reserve (rectObjects.GetSize ());
    for (const GS::ObjectState& objectState : rectObjects) {
        RectSpec rect;
        GS::UniString error;
        if (!ReadRectSpec (objectState, rect, error))
            return CreateErrorResponse (APIERR_BADPARS, error);
        if (std::any_of (rects.begin (), rects.end (), [&rect] (const RectSpec& existing) {
            return existing.id == rect.id;
        }))
            return CreateErrorResponse (APIERR_BADPARS, GS::UniString::Printf ("Duplicate rectId '%T'.", rect.id.ToPrintf ()));
        rects.push_back (rect);
    }

    // Build each group's text list exactly once, when the final response is
    // assembled.  ObjectState::AddList returns a value-backed adder; calling
    // AddList("texts") once during initialization and again for each match
    // can leave the appended values detached from the group that is returned.
    std::vector<std::vector<GS::ObjectState>> textsByRect (rects.size ());

    GS::Array<API_Guid> textElements;
    Int32 readErrorCount = 0;
    Int32 matchedTextCount = 0;
    if (ACAPI_Element_GetElemList (API_TextID, &textElements, APIFilt_None) != NoError) {
        ++readErrorCount;
    } else {
        if (textElements.GetSize () > MaxNativeElementList)
            return CreateNativeResourceLimitResponse ("The native Text catalog exceeds its host safety limit; no partial rows were returned.", SaturatingCount (textElements.GetSize ()), 0);
        for (const API_Guid& guid : textElements) {
            API_Elem_Head header = {};
            header.guid = guid;
            if (ACAPI_Element_GetHeader (&header) != NoError) {
                ++readErrorCount;
                continue;
            }
            API_Box3D bounds = {};
            if (ACAPI_Element_CalcBounds (&header, &bounds) != NoError) {
                ++readErrorCount;
                continue;
            }
            if (!IsFiniteBounds (bounds)) {
                ++readErrorCount;
                continue;
            }

            // Rectangles are independent focus groups.  Deliberate overlap is
            // valid: one Text is copied into every matching rectId group.
            std::vector<size_t> matches;
            for (size_t index = 0; index < rects.size (); ++index) {
                if (MatchesRect (rects[index], header.floorInd, bounds)) matches.push_back (index);
            }
            if (matches.empty ()) continue;
            if (matchedTextCount + static_cast<Int32> (matches.size ()) > pageSize)
                return CreateNativePageRequiredResponse ("The grouped Text projection requires another page; no partial rows were returned.", static_cast<Int32> (textElements.GetSize ()), matchedTextCount);

            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) {
                ++readErrorCount;
                continue;
            }
            for (const size_t groupIndex : matches) {
                GS::ObjectState textObject;
                AddTextObject (textObject, element, bounds, readErrorCount);
                textsByRect[groupIndex].push_back (textObject);
            }
            matchedTextCount += static_cast<Int32> (matches.size ());
        }
    }

    // Rebuild the groups after the per-group text arrays were filled. Object
    // state adders are value-backed, so this keeps the output deterministic on
    // both AC25 and AC28 rather than relying on reference invalidation rules.
    GS::ObjectState finalResponse;
    const auto& finalGroups = finalResponse.AddList<GS::ObjectState> ("groups");
    Int32 returnedTextCount = 0;
    for (size_t index = 0; index < rects.size (); ++index) {
        GS::ObjectState group ("rectId", rects[index].id);
        const auto& appendText = group.AddList<GS::ObjectState> ("texts");
        for (const GS::ObjectState& text : textsByRect[index]) {
            appendText (text);
            ++returnedTextCount;
        }
        finalGroups (group);
    }
    finalResponse.Add ("enumeratedTextCount", SaturatingCount (textElements.GetSize ()));
    finalResponse.Add ("returnedTextCount", returnedTextCount);
    finalResponse.Add ("readErrorCount", readErrorCount);
    finalResponse.Add ("coverageStatus", readErrorCount == 0 ? "complete" : "partial");
    return finalResponse;
    } catch (const std::bad_alloc&) {
        return CreateNativeResourceLimitResponse (
            "The grouped Text projection could not allocate its required working set; no partial rows were returned.",
            0,
            0);
    }
}
