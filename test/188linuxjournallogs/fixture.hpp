#pragma once
#include "../../src/adapters/linux_journal_reader.hpp"
#include "../184linuxservices/fixture.hpp"

namespace journal_fixture {
using namespace aiforge;
using Error = runtime::OpsObservationSourceError;
struct Entry {
  std::uint64_t timestamp{1000000000};
  std::vector<std::string> fields{
      "_BOOT_ID=12345678123412341234123456789abc", "_SYSTEMD_UNIT=ssh.service",
      "_SYSTEMD_INVOCATION_ID=01010101010101010101010101010101",
      "MESSAGE=hello"};
};
struct State {
  std::vector<Entry> entries{Entry{}};
  std::size_t calls{}, opens{}, closes{};
  std::optional<std::size_t> fail_at{}, expire_at{}, stop_at{};
  Error failure{Error::invalid_result};
  std::stop_source stop;
  std::optional<adapters::LinuxJournalSelection> selection;
  auto step(adapters::LinuxSystemdBudget& budget)
      -> std::expected<void, Error> {
    ++calls;
    if (expire_at == calls)
      budget.deadline = std::chrono::steady_clock::time_point::min();
    if (stop_at == calls) stop.request_stop();
    if (auto ready = budget.check(); !ready) return ready;
    if (fail_at == calls) return std::unexpected(failure);
    return {};
  }
};
class Cursor final : public adapters::LinuxJournalCursor {
 public:
  explicit Cursor(std::shared_ptr<State> state) : m_state(std::move(state)) {}
  ~Cursor() override { ++m_state->closes; }
  auto next(adapters::LinuxSystemdBudget& budget)
      -> std::expected<bool, Error> override {
    if (auto step = m_state->step(budget); !step)
      return std::unexpected(step.error());
    return m_entry++ < m_state->entries.size();
  }
  auto realtime_usec(adapters::LinuxSystemdBudget& budget)
      -> std::expected<std::uint64_t, Error> override {
    if (auto step = m_state->step(budget); !step)
      return std::unexpected(step.error());
    return m_state->entries.at(m_entry - 1).timestamp;
  }
  auto restart_fields(adapters::LinuxSystemdBudget& budget)
      -> std::expected<void, Error> override {
    m_field = 0;
    return m_state->step(budget);
  }
  auto next_field(adapters::LinuxSystemdBudget& budget)
      -> std::expected<std::optional<std::string_view>, Error> override {
    if (auto step = m_state->step(budget); !step)
      return std::unexpected(step.error());
    const auto& fields = m_state->entries.at(m_entry - 1).fields;
    if (m_field == fields.size()) return std::nullopt;
    return std::string_view{fields[m_field++]};
  }

 private:
  std::shared_ptr<State> m_state;
  std::size_t m_entry{}, m_field{};
};
class Factory final : public adapters::LinuxJournalFactory {
 public:
  std::shared_ptr<State> state = std::make_shared<State>();
  auto open(const adapters::LinuxJournalSelection& selection,
            adapters::LinuxSystemdBudget& budget)
      -> std::expected<std::unique_ptr<adapters::LinuxJournalCursor>,
                       Error> override {
    ++state->opens;
    if (auto step = state->step(budget); !step)
      return std::unexpected(step.error());
    state->selection = selection;
    return std::make_unique<Cursor>(state);
  }
};
class Clock final : public adapters::LinuxJournalClock {
 public:
  domain::EventTimestamp value{std::chrono::milliseconds{1000000}};
  mutable std::size_t calls{};
  std::optional<std::size_t> jump_at;
  std::chrono::milliseconds jump{};
  auto realtime() const -> domain::EventTimestamp override {
    return value + (jump_at && ++calls >= *jump_at
                        ? jump
                        : std::chrono::milliseconds{0});
  }
};
struct Fixture {
  service_fixture::Fixture service;
  Factory factory;
  Clock clock;
  domain::OpsObservationRequest request = service.request();
  adapters::LinuxSystemdBudget budget{std::chrono::steady_clock::now() +
                                          std::chrono::seconds{5},
                                      factory.state->stop.get_token()};
  Fixture() {
    request.operation = domain::OpsObservationOperation::linux_service_logs;
    std::get<domain::LinuxServiceIdentity>(request.resource).invocation_id =
        service_fixture::id<domain::OpsResourceUid>(
            "01010101010101010101010101010101");
    service.handshake();
    identity();
    identity();
    service.proof();
  }
  void identity() {
    auto& steps = service.platform->state->steps;
    steps.push_back(
        {"GetUnit", "",
         service_fixture::text(
             adapters::linux_systemd_unit_path("ssh.service").value(),
             DBUS_TYPE_OBJECT_PATH)});
    steps.push_back({"Get", "Id", service_fixture::property("ssh.service")});
    steps.push_back(
        {"Get", "InvocationID",
         service_fixture::invocation(std::vector<unsigned char>(16, 1))});
  }
  auto run() -> std::expected<domain::OpsObservation, Error> {
    return adapters::observe_linux_service_logs(request, *service.bus, factory,
                                                budget, clock.value, clock);
  }
};
} // namespace journal_fixture
