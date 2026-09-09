#include <aiforge/surfaces/admin_commands.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge::surfaces;
template <class T> auto action(std::string_view command) -> T {
  auto parsed = parse_admin_command(command);
  REQUIRE(parsed);
  REQUIRE(parsed->has_value());
  REQUIRE(std::holds_alternative<AdminAction>(**parsed));
  const auto& value = std::get<AdminAction>(**parsed);
  REQUIRE(std::holds_alternative<T>(value));
  return std::get<T>(value);
}
} // namespace
TEST_CASE("Admin command failures stay commands", "[admin][commands]") {
  for (const std::string_view text :
       {"/admin\n", "/admin\r health", "/admin\033", "/admin health\n",
        "/admin health extra", "/admin select", "/admin select -local",
        "/admin select Local", "/admin select local/../other", "/admin service",
        "/admin service bad", "/admin service -x.service",
        "/admin service 'x.service'", "/admin service x.service extra",
        "/admin service x.service;touch", "/admin toolbar maybe",
        "/admin toolbar show extra", "/admin unknown"}) {
    INFO(text);
    const auto parsed = parse_admin_command(text);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == ManualOpsErrorCode::invalid_input);
  }
  CHECK_FALSE(parse_admin_command("/admin select " + std::string(65, 'a')));
  CHECK_FALSE(parse_admin_command("/admin service " + std::string(248, 'a') +
                                  ".service"));
  CHECK_FALSE(parse_admin_command("/admin " + std::string(1024, ' ')));
  CHECK_FALSE(parse_admin_command(std::string{"/admin service x\xff.service"}));
}
TEST_CASE("Admin parser distinguishes other commands and returns typed actions",
          "[admin][commands]") {
  for (const auto text :
       {"hello", "/files", "/administrator", "/admin-health"}) {
    const auto result = parse_admin_command(text);
    REQUIRE(result);
    CHECK_FALSE(result->has_value());
  }
  static_cast<void>(action<AdminInspect>("/admin"));
  static_cast<void>(action<AdminInspect>("/admin targets"));
  static_cast<void>(action<AdminReadHealth>("/admin\t health  "));
  static_cast<void>(action<AdminReadServices>("/admin services"));
  static_cast<void>(action<AdminCancel>("/admin cancel"));
  static_cast<void>(action<AdminCloseView>("/admin close"));
  CHECK(action<AdminSelectTarget>("/admin select local").target.value() ==
        "local");
  CHECK(action<AdminSelectTarget>("/admin select " + std::string(64, 'a'))
            .target.value()
            .size() == 64);
  CHECK(action<AdminReadNamedService>("/admin service test@node:1.service")
            .unit == "test@node:1.service");
  CHECK(action<AdminReadNamedService>("/admin service " +
                                      std::string(247, 'a') + ".service")
            .unit.size() == 255);
  for (const bool visible : {false, true}) {
    const auto result = parse_admin_command(visible ? "/admin toolbar show"
                                                    : "/admin toolbar hide");
    REQUIRE(result);
    REQUIRE(result->has_value());
    REQUIRE(std::holds_alternative<AdminToolbarVisibility>(**result));
    CHECK(std::get<AdminToolbarVisibility>(**result).visible == visible);
  }
}
