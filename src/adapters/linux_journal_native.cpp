#include "linux_journal_reader.hpp"

#include <array>
#include <cerrno>
#include <systemd/sd-journal.h>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
auto native_error(int result) -> Error {
  if (result == -ENOMEM || result == -E2BIG || result == -ENOBUFS)
    return Error::resource_exhausted;
  if (result == -EACCES || result == -EPERM || result == -ENOENT ||
      result == -ENODATA)
    return Error::unavailable;
  return Error::invalid_result;
}
struct JournalDeleter {
  void operator()(sd_journal* journal) const noexcept {
    if (journal != nullptr) sd_journal_close(journal);
  }
};
using Journal = std::unique_ptr<sd_journal, JournalDeleter>;
class NativeCursor final : public LinuxJournalCursor {
 public:
  explicit NativeCursor(Journal journal) : m_journal(std::move(journal)) {}
  auto next(LinuxSystemdBudget& budget) -> std::expected<bool, Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const auto result = sd_journal_next(m_journal.get());
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    if (result < 0) return std::unexpected(native_error(result));
    return result != 0;
  }
  auto realtime_usec(LinuxSystemdBudget& budget)
      -> std::expected<std::uint64_t, Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    std::uint64_t timestamp{};
    const auto result =
        sd_journal_get_realtime_usec(m_journal.get(), &timestamp);
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    if (result < 0) return std::unexpected(native_error(result));
    return timestamp;
  }
  auto restart_fields(LinuxSystemdBudget& budget)
      -> std::expected<void, Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    sd_journal_restart_data(m_journal.get());
    return budget.check();
  }
  auto next_field(LinuxSystemdBudget& budget)
      -> std::expected<std::optional<std::string_view>, Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const void* data{};
    std::size_t length{};
    const auto result =
        sd_journal_enumerate_data(m_journal.get(), &data, &length);
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    if (result < 0) return std::unexpected(native_error(result));
    if (result == 0) return std::nullopt;
    if (data == nullptr) return std::unexpected(Error::invalid_result);
    // Return a borrowed view only. Common decoding charges/checks it before
    // copy.
    return std::string_view{static_cast<const char*>(data), length};
  }

 private:
  Journal m_journal;
};
class NativeFactory final : public LinuxJournalFactory {
 public:
  auto open(const LinuxJournalSelection& selection, LinuxSystemdBudget& budget)
      -> std::expected<std::unique_ptr<LinuxJournalCursor>, Error> override {
    try {
      if (auto ready = budget.check(); !ready)
        return std::unexpected(ready.error());
      if (!selection.source.invocation_id)
        return std::unexpected(Error::invalid_result);
      std::string boot;
      boot.reserve(32);
      for (char byte : selection.target.boot_id)
        if (byte != '-') boot.push_back(byte);
      const std::array matches{
          std::string{"_BOOT_ID="} + boot,
          std::string{"_SYSTEMD_UNIT="} + selection.source.unit_name,
          std::string{"_SYSTEMD_INVOCATION_ID="} +
              std::string{selection.source.invocation_id->value()}};
      sd_journal* raw{};
      const auto opened =
          sd_journal_open(&raw, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM);
      Journal journal{raw};
      if (auto ready = budget.check(); !ready)
        return std::unexpected(ready.error());
      if (opened < 0) return std::unexpected(native_error(opened));
      if (!journal) return std::unexpected(Error::internal_failure);
      auto result = sd_journal_set_data_threshold(
          journal.get(), linux_journal_field_threshold);
      if (result < 0) return std::unexpected(native_error(result));
      for (const auto& match : matches) {
        if (auto ready = budget.check(); !ready)
          return std::unexpected(ready.error());
        result =
            sd_journal_add_match(journal.get(), match.data(), match.size());
        if (result < 0) return std::unexpected(native_error(result));
      }
      if (auto ready = budget.check(); !ready)
        return std::unexpected(ready.error());
      result = sd_journal_seek_realtime_usec(journal.get(),
                                             selection.lower_realtime_usec);
      if (auto ready = budget.check(); !ready)
        return std::unexpected(ready.error());
      if (result < 0) return std::unexpected(native_error(result));
      return std::make_unique<NativeCursor>(std::move(journal));
    } catch (...) {
      return std::unexpected(Error::internal_failure);
    }
  }
};
} // namespace
auto make_linux_journal_factory()
    -> std::expected<std::shared_ptr<LinuxJournalFactory>, Error> {
  try {
    return std::make_shared<NativeFactory>();
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
