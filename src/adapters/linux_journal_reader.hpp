#pragma once

#include "linux_systemd_services.hpp"

namespace aiforge::adapters {
// Private native cursor views expire on the next cursor call. Construction,
// iteration and destruction belong to the same physical source worker thread.
struct LinuxJournalSelection {
  domain::LinuxOpsIdentity target;
  domain::LinuxServiceIdentity source;
  std::uint64_t lower_realtime_usec{};
};
class LinuxJournalCursor {
 public:
  virtual ~LinuxJournalCursor() = default;
  [[nodiscard]] virtual auto next(LinuxSystemdBudget& budget)
      -> std::expected<bool, runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto realtime_usec(LinuxSystemdBudget& budget)
      -> std::expected<std::uint64_t, runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto restart_fields(LinuxSystemdBudget& budget)
      -> std::expected<void, runtime::OpsObservationSourceError> = 0;
  [[nodiscard]] virtual auto next_field(LinuxSystemdBudget& budget)
      -> std::expected<std::optional<std::string_view>,
                       runtime::OpsObservationSourceError> = 0;
};
class LinuxJournalFactory {
 public:
  virtual ~LinuxJournalFactory() = default;
  [[nodiscard]] virtual auto open(const LinuxJournalSelection& selection,
                                  LinuxSystemdBudget& budget)
      -> std::expected<std::unique_ptr<LinuxJournalCursor>,
                       runtime::OpsObservationSourceError> = 0;
};
class LinuxJournalClock {
 public:
  virtual ~LinuxJournalClock() = default;
  [[nodiscard]] virtual auto realtime() const -> domain::EventTimestamp = 0;
};
[[nodiscard]] auto linux_journal_clock() -> const LinuxJournalClock&;
[[nodiscard]] auto make_linux_journal_factory()
    -> std::expected<std::shared_ptr<LinuxJournalFactory>,
                     runtime::OpsObservationSourceError>;
[[nodiscard]] auto validate_linux_journal_request(
    const domain::OpsObservationRequest& request)
    -> std::expected<void, runtime::OpsObservationSourceError>;
[[nodiscard]] auto observe_linux_service_logs(
    const domain::OpsObservationRequest& request, LinuxSystemdBus& bus,
    LinuxJournalFactory& factory, LinuxSystemdBudget& budget,
    domain::EventTimestamp started_at,
    const LinuxJournalClock& clock = linux_journal_clock())
    -> std::expected<domain::OpsObservation,
                     runtime::OpsObservationSourceError>;
// A threshold is only a native decompression hint. Reject lengths at or above
// this value so a possibly truncated field is never accepted as complete.
inline constexpr std::size_t linux_journal_field_threshold = 32777;
} // namespace aiforge::adapters
