"""
Live acceptance test for the all-or-none update boundary of MutateElements.

The second wall is locked before the batch is sent.  Archicad must reject the
batch, expose transactionRolledBack, and leave the first wall unchanged.  The
test is intentionally separate from the CRUD smoke test: it is the acceptance
gate for the project-owned atomic mode and should never be silently weakened to
accept a partial update.
"""

import math

import aclib


created = []
command_unknown = False


def valid_guid(value):
    return (isinstance(value, dict) and isinstance(value.get("guid"), str)
            and bool(value.get("guid")))


def valid_error(value):
    return (isinstance(value, dict) and isinstance(value.get("code"), int)
            and not isinstance(value.get("code"), bool)
            and isinstance(value.get("message"), str))


def validate_response(command, result):
    # A syntactically valid but semantically incomplete addOnCommandResponse
    # is an unknown post-dispatch outcome; cleanup must not write afterwards.
    if not isinstance(result, dict):
        raise RuntimeError(f"malformed {command} response: {result!r}")
    error = result.get("error")
    if error is not None:
        if not isinstance(error, dict) or not isinstance(error.get("code"), int) or not isinstance(error.get("message"), str):
            raise RuntimeError(f"malformed {command} error response: {result!r}")
        return result
    required = {
        "MutateElements": ("operation", "elementType", "requestedCount", "appliedCount", "mutationComplete", "readbackVerified", "partial", "mutationResult", "readback"),
        "GetDetailsOfElements": ("detailsOfElements",),
        "LockElements": ("success",),
        "UnlockElements": ("success",),
    }.get(command, ())
    missing = [field for field in required if field not in result]
    if missing:
        raise RuntimeError(f"malformed {command} response; missing {missing!r}: {result!r}")
    if command == "MutateElements":
        if (result["operation"] not in ("create", "update", "delete")
                or result["elementType"] not in ("Wall", "Slab", "Column", "Beam")
                or not isinstance(result["requestedCount"], int) or isinstance(result["requestedCount"], bool)
                or not isinstance(result["appliedCount"], int) or isinstance(result["appliedCount"], bool)
                or result["requestedCount"] < 0 or result["appliedCount"] < 0
                or not isinstance(result["mutationComplete"], bool) or not isinstance(result["readbackVerified"], bool)
                or not isinstance(result["partial"], bool) or not isinstance(result["mutationResult"], dict)
                or not isinstance(result["readback"], dict)):
            raise RuntimeError(f"malformed MutateElements response: {result!r}")
        operation = result["operation"]
        mutation_result = result["mutationResult"]
        readback = result["readback"]
        if operation in ("create", "update"):
            native_details = readback.get("nativeDetails")
            detail_rows = native_details.get("detailsOfElements") if isinstance(native_details, dict) else None
            element_rows = readback.get("elements")
            if not isinstance(native_details, dict) or not isinstance(detail_rows, list) or not isinstance(element_rows, list):
                raise RuntimeError(f"malformed MutateElements readback: {result!r}")
            for row in detail_rows:
                if (not isinstance(row, dict) or not valid_guid(row.get("elementId"))
                        or not isinstance(row.get("details"), dict)):
                    raise RuntimeError(f"malformed MutateElements detail readback: {result!r}")
            for row in element_rows:
                if (not isinstance(row, dict) or not valid_guid(row.get("elementId"))
                        or not isinstance(row.get("exists"), bool)
                        or not isinstance(row.get("elementTypeMatches"), bool)):
                    raise RuntimeError(f"malformed MutateElements element readback: {result!r}")
        else:
            deleted_rows = readback.get("deleted")
            if not isinstance(deleted_rows, list):
                raise RuntimeError(f"malformed MutateElements delete readback: {result!r}")
            for row in deleted_rows:
                if (not isinstance(row, dict) or not valid_guid(row.get("elementId"))
                        or not isinstance(row.get("absent"), bool)
                        or ("readbackErrorCode" in row
                            and (not isinstance(row["readbackErrorCode"], int)
                                 or isinstance(row["readbackErrorCode"], bool)))):
                    raise RuntimeError(f"malformed MutateElements delete row: {result!r}")
        if operation == "create":
            rows = mutation_result.get("elements")
            if not isinstance(rows, list):
                raise RuntimeError(f"malformed MutateElements create result: {result!r}")
            for row in rows:
                if (not isinstance(row, dict)
                        or ("elementId" not in row and "error" not in row)
                        or ("elementId" in row and not valid_guid(row["elementId"]))
                        or ("error" in row and not valid_error(row["error"]))):
                    raise RuntimeError(f"malformed MutateElements create item: {result!r}")
        elif operation == "update":
            rows = mutation_result.get("executionResults")
            if not isinstance(rows, list):
                raise RuntimeError(f"malformed MutateElements update result: {result!r}")
            for row in rows:
                if (not isinstance(row, dict) or not isinstance(row.get("success"), bool)
                        or ("error" in row and not valid_error(row["error"]))):
                    raise RuntimeError(f"malformed MutateElements update item: {result!r}")
        elif not isinstance(mutation_result.get("success"), bool):
            raise RuntimeError(f"malformed MutateElements delete result: {result!r}")
    elif command in ("LockElements", "UnlockElements") and not isinstance(result["success"], bool):
        raise RuntimeError(f"malformed {command} response: {result!r}")
    elif command == "GetDetailsOfElements":
        for row in result["detailsOfElements"]:
            if (not isinstance(row, dict) or not valid_guid(row.get("elementId"))
                    or not isinstance(row.get("details"), dict)):
                raise RuntimeError(f"malformed GetDetailsOfElements response: {result!r}")
    return result


