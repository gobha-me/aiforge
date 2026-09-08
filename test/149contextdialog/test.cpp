#include "../144chatsummary/fixture.hpp"
#include "../159foldergrant/fixture.hpp"
#include <aiforge/adapters/conversation_context_dialog.hpp>
#include <aiforge/adapters/transcript_view.hpp>
#include <aiforge/surfaces/local_source_browser.hpp>
namespace {
using namespace chat_summary_test;
auto key(termforge::Key value, char32_t character = 0, bool ctrl = false)
    -> termforge::Event {
  return termforge::KeyEvent{value, character, ctrl,
                             false, false,     termforge::KeyAction::Press};
}
auto rendered(adapters::ConversationContextDialog& dialog, int cols = 110,
              int rows = 32) -> std::string {
  termforge::Screen screen{cols, rows};
  dialog.draw(screen);
  std::string text;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x)
      text += screen.text_at(x, y);
    text += '\n';
  }
  return text;
}
} // namespace
TEST_CASE(
    "Context dialog failed previews and cancellation preserve the chat draft",
    "[contextdialog][failure]") {
  Fixture f;
  std::string draft = "Unsubmitted story draft";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  const auto before = f.chat->event_log().events();
  const auto missing = id<domain::ConversationSummaryId>("missing");
  CHECK_FALSE(
      dialog.execute(surfaces::PreviewConversationSummary{missing, {}}));
  CHECK_FALSE(dialog.review_available());
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(dialog.on_event(key(termforge::Key::Escape)));
  CHECK(draft == "Unsubmitted story draft");
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Context dialog exposes mandatory capacity failure and recovers "
          "after model change",
          "[contextdialog][capacity]") {
  Fixture f;
  f.models.window = 300;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("tiny")));
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(rendered(dialog).find("Capacity: 300") != std::string::npos);
  CHECK(dialog.status().find("context inspected") == std::string::npos);
  for (const auto& [cols, rows] :
       {std::pair{1, 1}, std::pair{8, 3}, std::pair{40, 10}})
    CHECK_NOTHROW(rendered(dialog, cols, rows));
  f.models.window = 32768;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("large")));
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(rendered(dialog).find("Capacity: 32768") != std::string::npos);
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Context menu and slash command select the same durable policy",
          "[contextdialog][parity]") {
  Fixture menu;
  Fixture command;
  adapters::ConversationContextDialog dialog{
      *menu.chat, [] { return std::string{"kept"}; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  static_cast<void>(rendered(dialog));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  static_cast<void>(rendered(dialog));
  REQUIRE(dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(dialog.on_event(key(termforge::Key::Down)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  const auto parsed = surfaces::parse_conversation_command("mode rolling");
  REQUIRE(parsed);
  adapters::ConversationContextDialog other{*command.chat,
                                            [] { return std::string{"kept"}; }};
  REQUIRE(other.execute(*parsed));
  const auto actual = menu.chat->conversation_policy();
  const auto expected = command.chat->conversation_policy();
  REQUIRE(actual);
  REQUIRE(expected);
  CHECK(actual->policy == expected->policy);
  CHECK(actual->policy.mode == domain::ConversationMode::rolling);
  CHECK(menu.backend.requests().empty());
  CHECK(command.backend.requests().empty());
}
TEST_CASE("Context preview is invalidated when the unsubmitted draft changes",
          "[contextdialog][stale]") {
  Fixture f;
  const auto candidate = f.candidate();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  std::string draft = "First draft";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(
      surfaces::PreviewConversationSummary{candidate.summary_id, {}}));
  REQUIRE(dialog.review_available());
  draft = "Second draft";
  static_cast<void>(rendered(dialog));
  CHECK_FALSE(dialog.review_available());
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.chat->event_log().events() == before);
  CHECK(draft == "Second draft");
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE("Context summary editor keeps separate text after stale save and "
          "rejects oversized paste",
          "[contextdialog][edit]") {
  Fixture f;
  const auto candidate = f.candidate();
  std::string draft = "Chat composer stays here";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(
      dialog.execute(surfaces::EditConversationSummary{candidate.summary_id}));
  static_cast<void>(rendered(dialog));
  const auto original = dialog.editing_text();
  REQUIRE(dialog.on_event(termforge::PasteEvent{
      std::string(domain::summary_maximum_text_bytes, 'x')}));
  CHECK(dialog.editing_text() == original);
  REQUIRE(dialog.on_event(termforge::PasteEvent{" edited"}));
  const auto edited = dialog.editing_text();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  REQUIRE(dialog.on_event(key(termforge::Key::Tab)));
  REQUIRE(dialog.on_event(key(termforge::Key::Enter)));
  CHECK(dialog.editing_text() == edited);
  CHECK(dialog.status().find("stale") != std::string::npos);
  CHECK(draft == "Chat composer stays here");
  CHECK(f.backend.requests().size() == 1);
}
TEST_CASE(
    "Context summary generation remains explicit with review before Apply",
    "[contextdialog][generation]") {
  Fixture f;
  std::string draft = "Unsubmitted instruction";
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; }};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  CHECK(f.backend.requests().empty());
  REQUIRE(dialog.execute(surfaces::GenerateConversationSummary{
      {id<domain::RunId>("source-run")}}));
  CHECK(dialog.status().find("paid request") != std::string::npos);
  f.drain();
  const auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  REQUIRE(catalog->unpublished.size() == 1);
  CHECK(catalog->snapshot.active.empty());
  CHECK_FALSE(dialog.review_available());
  REQUIRE(dialog.execute(surfaces::ReviewConversationSummary{
      catalog->unpublished.front().intent.summary_id}));
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.backend.requests().size() == 1);
  CHECK(draft == "Unsubmitted instruction");
}

TEST_CASE("Transcript summary exclusion preserves the default projection and "
          "reasoning redraw",
          "[contextdialog][transcript]") {
  Fixture f;
  const auto generated = f.generate();
  adapters::TranscriptView default_view;
  REQUIRE(default_view.rebuild(f.chat->event_log().events()));
  adapters::TranscriptView filtered;
  REQUIRE(filtered.rebuild(f.chat->event_log().events(), {generated.run_id}));
  CHECK(filtered.session_projection().runs().size() ==
        default_view.session_projection().runs().size());
  const auto text = [](adapters::TranscriptView& view) {
    termforge::Screen screen{160, 40};
    view.set_geometry({0, 0, 160, 40});
    view.draw(screen);
    std::string value;
    for (int row{}; row < 40; ++row) {
      for (int col{}; col < 160; ++col)
        value += screen.text_at(col, row);
      value += '\n';
    }
    return value;
  };
  CHECK(text(default_view)
            .find("Keep the open task and unresolved constraints.") !=
        std::string::npos);
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") ==
        std::string::npos);
  CHECK(text(filtered).find("Preserve the open task and its constraints.") !=
        std::string::npos);
  REQUIRE(filtered.set_reasoning_visibility(
      adapters::ReasoningVisibility::expanded));
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") ==
        std::string::npos);
  REQUIRE(filtered.rebuild(f.chat->event_log().events()));
  CHECK(text(filtered).find("Keep the open task and unresolved constraints.") !=
        std::string::npos);
}

