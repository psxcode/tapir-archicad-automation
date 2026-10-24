#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace NativeCrudInternal;

namespace {

constexpr const char* ZoneMemoWriterGate = "disposable-ac25-manual-polygon-v1";

constexpr const char* AutomaticZonePolygonProbeGate = "disposable-automatic-zone-polygon-v1";

} // namespace

namespace NativeCrudInternal {

bool HasNativeCrudTestFaultMetadata (const GS::ObjectState* semanticMetadata)
{
    return semanticMetadata != nullptr && semanticMetadata->Contains ("nativeCrudTestFaultAfterFirstApplied");
}

bool HasOnlyObjectStateFields (const GS::ObjectState& value, const NativeCrudSchema::FieldList& allowedFields)
{
    for (const GS::String& fieldName : value.GetFieldNames ()) {
        bool allowed = false;
        for (GSSize i = 0; i < allowedFields.count && !allowed; ++i)
            allowed = fieldName == GS::String (allowedFields.names[i]);
        if (!allowed)
            return false;
    }
    return true;
}

bool HasOnlyObjectStateFields (const GS::ObjectState& value, const std::initializer_list<const char*>& allowedFields)
{
    const GS::HashSet<GS::String> fieldNames = value.GetFieldNames ();
    for (const GS::String& fieldName : fieldNames) {
        bool allowed = false;
        for (const char* allowedField : allowedFields) {
            if (fieldName == GS::String (allowedField)) {
                allowed = true;
                break;
            }
        }
        if (!allowed)
            return false;
    }
    return true;
}

} // namespace NativeCrudInternal

namespace {

bool IsExactFinitePoint (const GS::ObjectState& point)
{
    const GS::HashSet<GS::String> fieldNames = point.GetFieldNames ();
    double x = 0.0;
    double y = 0.0;
    return fieldNames.GetSize () == 2 && point.Contains ("x") && point.Contains ("y")
        && point.Get ("x", x) && point.Get ("y", y)
        && std::isfinite (x) && std::isfinite (y);
}

GS::Optional<GS::UniString> ValidateZonePlacement (const GS::ObjectState& shape)
{
    if (shape.Contains ("referencePosition"))
        return "Zone referencePosition is read-only in Native CRUD.";

    const GS::ObjectState* stampPlacement = shape.Get ("stampPlacement");
    if (stampPlacement == nullptr)
        return {};
    const GS::HashSet<GS::String> stampFields = stampPlacement->GetFieldNames ();
    if (!HasOnlyObjectStateFields (*stampPlacement, { "position", "angle", "fixedAngle" }) || stampFields.IsEmpty ())
        return "Zone stampPlacement accepts only position, angle and fixedAngle fields.";
    const GS::ObjectState* position = stampPlacement->Get ("position");
    if (position != nullptr && !IsExactFinitePoint (*position))
        return "Zone stampPlacement.position must contain exactly finite x and y coordinates.";
    if (stampPlacement->Contains ("angle")) {
        double angle = 0.0;
        if (!stampPlacement->Get ("angle", angle) || !std::isfinite (angle))
            return "Zone stampPlacement.angle must be a finite number.";
    }
    if (stampPlacement->Contains ("fixedAngle")) {
        bool fixedAngle = false;
        if (!stampPlacement->Get ("fixedAngle", fixedAngle))
            return "Zone stampPlacement.fixedAngle must be a boolean.";
    }
    return {};
}

} // namespace

namespace NativeCrudInternal {

GS::Optional<GS::UniString> ValidateZoneTypedWriter (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    if (operation != "create" && operation != "update")
        return "The validated Zone typed writer only supports CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return "The validated Zone typed writer requires a bounded non-empty item list.";

    const std::initializer_list<const char*> fields = {
        "name", "number", "category", "polygon", "stampPlacement"
    };
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* shape = nullptr;
        if (operation == "create") {
            shape = item.Get ("payload");
            if (shape == nullptr || !HasOnlyObjectStateFields (*shape, fields))
                return "Zone CREATE accepts only the declared typed Zone fields.";
            if (!shape->Contains ("polygon"))
                return "Zone CREATE requires a manual polygon.";
        } else {
            shape = item.Get ("patch");
            if (shape == nullptr || !HasOnlyObjectStateFields (*shape, fields))
                return "Zone UPDATE accepts only the declared typed Zone fields.";
            if (shape->GetFieldNames ().IsEmpty ())
                return "Zone UPDATE requires a non-empty typed patch.";
        }
        const GS::Optional<GS::UniString> placementError = ValidateZonePlacement (*shape);
        if (placementError.HasValue ())
            return placementError.Get ();
    }
    return {};
}

} // namespace NativeCrudInternal

