#include "NativeCrudInternal.hpp"

#include "ElementCommands.hpp"
#include "ElementCreationCommands.hpp"
#include "ExtendedElementCommands.hpp"
#include "MigrationHelper.hpp"
#include "NativeOwnership.hpp"
#include "NativeRichTextMemo.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <cmath>

using namespace NativeCrudInternal;

namespace {

double NormalizeNativeArcCreateAngle (double angle)
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    double normalized = std::fmod (angle, twoPi);
    if (normalized < 0.0)
        normalized += twoPi;
    // Adding 2pi to a tiny negative remainder can round to 2pi; keep the
    // adapter's output in the promised half-open interval [0, 2pi).
    if (normalized >= twoPi || normalized == 0.0)
        return 0.0;
    return normalized;
}

} // namespace

namespace NativeCrudInternal {

GS::ObjectState BuildNativeMutationParameters (
    const GS::UniString& operation,
    const GS::UniString& typeName,
    const GS::Array<GS::ObjectState>& items)
{
    GS::ObjectState parameters;
    const auto& normalized = parameters.AddList<GS::ObjectState> ("items");
    for (const GS::ObjectState& item : items) {
        GS::ObjectState copy;
        const GS::ObjectState* id = item.Get ("elementId");
        if (id != nullptr) copy.Add ("elementId", *id);
        const GS::ObjectState* cloneSource = item.Get ("cloneSourceElementId");
        if (cloneSource != nullptr) copy.Add ("cloneSourceElementId", *cloneSource);
        const GS::ObjectState* payload = item.Get ("payload");
        if (payload == nullptr) payload = item.Get ("patch");
        if (payload != nullptr) {
            GS::ObjectState mapped;
            if (operation == "create") {
                const GS::ObjectState* placement = item.Get ("placement");
                Int32 storyIndex = 0;
                if (placement != nullptr && placement->Get ("storyIndex", storyIndex))
                    mapped.Add (typeName == "Hatch" ? "floorInd" : "floorIndex", storyIndex);
            }
            // Coordinates and scalar fields retain their native units.  The
            // Project-owned typed executors use their own field names; this
            // adapter is the one and only place where that vocabulary is
            // translated from native-crud-v2.
            if (typeName == "Wall") {
                CopyObjectField (*payload, "begCoordinate", mapped, "begCoordinate");
                CopyObjectField (*payload, "endCoordinate", mapped, "endCoordinate");
                if (operation == "create") {
                    // `elevation` is story-relative (D-306), and CreateWalls
                    // reads zCoordinate as the bottom offset once floorIndex
                    // pins the story.
                    CopyDoubleField (*payload, "elevation", mapped, "zCoordinate");
                } else {
                    // Wall UPDATE has a different native mask contract from
                    // CREATE.  Keep the canonical placement names internal
                    // to the narrow adapter; the update helper converts
                    // them to floorInd/bottomOffset without ever forwarding
                    // rejected floorIndex/zCoordinate fields.
                    CopyDoubleField (*payload, "elevation", mapped, "elevation");
                }
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyDoubleField (*payload, "thickness", mapped, "thickness");
                CopyDoubleField (*payload, "offset", mapped, "offset");
                CopyStringField (*payload, "structure", mapped, "structureType");
                CopyObjectField (*payload, "material", mapped, "buildingMaterialId");
                CopyObjectField (*payload, "profile", mapped, "profileId");
                CopyObjectField (*payload, "composite", mapped, "compositeId");
                CopyStringField (*payload, "referenceLine", mapped, "referenceLineLocation");
                CopyBoolField (*payload, "flipped", mapped, "flipped");
            } else if (typeName == "Column") {
                if (operation == "create") {
                    // Ordinary CREATE: origin x/y plus the story-relative
                    // elevation (D-306).  CreateColumns takes one absolute
                    // point, so add the placement story level here.
                    const GS::ObjectState* origin = payload->Get ("origin");
                    const GS::ObjectState* placement = item.Get ("placement");
                    double x = 0.0;
                    double y = 0.0;
                    double elevation = 0.0;
                    Int32 storyIndex = 0;
                    if (origin != nullptr && origin->Get ("x", x) && origin->Get ("y", y)
                        && payload->Get ("elevation", elevation)
                        && placement != nullptr && placement->Get ("storyIndex", storyIndex)) {
                        const double z = GetZPos (static_cast<short> (storyIndex), elevation, GetStories ());
                        mapped.Add ("coordinates", GS::ObjectState ("x", x, "y", y, "z", z));
                    }
                } else {
                    // ModifyColumns consumes the 2D update placement field
                    // `origin`; CREATE must never receive this field and
                    // UPDATE must never receive the incompatible `coordinates`
                    // field or a rejected zCoordinate translation.
                    CopyObjectField (*payload, "origin", mapped, "origin");
                }
                CopyDoubleField (*payload, "height", mapped, "height");
                if (operation != "create")
                    CopyDoubleField (*payload, "elevation", mapped, "bottomOffset");
                CopyDoubleField (*payload, "rotation", mapped, "axisRotationAngle");
                CopyDoubleField (*payload, "slant", mapped, "slantAngle");
                const GS::ObjectState* dimensions = payload->Get ("dimensions");
                if (dimensions != nullptr) {
                    CopyDoubleField (*dimensions, "width", mapped, "width");
                    CopyDoubleField (*dimensions, "depth", mapped, "depth");
                }
                CopyObjectField (*payload, "profile", mapped, "profileId");
                CopyObjectField (*payload, "material", mapped, "buildingMaterialId");
            } else if (typeName == "Text") {
                CopyObjectField (*payload, "richText", mapped, "richText");
                CopyObjectField (*payload, "coordinate", mapped, "coordinate");
                CopyIntField (*payload, "anchor", mapped, "anchor");
                CopyDoubleField (*payload, "angle", mapped, "angle");
                CopyDoubleField (*payload, "width", mapped, "width");
                CopyDoubleField (*payload, "height", mapped, "height");
                CopyIntField (*payload, "pen", mapped, "pen");
                CopyBoolField (*payload, "fixedSize", mapped, "fixedSize");
                CopyBoolField (*payload, "nonBreaking", mapped, "nonBreaking");
            } else if (typeName == "Zone") {
                CopyStringField (*payload, "name", mapped, "name");
                CopyStringField (*payload, "number", mapped, "numberStr");
                CopyObjectField (*payload, "category", mapped, "categoryAttributeId");
                const GS::ObjectState* stamp = payload->Get ("stampPlacement");
                if (stamp != nullptr) {
                CopyObjectField (*stamp, "position", mapped, "stampPosition");
                    CopyDoubleField (*stamp, "angle", mapped, "stampAngle");
                    CopyBoolField (*stamp, "fixedAngle", mapped, "fixedStampAngle");
                }
                const GS::ObjectState* polygon = payload->Get ("polygon");
                GS::ObjectState geometry;
                if (polygon != nullptr) {
                    CopyArrayField (*polygon, "coordinates", geometry, "polygonCoordinates");
                    CopyArrayField (*polygon, "arcs", geometry, "polygonArcs");
                    CopyArrayField (*polygon, "holes", geometry, "holes");
                }
                if (operation == "create") {
                    if (!geometry.IsEmpty ()) mapped.Add ("geometry", geometry);
                } else {
                    if (!geometry.IsEmpty ()) mapped.Add ("geometry", geometry);
                }
            } else if (typeName == "Door" || typeName == "Window") {
                CopyObjectField (*payload, "ownerWallId", mapped, "ownerWallId");
                if (operation == "create") CopyIntField (*payload, "floorIndex", mapped, "floorIndex");
                CopyDoubleField (*payload, "offset", mapped, "centerOffset");
                CopyDoubleField (*payload, "sill", mapped, "sillHeight");
                CopyDoubleField (*payload, "width", mapped, "width");
                CopyDoubleField (*payload, "height", mapped, "height");
                const GS::ObjectState* orientation = payload->Get ("orientation");
                if (orientation != nullptr) {
                    CopyBoolField (*orientation, "reflected", mapped, "reflected");
                    CopyBoolField (*orientation, "refSide", mapped, "refSide");
                    CopyBoolField (*orientation, "oSide", mapped, "oSide");
                }
            } else if (typeName == "Line") {
                CopyObjectField (*payload, "begin", mapped, "begCoordinate");
                CopyObjectField (*payload, "end", mapped, "endCoordinate");
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else if (typeName == "Arc") {
                CopyObjectField (*payload, "origin", mapped, "origin");
                CopyDoubleField (*payload, "radius", mapped, "radius");
                CopyDoubleField (*payload, "axisAngle", mapped, "axisAngle");
                CopyDoubleField (*payload, "ratio", mapped, "ratio");
                // This shared Native CRUD adapter is used by candidate and
                // future ordinary requests, and is separate from legacy Arc
                // commands. CREATE input angles are local to the ellipse
                // axis; the factual API_ArcType readback includes axisAngle.
                // Keep sparse UPDATE inputs direct until independently proven.
                double axisAngle = 0.0;
                payload->Get ("axisAngle", axisAngle);
                double beginAngle = 0.0;
                if (payload->Get ("beginAngle", beginAngle)) {
                    mapped.Add ("begAngle", operation == "create" && std::isfinite (axisAngle) && std::isfinite (beginAngle)
                        ? NormalizeNativeArcCreateAngle (beginAngle - axisAngle)
                        : beginAngle);
                }
                double endAngle = 0.0;
                if (payload->Get ("endAngle", endAngle)) {
                    mapped.Add ("endAngle", operation == "create" && std::isfinite (axisAngle) && std::isfinite (endAngle)
                        ? NormalizeNativeArcCreateAngle (endAngle - axisAngle)
                        : endAngle);
                }
                CopyBoolField (*payload, "reflected", mapped, "reflected");
                CopyIntField (*payload, "pen", mapped, "linePenIndex");
                CopyIntField (*payload, "lineType", mapped, "lineTypeIndex");
                CopyBoolField (*payload, "roomSeparator", mapped, "roomSeparator");
            } else if (typeName == "Hatch") {
                const GS::ObjectState* polygon = payload->Get ("polygon");
                if (polygon != nullptr)
                    CopyArrayField (*polygon, "coordinates", mapped, operation == "create" ? "hatchCoordinates" : "polygonOutline");
                CopyIntField (*payload, "fill", mapped, "fillAttributeIndex");
                CopyIntField (*payload, "contourPen", mapped, "contourPenIndex");
                CopyIntField (*payload, "foregroundPen", mapped, "fillPenIndex");
                CopyIntField (*payload, "backgroundPen", mapped, "fillBackgroundPenIndex");
            } else {
                mapped = *payload;
            }
            copy.Add ("payload", mapped);
        }
        normalized (copy);
    }
    parameters.Add ("operation", operation);
    return parameters;
}

} // namespace NativeCrudInternal