TEST_CASE(
    "Failed replacement preview cannot leave an older summary armed for Apply",
    "[contextdialog][stale]") {
  Fixture f;
  const auto first = f.candidate();
  const auto second = f.candidate();
  const auto edited = f.chat->edit_conversation_summary(
      f.chat->event_log().last_sequence(), version(second),
      std::string(3500, 'x'));
  INFO((edited ? "edited within the producer bound" : edited.error().message));
  REQUIRE(edited);
  f.models.window = 3000;
  REQUIRE(f.chat->select_model(id<domain::ModelId>("small")));
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }};
  const auto first_preview = dialog.execute(
      surfaces::PreviewConversationSummary{first.summary_id, {}});
  INFO((first_preview ? "first preview fits" : first_preview.error().message));
  REQUIRE(first_preview);
  REQUIRE(dialog.review_available());
  const auto before = f.chat->event_log().events();
  CHECK_FALSE(dialog.execute(
      surfaces::PreviewConversationSummary{second.summary_id, {}}));
  CHECK_FALSE(dialog.review_available());
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.chat->event_log().events() == before);
  CHECK(f.backend.requests().size() == 2);
}

namespace {
auto evidence_outcome(Fixture& fixture) -> surfaces::ChatEvidenceOutcome {
  std::optional<surfaces::ChatEvidenceOutcome> outcome;
  REQUIRE(folder_grant_test::until([&] {
    auto polled = fixture.chat->poll_evidence_work();
    INFO((polled ? "polled" : polled.error().message));
    REQUIRE(polled);
    if (*polled) outcome = std::move(**polled);
    return outcome.has_value();
  }));
  REQUIRE(outcome);
  return std::move(*outcome);
}
auto finish_evidence(Fixture& fixture,
                     adapters::ConversationContextDialog& dialog) -> void {
  const auto outcome = evidence_outcome(fixture);
  auto completed = dialog.complete_evidence_work(outcome);
  INFO((completed ? "completed" : completed.error().message));
  REQUIRE(completed);
  REQUIRE(*completed);
}
} // namespace