namespace {

bool HasZoneMemoWriterGate (const GS::ObjectState* semanticMetadata)
{
    // This candidate writer is deliberately compiled behind the disposable
    // AC25 identity.  A matching marker in an AC28 (or unknown) APX must not
    // turn a read-only Zone capability into a write path.
#if TAPIR_AC_VERSION != 25
    (void) semanticMetadata;
    return false;
#else
    GS::UniString gate;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("zoneMemoWriterGate", gate)
        && gate == ZoneMemoWriterGate;
#endif
}

} // namespace

namespace NativeCrudInternal {

bool HasAutomaticZonePolygonProbeGate (const GS::ObjectState* semanticMetadata)
{
#if TAPIR_AC_VERSION == 25 || TAPIR_AC_VERSION == 28
    GS::UniString gate;
    return semanticMetadata != nullptr
        && semanticMetadata->Get ("zoneMemoWriterGate", gate)
        && gate == AutomaticZonePolygonProbeGate;
#else
    (void) semanticMetadata;
    return false;
#endif
}

bool HasAnyZoneMemoWriterGate (const GS::ObjectState* semanticMetadata)
{
    return HasZoneMemoWriterGate (semanticMetadata) || HasAutomaticZonePolygonProbeGate (semanticMetadata);
}

GS::Optional<GS::UniString> ValidateArcTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    // The Arc CREATE fields of the typed schema; the Arc UPDATE fields are the same list.
    const NativeCrudSchema::FieldList allowedFields = { NativeCrudSchema::ArcCreateFields, sizeof (NativeCrudSchema::ArcCreateFields) / sizeof (NativeCrudSchema::ArcCreateFields[0]) };
    for (GSSize index = 0; index < static_cast<GSSize> (items.GetSize ()); ++index) {
        const GS::ObjectState* shape = items[index].Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr || shape->GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (*shape, allowedFields))
            return "Arc typed payload must contain a non-empty patch with only declared Arc fields.";
        if (operation == "create" && static_cast<GSSize> (shape->GetFieldNames ().GetSize ()) != allowedFields.count)
            return "Arc CREATE must provide all ten typed element fields; placement is separate.";
        const GS::ObjectState* origin = shape->Get ("origin");
        if (origin != nullptr) {
            double x = 0.0, y = 0.0;
            if (!HasOnlyObjectStateFields (*origin, { "x", "y" }) || !origin->Get ("x", x) || !origin->Get ("y", y) || !std::isfinite (x) || !std::isfinite (y))
                return "Arc origin must contain finite x and y coordinates only.";
        }
        double radius = 0.0, ratio = 0.0, axisAngle = 0.0, beginAngle = 0.0, endAngle = 0.0;
        if (shape->Contains ("radius") && (!shape->Get ("radius", radius) || !std::isfinite (radius) || radius <= 0.0)) return "Arc radius must be finite and positive.";
        if (shape->Contains ("ratio") && (!shape->Get ("ratio", ratio) || !std::isfinite (ratio) || ratio <= 0.0)) return "Arc ratio must be finite and positive.";
        if (shape->Contains ("axisAngle") && (!shape->Get ("axisAngle", axisAngle) || !std::isfinite (axisAngle))) return "Arc axisAngle must be finite.";
        if (shape->Contains ("beginAngle") && (!shape->Get ("beginAngle", beginAngle) || !std::isfinite (beginAngle))) return "Arc beginAngle must be finite.";
        if (shape->Contains ("endAngle") && (!shape->Get ("endAngle", endAngle) || !std::isfinite (endAngle))) return "Arc endAngle must be finite.";
        if (operation == "create" && beginAngle == endAngle) return "Arc CREATE requires a non-full open arc.";
        Int32 lineType = 0, pen = 0;
        if (shape->Contains ("lineType") && (!shape->Get ("lineType", lineType) || lineType <= 0)) return "Arc lineType must be positive.";
        if (shape->Contains ("pen") && (!shape->Get ("pen", pen) || pen <= 0)) return "Arc pen must be positive.";
        bool booleanValue = false;
        if (shape->Contains ("reflected") && !shape->Get ("reflected", booleanValue)) return "Arc reflected must be boolean.";
        if (shape->Contains ("roomSeparator") && !shape->Get ("roomSeparator", booleanValue)) return "Arc roomSeparator must be boolean.";
    }
    return {};
}

} // namespace NativeCrudInternal

