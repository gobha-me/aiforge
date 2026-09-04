#include <aiforge/runtime/user_global_instructions.hpp>

#include <utility>

namespace aiforge::runtime {
namespace {

[[nodiscard]] auto failure(const UserGlobalInstructionContextErrorCode code,
                           std::string message)
    -> std::unexpected<UserGlobalInstructionContextError> {
  return std::unexpected(
      UserGlobalInstructionContextError{code, std::move(message)});
}

template <typename Id>
[[nodiscard]] auto fixed_id(const std::string_view value)
    -> std::expected<Id, UserGlobalInstructionContextError> {
  auto id = Id::from(std::string{value});
  if (!id) {
    return failure(UserGlobalInstructionContextErrorCode::invalid_identity,
                   "user-global instruction context identity is invalid");
  }
  return std::move(*id);
}

} // namespace

auto user_global_instruction_input(
    const domain::UserGlobalInstructionDocument& document,
    const std::uint64_t estimated_tokens, const std::uint64_t order)
    -> std::expected<domain::InstructionInput,
                     UserGlobalInstructionContextError> {
  try {
    if (!domain::validate_user_global_instruction_document(document)) {
      return failure(UserGlobalInstructionContextErrorCode::invalid_document,
                     "user-global instruction document is invalid");
    }
    if (estimated_tokens == 0 || order == 0) {
      return failure(UserGlobalInstructionContextErrorCode::invalid_estimate,
                     "user-global instruction estimate and order must be "
                     "positive");
    }
    auto entry_id =
        fixed_id<domain::ContextEntryId>("user-global-instruction-entry");
    auto message_id =
        fixed_id<domain::MessageId>("user-global-instruction-message");
    if (!entry_id) return std::unexpected(entry_id.error());
    if (!message_id) return std::unexpected(message_id.error());
    return domain::InstructionInput{
        std::move(*entry_id),
        domain::InstructionLayer::user_global,
        domain::InstructionOperation::add,
        std::nullopt,
        domain::Message{std::move(*message_id),
                        domain::Role::system,
                        {domain::TextBlock{document.text}},
                        std::nullopt},
        {document.reference.source_id, document.reference.source_location,
         document.reference.content_digest.algorithm + ":" +
             document.reference.content_digest.value},
        0,
        order,
        estimated_tokens};
  } catch (...) {
    return failure(UserGlobalInstructionContextErrorCode::internal_failure,
                   "user-global instruction context preparation failed "
                   "internally");
  }
}

} // namespace aiforge::runtime