TEST_CASE("Async Context completion binds exact operation and preserves the "
          "prior view on failure",
          "[contextdialog][async]") {
  Fixture f;
  std::string draft{"Unsubmitted draft"};
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; },
                                             true};
  REQUIRE(dialog.execute(surfaces::InspectConversationSummaries{}));
  const auto body = dialog.display_text();
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  const auto token = dialog.pending_evidence_work();
  REQUIRE(token);
  CHECK(dialog.display_text() == body);
  auto outcome = evidence_outcome(f);
  auto foreign = outcome;
  ++foreign.token.generation;
  auto ignored = dialog.complete_evidence_work(std::move(foreign));
  REQUIRE(ignored);
  CHECK_FALSE(*ignored);
  CHECK(dialog.pending_evidence_work() == token);
  CHECK_FALSE(dialog.fail_evidence_work(
      surfaces::ChatEvidenceWorkToken{token->session_id, token->model_id,
                                      token->generation + 1, token->purpose},
      {surfaces::ChatSessionErrorCode::context_failed, "wrong operation"}));
  REQUIRE(dialog.fail_evidence_work(
      *token,
      {surfaces::ChatSessionErrorCode::context_failed, "Sources unavailable"}));
  CHECK(dialog.display_text() == body);
  auto late = dialog.complete_evidence_work(std::move(outcome));
  REQUIRE(late);
  CHECK_FALSE(*late);
  CHECK(draft == "Unsubmitted draft");
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Async Context draft changes and close reject late outcomes without "
          "cancelling foreign work",
          "[contextdialog][async]") {
  Fixture f;
  std::string draft{"First draft"};
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; },
                                             true};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  auto outcome = evidence_outcome(f);
  draft = "Changed draft";
  CHECK_FALSE(dialog.complete_evidence_work(std::move(outcome)));
  CHECK_FALSE(dialog.pending_evidence_work());
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  f.chat->cancel_evidence_work();
  REQUIRE(f.chat->request_context_inspection("Foreign owner draft"));
  const auto foreign = f.chat->pending_evidence_work();
  REQUIRE(foreign);
  REQUIRE(dialog.on_event(key(termforge::Key::Escape)));
  CHECK(f.chat->pending_evidence_work() == foreign);
  CHECK_FALSE(dialog.pending_evidence_work());
  f.chat->cancel_evidence_work();
  CHECK(f.backend.requests().empty());
}
TEST_CASE("Async Context preview and apply stay explicit and do not duplicate "
          "committed events",
          "[contextdialog][async]") {
  Fixture f;
  const auto candidate = f.candidate();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  std::string draft{"Unsubmitted draft"};
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; },
                                             true};
  unsigned callbacks{};
  dialog.on_committed([&](std::vector<domain::RunEvent>) { ++callbacks; });
  REQUIRE(dialog.execute(
      surfaces::PreviewConversationSummary{candidate.summary_id, {}}));
  CHECK_FALSE(dialog.review_available());
  finish_evidence(f, dialog);
  REQUIRE(dialog.review_available());
  CHECK(dialog.status() == "Preview ready; Apply is explicit");
  auto catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->snapshot.active.empty());
  REQUIRE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK_FALSE(dialog.review_available());
  const auto apply_token = dialog.pending_evidence_work();
  REQUIRE(apply_token);
  f.chat->cancel_evidence_work();
  REQUIRE(dialog.fail_evidence_work(
      *apply_token,
      {surfaces::ChatSessionErrorCode::cancelled, "Cancelled before commit"}));
  CHECK(dialog.review_available());
  REQUIRE(dialog.execute(surfaces::ApplyConversationSummary{}));
  finish_evidence(f, dialog); // Apply commits; app handles committed events.
  REQUIRE(dialog.pending_evidence_work()); // Updated inspection follows
                                           // asynchronously.
  finish_evidence(f, dialog);
  catalog = f.chat->summary_catalog();
  REQUIRE(catalog);
  CHECK(catalog->snapshot.active.size() == 1);
  CHECK(callbacks == 0);
  CHECK(f.backend.requests().size() == 1);
  CHECK(draft == "Unsubmitted draft");
}
TEST_CASE("Failed new async summary preview retains displayed text without "
          "reviving old Apply",
          "[contextdialog][async]") {
  Fixture f;
  const auto candidate = f.candidate();
  REQUIRE(f.chat->set_conversation_policy(0, domain::ConversationMode::rolling,
                                          {}));
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }, true};
  REQUIRE(dialog.execute(
      surfaces::PreviewConversationSummary{candidate.summary_id, {}}));
  finish_evidence(f, dialog);
  REQUIRE(dialog.review_available());
  const auto displayed = dialog.display_text();
  REQUIRE(dialog.execute(
      surfaces::PreviewConversationSummary{candidate.summary_id, {}}));
  const auto token = dialog.pending_evidence_work();
  REQUIRE(token);
  f.chat->cancel_evidence_work();
  REQUIRE(dialog.fail_evidence_work(
      *token, {surfaces::ChatSessionErrorCode::cancelled, "Cancelled"}));
  CHECK(dialog.display_text() == displayed);
  CHECK_FALSE(dialog.review_available());
  CHECK_FALSE(dialog.execute(surfaces::ApplyConversationSummary{}));
  CHECK(f.backend.requests().size() == 1);
}

