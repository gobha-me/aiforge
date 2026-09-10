#if defined(PROBE_TERMFORGE)
#include <termforge/core/screen.hpp>
#include <termforge/widgets/choice_wizard_dialog.hpp>

auto main() -> int {
  termforge::Screen screen{2, 1};
  return screen.cols() == 2 && screen.rows() == 1 ? 0 : 1;
}
#elif defined(PROBE_VENICE_CPP)
#include <cstddef>
#include <expected>
#include <type_traits>

#include <ares.h>
#include <venice/venice.hpp>

auto main() -> int {
  const venice::Client client{"dependency-probe-key"};
  // Exercise the actual canonical HTTP and new public header closure without
  // connecting or creating a c-ares process owner.
  httplib::Request multipart;
  (void)multipart.form.files;
  (void)multipart.form.fields;
  httplib::UploadFormDataItems uploads;
  (void)uploads;
  static_assert(std::is_move_constructible_v<venice::VideoDownloadRuntime>);
  // This version query needs no resolver initialization and proves that the
  // selected exported target actually links its new public client dependency.
  if (ares_version(nullptr) == nullptr) {
    return 1;
  }
  venice::CharacterQuery query;
  query.is_adult = false;
  query.limit = 100;
  query.offset = 0;
  venice::RequestOptions options;
  options.maximum_response_bytes = std::size_t{4096};
  using CharacterPageResult = decltype(client.characters(query, options));
  using CharacterResult = decltype(client.character("probe", options));
  static_assert(
      std::is_same_v<CharacterPageResult,
                     std::expected<venice::CharacterPage, venice::Error>>);
  static_assert(
      std::is_same_v<CharacterResult,
                     std::expected<venice::Character, venice::Error>>);
  (void)client;
  return 0;
}
#elif defined(PROBE_RASTERFORGE)
#include <rasterforge/rasterforge.hpp>

auto main() -> int {
  const auto image = rasterforge::Image::create({1, 1});
  return image && image->size_bytes() == 4 ? 0 : 1;
}
#elif defined(PROBE_DBUS1)
#include <dbus/dbus.h>

auto main() -> int {
  static_assert(DBUS_VERSION >= ((1 << 16) | (16 << 8) | 2));
  int major{}, minor{}, micro{};
  dbus_get_version(&major, &minor, &micro);
  // Pure linked-library check: never connect to a bus or query a service.
  return major > 1 ||
                 (major == 1 && (minor > 16 || (minor == 16 && micro >= 2)))
             ? 0
             : 1;
}
#elif defined(PROBE_SYSTEMD_JOURNAL)
#include <systemd/sd-journal.h>

auto main() -> int {
  // Force actual client symbol resolution without opening a journal.
  auto volatile open = &sd_journal_open;
  auto volatile close = &sd_journal_close;
  auto volatile match = &sd_journal_add_match;
  auto volatile seek = &sd_journal_seek_realtime_usec;
  auto volatile next = &sd_journal_next;
  auto volatile timestamp = &sd_journal_get_realtime_usec;
  auto volatile restart = &sd_journal_restart_data;
  auto volatile field = &sd_journal_enumerate_data;
  auto volatile threshold = &sd_journal_set_data_threshold;
  return !(open && close && match && seek && next && timestamp && restart &&
           field && threshold);
}
#elif defined(PROBE_YAML_CPP)
#include <sstream>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/parser.h>

namespace {
class ProbeHandler final : public YAML::EventHandler {
 public:
  void OnDocumentStart(const YAML::Mark&) override {}
  void OnDocumentEnd() override {}
  void OnNull(const YAML::Mark&, YAML::anchor_t) override {}
  void OnAlias(const YAML::Mark&, YAML::anchor_t) override {}
  void OnScalar(const YAML::Mark&, const std::string&, YAML::anchor_t,
                const std::string& value) override {
    matched = value == "probe";
  }
  void OnSequenceStart(const YAML::Mark&, const std::string&, YAML::anchor_t,
                       YAML::EmitterStyle::value) override {}
  void OnSequenceEnd() override {}
  void OnMapStart(const YAML::Mark&, const std::string&, YAML::anchor_t,
                  YAML::EmitterStyle::value) override {}
  void OnMapEnd() override {}
  bool matched{};
};
} // namespace
auto main() -> int {
  std::istringstream input{"probe"};
  YAML::Parser parser{input};
  ProbeHandler handler;
  return parser.HandleNextDocument(handler) && handler.matched &&
                 !parser.HandleNextDocument(handler)
             ? 0
             : 1;
}
#elif defined(PROBE_SQLITE3)
#include <sqlite3.h>

auto main() -> int {
  return sqlite3_libversion_number() >= 3045001 ? 0 : 1;
}
#else
#error "No dependency probe was selected"
#endif
