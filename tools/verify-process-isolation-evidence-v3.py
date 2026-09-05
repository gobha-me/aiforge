#!/usr/bin/env python3
"""Fail closed unless exact-revision, exact-host v3 evidence is enforced."""

from __future__ import annotations

import json
import pathlib
import re
import sys


MAXIMUM_REPORT_BYTES = 8192
MAXIMUM_PLATFORM_METADATA_BYTES = 128
METADATA_PATTERN = re.compile(r"[A-Za-z0-9._+\-]{1,128}")
ALL_PROBES = (
    "direct_process_tree_cgroup_nonescape",
    "low_capability_nonescalation",
    "private_root_capability_discard",
)
LOW_REQUIRED_PROBES = ALL_PROBES[:2]
STATES = frozenset({"enforced", "unavailable", "probe_error"})
UNAVAILABLE_REASONS = frozenset(
    {
        "unsupported_kernel",
        "unsupported_architecture",
        "permission_denied",
        "mechanism_absent",
        "missing_delegation",
        "missing_controller",
        "enforcement_failed",
        "prerequisite_unavailable",
        "unsupported_combination",
    }
)
PROBE_ERROR_REASONS = frozenset(
    {
        "timeout",
        "cancelled",
        "pid_reuse",
        "setup_race",
        "signaled",
        "nonzero_exit",
        "malformed_protocol",
        "output_limit",
        "cleanup_failed",
        "internal_error",
    }
)
REASONS = frozenset({"none"}) | UNAVAILABLE_REASONS | PROBE_ERROR_REASONS


def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def valid_metadata(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) <= MAXIMUM_PLATFORM_METADATA_BYTES
        and METADATA_PATTERN.fullmatch(value) is not None
    )


def valid_correlation(state: object, reason: object) -> bool:
    return (
        (state == "enforced" and reason == "none")
        or (state == "unavailable" and reason in UNAVAILABLE_REASONS)
        or (state == "probe_error" and reason in PROBE_ERROR_REASONS)
    )


def validate_report(
    report: object,
    source_sha: str,
    expected_kernel: str,
    expected_architecture: str,
) -> dict[str, dict[str, object]]:
    if (
        not isinstance(report, dict)
        or set(report)
        != {
            "architecture",
            "kernel",
            "platform",
            "probes",
            "schema_version",
            "source_sha",
        }
        or type(report.get("schema_version")) is not int
        or report.get("schema_version") != 3
    ):
        raise ValueError("evidence report schema is not v3")
    if re.fullmatch(r"[0-9a-f]{40}", source_sha) is None:
        raise ValueError("expected source revision is invalid")
    if not valid_metadata(expected_kernel) or not valid_metadata(
        expected_architecture
    ):
        raise ValueError("expected Linux host identity is invalid")
    if (
        report.get("source_sha") != source_sha
        or report.get("platform") != "linux"
        or report.get("kernel") != expected_kernel
        or report.get("architecture") != expected_architecture
    ):
        raise ValueError("evidence provenance does not match this Linux revision and host")
    rows = report.get("probes")
    if not isinstance(rows, list):
        raise ValueError("evidence probe catalog is missing")
    by_id: dict[str, dict[str, object]] = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"probe_id", "state", "reason"}:
            raise ValueError("evidence probe row is malformed")
        probe_id = row.get("probe_id")
        state = row.get("state")
        reason = row.get("reason")
        if not isinstance(probe_id, str) or probe_id in by_id:
            raise ValueError("evidence probe identity is invalid or duplicated")
        if (
            not isinstance(state, str)
            or state not in STATES
            or not isinstance(reason, str)
            or reason not in REASONS
            or not valid_correlation(state, reason)
        ):
            raise ValueError(f"evidence probe state is invalid: {probe_id}")
        by_id[probe_id] = row
    if tuple(by_id) != ALL_PROBES:
        raise ValueError("evidence probe catalog is incomplete, reordered, or unknown")
    for probe_id in LOW_REQUIRED_PROBES:
        row = by_id[probe_id]
        if row.get("state") != "enforced" or row.get("reason") != "none":
            raise ValueError(f"required low evidence is not enforced: {probe_id}")
    for probe_id in ALL_PROBES:
        if by_id[probe_id].get("state") == "probe_error":
            raise ValueError(f"supplemental evidence is indeterminate: {probe_id}")
    return by_id


def load_report(path: pathlib.Path) -> object:
    with path.open("rb") as source:
        document = source.read(MAXIMUM_REPORT_BYTES + 1)
    if not document or len(document) > MAXIMUM_REPORT_BYTES:
        raise ValueError("evidence report is empty or oversized")
    return json.loads(document, object_pairs_hook=reject_duplicates)


def main() -> int:
    if (
        len(sys.argv) != 5
        or re.fullmatch(r"[0-9a-f]{40}", sys.argv[2]) is None
        or not valid_metadata(sys.argv[3])
        or not valid_metadata(sys.argv[4])
    ):
        print(
            f"usage: {sys.argv[0]} <report> <source-sha> <kernel> <architecture>",
            file=sys.stderr,
        )
        return 2
    report = load_report(pathlib.Path(sys.argv[1]))
    validate_report(report, sys.argv[2], sys.argv[3], sys.argv[4])
    print(
        f"verified {len(LOW_REQUIRED_PROBES)} low-required and "
        f"{len(ALL_PROBES)} exact-revision, exact-host supplemental rows"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