namespace {
struct ContextSourceObservation {
  std::shared_ptr<folder_grant_test::Gate> gate;
};
class ContextSourceLease final : public runtime::LocalSourceLease {
 public:
  ContextSourceLease(runtime::LocalFolderGrantRequest request,
                     std::shared_ptr<ContextSourceObservation> observation)
      : m_request(std::move(request)), m_observation(std::move(observation)) {}
  auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& override {
    return m_root;
  }
  auto session_id() const noexcept -> const domain::SessionId& override {
    return m_request.token.session_id;
  }
  auto lease_generation() const noexcept -> std::uint64_t override {
    return m_request.lease_generation;
  }
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto revoke() noexcept -> void override {}
  auto list(runtime::LocalListRequest request, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    return runtime::LocalListResult{request.token,
                                    request.directory,
                                    {},
                                    0,
                                    runtime::LocalListingState::complete};
  }
  auto preview(runtime::LocalPreviewRequest request, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    return runtime::LocalPreviewResult{
        request.token,
        request.relative_path,
        5,
        "hello",
        runtime::LocalPreviewState::complete,
        domain::LocalSourceIdentity{
            m_root,
            request.relative_path,
            {"sha256",
             "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
             5}}};
  }
  auto revalidate(runtime::LocalRevalidateRequest request, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    if (m_observation->gate) m_observation->gate->wait();
    return runtime::LocalReadResult{request.token, request.expected_source,
                                    "hello"};
  }