def run(command, parameters):
    global command_unknown
    try:
        return validate_response(command, aclib.RunTapirCommand(command, parameters, debug=False))
    except Exception:
        # A timeout/disconnect may have applied the write.  Never issue a
        # second command in that process; disposable shutdown is recovery.
        command_unknown = True
        raise


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def create_wall(x):
    result = run("MutateElements", {
        "operation": "create",
        "elementType": "Wall",
        "items": [{"payload": {
            "begCoordinate": {"x": x, "y": 20.0},
            "endCoordinate": {"x": x + 4.0, "y": 20.0},
            "height": 3.0,
            "thickness": 0.2,
        }}],
    })
    require(result and result.get("mutationComplete"), f"wall create failed: {result!r}")
    require(result.get("readbackVerified"), f"wall create readback failed: {result!r}")
    rows = result["readback"]["nativeDetails"]["detailsOfElements"]
    guid = rows[0]["elementId"]["guid"]
    created.append(guid)
    return guid


def wall_height(guid):
    result = run("GetDetailsOfElements", {"elements": [{"elementId": {"guid": guid}}]})
    return result["detailsOfElements"][0]["details"]["height"]


def run_test():
    print("TEST -- MutateElements update is all-or-none when one wall is locked")
    first = create_wall(0.0)
    second = create_wall(8.0)
    before = {first: wall_height(first), second: wall_height(second)}

    locked = run("LockElements", {"elements": [{"elementId": {"guid": second}}]})
    require(locked and locked.get("success") is True, f"lock setup failed: {locked!r}")
    try:
        result = run("MutateElements", {
            "operation": "update",
            "elementType": "Wall",
            "items": [
                {"elementId": {"guid": first}, "payload": {"height": 4.0}},
                {"elementId": {"guid": second}, "payload": {"height": 5.0}},
            ],
        })
        require(result and result.get("mutationComplete") is False,
                f"locked batch unexpectedly succeeded: {result!r}")
        mutation_result = result.get("mutationResult", {})
        require(mutation_result.get("transactionRolledBack") is True,
                f"atomic rollback evidence is missing: {result!r}")
        after = {first: wall_height(first), second: wall_height(second)}
        for guid in (first, second):
            require(math.isclose(before[guid], after[guid], rel_tol=0.0, abs_tol=1.0e-6),
                    f"atomic batch changed {guid}: before={before[guid]}, after={after[guid]}")
    finally:
        if not command_unknown:
            run("UnlockElements", {"elements": [{"elementId": {"guid": second}}]})


try:
    run_test()
    print("=" * 60)
    print("PASS -- MutateElements atomic update regression")
    print("=" * 60)
finally:
    if command_unknown:
        print("cleanup skipped: a command outcome is unknown; disposable process shutdown is the recovery boundary")
        created.clear()
    if created:
        # Cleanup is one best-effort envelope call.  Never retry an unknown
        # POST through legacy DeleteElements: the first request may already
        # have been applied.  The disposable process is the recovery boundary.
        items = [{"elementId": {"guid": guid}} for guid in created]
        try:
            cleanup_result = run("MutateElements", {
                "operation": "delete",
                "elementType": "Wall",
                "items": items,
            })
            require(cleanup_result and cleanup_result.get("mutationComplete") is True and cleanup_result.get("readbackVerified") is True,
                    f"cleanup mutation did not prove deletion: {cleanup_result!r}")
        except Exception as cleanup_error:
            print(f"cleanup outcome failed or unknown for atomicity fixture: {cleanup_error!r}")
            raise
