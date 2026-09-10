#include "kubernetes_observation_projection_internal.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace aiforge::adapters::kubernetes_projection_detail {
[[noreturn]] auto reject(Failure failure) -> void {
  throw Rejected{failure};
}
auto require(bool condition, Failure failure) -> void {
  if (!condition) reject(failure);
}
auto Budget::event() -> void {
  require(!m_stop.stop_requested(), Failure::cancelled);
  require(m_events < event_limit, Failure::resource_exhausted);
  ++m_events;
}
auto Budget::enter() -> void {
  require(m_depth < depth_limit, Failure::resource_exhausted);
  ++m_depth;
}
auto Budget::leave() -> void {
  require(m_depth != 0);
  --m_depth;
}
auto Budget::key(std::size_t bytes) -> void {
  require(bytes <= key_limit - m_keys, Failure::resource_exhausted);
  m_keys += bytes;
}
auto Budget::release_keys(std::size_t bytes) -> void {
  require(bytes <= m_keys);
  m_keys -= bytes;
}
auto Budget::value(std::size_t bytes) -> void {
  require(bytes <= value_limit - m_values, Failure::resource_exhausted);
  m_values += bytes;
}
auto Budget::scalar(std::size_t bytes) -> void {
  require(bytes <= scalar_limit, Failure::resource_exhausted);
}
auto Budget::record() -> void {
  require(m_records < record_limit, Failure::resource_exhausted);
  ++m_records;
}
namespace {
using Operation = domain::OpsObservationOperation;
using Json = nlohmann::json;
enum class Shape {
  ignored,
  root,
  list_metadata,
  pods,
  pod,
  pod_metadata,
  spec,
  declarations,
  declaration,
  status,
  statuses,
  container,
  state,
  waiting,
  running,
  terminated,
  conditions,
  condition,
  events,
  event,
  event_metadata,
  regarding,
  series
};
enum class Field {
  ignored,
  api_version,
  kind,
  metadata,
  items,
  version,
  continuation,
  remaining,
  namespace_name,
  name,
  uid,
  deleting,
  spec,
  status,
  regular,
  init,
  ephemeral,
  phase,
  conditions,
  type,
  condition_status,
  state,
  waiting,
  running,
  terminated,
  ready,
  restarts,
  runtime,
  reason,
  exit_status,
  regarding,
  count,
  series,
  event_time,
  first_time,
  last_time,
  series_time
};
struct Rule {
  Shape shape;
  std::string_view name;
  Field field;
};
constexpr std::array rules{
    Rule{Shape::root, "apiVersion", Field::api_version},
    Rule{Shape::root, "kind", Field::kind},
    Rule{Shape::root, "metadata", Field::metadata},
    Rule{Shape::root, "items", Field::items},
    Rule{Shape::list_metadata, "resourceVersion", Field::version},
    Rule{Shape::list_metadata, "continue", Field::continuation},
    Rule{Shape::list_metadata, "remainingItemCount", Field::remaining},
    Rule{Shape::pod, "apiVersion", Field::api_version},
    Rule{Shape::pod, "kind", Field::kind},
    Rule{Shape::pod, "metadata", Field::metadata},
    Rule{Shape::pod, "spec", Field::spec},
    Rule{Shape::pod, "status", Field::status},
    Rule{Shape::spec, "containers", Field::regular},
    Rule{Shape::spec, "initContainers", Field::init},
    Rule{Shape::spec, "ephemeralContainers", Field::ephemeral},
    Rule{Shape::declaration, "name", Field::name},
    Rule{Shape::status, "phase", Field::phase},
    Rule{Shape::status, "conditions", Field::conditions},
    Rule{Shape::status, "containerStatuses", Field::regular},
    Rule{Shape::status, "initContainerStatuses", Field::init},
    Rule{Shape::status, "ephemeralContainerStatuses", Field::ephemeral},
    Rule{Shape::condition, "type", Field::type},
    Rule{Shape::condition, "status", Field::condition_status},
    Rule{Shape::container, "name", Field::name},
    Rule{Shape::container, "containerID", Field::runtime},
    Rule{Shape::container, "ready", Field::ready},
    Rule{Shape::container, "restartCount", Field::restarts},
    Rule{Shape::container, "state", Field::state},
    Rule{Shape::state, "waiting", Field::waiting},
    Rule{Shape::state, "running", Field::running},
    Rule{Shape::state, "terminated", Field::terminated},
    Rule{Shape::waiting, "reason", Field::reason},
    Rule{Shape::terminated, "reason", Field::reason},
    Rule{Shape::terminated, "exitCode", Field::exit_status},
    Rule{Shape::event, "apiVersion", Field::api_version},
    Rule{Shape::event, "kind", Field::kind},
    Rule{Shape::event, "metadata", Field::metadata},
    Rule{Shape::event, "involvedObject", Field::regarding},
    Rule{Shape::event, "type", Field::type},
    Rule{Shape::event, "reason", Field::reason},
    Rule{Shape::event, "count", Field::count},
    Rule{Shape::event, "series", Field::series},
    Rule{Shape::event, "eventTime", Field::event_time},
    Rule{Shape::event, "firstTimestamp", Field::first_time},
    Rule{Shape::event, "lastTimestamp", Field::last_time},
    Rule{Shape::regarding, "kind", Field::kind},
    Rule{Shape::regarding, "apiVersion", Field::api_version},
    Rule{Shape::series, "count", Field::count},
    Rule{Shape::series, "lastObservedTime", Field::series_time}};
auto field(Shape shape, std::string_view key) -> Field {
  if (shape == Shape::pod_metadata || shape == Shape::event_metadata ||
      shape == Shape::regarding) {
    if (key == "namespace") return Field::namespace_name;
    if (key == "name") return Field::name;
    if (key == "uid") return Field::uid;
    if (key == "resourceVersion") return Field::version;
    if (key == "deletionTimestamp") return Field::deleting;
  }
  const auto found = std::ranges::find_if(rules, [&](const auto& rule) {
    return rule.shape == shape && rule.name == key;
  });
  return found == rules.end() ? Field::ignored : found->field;
}
struct Frame {
  Shape shape{Shape::ignored};
  bool object{};
  Field pending{Field::ignored};
  bool has_pending{};
  std::size_t category{}, key_bytes{};
  std::set<std::string, std::less<>> keys;
};
// The nlohmann SAX concept is structural; no abstract-template inheritance or
// virtual dispatch is needed for this private handler.
class Reader final {
 public:
  Reader(Operation operation, std::size_t maximum, std::stop_token stop)
      : m_operation(operation), m_maximum(maximum), m_budget(stop) {}
  auto null() -> bool;
  auto boolean(bool value) -> bool;
  auto number_integer(Json::number_integer_t value) -> bool;
  auto number_unsigned(Json::number_unsigned_t value) -> bool;
  auto number_float(Json::number_float_t, const std::string&) -> bool;
  auto string(std::string& value) -> bool;
  auto binary(Json::binary_t&) -> bool { reject(); }
  auto start_object(std::size_t) -> bool { return start(true); }
  auto end_object() -> bool { return end(true); }
  auto start_array(std::size_t) -> bool { return start(false); }
  auto end_array() -> bool { return end(false); }
  auto key(std::string& value) -> bool;
  auto parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) -> bool {
    reject();
  }
  auto finish() -> Document;

