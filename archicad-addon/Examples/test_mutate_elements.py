"""
Live regression test for the project-owned MutateElements envelope.

The test talks to the foreground Archicad instance through aclib and must be
run against an isolated disposable project.  Every created element is deleted
in finally, with the legacy DeleteElements command as a last-resort cleanup
path.
"""

import math

import aclib


created = []


def run(command, parameters):
    return aclib.RunTapirCommand(command, parameters, debug=False)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def element_id(result):
    require(result and result.get("mutationComplete"), f"mutation failed: {result!r}")
    items = result["readback"]["nativeDetails"]["detailsOfElements"]
    require(items and "elementId" in items[0], f"missing readback element id: {result!r}")
    return items[0]["elementId"]["guid"]


def details(result):
    return result["readback"]["nativeDetails"]["detailsOfElements"][0]["details"]


def create(element_type, payload):
    result = run("MutateElements", {
        "operation": "create",
        "elementType": element_type,
        "items": [{"payload": payload}],
    })
    guid = element_id(result)
    created.append((element_type, guid))
    require(result["readbackVerified"], f"create readback was not verified: {result!r}")
    return result, guid


def update(element_type, guid, payload):
    return run("MutateElements", {
        "operation": "update",
        "elementType": element_type,
        "items": [{"elementId": {"guid": guid}, "payload": payload}],
    })


def delete(element_type, guid):
    return run("MutateElements", {
        "operation": "delete",
        "elementType": element_type,
        "items": [{"elementId": {"guid": guid}}],
    })


