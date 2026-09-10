#include "static_kubernetes_config_parser.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <sstream>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/parser.h>

namespace aiforge::adapters::static_kubernetes_detail {
namespace {
auto lower(char value) -> char {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}
auto equal_folded(std::string_view left, std::string_view right) -> bool {
  return std::ranges::equal(left, right,
                            [](char a, char b) { return lower(a) == b; });
}
auto ambiguous_plain(std::string_view value) -> bool {
  constexpr std::array spellings{"null", "~",    "true",  "false", "yes",
                                 "no",   "on",   "off",   "y",     "n",
                                 ".nan", ".inf", "+.inf", "-.inf"};
  if (std::ranges::any_of(spellings, [&](auto spelling) {
        return equal_folded(value, spelling);
      }))
    return true;
  // Conservative: numeric-looking plain strings (including timestamps and
  // nondecimal numbers) must be quoted. No YAML numeric coercion is performed.
  if (!value.empty() && (value.front() == '+' || value.front() == '-'))
    value.remove_prefix(1);
  if (!value.empty() && value.front() == '.') value.remove_prefix(1);
  return !value.empty() && value.front() >= '0' && value.front() <= '9';
}
auto tag_is(std::string_view tag, std::string_view standard) -> bool {
  return tag == "?" || tag == "!" || tag == standard;
}
class YamlHandler final : public YAML::EventHandler {
 public:
  explicit YamlHandler(Sink& sink) : m_sink(sink) {}
  void OnDocumentStart(const YAML::Mark&) override { m_sink.document_start(); }
  void OnDocumentEnd() override { m_sink.document_end(); }
  void OnNull(const YAML::Mark&, YAML::anchor_t) override { reject(); }
  void OnAlias(const YAML::Mark&, YAML::anchor_t) override {
    reject(Failure::unsupported);
  }
  void OnAnchor(const YAML::Mark&, const std::string&) override {
    reject(Failure::unsupported);
  }
  void OnScalar(const YAML::Mark&, const std::string& tag,
                YAML::anchor_t anchor, const std::string& value) override {
    require(anchor == 0, Failure::unsupported);
    const bool plain = tag == "?";
    if (tag == "tag:yaml.org,2002:bool" ||
        (plain &&
         (equal_folded(value, "false") || equal_folded(value, "true")))) {
      require(equal_folded(value, "false") || equal_folded(value, "true"));
      m_sink.boolean(equal_folded(value, "true"));
      return;
    }
    require(tag_is(tag, "tag:yaml.org,2002:str"), Failure::unsupported);
    m_sink.string(value, plain && ambiguous_plain(value));
  }
  void OnSequenceStart(const YAML::Mark&, const std::string& tag,
                       YAML::anchor_t anchor,
                       YAML::EmitterStyle::value) override {
    require(anchor == 0 && tag_is(tag, "tag:yaml.org,2002:seq"),
            Failure::unsupported);
    m_sink.sequence_start();
  }
  void OnSequenceEnd() override { m_sink.sequence_end(); }
  void OnMapStart(const YAML::Mark&, const std::string& tag,
                  YAML::anchor_t anchor, YAML::EmitterStyle::value) override {
    require(anchor == 0 && tag_is(tag, "tag:yaml.org,2002:map"),
            Failure::unsupported);
    m_sink.mapping_start();
  }
  void OnMapEnd() override { m_sink.mapping_end(); }

 private:
  Sink& m_sink;
};

// nlohmann checks this SAX interface structurally; no runtime virtual dispatch
// or instantiation of the library's optional abstract template is needed.
class JsonHandler final {
 public:
  explicit JsonHandler(Sink& sink) : m_sink(sink) {}
  auto null() -> bool { reject(); }
  auto boolean(bool value) -> bool {
    m_sink.boolean(value);
    return true;
  }
  auto number_integer(nlohmann::json::number_integer_t) -> bool { reject(); }
  auto number_unsigned(nlohmann::json::number_unsigned_t) -> bool { reject(); }
  auto number_float(nlohmann::json::number_float_t, const std::string&)
      -> bool {
    reject();
  }
  auto string(std::string& value) -> bool {
    m_sink.string(value);
    return true;
  }
  auto binary(nlohmann::json::binary_t&) -> bool { reject(); }
  auto start_object(std::size_t) -> bool {
    m_sink.mapping_start();
    return true;
  }
  auto key(std::string& value) -> bool {
    m_sink.key(value);
    return true;
  }
  auto end_object() -> bool {
    m_sink.mapping_end();
    return true;
  }
  auto start_array(std::size_t) -> bool {
    m_sink.sequence_start();
    return true;
  }
  auto end_array() -> bool {
    m_sink.sequence_end();
    return true;
  }
  auto parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) -> bool {
    reject();
  }

 private:
  Sink& m_sink;
};
} // namespace
auto parse_yaml(std::string_view bytes, Sink& sink) -> void {
  try {
    // The public parser checks the 256 KiB input ceiling first. Keep this
    // bounded copy private and ephemeral; libc++18 has no C++23 spanstream.
    std::istringstream input{std::string{bytes}};
    YAML::Parser parser{input};
    YamlHandler handler{sink};
    require(parser.HandleNextDocument(handler));
    // A second complete or malformed document must not disappear as trailing
    // input. The same handler rejects its document-start before traversal.
    require(!parser.HandleNextDocument(handler));
  } catch (const YAML::Exception&) {
    // Never retain the parser's diagnostic or input excerpts.
    reject();
  }
}
auto parse_json(std::string_view bytes, Sink& sink) -> void {
  JsonHandler handler{sink};
  sink.document_start();
  require(nlohmann::json::sax_parse(bytes.begin(), bytes.end(), &handler,
                                    nlohmann::json::input_format_t::json, true,
                                    false));
  sink.document_end();
}
} // namespace aiforge::adapters::static_kubernetes_detail
