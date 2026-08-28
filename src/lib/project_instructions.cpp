#include <aiforge/runtime/project_instructions.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const ProjectInstructionContextErrorCode code,
                           std::string message)
    -> std::unexpected<ProjectInstructionContextError> {
  return std::unexpected(
      ProjectInstructionContextError{code, std::move(message)});
}

[[nodiscard]] auto valid_relative_directory(const std::string& value) -> bool {
  if (value.size() > 4096 || value.find('\0') != std::string::npos) {
    return false;
  }
  if (value.empty()) return true;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
      path.generic_string() != value) {
    return false;
  }
  for (const auto& part : path) {
    if (part.empty() || part == "." || part == "..") return false;
  }
  return true;
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value) -> bool {
  if (value.empty() || value.size() > 1024U * 1024U) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7FU ||
        (first < 0x20U && first != '\t' && first != '\n' && first != '\r')) {
      return false;
    }
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
      codepoint = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto valid_digest(const domain::ContentDigest& digest,
                                const std::uint64_t expected_size) -> bool {
  if (digest.algorithm.empty() || digest.algorithm.size() > 128 ||
      digest.value.empty() || digest.value.size() > 512 ||
      digest.byte_size != expected_size) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto subtree_specificity(const std::string& subtree)
    -> std::uint32_t {
  return subtree.empty() ? 0
                         : static_cast<std::uint32_t>(std::ranges::distance(
                               std::filesystem::path{subtree}));
}

[[nodiscard]] auto applies_to_target(const std::string& scope,
                                     const std::string& target) -> bool {
  return scope.empty() || target == scope ||
         (target.size() > scope.size() && target.starts_with(scope) &&
          target[scope.size()] == '/');
}

[[nodiscard]] auto expected_instruction_path(const std::string& subtree)
    -> std::string {
  return subtree.empty() ? "AGENTS.md" : subtree + "/AGENTS.md";
}

[[nodiscard]] auto message_id(const domain::ProjectInstructionId& id)
    -> std::expected<domain::MessageId, ProjectInstructionContextError> {
  auto value = domain::MessageId::from(std::string{id.value()});
  if (!value) {
    return failure(ProjectInstructionContextErrorCode::invalid_identity,
                   "project instruction message identity is invalid");
  }
  return std::move(*value);
}

[[nodiscard]] auto entry_id(const domain::ProjectInstructionId& id)
    -> std::expected<domain::ContextEntryId, ProjectInstructionContextError> {
  auto value = domain::ContextEntryId::from(std::string{id.value()});
  if (!value) {
    return failure(ProjectInstructionContextErrorCode::invalid_identity,
                   "project instruction context identity is invalid");
  }
  return std::move(*value);
}

[[nodiscard]] auto source_id(const domain::ProjectInstructionId& id)
    -> std::expected<domain::ContextSourceId, ProjectInstructionContextError> {
  auto value = domain::ContextSourceId::from(std::string{id.value()});
  if (!value) {
    return failure(ProjectInstructionContextErrorCode::invalid_identity,
                   "project instruction source identity is invalid");
  }
  return std::move(*value);
}

} // namespace

auto project_instruction_inputs(
    const domain::ProjectInstructionDiscovery& discovery,
    const domain::RepositorySnapshotIdentity& current_snapshot,
    const std::span<const ProjectInstructionTokenEstimate> estimates)
    -> std::expected<std::vector<domain::InstructionInput>,
                     ProjectInstructionContextError> {
  try {
    if (!domain::same_source_state(discovery.source_snapshot,
                                   current_snapshot)) {
      return failure(ProjectInstructionContextErrorCode::stale_snapshot,
                     "project instructions were discovered from stale source");
    }
    if (!valid_relative_directory(discovery.target_subtree)) {
      return failure(ProjectInstructionContextErrorCode::invalid_discovery,
                     "project instruction target subtree is invalid");
    }

    std::map<domain::ProjectInstructionId, std::uint64_t> estimates_by_id;
    for (const auto& estimate : estimates) {
      if (estimate.estimated_tokens == 0) {
        return failure(ProjectInstructionContextErrorCode::invalid_estimate,
                       "project instruction token estimates must be positive");
      }
      if (!estimates_by_id
               .emplace(estimate.instruction_id, estimate.estimated_tokens)
               .second) {
        return failure(ProjectInstructionContextErrorCode::duplicate_estimate,
                       "project instruction token estimate is duplicated");
      }
    }
    if (estimates_by_id.size() != discovery.documents.size()) {
      return failure(ProjectInstructionContextErrorCode::missing_estimate,
                     "each project instruction requires one token estimate");
    }

    std::set<domain::ProjectInstructionId> instruction_ids;
    std::set<std::string> source_paths;
    std::uint64_t previous_order{};
    std::uint32_t previous_specificity{};
    std::vector<domain::InstructionInput> result;
    result.reserve(discovery.documents.size());
    for (const auto& document : discovery.documents) {
      if (!instruction_ids.insert(document.instruction_id).second ||
          !source_paths.insert(document.source.relative_path).second ||
          !valid_relative_directory(document.applicable_subtree) ||
          !applies_to_target(document.applicable_subtree,
                             discovery.target_subtree) ||
          document.source.relative_path !=
              expected_instruction_path(document.applicable_subtree) ||
          !domain::same_source_state(document.source.snapshot,
                                     discovery.source_snapshot) ||
          document.source.range || !valid_utf8_text(document.text) ||
          !valid_digest(document.source.content_digest, document.text.size()) ||
          document.specificity !=
              subtree_specificity(document.applicable_subtree) ||
          document.discovery_order != result.size() + 1U ||
          document.discovery_order <= previous_order ||
          (!result.empty() && document.specificity <= previous_specificity)) {
        return failure(ProjectInstructionContextErrorCode::invalid_discovery,
                       "project instruction discovery is inconsistent");
      }
      previous_order = document.discovery_order;
      previous_specificity = document.specificity;
      const auto estimate = estimates_by_id.find(document.instruction_id);
      if (estimate == estimates_by_id.end()) {
        return failure(ProjectInstructionContextErrorCode::missing_estimate,
                       "project instruction token estimate is missing");
      }
      auto context_entry_id = entry_id(document.instruction_id);
      auto context_message_id = message_id(document.instruction_id);
      auto context_source_id = source_id(document.instruction_id);
      if (!context_entry_id) return std::unexpected(context_entry_id.error());
      if (!context_message_id)
        return std::unexpected(context_message_id.error());
      if (!context_source_id) return std::unexpected(context_source_id.error());

      result.push_back(domain::InstructionInput{
          std::move(*context_entry_id), domain::InstructionLayer::project,
          domain::InstructionOperation::add, std::nullopt,
          domain::Message{std::move(*context_message_id),
                          domain::Role::system,
                          {domain::TextBlock{document.text}},
                          std::nullopt},
          domain::ContextProvenance{
              std::move(*context_source_id), document.source.relative_path,
              document.source.content_digest.algorithm + ":" +
                  document.source.content_digest.value},
          document.specificity, document.discovery_order, estimate->second});
    }
    return result;
  } catch (...) {
    return failure(ProjectInstructionContextErrorCode::internal_failure,
                   "project instruction context preparation failed internally");
  }
}

} // namespace aiforge::runtime
