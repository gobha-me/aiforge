#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/repository.hpp>

namespace aiforge::domain {

enum class VerificationKind {
  build,
  test,
  static_analysis,
  diagnostic,
  diff,
  runtime,
  unknown,
};

enum class VerificationOutcome {
  passed,
  failed,
  partial,
  cancelled,
  timed_out,
  unavailable,
  unknown,
};

enum class VerificationOutputStream {
  standard_output,
  standard_error,
};

enum class VerificationDiagnosticSeverity {
  note,
  warning,
  error,
  fatal,
  unknown,
};

struct VerificationProducer {
  std::string name;
  std::string version;
  std::string tool_name;
  InvocationId invocation_id;
  auto operator==(const VerificationProducer&) const -> bool = default;
};

struct VerificationOutputExcerpt {
  VerificationOutputStream stream{VerificationOutputStream::standard_output};
  std::string text;
  std::uint64_t represented_bytes{};
  bool truncated{};
  std::optional<ArtifactId> complete_artifact_id;
  auto operator==(const VerificationOutputExcerpt&) const -> bool = default;
};

struct VerificationDiagnostic {
  VerificationDiagnosticSeverity severity{VerificationDiagnosticSeverity::unknown};
  std::string code;
  std::string message;
  std::optional<RepositorySourceIdentity> source;
  auto operator==(const VerificationDiagnostic&) const -> bool = default;
};

struct VerificationEvidence {
  VerificationEvidenceId evidence_id;
  VerificationKind kind{VerificationKind::unknown};
  std::optional<std::string> extension_name;
  VerificationOutcome outcome{VerificationOutcome::unknown};
  RepositorySnapshotIdentity source_snapshot;
  std::optional<RepositorySnapshotIdentity> baseline_snapshot;
  std::optional<ContentDigest> build_configuration;
  VerificationProducer producer;
  std::chrono::sys_time<std::chrono::milliseconds> observed_at;
  std::string summary;
  std::vector<VerificationOutputExcerpt> output;
  std::vector<VerificationDiagnostic> diagnostics;
  std::vector<ArtifactId> artifacts;
  auto operator==(const VerificationEvidence&) const -> bool = default;
};

struct VerificationEvidenceReference {
  VerificationEvidenceId evidence_id;
  std::vector<ArtifactId> artifact_ids;
  auto operator==(const VerificationEvidenceReference&) const -> bool = default;
};

}  // namespace aiforge::domain
