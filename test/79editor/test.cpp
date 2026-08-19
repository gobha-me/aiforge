#include <aiforge/adapters/process_draft_editor.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

class EnvironmentGuard final {
 public:
  explicit EnvironmentGuard(const char* name) : m_name(name) {
    if (const char* value = std::getenv(name); value != nullptr)
      m_prior = value;
  }
  ~EnvironmentGuard() {
    if (m_prior) {
      static_cast<void>(::setenv(m_name.c_str(), m_prior->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(m_name.c_str()));
    }
  }

 private:
  std::string m_name;
  std::optional<std::string> m_prior;
};

}  // namespace

TEST_CASE("editor configuration is one executable and never shell text",
          "[editor][failure]") {
  EnvironmentGuard visual{"VISUAL"};
  EnvironmentGuard editor{"EDITOR"};
  REQUIRE(::unsetenv("VISUAL") == 0);
  REQUIRE(::unsetenv("EDITOR") == 0);
  adapters::ProcessDraftEditor adapter;
  const auto missing = adapter.edit("draft");
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          surfaces::DraftEditorErrorCode::not_configured);

  REQUIRE(::setenv("EDITOR", "/bin/true --flag", 1) == 0);
  const auto arguments = adapter.edit("draft");
  REQUIRE_FALSE(arguments);
  REQUIRE(arguments.error().code ==
          surfaces::DraftEditorErrorCode::invalid_configuration);
}

TEST_CASE("editor round trip normalizes lines and validates its result",
          "[editor]") {
  EnvironmentGuard visual{"VISUAL"};
  EnvironmentGuard editor{"EDITOR"};
  REQUIRE(::unsetenv("VISUAL") == 0);
  REQUIRE(::setenv("EDITOR", EDITOR_TEST_FIXTURE, 1) == 0);
  adapters::ProcessDraftEditor adapter;

  const auto edited = adapter.edit("original");
  REQUIRE(edited);
  REQUIRE(*edited == "edited\nsecond");

  const auto invalid = adapter.edit("invalid");
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code ==
          surfaces::DraftEditorErrorCode::invalid_result);

  const auto symlink = adapter.edit("symlink");
  REQUIRE_FALSE(symlink);
  REQUIRE(symlink.error().code ==
          surfaces::DraftEditorErrorCode::invalid_result);

  const auto permissions = adapter.edit("permissions");
  REQUIRE_FALSE(permissions);
  REQUIRE(permissions.error().code ==
          surfaces::DraftEditorErrorCode::invalid_result);
}

TEST_CASE("editor failure timeout and size limits preserve caller state",
          "[editor][failure]") {
  EnvironmentGuard visual{"VISUAL"};
  EnvironmentGuard editor{"EDITOR"};
  REQUIRE(::unsetenv("VISUAL") == 0);
  REQUIRE(::setenv("EDITOR", "/bin/false", 1) == 0);
  adapters::ProcessDraftEditor adapter;
  const auto failed = adapter.edit("unchanged");
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code ==
          surfaces::DraftEditorErrorCode::process_failed);

  REQUIRE(::setenv("EDITOR", EDITOR_TEST_FIXTURE, 1) == 0);
  adapters::ProcessDraftEditor limited{{32, 50ms, 20ms}};
  const auto oversized = limited.edit("oversized");
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          surfaces::DraftEditorErrorCode::invalid_result);
  const auto timeout = limited.edit("hang");
  REQUIRE_FALSE(timeout);
  REQUIRE(timeout.error().code ==
          surfaces::DraftEditorErrorCode::process_failed);
}