def run_test():
    print("TEST -- Wall create, sparse update and native readback")
    created_result, wall_guid = create("Wall", {
        "begCoordinate": {"x": 0.0, "y": 0.0},
        "endCoordinate": {"x": 4.0, "y": 0.0},
        "height": 3.0,
        "thickness": 0.2,
    })
    require(math.isclose(details(created_result)["height"], 3.0, rel_tol=0.0, abs_tol=1.0e-6),
            f"unexpected create readback: {created_result!r}")

    updated = update("Wall", wall_guid, {"height": 4.0})
    require(updated and updated.get("mutationComplete"), f"sparse update failed: {updated!r}")
    require(updated["readbackVerified"], f"sparse update readback failed: {updated!r}")
    require(math.isclose(details(updated)["height"], 4.0, rel_tol=0.0, abs_tol=1.0e-6),
            f"sparse update did not round-trip: {updated!r}")

    print("TEST -- invalid enum/range/conflicting references do not mutate")
    before = run("GetDetailsOfElements", {"elements": [{"elementId": {"guid": wall_guid}}]},)
    before_height = before["detailsOfElements"][0]["details"]["height"]

    invalid_enum = update("Wall", wall_guid, {"referenceLineLocation": "NotAReferenceLine"})
    require(invalid_enum and "error" in invalid_enum, f"invalid enum was accepted: {invalid_enum!r}")

    invalid_range = update("Wall", wall_guid, {"height": -1.0})
    require(invalid_range and "error" in invalid_range, f"invalid range was accepted: {invalid_range!r}")

    unsupported_geometry = update("Wall", wall_guid, {
        "coordinates": {"x": 1.0, "y": 1.0, "z": 0.0},
    })
    require(unsupported_geometry and "error" in unsupported_geometry,
            f"unsupported geometry was accepted: {unsupported_geometry!r}")

    conflicting = update("Wall", wall_guid, {
        "buildingMaterialId": {"guid": "not-a-guid"},
        "profileId": {"guid": "also-not-a-guid"},
    })
    require(conflicting and "error" in conflicting, f"conflicting references were accepted: {conflicting!r}")

    after = run("GetDetailsOfElements", {"elements": [{"elementId": {"guid": wall_guid}}]},)
    after_height = after["detailsOfElements"][0]["details"]["height"]
    require(math.isclose(before_height, after_height, rel_tol=0.0, abs_tol=1.0e-6),
            f"negative preflight changed the wall: before={before_height}, after={after_height}")

    print("TEST -- Slab create and sparse geometry/attribute update")
    slab_outline = [
        {"x": 5.0, "y": 5.0}, {"x": 9.0, "y": 5.0},
        {"x": 9.0, "y": 9.0}, {"x": 5.0, "y": 9.0},
    ]
    slab_result, slab_guid = create("Slab", {
        "level": 0.0,
        "thickness": 0.2,
        "polygonCoordinates": slab_outline,
    })
    require(math.isclose(details(slab_result)["thickness"], 0.2, rel_tol=0.0, abs_tol=1.0e-6),
            f"unexpected slab create readback: {slab_result!r}")
    slab_update = update("Slab", slab_guid, {
        "thickness": 0.25,
        "polygonOutline": slab_outline,
        "holes": [],
    })
    require(slab_update and slab_update.get("mutationComplete"), f"slab update failed: {slab_update!r}")
    require(slab_update["readbackVerified"], f"slab update readback failed: {slab_update!r}")

    print("TEST -- Column and Beam section updates cover every native memo segment")
    _, column_guid = create("Column", {
        "coordinates": {"x": 10.0, "y": 0.0, "z": 0.0},
        "height": 3.0,
        "width": 0.3,
        "depth": 0.3,
    })
    column_update = update("Column", column_guid, {"width": 0.45, "depth": 0.5})
    require(column_update and column_update.get("mutationComplete"), f"column section update failed: {column_update!r}")
    column_sections = column_update["readback"]["nativeSections"][0]["segments"]
    require(column_sections, f"column section readback is empty: {column_update!r}")
    require(all(math.isclose(s["nominalWidth"], 0.45, rel_tol=0.0, abs_tol=1.0e-6) and
                math.isclose(s["nominalHeight"], 0.5, rel_tol=0.0, abs_tol=1.0e-6)
                for s in column_sections), f"not every column segment was updated: {column_sections!r}")

    _, beam_guid = create("Beam", {
        "begCoordinate": {"x": 15.0, "y": 0.0},
        "endCoordinate": {"x": 19.0, "y": 0.0},
        "zCoordinate": 3.0,
        "width": 0.25,
        "height": 0.4,
    })
    beam_update = update("Beam", beam_guid, {"width": 0.35, "height": 0.55})
    require(beam_update and beam_update.get("mutationComplete"), f"beam section update failed: {beam_update!r}")
    beam_sections = beam_update["readback"]["nativeSections"][0]["segments"]
    require(beam_sections, f"beam section readback is empty: {beam_update!r}")
    require(all(math.isclose(s["nominalWidth"], 0.35, rel_tol=0.0, abs_tol=1.0e-6) and
                math.isclose(s["nominalHeight"], 0.55, rel_tol=0.0, abs_tol=1.0e-6)
                for s in beam_sections), f"not every beam segment was updated: {beam_sections!r}")

    print("TEST -- delete returns explicit absence confirmation")
    deleted = delete("Wall", wall_guid)
    require(deleted and deleted.get("mutationComplete"), f"delete failed: {deleted!r}")
    require(deleted["readbackVerified"], f"delete readback was not verified: {deleted!r}")
    require(deleted["readback"]["deleted"] == [{"elementId": {"guid": wall_guid}, "absent": True}],
            f"delete did not confirm absence: {deleted!r}")
    created.remove(("Wall", wall_guid))


try:
    run_test()
    print("=" * 60)
    print("PASS -- MutateElements native CRUD regression")
    print("=" * 60)
finally:
    # Keep cleanup explicit and best-effort.  If the new envelope itself is
    # the thing under test and fails, the mature Tapir delete path still
    # removes the disposable elements before the script exits.
    by_type = {}
    for element_type, guid in created:
        by_type.setdefault(element_type, []).append(guid)
    for element_type, guids in by_type.items():
        result = run("MutateElements", {
            "operation": "delete",
            "elementType": element_type,
            "items": [{"elementId": {"guid": guid}} for guid in guids],
        })
        if not result or not result.get("readbackVerified"):
            run("DeleteElements", {"elements": [{"elementId": {"guid": guid}} for guid in guids]})
