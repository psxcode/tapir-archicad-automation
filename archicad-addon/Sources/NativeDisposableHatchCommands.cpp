#include "NativeDisposableHatchCommands.hpp"

#if defined (TAPIR_ENABLE_NATIVE_DISPOSABLE_HATCH)

#include "hatch-contract.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

static_assert (tapir::disposable_channel::kNativeHatchMaxItems == 2, "Offline Hatch candidate batch bound changed");
static_assert (tapir::disposable_channel::kNativeHatchMaxVertices == 64, "Offline Hatch vertex bound changed");

namespace {

using tapir::disposable_channel::NativeHatchPoint2;
using tapir::disposable_channel::NativeHatchPolygon;
using tapir::disposable_channel::NativeHatchState;

bool SamePoint (const NativeHatchPoint2& left, const NativeHatchPoint2& right)
{
    return left.x == right.x && left.y == right.y;
}

double Cross (const NativeHatchPoint2& a, const NativeHatchPoint2& b, const NativeHatchPoint2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool PointOnSegment (const NativeHatchPoint2& point,
                    const NativeHatchPoint2& start,
                    const NativeHatchPoint2& end)
{
    if (Cross (start, end, point) != 0.0) return false;
    return point.x >= std::min (start.x, end.x) && point.x <= std::max (start.x, end.x)
        && point.y >= std::min (start.y, end.y) && point.y <= std::max (start.y, end.y);
}

bool SegmentsIntersect (const NativeHatchPoint2& a,
                        const NativeHatchPoint2& b,
                        const NativeHatchPoint2& c,
                        const NativeHatchPoint2& d)
{
    const double abC = Cross (a, b, c);
    const double abD = Cross (a, b, d);
    const double cdA = Cross (c, d, a);
    const double cdB = Cross (c, d, b);
    if (((abC > 0.0 && abD < 0.0) || (abC < 0.0 && abD > 0.0))
        && ((cdA > 0.0 && cdB < 0.0) || (cdA < 0.0 && cdB > 0.0))) return true;
    return (abC == 0.0 && PointOnSegment (c, a, b))
        || (abD == 0.0 && PointOnSegment (d, a, b))
        || (cdA == 0.0 && PointOnSegment (a, c, d))
        || (cdB == 0.0 && PointOnSegment (b, c, d));
}

bool AdjacentEdge (std::size_t left, std::size_t right, std::size_t count)
{
    return right == left + 1 || (left == 0 && right == count - 1);
}

bool ValidSimpleExternalRing (const NativeHatchPolygon& polygon)
{
    if (polygon.coordinates.size () < 3 || polygon.coordinates.size () > tapir::disposable_channel::kNativeHatchMaxVertices)
        return false;
    double area = 0.0;
    for (std::size_t index = 0; index < polygon.coordinates.size (); ++index) {
        const NativeHatchPoint2& current = polygon.coordinates[index];
        const NativeHatchPoint2& next = polygon.coordinates[(index + 1) % polygon.coordinates.size ()];
        if (!std::isfinite (current.x) || !std::isfinite (current.y) || SamePoint (current, next)) return false;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (SamePoint (current, polygon.coordinates[previous])) return false;
        area += current.x * next.y - next.x * current.y;
    }
    if (!std::isfinite (area) || area == 0.0) return false;
    for (std::size_t left = 0; left < polygon.coordinates.size (); ++left) {
        const NativeHatchPoint2& leftEnd = polygon.coordinates[(left + 1) % polygon.coordinates.size ()];
        for (std::size_t right = left + 1; right < polygon.coordinates.size (); ++right) {
            const NativeHatchPoint2& rightStart = polygon.coordinates[right];
            const NativeHatchPoint2& rightEnd = polygon.coordinates[(right + 1) % polygon.coordinates.size ()];
            if (!SegmentsIntersect (polygon.coordinates[left], leftEnd, rightStart, rightEnd)) continue;
            if (!AdjacentEdge (left, right, polygon.coordinates.size ())) return false;
            const NativeHatchPoint2& shared = left == 0 && right == polygon.coordinates.size () - 1
                ? polygon.coordinates[0] : rightStart;
            const NativeHatchPoint2 endpoints[] = { polygon.coordinates[left], leftEnd, rightStart, rightEnd };
            for (const NativeHatchPoint2& candidate : endpoints) {
                if (!SamePoint (candidate, shared)
                    && PointOnSegment (candidate, polygon.coordinates[left], leftEnd)
                    && PointOnSegment (candidate, rightStart, rightEnd)) return false;
            }
        }
    }
    return true;
}

bool ValidRetrievedSimpleRingMemo (const API_Polygon& polygon, const API_ElementMemo& source)
{
    if (polygon.nCoords < 4 || polygon.nCoords > static_cast<Int32> (tapir::disposable_channel::kNativeHatchMaxVertices + 1)
        || polygon.nSubPolys != 1 || polygon.nArcs != 0 || source.parcs != nullptr
        || source.coords == nullptr || *source.coords == nullptr
        || source.pends == nullptr || *source.pends == nullptr
        || source.vertexIDs == nullptr || *source.vertexIDs == nullptr
        || source.edgeIDs == nullptr || *source.edgeIDs == nullptr
        || source.contourIDs == nullptr || *source.contourIDs == nullptr) return false;

    const GSSize coordinateCount = static_cast<GSSize> (polygon.nCoords) + 1;
    if (BMhGetSize ((GSHandle) source.coords) != coordinateCount * sizeof (API_Coord)
        || BMhGetSize ((GSHandle) source.pends) != 2 * sizeof (Int32)
        || BMhGetSize ((GSHandle) source.vertexIDs) != coordinateCount * sizeof (UInt32)
        || BMhGetSize ((GSHandle) source.edgeIDs) != coordinateCount * sizeof (UInt32)
        || BMhGetSize ((GSHandle) source.contourIDs) != coordinateCount * sizeof (UInt32)) return false;

    if ((*source.coords)[0].x != 0.0 || (*source.coords)[0].y != 0.0
        || (*source.pends)[0] != 0 || (*source.pends)[1] != polygon.nCoords
        || (*source.coords)[polygon.nCoords].x != (*source.coords)[1].x
        || (*source.coords)[polygon.nCoords].y != (*source.coords)[1].y
        || (*source.vertexIDs)[polygon.nCoords] != (*source.vertexIDs)[1]
        || (*source.edgeIDs)[polygon.nCoords] != (*source.edgeIDs)[1]
        || (*source.contourIDs)[polygon.nCoords] != (*source.contourIDs)[1]) return false;

    try {
        NativeHatchPolygon ring;
        ring.coordinates.reserve (static_cast<std::size_t> (polygon.nCoords - 1));
        for (Int32 index = 1; index < polygon.nCoords; ++index)
            ring.coordinates.push_back ({ (*source.coords)[index].x, (*source.coords)[index].y });
        return ValidSimpleExternalRing (ring);
    } catch (...) {
        return false;
    }
}

bool SamePolygon (const NativeHatchPolygon& left, const NativeHatchPolygon& right)
{
    if (left.coordinates.size () != right.coordinates.size ()) return false;
    for (std::size_t index = 0; index < left.coordinates.size (); ++index)
        if (!SamePoint (left.coordinates[index], right.coordinates[index])) return false;
    return true;
}

// Dedicated, unregistered simple-Hatch memo owner.  The API_ElementMemo is
// value-initialized before any allocation/API call; its raw handles never
// escape this noncopyable owner and no populated destination is overwritten.
class NativeSimpleHatchMemoOwner final {
public:
    NativeSimpleHatchMemoOwner () = default;
    ~NativeSimpleHatchMemoOwner () noexcept
    {
        DisposeExactlyOnce ();
    }

    NativeSimpleHatchMemoOwner (const NativeSimpleHatchMemoOwner&) = delete;
    NativeSimpleHatchMemoOwner& operator= (const NativeSimpleHatchMemoOwner&) = delete;
    NativeSimpleHatchMemoOwner (NativeSimpleHatchMemoOwner&&) = delete;
    NativeSimpleHatchMemoOwner& operator= (NativeSimpleHatchMemoOwner&&) = delete;

    bool IsPoisoned () const noexcept { return poisoned; }

    GSErrCode BuildCreateMemo (API_Polygon& polygon, const NativeHatchPolygon& source)
    {
        if (HasAnyHandle () || poisoned || disposed) return Fail (APIERR_BADPARS);
        try {
            if (!ValidSimpleExternalRing (source)) return Fail (APIERR_BADPARS);
            const GSSize vertexCount = static_cast<GSSize> (source.coordinates.size ());
            if (vertexCount > static_cast<GSSize> (std::numeric_limits<Int32>::max () - 1)
                || vertexCount + 2 > std::numeric_limits<GSSize>::max () / sizeof (API_Coord)
                || 2 > std::numeric_limits<GSSize>::max () / sizeof (Int32)
                || vertexCount + 2 > std::numeric_limits<GSSize>::max () / sizeof (UInt32)) return Fail (APIERR_BADPARS);

            memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle ((vertexCount + 2) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
            memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
            memo.vertexIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((vertexCount + 2) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            memo.edgeIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((vertexCount + 2) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            memo.contourIDs = reinterpret_cast<UInt32**> (BMAllocateHandle ((vertexCount + 2) * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            if (memo.coords == nullptr || *memo.coords == nullptr || memo.pends == nullptr || *memo.pends == nullptr
                || memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr || memo.edgeIDs == nullptr || *memo.edgeIDs == nullptr
                || memo.contourIDs == nullptr || *memo.contourIDs == nullptr) return Fail (APIERR_MEMFULL);

            (*memo.coords)[0] = { 0.0, 0.0 };
            for (GSSize index = 0; index < vertexCount; ++index) {
                (*memo.coords)[index + 1] = { source.coordinates[index].x, source.coordinates[index].y };
                (*memo.vertexIDs)[index + 1] = 0;
                (*memo.edgeIDs)[index + 1] = 0;
                (*memo.contourIDs)[index + 1] = 0;
            }
            (*memo.coords)[vertexCount + 1] = (*memo.coords)[1];
            (*memo.vertexIDs)[vertexCount + 1] = 0;
            (*memo.edgeIDs)[vertexCount + 1] = 0;
            (*memo.contourIDs)[vertexCount + 1] = 0;
            (*memo.pends)[0] = 0;
            (*memo.pends)[1] = static_cast<Int32> (vertexCount + 1);
            polygon.nCoords = static_cast<Int32> (vertexCount + 1);
            polygon.nSubPolys = 1;
            polygon.nArcs = 0;
            return NoError;
        } catch (...) {
            return Fail (APIERR_MEMFULL);
        }
    }

    // Copy retrieved IDs into this owner without borrowing or modifying the
    // source.  Retrieved simple Hatch geometry remains one contour with no
    // arcs; closure/sentinel entries are copied as factual IDs.
    GSErrCode AdoptRetrievedMemo (API_Polygon& polygon, const API_ElementMemo& source)
    {
        if (HasAnyHandle () || poisoned || disposed || !ValidRetrievedSimpleRingMemo (polygon, source)) return Fail (APIERR_BADPARS);
        try {
            const GSSize coordinateCount = static_cast<GSSize> (polygon.nCoords) + 1;
            if (coordinateCount > std::numeric_limits<GSSize>::max () / sizeof (API_Coord)
                || 2 > std::numeric_limits<GSSize>::max () / sizeof (Int32)
                || coordinateCount > std::numeric_limits<GSSize>::max () / sizeof (UInt32)) return Fail (APIERR_BADPARS);
            memo.coords = reinterpret_cast<API_Coord**> (BMAllocateHandle (coordinateCount * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
            memo.pends = reinterpret_cast<Int32**> (BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
            memo.vertexIDs = reinterpret_cast<UInt32**> (BMAllocateHandle (coordinateCount * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            memo.edgeIDs = reinterpret_cast<UInt32**> (BMAllocateHandle (coordinateCount * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            memo.contourIDs = reinterpret_cast<UInt32**> (BMAllocateHandle (coordinateCount * sizeof (UInt32), ALLOCATE_CLEAR, 0));
            if (memo.coords == nullptr || *memo.coords == nullptr || memo.pends == nullptr || *memo.pends == nullptr
                || memo.vertexIDs == nullptr || *memo.vertexIDs == nullptr || memo.edgeIDs == nullptr || *memo.edgeIDs == nullptr
                || memo.contourIDs == nullptr || *memo.contourIDs == nullptr) return Fail (APIERR_MEMFULL);
            for (GSSize index = 0; index < coordinateCount; ++index) {
                (*memo.coords)[index] = (*source.coords)[index];
                (*memo.vertexIDs)[index] = (*source.vertexIDs)[index];
                (*memo.edgeIDs)[index] = (*source.edgeIDs)[index];
                (*memo.contourIDs)[index] = (*source.contourIDs)[index];
            }
            (*memo.pends)[0] = (*source.pends)[0];
            (*memo.pends)[1] = (*source.pends)[1];
            return NoError;
        } catch (...) {
            return Fail (APIERR_MEMFULL);
        }
    }

    // Scalar-only UPDATE is deliberately separate from memo construction: a
    // caller cannot rebuild, reorder, reverse or repair geometry here.
    bool ApplyScalarUpdate (const NativeHatchState& before, const NativeHatchState& result) noexcept
    {
        if (poisoned || !HasAnyHandle ()) {
            Fail (APIERR_BADPARS);
            return false;
        }
        if (!SamePolygon (before.polygon, result.polygon)) {
            Fail (APIERR_BADPARS);
            return false;
        }
        if (before.storyIndex == result.storyIndex && before.fill == result.fill
            && before.contourPen == result.contourPen && before.foregroundPen == result.foregroundPen
            && before.backgroundPen == result.backgroundPen) {
            Fail (APIERR_BADPARS);
            return false;
        }
        return true;
    }

private:
    GSErrCode Fail (GSErrCode error) noexcept
    {
        poisoned = true;
        return error;
    }

    bool HasAnyHandle () const noexcept
    {
        return memo.coords != nullptr || memo.pends != nullptr || memo.parcs != nullptr || memo.vertexIDs != nullptr
            || memo.edgeIDs != nullptr || memo.contourIDs != nullptr;
    }

    void DisposeExactlyOnce () noexcept
    {
        if (disposed) return;
        disposed = true;
        ACAPI_DisposeElemMemoHdls (&memo);
    }

    API_ElementMemo memo = {};
    bool poisoned = false;
    bool disposed = false;
};

} // namespace

NativeDisposableHatchContractCommand::NativeDisposableHatchContractCommand () :
    CommandBase (CommonSchema::NotUsed)
{
}

GS::String NativeDisposableHatchContractCommand::GetName () const
{
    return "NativeDisposableHatchContract";
}

GS::Optional<GS::UniString> NativeDisposableHatchContractCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {},
        "required": []
    })";
}

GS::Optional<GS::UniString> NativeDisposableHatchContractCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "status": { "enum": ["unavailable"] },
            "executorVersion": { "enum": ["native-disposable-hatch-executor-v1"] },
            "candidateVersion": { "enum": ["native-hatch-candidate-v1"] },
            "readbackVersion": { "enum": ["native-hatch-readback-v1"] },
            "receiptVersion": { "enum": ["native-hatch-receipt-v1"] },
            "liveVerified": { "enum": [false] },
            "mutationDispatched": { "enum": [false] },
            "authority": { "enum": ["none"] },
            "reason": { "type": "string", "minLength": 1 }
        },
        "required": ["status", "executorVersion", "candidateVersion", "readbackVersion", "receiptVersion", "liveVerified", "mutationDispatched", "authority", "reason"]
    })";
}

GS::ObjectState NativeDisposableHatchContractCommand::Execute (const GS::ObjectState& /*parameters*/, GS::ProcessControl& /*processControl*/) const
{
    // The isolated profile intentionally has no authenticated context.  It
    // returns an unavailable marker and never calls a native Hatch API.
    GS::ObjectState result;
    result.Add ("status", "unavailable");
    result.Add ("executorVersion", tapir::disposable_channel::kNativeHatchExecutorVersion);
    result.Add ("candidateVersion", tapir::disposable_channel::kNativeHatchCandidateContractVersion);
    result.Add ("readbackVersion", tapir::disposable_channel::kNativeHatchReadbackVersion);
    result.Add ("receiptVersion", tapir::disposable_channel::kNativeHatchReceiptVersion);
    result.Add ("liveVerified", false);
    result.Add ("mutationDispatched", false);
    result.Add ("authority", "none");
    result.Add ("reason", "NATIVE_DISPOSABLE_HATCH_CONTEXT_NOT_CONFIGURED");
    return result;
}

#endif
