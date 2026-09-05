#!/usr/bin/env python3
"""Adversarial tests for the exact-SHA process-isolation evidence gate."""

from __future__ import annotations

import copy
import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name(
    "verify-process-isolation-evidence-v2.py"
)
SPEC = importlib.util.spec_from_file_location("process_isolation_verifier", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load process-isolation verifier")
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)
SOURCE_SHA = "a" * 40


def canonical_report() -> dict[str, object]:
    return {
        "architecture": "x86_64",
        "kernel": "6.11.0-1018-azure",
        "platform": "linux",
        "probes": [
            {"probe_id": probe_id, "state": "enforced", "reason": "none"}
            for probe_id in VERIFIER.ALL_PROBES
        ],
        "schema_version": 2,
        "source_sha": SOURCE_SHA,
    }


class EvidenceVerifierTests(unittest.TestCase):
    def test_canonical_report_is_accepted(self) -> None:
        VERIFIER.validate_report(canonical_report(), SOURCE_SHA)

    def test_metadata_types_bounds_and_alphabet_fail_closed(self) -> None:
        for field in ("architecture", "kernel"):
            for invalid in (None, 7, "", "a" * 129, "linux/escape", "line\nbreak"):
                with self.subTest(field=field, invalid=invalid):
                    report = canonical_report()
                    report[field] = invalid
                    with self.assertRaises(ValueError):
                        VERIFIER.validate_report(report, SOURCE_SHA)

    def test_schema_platform_and_source_types_fail_closed(self) -> None:
        for field, invalid in (
            ("schema_version", True),
            ("schema_version", "2"),
            ("platform", ["linux"]),
            ("platform", "Linux"),
            ("source_sha", [SOURCE_SHA]),
            ("source_sha", "b" * 40),
        ):
            with self.subTest(field=field, invalid=invalid):
                report = canonical_report()
                report[field] = invalid
                with self.assertRaises(ValueError):
                    VERIFIER.validate_report(report, SOURCE_SHA)

    def test_every_row_rejects_nonclosed_state_and_reason_values(self) -> None:
        for index, probe_id in enumerate(VERIFIER.ALL_PROBES):
            for field, invalid in (
                ("state", "future_state"),
                ("state", 1),
                ("reason", "future_reason"),
                ("reason", 1),
            ):
                with self.subTest(probe_id=probe_id, field=field, invalid=invalid):
                    report = canonical_report()
                    report["probes"][index][field] = invalid
                    with self.assertRaises(ValueError):
                        VERIFIER.validate_report(report, SOURCE_SHA)

    def test_state_reason_correlations_match_the_closed_schema(self) -> None:
        probe_index = VERIFIER.ALL_PROBES.index("landlock_read_confinement")
        for state in VERIFIER.STATES:
            for reason in VERIFIER.REASONS:
                with self.subTest(state=state, reason=reason):
                    report = canonical_report()
                    report["probes"][probe_index]["state"] = state
                    report["probes"][probe_index]["reason"] = reason
                    if VERIFIER.valid_correlation(state, reason):
                        VERIFIER.validate_report(report, SOURCE_SHA)
                    else:
                        with self.assertRaises(ValueError):
                            VERIFIER.validate_report(report, SOURCE_SHA)

    def test_required_rows_cannot_be_valid_but_unenforced(self) -> None:
        for probe_id in VERIFIER.REQUIRED_PROBES:
            with self.subTest(probe_id=probe_id):
                report = canonical_report()
                index = VERIFIER.ALL_PROBES.index(probe_id)
                report["probes"][index]["state"] = "unavailable"
                report["probes"][index]["reason"] = "missing_delegation"
                with self.assertRaises(ValueError):
                    VERIFIER.validate_report(report, SOURCE_SHA)

    def test_duplicate_json_fields_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            VERIFIER.reject_duplicates([("state", "enforced"), ("state", "enforced")])

    def test_catalog_shape_order_and_identity_fail_closed(self) -> None:
        mutations = []
        missing = canonical_report()
        missing["probes"].pop()
        mutations.append(missing)
        reordered = canonical_report()
        reordered["probes"][0], reordered["probes"][1] = (
            reordered["probes"][1],
            reordered["probes"][0],
        )
        mutations.append(reordered)
        duplicated = canonical_report()
        duplicated["probes"][1] = copy.deepcopy(duplicated["probes"][0])
        mutations.append(duplicated)
        unknown_field = canonical_report()
        unknown_field["probes"][0]["extra"] = True
        mutations.append(unknown_field)
        for report in mutations:
            with self.subTest(report=report):
                with self.assertRaises(ValueError):
                    VERIFIER.validate_report(report, SOURCE_SHA)


if __name__ == "__main__":
    unittest.main()
