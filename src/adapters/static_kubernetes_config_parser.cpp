#include "static_kubernetes_config_parser.hpp"

#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <array>
#include <utility>

namespace aiforge::adapters::static_kubernetes_detail {
namespace {
auto bit(Field field) -> std::uint32_t {
  return std::uint32_t{1} << std::to_underlying(field);
}
auto is_sequence(Shape shape) -> bool {
  return shape == Shape::clusters || shape == Shape::users ||
         shape == Shape::contexts;
}
struct FieldName {
  Field field;
  std::string_view name;
};
constexpr std::array field_names{
    FieldName{Field::api_version, "apiVersion"},
    FieldName{Field::kind, "kind"},
    FieldName{Field::clusters, "clusters"},
    FieldName{Field::users, "users"},
    FieldName{Field::contexts, "contexts"},
    FieldName{Field::current_context, "current-context"},
    FieldName{Field::preferences, "preferences"},
    FieldName{Field::name, "name"},
    FieldName{Field::cluster, "cluster"},
    FieldName{Field::user, "user"},
    FieldName{Field::context, "context"},
    FieldName{Field::server, "server"},
    FieldName{Field::ca_data, "certificate-authority-data"},
    FieldName{Field::insecure, "insecure-skip-tls-verify"},
    FieldName{Field::token, "token"},
    FieldName{Field::cert_data, "client-certificate-data"},
    FieldName{Field::key_data, "client-key-data"},
    FieldName{Field::namespace_name, "namespace"}};
auto fields(std::initializer_list<Field> values) -> std::uint32_t {
  std::uint32_t result{};
  for (const auto value : values)
    result |= bit(value);
  return result;
}
auto required_fields(Shape shape) -> std::uint32_t {
  switch (shape) {
    case Shape::root:
      return fields({Field::api_version, Field::kind, Field::clusters,
                     Field::users, Field::contexts});
    case Shape::cluster_entry: return fields({Field::name, Field::cluster});
    case Shape::user_entry: return fields({Field::name, Field::user});
    case Shape::context_entry: return fields({Field::name, Field::context});
    case Shape::cluster: return fields({Field::server, Field::ca_data});
    case Shape::context: return fields({Field::cluster, Field::user});
    default: return 0;
  }
}
auto allowed_fields(Shape shape) -> std::uint32_t {
  auto mask = required_fields(shape);
  switch (shape) {
    case Shape::root:
      return mask | fields({Field::current_context, Field::preferences});
    case Shape::cluster: return mask | bit(Field::insecure);
    case Shape::user:
      return fields({Field::token, Field::cert_data, Field::key_data});
    case Shape::context: return mask | bit(Field::namespace_name);
    default: return mask;
  }
}
template <class T> auto add_entry(std::vector<T>& entries) -> std::size_t {
  require(entries.size() < entry_limit, Failure::resource_exhausted);
  entries.emplace_back();
  return entries.size() - 1;
}
} // namespace

auto require(bool condition, Failure failure) -> void {
  if (!condition) reject(failure);
}
auto safe_text(std::string_view value, std::size_t maximum) -> bool {
  return !value.empty() && value.size() <= maximum &&
         detail::is_safe_utf8_text(value) &&
         std::ranges::none_of(
             value, [](unsigned char c) { return c < 32 || c == 127; });
}
auto Budget::checkpoint() const -> void {
  require(!m_stop.stop_requested(), Failure::cancelled);
}
auto Budget::event() -> void {
  checkpoint();
  require(m_events < event_limit, Failure::resource_exhausted);
  ++m_events;
}
auto Budget::scalar(std::string_view value) -> void {
  event();
  require(value.size() <= scalar_limit && value.size() <= input_limit - m_bytes,
          Failure::resource_exhausted);
  m_bytes += value.size();
  require(value.empty() || detail::is_safe_utf8_text(value));
}
auto Budget::enter() -> void {
  event();
  require(m_depth < depth_limit, Failure::resource_exhausted);
  ++m_depth;
}
auto Budget::leave() -> void {
  event();
  require(m_depth != 0);
  --m_depth;
}
auto Sink::document_start() -> void {
  m_budget.event();
  require(!m_started);
  m_started = true;
}
auto Sink::document_end() -> void {
  m_budget.event();
  require(m_started && m_root && !m_ended && m_frames.empty());
  m_ended = true;
}
auto Sink::next_mapping() -> Frame {
  if (m_frames.empty()) {
    require(m_started && !m_root && !m_ended);
    m_root = true;
    return {Shape::root};
  }
  auto& parent = m_frames.back();
  switch (parent.shape) {
    case Shape::clusters:
      return {Shape::cluster_entry, add_entry(m_document.clusters)};
    case Shape::users: return {Shape::user_entry, add_entry(m_document.users)};
    case Shape::contexts:
      return {Shape::context_entry, add_entry(m_document.contexts)};
    default: break;
  }
  const auto field = std::exchange(parent.pending, Field::none);
  if (parent.shape == Shape::root && field == Field::preferences)
    return {Shape::preferences};
  if (parent.shape == Shape::cluster_entry && field == Field::cluster)
    return {Shape::cluster, parent.index};
  if (parent.shape == Shape::user_entry && field == Field::user)
    return {Shape::user, parent.index};
  if (parent.shape == Shape::context_entry && field == Field::context)
    return {Shape::context, parent.index};
  reject();
}
auto Sink::mapping_start() -> void {
  m_budget.enter();
  const auto frame = next_mapping();
  m_frames.push_back(frame);
}
auto Sink::mapping_end() -> void {
  m_budget.leave();
  require(!m_frames.empty());
  const auto& frame = m_frames.back();
  require(!is_sequence(frame.shape) && frame.pending == Field::none);
  const auto required = required_fields(frame.shape);
  require((frame.seen & required) == required);
  m_frames.pop_back();
}
auto Sink::sequence_start() -> void {
  m_budget.enter();
  require(!m_frames.empty() && m_frames.back().shape == Shape::root);
  const auto field = std::exchange(m_frames.back().pending, Field::none);
  Shape shape{};
  switch (field) {
    case Field::clusters: shape = Shape::clusters; break;
    case Field::users: shape = Shape::users; break;
    case Field::contexts: shape = Shape::contexts; break;
    default: reject();
  }
  m_frames.push_back({shape});
}
auto Sink::sequence_end() -> void {
  m_budget.leave();
  require(!m_frames.empty() && is_sequence(m_frames.back().shape));
  m_frames.pop_back();
}
auto Sink::accept_key(std::string_view value) -> void {
  require(!m_frames.empty());
  auto& frame = m_frames.back();
  require(!is_sequence(frame.shape) && frame.pending == Field::none);
  const auto found = std::ranges::find(field_names, value, &FieldName::name);
  require(found != field_names.end(), Failure::unsupported);
  const auto mask = bit(found->field);
  require((allowed_fields(frame.shape) & mask) != 0, Failure::unsupported);
  require((frame.seen & mask) == 0);
  frame.seen |= mask;
  frame.pending = found->field;
}
auto Sink::key(std::string_view value) -> void {
  m_budget.scalar(value);
  accept_key(value);
}
auto Sink::set_string(Frame& frame, Field field, std::string_view value)
    -> void {
  switch (frame.shape) {
    case Shape::root:
      if (field == Field::api_version) {
        require(value == "v1");
        return;
      }
      if (field == Field::kind) {
        require(value == "Config");
        return;
      }
      require(field == Field::current_context &&
              (value.empty() || safe_text(value, 256)));
      return;
    case Shape::cluster_entry:
      require(field == Field::name && safe_text(value, 256));
      m_document.clusters[frame.index].name = value;
      return;
    case Shape::user_entry:
      require(field == Field::name && safe_text(value, 256));
      m_document.users[frame.index].name = value;
      return;
    case Shape::context_entry:
      require(field == Field::name && safe_text(value, 256));
      m_document.contexts[frame.index].name = value;
      return;
    case Shape::cluster: {
      auto& cluster = m_document.clusters[frame.index];
      if (field == Field::server) {
        require(safe_text(value, 512));
        cluster.server = value;
        return;
      }
      require(field == Field::ca_data && !value.empty());
      cluster.ca_data = value;
      return;
    }
    case Shape::user: {
      auto& user = m_document.users[frame.index];
      require(!value.empty());
      switch (field) {
        case Field::token:
          require(safe_text(value, token_limit));
          user.token = value;
          return;
        case Field::cert_data: user.cert_data = value; return;
        case Field::key_data: user.key_data = value; return;
        default: reject();
      }
    }
    case Shape::context: {
      auto& context = m_document.contexts[frame.index];
      require(safe_text(value, 256));
      switch (field) {
        case Field::cluster: context.cluster = value; return;
        case Field::user: context.user = value; return;
        case Field::namespace_name: context.namespace_name = value; return;
        default: reject();
      }
    }
    default: reject();
  }
}
auto Sink::string(std::string_view value, bool ambiguous) -> void {
  m_budget.scalar(value);
  require(!ambiguous && !m_frames.empty());
  auto& frame = m_frames.back();
  if (frame.pending == Field::none) {
    accept_key(value);
    return;
  }
  const auto field = std::exchange(frame.pending, Field::none);
  set_string(frame, field, value);
}
auto Sink::boolean(bool value) -> void {
  m_budget.event();
  require(!m_frames.empty());
  auto& frame = m_frames.back();
  require(frame.shape == Shape::cluster && frame.pending == Field::insecure);
  require(!value, Failure::unsupported);
  frame.pending = Field::none;
}
auto Sink::finish() -> Document {
  m_budget.checkpoint();
  require(m_started && m_ended && m_frames.empty());
  return std::move(m_document);
}
} // namespace aiforge::adapters::static_kubernetes_detail