 private:
  runtime::LocalFolderGrantRequest m_request;
  std::shared_ptr<ContextSourceObservation> m_observation;
  domain::LocalRootIdentity m_root{1, std::string(64, 'a')};
};
class ContextSourceFactory final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<ContextSourceObservation> observation{
      std::make_shared<ContextSourceObservation>()};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto grant(const runtime::LocalFolderGrantRequest& request, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    auto lease = std::make_shared<ContextSourceLease>(request, observation);
    return runtime::LocalFolderGrantResult{
        request.token, lease->root_identity(), std::move(lease)};
  }
};
} // namespace
TEST_CASE("Async Context closes while a local exact read is blocked and "
          "displays actual admission",
          "[contextdialog][async][local]") {
  auto factory = std::make_shared<ContextSourceFactory>();
  auto browser = surfaces::LocalSourceBrowser::create(factory);
  REQUIRE(browser);
  Fixture f;
  REQUIRE((*browser)->activate_session(f.chat->session_id()));
  REQUIRE((*browser)->add_folder("/session-files"));
  const auto finish_browser = [&] {
    REQUIRE(folder_grant_test::until([&] {
      const auto polled = (*browser)->poll();
      REQUIRE(polled);
      return !(*browser)->state().granting && !(*browser)->state().reading;
    }));
    REQUIRE(folder_grant_test::until(
        [&] { return (*browser)->occupied_workers() == 0; }));
  };
  finish_browser();
  REQUIRE((*browser)->add_evidence((*browser)->state().folders.front().root,
                                   "notes.txt"));
  finish_browser();
  f.dependencies.local_sources = browser->get();
  f.reopen();
  std::string draft{"Retained draft"};
  adapters::ConversationContextDialog dialog{*f.chat, [&] { return draft; },
                                             true};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  finish_evidence(f, dialog);
  CHECK(dialog.display_text().find("notes.txt | 5 tokens | included") !=
        std::string::npos);
  REQUIRE(folder_grant_test::until(
      [&] { return (*browser)->occupied_workers() == 0; }));
  const auto displayed = dialog.display_text();
  auto gate = std::make_shared<folder_grant_test::Gate>();
  factory->observation->gate = gate;
  folder_grant_test::Release release{gate};
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  REQUIRE(gate->await());
  REQUIRE(dialog.on_event(key(termforge::Key::Escape)));
  CHECK_FALSE(f.chat->pending_evidence_work());
  CHECK_FALSE(dialog.pending_evidence_work());
  CHECK(dialog.display_text() == displayed);
  CHECK((*browser)->state().selection.size() == 1);
  gate->release();
  finish_browser();
  CHECK(draft == "Retained draft");
  CHECK(f.backend.requests().empty());
}

TEST_CASE("Async Context malformed completion ends preparation without "
          "replacing displayed review",
          "[contextdialog][async]") {
  Fixture f;
  adapters::ConversationContextDialog dialog{
      *f.chat, [] { return std::string{"draft"}; }, true};
  REQUIRE(dialog.execute(surfaces::InspectConversationSummaries{}));
  const auto body = dialog.display_text();
  REQUIRE(dialog.execute(surfaces::InspectConversation{}));
  auto outcome = evidence_outcome(f);
  outcome.result = surfaces::ChatEvidenceActionCompleted{};
  CHECK_FALSE(dialog.complete_evidence_work(std::move(outcome)));
  CHECK_FALSE(dialog.pending_evidence_work());
  CHECK(dialog.status().find("type does not match") != std::string::npos);
  CHECK(dialog.display_text() == body);
  CHECK(f.backend.requests().empty());
}
