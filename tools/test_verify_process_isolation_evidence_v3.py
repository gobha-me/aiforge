#!/usr/bin/env python3
"""Adversarial tests for the exact-host schema-v3 evidence gate."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name(
    "verify-process-isolation-evidence-v3.py"
)
SPEC = importlib.util.spec_from_file_location("process_isolation_v3_verifier", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load process-isolation v3 verifier")
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)
SOURCE_SHA = "a" * 40
KERNEL = "6.11.0-1018-azure"
ARCHITECTURE = "x86_64"


def canonical_report() -> dict[str, object]:
    return {
        "architecture": ARCHITECTURE,
        "kernel": KERNEL,
        "platform": "linux",
        "probes": [
            {"probe_id": probe_id, "state": "enforced", "reason": "none"}
            for probe_id in VERIFIER.ALL_PROBES
        ],
        "schema_version": 3,
        "source_sha": SOURCE_SHA,
    }


class EvidenceVerifierV3Tests(unittest.TestCase):
    def validate(self, report: object) -> None:
        VERIFIER.validate_report(report, SOURCE_SHA, KERNEL, ARCHITECTURE)

    def test_canonical_report_is_accepted(self) -> None:
        self.validate(canonical_report())

    def test_exact_revision_and_host_identity_fail_closed(self) -> None:
        for field, invalid in (
            ("schema_version", True),
            ("schema_version", "3"),
            ("platform", "Linux"),
            ("source_sha", "b" * 40),
            ("kernel", "6.12.0"),
            ("architecture", "aarch64"),
        ):
            with self.subTest(field=field, invalid=invalid):
                report = canonical_report()
                report[field] = invalid
                with self.assertRaises(ValueError):
                    self.validate(report)
        for kernel, architecture in (
            ("", ARCHITECTURE),
            ("host/path", ARCHITECTURE),
            (KERNEL, ""),
            (KERNEL, "a" * 129),
        ):
            with self.subTest(kernel=kernel, architecture=architecture):
                with self.assertRaises(ValueError):
                    VERIFIER.validate_report(
                        canonical_report(), SOURCE_SHA, kernel, architecture
                    )
        for source_sha in ("", "A" * 40, "a" * 39, "g" * 40):
            with self.subTest(source_sha=source_sha):
                report = canonical_report()
                report["source_sha"] = source_sha
                with self.assertRaises(ValueError):
                    VERIFIER.validate_report(
                        report, source_sha, KERNEL, ARCHITECTURE
                    )

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
                        self.validate(report)

    def test_state_reason_correlations_match_the_closed_schema(self) -> None:
        for state in VERIFIER.STATES:
            for reason in VERIFIER.REASONS:
                with self.subTest(state=state, reason=reason):
                    report = canonical_report()
                    report["probes"][0]["state"] = state
                    report["probes"][0]["reason"] = reason
                    if state == "enforced" and reason == "none":
                        self.validate(report)
                    else:
                        with self.assertRaises(ValueError):
                            self.validate(report)
                    self.assertEqual(
                        VERIFIER.valid_correlation(state, reason),
                        (state == "enforced" and reason == "none")
                        or (state == "unavailable" and reason in VERIFIER.UNAVAILABLE_REASONS)
                        or (state == "probe_error" and reason in VERIFIER.PROBE_ERROR_REASONS),
                    )

    def test_every_required_row_rejects_unavailable_or_indeterminate(self) -> None:
        for probe_id in VERIFIER.LOW_REQUIRED_PROBES:
            index = VERIFIER.ALL_PROBES.index(probe_id)
            for state, reason in (
                ("unavailable", "mechanism_absent"),
                ("probe_error", "cleanup_failed"),
            ):
                with self.subTest(probe_id=probe_id, state=state):
                    report = canonical_report()
                    report["probes"][index]["state"] = state
                    report["probes"][index]["reason"] = reason
                    with self.assertRaises(ValueError):
                        self.validate(report)

    def test_truthful_unavailable_high_row_preserves_the_low_gate(self) -> None:
        report = canonical_report()
        high_index = VERIFIER.ALL_PROBES.index(
            "private_root_capability_discard"
        )
        report["probes"][high_index]["state"] = "unavailable"
        report["probes"][high_index]["reason"] = "permission_denied"
        self.validate(report)

        report["probes"][high_index]["state"] = "probe_error"
        report["probes"][high_index]["reason"] = "cleanup_failed"
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_duplicate_fields_and_catalog_mutations_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            VERIFIER.reject_duplicates([("state", "enforced"), ("state", "enforced")])
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
        additional = canonical_report()
        additional["probes"].append(copy.deepcopy(additional["probes"][-1]))
        mutations.append(additional)
        unknown = canonical_report()
        unknown["probes"][0]["probe_id"] = "future_probe"
        mutations.append(unknown)
        unknown_field = canonical_report()
        unknown_field["probes"][0]["extra"] = True
        mutations.append(unknown_field)
        for report in mutations:
            with self.subTest(report=report):
                with self.assertRaises(ValueError):
                    self.validate(report)

    def test_bounded_loader_rejects_empty_oversized_and_duplicate_json(self) -> None:
        documents = (
            b"",
            b"x" * (VERIFIER.MAXIMUM_REPORT_BYTES + 1),
            b'{"schema_version":3,"schema_version":3}',
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            for document in documents:
                with self.subTest(size=len(document)):
                    path.write_bytes(document)
                    with self.assertRaises(ValueError):
                        VERIFIER.load_report(path)

            path.write_text(json.dumps(canonical_report()), encoding="utf-8")
            self.assertEqual(VERIFIER.load_report(path), canonical_report())


if __name__ == "__main__":
    unittest.main()
