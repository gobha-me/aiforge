#include <aiforge/surfaces/local_browser_commands.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
auto state() -> surfaces::LocalBrowserState {
  surfaces::LocalBrowserState result;
  result.folders.push_back({{1, std::string(64, 'b')}, 2, "/private/one"});
  result.folders.push_back({{1, std::string(64, 'a')}, 3, "/private/two"});
  return result;
}
} // namespace
TEST_CASE("file commands reject malformed quoting scope and bounds") {
  const auto current = state();
  for (const auto* text : {"/files open",
                           "/files open 0",
                           "/files open -1",
                           "/files open 3",
                           "/files open 9999999999999999999999999999",
                           "/files preview 1",
                           "/files preview 1 ../escape",
                           "/files preview 1 /absolute",
                           "/files preview 1 a b",
                           "/files clear extra",
                           "/files cancel extra",
                           "/files add-folder relative",
                           "/files add-folder \"\"",
                           "/files add-folder \"/unfinished",
                           "/files add-folder \"/path\"suffix",
                           "/files add-folder /un\"quoted",
                           "/files open 1 \"\" invalid/filter",
                           "/files unknown 1 file",
                           "/files tray x",
                           "/files add-folder /path\nsecond",
                           "/files open 1 a b c d e"}) {
    INFO(text);
    const auto result = surfaces::parse_local_browser_command(text, current);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find("/private") == std::string::npos);
  }
  REQUIRE_FALSE(surfaces::parse_local_browser_command(
      "/files add-folder /" + std::string(9000, 'x'), current));
  REQUIRE_FALSE(surfaces::parse_local_browser_command(
      "/files preview 1 " + std::string(4097, 'x'), current));
  auto invalid = current;
  invalid.folders.front().root.binding = "not-a-root";
  REQUIRE_FALSE(
      surfaces::parse_local_browser_command("/files open 1", invalid));
  REQUIRE_FALSE(surfaces::parse_local_browser_command("/files open 1", {}));
}
TEST_CASE(
    "file commands bind current raw identities without shell interpretation") {
  const auto current = state();
  auto parsed = surfaces::parse_local_browser_command(
      "/files add-folder \"/folder with spaces/$(touch marker)\"", current);
  REQUIRE(parsed);
  REQUIRE(*parsed);
  REQUIRE(std::get<surfaces::LocalBrowseAddFolder>(**parsed).path ==
          "/folder with spaces/$(touch marker)");
  parsed = surfaces::parse_local_browser_command(
      "/files open 2 'sub folder' '*.cpp'", current);
  REQUIRE(parsed);
  const auto& navigate = std::get<surfaces::LocalBrowseNavigate>(**parsed);
  REQUIRE(navigate.root == current.folders[1].root);
  REQUIRE(navigate.directory == "sub folder");
  REQUIRE(navigate.filter == "*.cpp");
  parsed = surfaces::parse_local_browser_command(
      "/files preview 1 'quote\\\'d.txt'", current);
  REQUIRE(parsed);
  REQUIRE(std::get<surfaces::LocalBrowsePreview>(**parsed).path ==
          "quote'd.txt");
  auto reordered = current;
  std::swap(reordered.folders[0], reordered.folders[1]);
  parsed = surfaces::parse_local_browser_command("/files add 1 notes.txt",
                                                 reordered);
  REQUIRE(parsed);
  REQUIRE(std::get<surfaces::LocalBrowseAddEvidence>(**parsed).root ==
          current.folders[1].root);
  parsed = surfaces::parse_local_browser_command("/files remove 2 notes.txt",
                                                 current);
  REQUIRE(parsed);
  REQUIRE(std::get<surfaces::LocalBrowseRemoveEvidence>(**parsed).root ==
          current.folders[1].root);
  parsed =
      surfaces::parse_local_browser_command("/files remove-folder 1", current);
  REQUIRE(parsed);
  REQUIRE(std::get<surfaces::LocalBrowseRemoveFolder>(**parsed).root ==
          current.folders[0].root);
}
TEST_CASE(
    "file commands distinguish inspection from mutation and unrelated text") {
  const auto current = state();
  for (const auto* text :
       {"hello", "/context history", "/files-extra", "/filesx"}) {
    const auto parsed = surfaces::parse_local_browser_command(text, current);
    REQUIRE(parsed);
    REQUIRE_FALSE(*parsed);
  }
  for (const auto* text : {"/files", "/files folders", "/files tray"}) {
    const auto parsed = surfaces::parse_local_browser_command(text, current);
    REQUIRE(parsed);
    REQUIRE(*parsed);
    REQUIRE(std::holds_alternative<surfaces::LocalBrowseInspect>(**parsed));
  }
  const auto clear =
      surfaces::parse_local_browser_command("/files clear", current);
  REQUIRE(clear);
  REQUIRE(std::holds_alternative<surfaces::LocalBrowseClearEvidence>(**clear));
  const auto cancel =
      surfaces::parse_local_browser_command("/files cancel", current);
  REQUIRE(cancel);
  REQUIRE(std::holds_alternative<surfaces::LocalBrowseCancel>(**cancel));
}