namespace {

bool HatchPointOnSegment (const API_Coord& point, const API_Coord& begin, const API_Coord& end)
{
    return point.x >= std::min (begin.x, end.x) && point.x <= std::max (begin.x, end.x)
        && point.y >= std::min (begin.y, end.y) && point.y <= std::max (begin.y, end.y);
}

double HatchOrientation (const API_Coord& first, const API_Coord& second, const API_Coord& third)
{
    return (second.x - first.x) * (third.y - first.y) - (second.y - first.y) * (third.x - first.x);
}

bool HatchSegmentsIntersect (const API_Coord& a, const API_Coord& b, const API_Coord& c, const API_Coord& d)
{
    const double o1 = HatchOrientation (a, b, c);
    const double o2 = HatchOrientation (a, b, d);
    const double o3 = HatchOrientation (c, d, a);
    const double o4 = HatchOrientation (c, d, b);
    if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0))
        && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0))) return true;
    return (o1 == 0.0 && HatchPointOnSegment (c, a, b)) || (o2 == 0.0 && HatchPointOnSegment (d, a, b))
        || (o3 == 0.0 && HatchPointOnSegment (a, c, d)) || (o4 == 0.0 && HatchPointOnSegment (b, c, d));
}

GS::Optional<GS::UniString> ValidateHatchTypedShape (const GS::ObjectState& shape, const GS::UniString& operation)
{
    // The Hatch CREATE fields of the typed schema; the Hatch UPDATE fields are the same list.
    const NativeCrudSchema::FieldList allowedFields = { NativeCrudSchema::HatchCreateFields, sizeof (NativeCrudSchema::HatchCreateFields) / sizeof (NativeCrudSchema::HatchCreateFields[0]) };
    if (shape.GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (shape, allowedFields))
        return "Hatch payload must contain only its typed simple-polygon and style fields.";
    if (operation == "create" && static_cast<GSSize> (shape.GetFieldNames ().GetSize ()) != allowedFields.count)
        return "Hatch CREATE requires polygon and all four style fields; story placement is separate.";
    const GS::ObjectState* polygon = shape.Get ("polygon");
    if (polygon != nullptr) {
        if (!HasOnlyObjectStateFields (*polygon, { "coordinates" })) return "Hatch polygon accepts only one external coordinates ring; holes and arcs are unsupported.";
        GS::Array<GS::ObjectState> points;
        if (!polygon->Get ("coordinates", points) || points.GetSize () < 3 || points.GetSize () > 64)
            return "Hatch polygon requires between 3 and 64 unclosed vertices.";
        std::vector<API_Coord> ring;
        ring.reserve (static_cast<GSSize> (points.GetSize ()));
        for (const GS::ObjectState& point : points) {
            if (!IsExactFinitePoint (point)) return "Hatch vertices must contain finite x and y only.";
            ring.push_back (Get2DCoordinateFromObjectState (point));
        }
        double twiceArea = 0.0;
        for (GSSize index = 0; index < ring.size (); ++index) {
            const API_Coord& point = ring[index];
            const API_Coord& next = ring[(index + 1) % ring.size ()];
            if (IsSame2DCoordinate (point, next)) return "Hatch polygon cannot contain a zero-length edge or duplicated closing vertex.";
            for (GSSize prior = 0; prior < index; ++prior)
                if (IsSame2DCoordinate (point, ring[prior])) return "Hatch polygon vertices must be unique.";
            twiceArea += point.x * next.y - next.x * point.y;
        }
        if (!std::isfinite (twiceArea) || twiceArea == 0.0) return "Hatch polygon must have finite nonzero area.";
        for (GSSize first = 0; first < ring.size (); ++first) {
            const GSSize firstNext = (first + 1) % ring.size ();
            for (GSSize second = first + 1; second < ring.size (); ++second) {
                const GSSize secondNext = (second + 1) % ring.size ();
                if (first == second || firstNext == second || secondNext == first) continue;
                if (HatchSegmentsIntersect (ring[first], ring[firstNext], ring[second], ring[secondNext]))
                    return "Hatch polygon must be a simple non-self-intersecting ring.";
            }
        }
    }
    for (const char* fieldName : { "fill", "contourPen", "foregroundPen", "backgroundPen" }) {
        if (!shape.Contains (fieldName)) {
            if (operation == "create") return GS::UniString::Printf ("Hatch CREATE is missing '%s'.", fieldName);
            continue;
        }
        Int32 value = 0;
        if (!shape.Get (fieldName, value) || value <= 0) return GS::UniString::Printf ("Hatch '%s' must be a positive integer index.", fieldName);
    }
    if (operation == "create" && polygon == nullptr) return "Hatch CREATE requires a polygon.";
    return {};
}

} // namespace

