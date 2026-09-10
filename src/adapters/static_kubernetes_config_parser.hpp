#pragma once

#include "static_kubernetes_config.hpp"
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace aiforge::adapters::static_kubernetes_detail {
using Failure = StaticKubernetesConfigFailure;
inline constexpr std::size_t input_limit = std::size_t{256} * 1024U;
inline constexpr std::size_t scalar_limit = std::size_t{128} * 1024U;
inline constexpr std::size_t output_limit = std::size_t{512} * 1024U;
inline constexpr std::size_t token_limit = std::size_t{16} * 1024U;
inline constexpr std::size_t event_limit = 32768;
inline constexpr std::size_t depth_limit = 16;
inline constexpr std::size_t entry_limit = 128;

// Fixed private exception aborts either frontend. It owns no input text.
struct Rejected {
  Failure failure;
};
[[noreturn]] inline auto reject(Failure failure = Failure::invalid_input)
    -> void {
  throw Rejected{failure};
}
auto require(bool condition, Failure failure = Failure::invalid_input) -> void;
auto safe_text(std::string_view value, std::size_t maximum) -> bool;

// Shared guards also expose otherwise unreachable defensive ceilings to
// focused tests. No caller can override production parser limits.
class Budget final {
 public:
  explicit Budget(std::stop_token stop = {}) : m_stop(std::move(stop)) {}
  auto event() -> void;
  auto scalar(std::string_view value) -> void;
  auto enter() -> void;
  auto leave() -> void;
  auto checkpoint() const -> void;

 private:
  std::stop_token m_stop;
  std::size_t m_events{}, m_bytes{}, m_depth{};
};

struct Cluster {
  std::string name, server, ca_data;
};
struct User {
  std::string name, token, cert_data, key_data;
};
struct Context {
  std::string name, cluster, user, namespace_name;
};
struct Document {
  std::vector<Cluster> clusters;
  std::vector<User> users;
  std::vector<Context> contexts;
};
enum class Shape {
  root,
  clusters,
  users,
  contexts,
  cluster_entry,
  cluster,
  user_entry,
  user,
  context_entry,
  context,
  preferences
};
enum class Field : std::uint8_t {
  none,
  api_version,
  kind,
  clusters,
  users,
  contexts,
  current_context,
  preferences,
  name,
  cluster,
  user,
  context,
  server,
  ca_data,
  insecure,
  token,
  cert_data,
  key_data,
  namespace_name
};

// A closed kubeconfig schema, not a generic YAML/JSON object graph. Frames
// contain only field presence and an index into bounded typed records.
class Sink final {
 public:
  explicit Sink(std::stop_token stop) : m_budget(stop) {}
  auto document_start() -> void;
  auto document_end() -> void;
  auto mapping_start() -> void;
  auto mapping_end() -> void;
  auto sequence_start() -> void;
  auto sequence_end() -> void;
  auto key(std::string_view value) -> void;
  auto string(std::string_view value, bool ambiguous = false) -> void;
  auto boolean(bool value) -> void;
  [[nodiscard]] auto finish() -> Document;

 private:
  struct Frame {
    Shape shape{Shape::root};
    std::size_t index{};
    std::uint32_t seen{};
    Field pending{Field::none};
  };
  auto accept_key(std::string_view value) -> void;
  auto next_mapping() -> Frame;
  auto set_string(Frame& frame, Field field, std::string_view value) -> void;
  Budget m_budget;
  Document m_document;
  std::vector<Frame> m_frames;
  bool m_started{}, m_ended{}, m_root{};
};

auto parse_yaml(std::string_view bytes, Sink& sink) -> void;
auto parse_json(std::string_view bytes, Sink& sink) -> void;
auto decode_base64(std::string_view value) -> std::string;
auto validate_pem(std::string_view value, bool private_key) -> void;
auto parse_endpoint(std::string_view value) -> domain::OpsHttpsEndpoint;

// Two-pass emitter counts all escaping before allocating/emitting output.
class Writer final {
 public:
  explicit Writer(std::string* output = nullptr) : m_output(output) {}
  auto raw(std::string_view value) -> void;
  auto quoted(std::string_view value) -> void;
  [[nodiscard]] auto size() const noexcept -> std::size_t { return m_size; }

 private:
  std::string* m_output;
  std::size_t m_size{};
};
} // namespace aiforge::adapters::static_kubernetes_detail