 private:
  auto take() -> Field;
  auto start(bool object) -> bool;
  auto end(bool object) -> bool;
  auto child(Field next, bool object) -> Shape;
  auto child_object(Field next) -> Shape;
  auto child_array(Field next) -> Shape;
  auto array_item(Shape shape, bool object) -> Shape;
  auto copy(std::string& output, std::string_view value,
            std::size_t maximum = 512) -> void;
  auto metadata() -> Metadata&;
  auto text(Field next, std::string_view value) -> void;
  auto type_metadata(Field next, std::string_view value) -> void;
  auto number(Field next, std::int64_t value) -> void;
  auto close(Shape shape, std::size_t category) -> void;
  Operation m_operation;
  std::size_t m_maximum;
  Budget m_budget;
  std::vector<Frame> m_frames;
  Document m_document;
  Pod m_pod;
  Event m_event;
  Container m_container;
  std::string m_declaration, m_condition_type, m_condition_status;
  bool m_started{}, m_done{};
};
auto Reader::take() -> Field {
  require(!m_frames.empty());
  auto& frame = m_frames.back();
  if (!frame.object) {
    require(frame.shape == Shape::ignored);
    return Field::ignored;
  }
  require(frame.has_pending);
  frame.has_pending = false;
  return std::exchange(frame.pending, Field::ignored);
}
auto Reader::key(std::string& value) -> bool {
  m_budget.event();
  m_budget.scalar(value.size());
  require(!m_frames.empty() && m_frames.back().object);
  require(value.size() <= 1024, Failure::resource_exhausted);
  auto& frame = m_frames.back();
  require(!frame.has_pending);
  m_budget.key(value.size());
  frame.key_bytes += value.size();
  require(frame.keys.insert(value).second);
  frame.pending = field(frame.shape, value);
  if (m_operation == Operation::kubernetes_workloads &&
      (frame.pending == Field::spec || frame.pending == Field::regular ||
       frame.pending == Field::init || frame.pending == Field::ephemeral))
    frame.pending = Field::ignored;
  frame.has_pending = true;
  return true;
}
auto Reader::copy(std::string& output, std::string_view value,
                  std::size_t maximum) -> void {
  require(value.size() <= maximum, Failure::resource_exhausted);
  m_budget.value(value.size());
  output = value;
}
auto Reader::metadata() -> Metadata& {
  switch (m_frames.back().shape) {
    case Shape::pod_metadata: return m_pod.metadata;
    case Shape::event_metadata: return m_event.metadata;
    case Shape::regarding: return m_event.regarding;
    default: reject();
  }
}
auto Reader::array_item(Shape shape, bool object) -> Shape {
  if (shape == Shape::ignored) return Shape::ignored;
  require(object);
  switch (shape) {
    case Shape::pods:
      m_budget.record();
      require(m_document.pods.size() < m_maximum, Failure::resource_exhausted);
      m_pod = {};
      return Shape::pod;
    case Shape::events:
      m_budget.record();
      require(m_document.events.size() < m_maximum,
              Failure::resource_exhausted);
      m_event = {};
      return Shape::event;
    case Shape::declarations:
      m_budget.record();
      m_declaration.clear();
      return Shape::declaration;
    case Shape::statuses:
      m_budget.record();
      m_container = {};
      return Shape::container;
    case Shape::conditions:
      m_condition_type.clear();
      m_condition_status.clear();
      return Shape::condition;
    default: reject();
  }
}
auto Reader::child(Field next, bool object) -> Shape {
  if (next == Field::ignored) return Shape::ignored;
  return object ? child_object(next) : child_array(next);
}
auto Reader::child_array(Field next) -> Shape {
  switch (next) {
    case Field::items:
      m_document.items_seen = true;
      return m_operation == Operation::kubernetes_workloads ? Shape::pods
                                                            : Shape::events;
    case Field::conditions: return Shape::conditions;
    case Field::regular:
    case Field::init:
    case Field::ephemeral:
      if (m_frames.back().shape == Shape::spec) {
        if (next == Field::regular) m_pod.regular_seen = true;
        return Shape::declarations;
      }
      return Shape::statuses;
    default: reject();
  }
}
auto Reader::child_object(Field next) -> Shape {
  switch (next) {
    case Field::metadata:
      if (m_frames.back().shape == Shape::root) return Shape::list_metadata;
      if (m_frames.back().shape == Shape::pod) return Shape::pod_metadata;
      return Shape::event_metadata;
    case Field::spec: m_pod.spec_seen = true; return Shape::spec;
    case Field::status: return Shape::status;
    case Field::state: return Shape::state;
    case Field::regarding: return Shape::regarding;
    case Field::series: m_event.series_seen = true; return Shape::series;
    case Field::waiting:
      m_container.state = domain::OpsContainerState::waiting;
      break;
    case Field::running:
      m_container.state = domain::OpsContainerState::running;
      break;
    case Field::terminated:
      m_container.state = domain::OpsContainerState::terminated;
      break;
    default: reject();
  }
  require(++m_container.state_members == 1);
  if (next == Field::waiting) return Shape::waiting;
  return next == Field::running ? Shape::running : Shape::terminated;
}
auto Reader::start(bool object) -> bool {
  m_budget.event();
  m_budget.enter();
  require(!m_done);
  Frame frame;
  frame.object = object;
  if (m_frames.empty()) {
    require(object && !m_started);
    m_started = true;
    frame.shape = m_operation == Operation::kubernetes_pod_health ? Shape::pod
                                                                  : Shape::root;
    if (frame.shape == Shape::pod) m_budget.record();
  } else {
    frame.category = m_frames.back().category;
    if (m_frames.back().object) {
      const auto next = take();
      frame.shape = child(next, object);
      if (next == Field::regular) frame.category = 0;
      if (next == Field::init) frame.category = 1;
      if (next == Field::ephemeral) frame.category = 2;
    } else
      frame.shape = array_item(m_frames.back().shape, object);
  }
  m_frames.push_back(std::move(frame));
  return true;
}
auto Reader::close(Shape shape, std::size_t category) -> void {
  switch (shape) {
    case Shape::pod: m_document.pods.push_back(std::move(m_pod)); break;
    case Shape::event: m_document.events.push_back(std::move(m_event)); break;
    case Shape::declaration:
      require(m_pod.declared[category].size() < m_maximum,
              Failure::resource_exhausted);
      require(!m_declaration.empty());
      m_pod.declared[category].push_back(std::move(m_declaration));
      break;
    case Shape::container:
      require(m_pod.statuses[category].size() < m_maximum,
              Failure::resource_exhausted);
      m_pod.statuses[category].push_back(std::move(m_container));
      break;
    case Shape::condition:
      if (m_condition_type == "Ready") {
        require(!m_pod.ready_seen);
        m_pod.ready_seen = true;
        if (m_condition_status == "True") m_pod.ready = true;
        if (m_condition_status == "False") m_pod.ready = false;
      }
      break;
    default: break;
  }
}
auto Reader::end(bool object) -> bool {
  m_budget.event();
  require(!m_frames.empty());
  const auto& frame = m_frames.back();
  require(frame.object == object && !frame.has_pending);
  close(frame.shape, frame.category);
  m_budget.release_keys(frame.key_bytes);
  m_budget.leave();
  m_frames.pop_back();
  if (m_frames.empty()) m_done = true;
  return true;
}
auto Reader::type_metadata(Field next, std::string_view value) -> void {
  require(!value.empty());
  const bool version = next == Field::api_version;
  switch (m_frames.back().shape) {
    case Shape::root:
      copy(version ? m_document.api_version : m_document.kind, value, 32);
      return;
    case Shape::pod:
      copy(version ? m_pod.api_version : m_pod.kind, value, 32);
      return;
    case Shape::regarding:
      copy(version ? m_event.regarding_api : m_event.regarding_kind, value,
           128);
      return;
    default: copy(version ? m_event.api_version : m_event.kind, value, 32);
  }
}
auto Reader::text(Field next, std::string_view value) -> void {
  const auto shape = m_frames.back().shape;
  switch (next) {
    case Field::ignored: return;
    case Field::api_version:
    case Field::kind: type_metadata(next, value); return;
    case Field::version:
      if (shape == Shape::list_metadata)
        copy(m_document.version, value, 128);
      else
        copy(metadata().version, value, 128);
      return;
    case Field::continuation: m_document.continued = !value.empty(); return;
    case Field::namespace_name:
      copy(metadata().namespace_name, value, 63);
      return;
    case Field::uid: copy(metadata().uid, value, 128); return;
    case Field::name:
      if (shape == Shape::declaration)
        copy(m_declaration, value, 63);
      else if (shape == Shape::container)
        copy(m_container.name, value, 63);
      else
        copy(metadata().name, value, 253);
      return;
    case Field::deleting:
      require(!value.empty());
      metadata().deleting = true;
      return;
    case Field::phase: copy(m_pod.phase, value, 64); return;
    case Field::type:
      if (shape == Shape::condition)
        copy(m_condition_type, value, 128);
      else
        copy(m_event.type, value, 128);
      return;
    case Field::condition_status: copy(m_condition_status, value, 64); return;
    case Field::runtime: copy(m_container.runtime, value, 512); return;
    case Field::reason:
      if (shape == Shape::event)
        copy(m_event.reason, value, 256);
      else
        copy(m_container.reason, value, 256);
      return;
    case Field::event_time: copy(m_event.event_time, value, 64); return;
    case Field::first_time: copy(m_event.first_time, value, 64); return;
    case Field::last_time: copy(m_event.last_time, value, 64); return;
    case Field::series_time: copy(m_event.series_time, value, 64); return;
    default: reject();
  }
}
auto Reader::string(std::string& value) -> bool {
  m_budget.event();
  m_budget.scalar(value.size());
  text(take(), value);
  return true;
}
auto Reader::null() -> bool {
  m_budget.event();
  switch (take()) {
    case Field::ignored:
    case Field::deleting:
    case Field::event_time:
    case Field::first_time:
    case Field::last_time:
    case Field::series_time:
    case Field::series: return true;
    default: reject();
  }
}
auto Reader::boolean(bool value) -> bool {
  m_budget.event();
  const auto next = take();
  if (next == Field::ignored) return true;
  require(next == Field::ready);
  m_container.ready = value;
  return true;
}
auto Reader::number(Field next, std::int64_t value) -> void {
  if (next == Field::ignored) return;
  require(value >= 0);
  switch (next) {
    case Field::remaining: return;
    case Field::restarts: m_container.restarts = value; return;
    case Field::exit_status: m_container.exit_status = value; return;
    case Field::count:
      if (m_frames.back().shape == Shape::series)
        m_event.series_count = value;
      else
        m_event.count = value;
      return;
    default: reject();
  }
}
auto Reader::number_integer(Json::number_integer_t value) -> bool {
  m_budget.event();
  number(take(), value);
  return true;
}
auto Reader::number_unsigned(Json::number_unsigned_t value) -> bool {
  m_budget.event();
  const auto next = take();
  if (next == Field::ignored) return true;
  require(value <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
  number(next, static_cast<std::int64_t>(value));
  return true;
}
auto Reader::number_float(Json::number_float_t, const std::string& token)
    -> bool {
  m_budget.event();
  m_budget.scalar(token.size());
  require(take() == Field::ignored);
  return true;
}
auto Reader::finish() -> Document {
  require(m_started && m_done && m_frames.empty());
  return std::move(m_document);
}
} // namespace

auto parse(std::string_view input, Operation operation,
           std::size_t maximum_entries, std::stop_token stop) -> Document {
  Reader reader{operation, maximum_entries, stop};
  require(Json::sax_parse(input, &reader, Json::input_format_t::json, true));
  return reader.finish();
}
} // namespace aiforge::adapters::kubernetes_projection_detail