namespace NativeCrudInternal {

GS::Optional<GS::UniString> ValidateHatchTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    if (operation != "create" && operation != "update") return "Hatch typed writer supports only CREATE and UPDATE.";
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems) return "Hatch typed writer requires one to twenty-five items.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* shape = item.Get (operation == "create" ? "payload" : "patch");
        if (shape == nullptr) return "Hatch item is missing its typed payload or patch.";
        const GS::Optional<GS::UniString> shapeError = ValidateHatchTypedShape (*shape, operation);
        if (shapeError.HasValue ()) return shapeError;
        if (operation == "create") {
            const GS::ObjectState* placement = item.Get ("placement");
            Int32 storyIndex = 0;
            if (placement == nullptr || !HasOnlyObjectStateFields (*placement, { "storyIndex" }) || !placement->Get ("storyIndex", storyIndex))
                return "Hatch CREATE requires exact separate placement.storyIndex.";
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateZoneMemoWriterCandidate (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items,
    const GS::ObjectState* semanticMetadata)
{
    if (!HasAnyZoneMemoWriterGate (semanticMetadata))
        return {};
    if (operation != "update")
        return "The disposable Zone memo writer only supports UPDATE.";
    if (items.GetSize () != 1)
        return "The disposable Zone memo writer accepts exactly one Zone item.";

    if (items[0].Contains ("payload"))
        return "The disposable Zone memo writer rejects payload; use a polygon-only patch.";
    if (!HasOnlyObjectStateFields (items[0], { "elementId", "expectedFingerprint", "patch" }))
        return "The disposable Zone memo writer accepts only elementId, expectedFingerprint and patch fields.";
    const GS::ObjectState* patch = items[0].Get ("patch");
    if (patch == nullptr || !HasOnlyObjectStateFields (*patch, { "polygon" }))
        return "The disposable Zone memo writer accepts a polygon-only patch.";
    const GS::ObjectState* polygon = patch->Get ("polygon");
    if (polygon == nullptr || !HasOnlyObjectStateFields (*polygon, { "coordinates", "arcs", "holes" }))
        return "The disposable Zone memo writer accepts only polygon coordinates, arcs and holes fields.";

    GS::Array<GS::ObjectState> coordinates;
    if (!polygon->Get ("coordinates", coordinates))
        return "The disposable Zone memo writer requires polygon.coordinates.";
    GS::Array<GS::ObjectState> arcs;
    if (polygon->Contains ("arcs") && !polygon->Get ("arcs", arcs))
        return "The disposable Zone memo writer requires polygon.arcs to be an array.";
    GS::Array<GS::ObjectState> holes;
    if (polygon->Contains ("holes") && !polygon->Get ("holes", holes))
        return "The disposable Zone memo writer requires polygon.holes to be an array.";
    if (!arcs.IsEmpty ())
        return "The disposable Zone memo writer rejects polygon arcs.";
    if (!holes.IsEmpty ())
        return "The disposable Zone memo writer rejects polygon holes.";
    return {};
}

GS::Optional<GS::UniString> ValidateDoorTypedPayloads (
    const GS::UniString& operation,
    const GS::Array<GS::ObjectState>& items)
{
    const bool create = operation == "create";
    if (!create && operation != "update") return "Door typed writer supports only CREATE and UPDATE.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* payload = item.Get (create ? "payload" : "patch");
        if (payload == nullptr) return "Door typed writer requires its exact payload or sparse patch.";
        const GS::HashSet<GS::String> names = payload->GetFieldNames ();
        if (names.IsEmpty ()) return "Door typed writer payload/patch must not be empty.";
        if (create
            ? !HasOnlyObjectStateFields (*payload, { "ownerWallId", "offset", "sill", "width", "height", "orientation" })
            : !HasOnlyObjectStateFields (*payload, { "offset", "sill", "width", "height", "orientation" }))
            return "Door typed writer received a field outside its declared contract.";
        if (create && names.GetSize () != 6)
            return "Door CREATE requires ownerWallId, offset, sill, width, height and orientation.";
        for (const char* field : { "offset", "sill", "width", "height" }) {
            if (!payload->Contains (field)) continue;
            double value = 0.0;
            if (!payload->Get (field, value) || !std::isfinite (value)
                || ((GS::UniString (field) == "width" || GS::UniString (field) == "height") && value <= 0.0))
                return GS::UniString::Printf ("Door '%s' must be a finite%s number.", field,
                    (GS::UniString (field) == "width" || GS::UniString (field) == "height") ? " positive" : "");
        }
        if (create) {
            const GS::ObjectState* owner = payload->Get ("ownerWallId");
            if (owner == nullptr || GetGuidFromObjectState (*owner) == APINULLGuid)
                return "Door CREATE requires an exact ownerWallId GUID.";
        }
        const GS::ObjectState* orientation = payload->Get ("orientation");
        if (orientation != nullptr) {
            const GS::HashSet<GS::String> orientationNames = orientation->GetFieldNames ();
            if (orientationNames.IsEmpty () || !HasOnlyObjectStateFields (*orientation, { "reflected", "refSide", "oSide" }))
                return "Door orientation accepts one or more of reflected, refSide and oSide.";
            for (const GS::String& name : orientationNames) {
                bool value = false;
                if (!orientation->Get (name, value)) return "Door orientation values must be boolean.";
            }
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateWindowTypedPayloads (const GS::UniString& operation, const GS::Array<GS::ObjectState>& items)
{
    const bool create = operation == "create";
    if (!create && operation != "update") return "Window typed writer supports only CREATE and UPDATE.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* payload = item.Get (create ? "payload" : "patch");
        if (payload == nullptr || payload->GetFieldNames ().IsEmpty ()) return "Window typed writer requires a non-empty exact payload or sparse patch.";
        if (create ? !HasOnlyObjectStateFields (*payload, { "ownerWallId", "offset", "sill", "width", "height", "orientation" })
                   : !HasOnlyObjectStateFields (*payload, { "offset", "sill", "width", "height", "orientation" }))
            return "Window typed writer received a field outside its declared contract.";
        if (create && payload->GetFieldNames ().GetSize () != 6) return "Window CREATE requires ownerWallId, offset, sill, width, height and orientation.";
        for (const char* field : { "offset", "sill", "width", "height" }) {
            if (!payload->Contains (field)) continue;
            double value = 0.0;
            if (!payload->Get (field, value) || !std::isfinite (value)
                || ((GS::UniString (field) == "width" || GS::UniString (field) == "height") && value <= 0.0))
                return GS::UniString::Printf ("Window '%s' must be a finite%s number.", field,
                    (GS::UniString (field) == "width" || GS::UniString (field) == "height") ? " positive" : "");
        }
        if (create) {
            const GS::ObjectState* owner = payload->Get ("ownerWallId");
            if (owner == nullptr || GetGuidFromObjectState (*owner) == APINULLGuid) return "Window CREATE requires an exact ownerWallId GUID.";
        }
        const GS::ObjectState* orientation = payload->Get ("orientation");
        if (orientation != nullptr) {
            if (orientation->GetFieldNames ().IsEmpty () || !HasOnlyObjectStateFields (*orientation, { "reflected", "refSide", "oSide" })) return "Window orientation accepts reflected, refSide and oSide only.";
            for (const GS::String& name : orientation->GetFieldNames ()) { bool value = false; if (!orientation->Get (name, value)) return "Window orientation values must be boolean."; }
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateDoorCloneSource (const API_Guid& sourceGuid)
{
    if (sourceGuid == APINULLGuid) return "Door clone source GUID is missing.";
    API_Element source = {};
    source.header.guid = sourceGuid;
    if (ACAPI_Element_Get (&source) != NoError || GetElemTypeId (source.header) != API_DoorID || source.header.guid != sourceGuid)
        return "Door clone source must resolve to the exact same-file Door element.";
    if (source.door.openingBase.libInd <= 0)
        return "Door clone source has no valid same-file opening library part.";
    API_LibPart doorPart = {};
    LibraryPartLocationGuard doorPartLocationGuard (doorPart);
    doorPart.index = source.door.openingBase.libInd;
    if (ACAPI_LibraryPart_Get (&doorPart) != NoError || doorPart.missingDef || doorPart.version <= 0
        || doorPart.ownUnID[0] == '\0' || doorPart.file_UName[0] == '\0')
        return "Door clone source opening library identity/revision is incomplete.";
    const API_Guid stampGuid = source.door.openingBase.markGuid;
    if (stampGuid == APINULLGuid) return {};
    // Marker-bearing copying is supplemental under D-272. Non-null identities,
    // including identities that might be unresolved, fail closed here.
    return "Door clone source is marker-bearing; this candidate packet accepts only factual markerless templates.";
}

} // namespace NativeCrudInternal

namespace {

bool WindowNativeTopologySupported (const API_Element& element)
{
    return GetElemTypeId (element.header) == API_WindowID
        && element.window.openingBase.libInd > 0
        && element.window.fixPoint == APIHoleAnchor_CenterFix
        && element.window.openingBase.verticalLink.linkType == API_LinkSillToWallBottom
        && element.window.directionType == API_WDAssociativeToWall;
}

} // namespace

namespace NativeCrudInternal {

GS::Optional<GS::UniString> ValidateWindowCloneSource (const API_Guid& sourceGuid)
{
    if (sourceGuid == APINULLGuid) return "Window clone source GUID is missing.";
    API_Element source = {};
    source.header.guid = sourceGuid;
    if (ACAPI_Element_Get (&source) != NoError || GetElemTypeId (source.header) != API_WindowID || source.header.guid != sourceGuid)
        return "Window clone source must resolve to the exact same-file native Window.";
    if (source.window.openingBase.markGuid != APINULLGuid) return "Window clone source is marker-bearing; only factual markerless templates are admitted.";
    if (!WindowNativeTopologySupported (source))
        return "Window clone source is outside the directly observable centered, sill-linked, wall-associative native subset.";
    API_LibPart part = {};
    LibraryPartLocationGuard partGuard (part);
    part.index = source.window.openingBase.libInd;
    if (ACAPI_LibraryPart_Get (&part) != NoError || part.missingDef || part.version <= 0 || part.ownUnID[0] == '\0' || part.file_UName[0] == '\0')
        return "Window clone source library identity/revision is incomplete.";
    return {};
}

GS::Optional<GS::UniString> ValidateNativeCreatePlacement (
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    const Stories stories = GetStories ();
    if (stories.empty ())
        return "CREATE story placement cannot be checked because Archicad story settings are unavailable.";
    for (const GS::ObjectState& item : items) {
        const GS::ObjectState* placement = item.Get ("placement");
        Int32 storyIndex = 0;
        if (placement == nullptr || !HasOnlyObjectStateFields (*placement, { "storyIndex" })
            || !placement->Get ("storyIndex", storyIndex)
            || storyIndex < static_cast<Int32> (std::numeric_limits<short>::min ())
            || storyIndex > static_cast<Int32> (std::numeric_limits<short>::max ()))
            return "CREATE requires placement.storyIndex as one exact Archicad story index.";
        const short nativeStoryIndex = static_cast<short> (storyIndex);
        if (stories.find (nativeStoryIndex) == stories.end ())
            return "CREATE placement.storyIndex does not identify a current Archicad story.";

        if (typeName == "Door" || typeName == "Window") {
            const GS::ObjectState* payload = item.Get ("payload");
            const GS::ObjectState* ownerWallId = payload == nullptr ? nullptr : payload->Get ("ownerWallId");
            const API_Guid ownerWallGuid = ownerWallId == nullptr ? APINULLGuid : GetGuidFromObjectState (*ownerWallId);
            if (ownerWallGuid == APINULLGuid)
                return "Door/Window CREATE requires the exact ownerWallId to validate host story placement.";
            API_Element hostWall = {};
            hostWall.header.guid = ownerWallGuid;
            if (ACAPI_Element_Get (&hostWall) != NoError || GetElemTypeId (hostWall.header) != API_WallID)
                return "Door/Window CREATE ownerWallId must resolve to an exact current Wall in this file.";
            if (hostWall.header.floorInd != nativeStoryIndex)
                return "Door/Window CREATE placement.storyIndex must match the exact host Wall story.";
        }
    }
    return {};
}

GS::Optional<GS::UniString> ValidateMutationItems (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    if (items.IsEmpty () || items.GetSize () > MaxMutationItems)
        return GS::UniString ("items must contain between 1 and 25 entries.");
    GS::Array<API_Guid> seenGuids;
    for (const GS::ObjectState& item : items) {
        if (operation == "create") {
            if (!HasOnlyObjectStateFields (item, { "payload", "placement", "cloneSourceElementId", "historySnapshotRef" }))
                return GS::UniString ("CREATE items accept only payload, placement and one source reference.");
            if (item.Get ("elementId") != nullptr || item.Get ("payload") == nullptr)
                return GS::UniString ("CREATE items require payload and must not contain elementId.");
            const GS::ObjectState* payload = item.Get ("payload");
            if (payload != nullptr && (payload->Contains ("storyIndex") || payload->Contains ("floorIndex") || payload->Contains ("floorInd")))
                return GS::UniString ("storyIndex is read-only evidence and cannot appear in a CREATE payload.");
            if (item.Get ("patch") != nullptr || item.Get ("expectedFingerprint") != nullptr)
                return GS::UniString ("CREATE items accept only a typed payload and an optional source.");
            const bool hasClone = item.Get ("cloneSourceElementId") != nullptr;
            const bool hasHistory = item.Get ("historySnapshotRef") != nullptr;
            if (hasClone && hasHistory)
                return GS::UniString ("CREATE items may use only one clone source.");
            if ((typeName == "Door" || typeName == "Window") && !hasClone && !hasHistory)
                return GS::UniString ("Door/Window CREATE requires a same-file clone source.");
            if (typeName != "Door" && typeName != "Window" && hasClone)
                return GS::UniString ("Only Door/Window CREATE may use a live clone source.");
            if (hasClone) {
                const GS::ObjectState* cloneID = item.Get ("cloneSourceElementId");
                if (cloneID == nullptr || GetGuidFromObjectState (*cloneID) == APINULLGuid)
                    return GS::UniString ("cloneSourceElementId must contain a valid GUID.");
            }
            if (hasHistory) {
                GS::UniString historyRef;
                if (!item.Get ("historySnapshotRef", historyRef) || historyRef.IsEmpty ())
                    return GS::UniString ("historySnapshotRef must be a non-empty same-file receipt reference.");
            }
        } else {
            if (operation == "update" && !HasOnlyObjectStateFields (item, { "elementId", "expectedFingerprint", "patch" }))
                return GS::UniString ("UPDATE items accept only elementId, expectedFingerprint and patch.");
            if (operation == "delete" && !HasOnlyObjectStateFields (item, { "elementId", "expectedFingerprint" }))
                return GS::UniString ("DELETE items accept only elementId and expectedFingerprint.");
            const GS::ObjectState* id = item.Get ("elementId");
            if (id == nullptr || GetGuidFromObjectState (*id) == APINULLGuid)
                return GS::UniString ("UPDATE/DELETE items require a valid elementId.");
            const API_Guid guid = GetGuidFromObjectState (*id);
            if (seenGuids.Contains (guid))
                return GS::UniString ("UPDATE/DELETE items may not repeat a GUID.");
            seenGuids.Push (guid);
            GS::UniString expectedFingerprint;
            if (!item.Get ("expectedFingerprint", expectedFingerprint) || expectedFingerprint.IsEmpty ())
                return GS::UniString ("UPDATE/DELETE items require expectedFingerprint from an exact native read.");
            if (!IsHex64 (expectedFingerprint))
                return GS::UniString ("expectedFingerprint must be a 64-hex native fingerprint.");
            if (operation == "update" && item.Get ("patch") == nullptr)
                return GS::UniString ("UPDATE items require a sparse patch.");
            const GS::ObjectState* patch = item.Get ("patch");
            if (operation == "update" && patch != nullptr
                && (patch->Contains ("storyIndex") || patch->Contains ("floorIndex") || patch->Contains ("floorInd")))
                return GS::UniString ("storyIndex is read-only evidence and cannot appear in an UPDATE patch.");
            if (operation == "update" && item.Get ("payload") != nullptr)
                return GS::UniString ("UPDATE items use patch, not payload.");
            if (operation == "delete" && (item.Get ("patch") != nullptr || item.Get ("payload") != nullptr))
                return GS::UniString ("DELETE items must not contain a payload or patch.");
        }
    }
    return {};
}

} // namespace NativeCrudInternal
