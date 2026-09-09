#pragma once

#include "kubernetes_observation_projection.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aiforge::adapters::kubernetes_projection_detail {
using Failure = runtime::OpsObservationSourceError;
inline constexpr std::size_t input_limit = std::size_t{1024} * 1024U;
inline constexpr std::size_t scalar_limit = std::size_t{128} * 1024U;
inline constexpr std::size_t key_limit = std::size_t{128} * 1024U;
inline constexpr std::size_t value_limit = std::size_t{256} * 1024U;
inline constexpr std::size_t event_limit = 32768;
inline constexpr std::size_t depth_limit = 32;
inline constexpr std::size_t record_limit = 1024;
struct Rejected {
  Failure failure;
};
[[noreturn]] auto reject(Failure failure = Failure::invalid_result) -> void;
auto require(bool condition, Failure failure = Failure::invalid_result) -> void;
class Budget final {
 public:
  explicit Budget(std::stop_token stop) : m_stop(std::move(stop)) {}
  auto event() -> void;
  auto enter() -> void;
  auto leave() -> void;
  auto key(std::size_t bytes) -> void;
  auto release_keys(std::size_t bytes) -> void;
  auto value(std::size_t bytes) -> void;
  auto scalar(std::size_t bytes) -> void;
  auto record() -> void;

 private:
  std::stop_token m_stop;
  std::size_t m_events{}, m_depth{}, m_keys{}, m_values{}, m_records{};
};
struct Metadata {
  std::string namespace_name, name, uid, version;
  bool deleting{};
};
struct Container {
  std::string name, runtime, reason;
  std::optional<bool> ready;
  std::optional<std::int64_t> restarts, exit_status;
  domain::OpsContainerState state{domain::OpsContainerState::unknown};
  unsigned state_members{};
};
struct Pod {
  Metadata metadata;
  std::string api_version, kind, phase;
  bool spec_seen{}, regular_seen{}, ready_seen{};
  std::optional<bool> ready;
  std::array<std::vector<std::string>, 3> declared;
  std::array<std::vector<Container>, 3> statuses;
};
struct Event {
  Metadata metadata, regarding;
  std::string api_version, kind, regarding_kind, regarding_api, type, reason;
  std::string event_time, first_time, last_time, series_time;
  std::optional<std::int64_t> count, series_count;
  bool series_seen{};
};
struct Document {
  std::string api_version, kind, version;
  bool items_seen{}, continued{};
  std::vector<Pod> pods;
  std::vector<Event> events;
};
[[nodiscard]] auto parse(std::string_view input,
                         domain::OpsObservationOperation operation,
                         std::size_t maximum_entries, std::stop_token stop)
    -> Document;
} // namespace aiforge::adapters::kubernetes_projection_detail
