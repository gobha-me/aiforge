#pragma once
#include "../159foldergrant/fixture.hpp"
#include <aiforge/surfaces/local_source_browser.hpp>
#include <catch2/catch_test_macros.hpp>

namespace chat_evidence_test {
using namespace folder_grant_test;
using Browser = surfaces::LocalSourceBrowser;
using Code = domain::LocalSourceErrorCode;
inline auto root() -> domain::LocalRootIdentity {
  return {1, std::string(64, 'a')};
}
inline auto source(std::string path) -> domain::LocalSourceIdentity {
  return {root(),
          std::move(path),
          {"sha256",
           "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
           5}};
}
struct Observation {
  std::atomic<unsigned> grants{}, lists{}, previews{}, exact{}, reads{},
      revokes{};
  std::atomic<bool> fail{}, changed{};
  std::thread::id owner{std::this_thread::get_id()};
  std::atomic<unsigned> owner_io{};
  std::shared_ptr<Gate> grant_gate, read_gate;
};
class BrowserLease final : public runtime::LocalSourceLease {
 public:
  BrowserLease(runtime::LocalFolderGrantRequest request,
               std::shared_ptr<Observation> observation)
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
  auto revoke() noexcept -> void override {
    m_revoked = true;
    ++m_observation->revokes;
  }
  auto list(runtime::LocalListRequest value, std::stop_token)
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override {
    ++m_observation->lists;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    return runtime::LocalListResult{
        value.token,
        value.directory,
        {{"notes.txt", runtime::LocalEntryKind::regular_file}},
        1,
        runtime::LocalListingState::partial};
  }
  auto preview(runtime::LocalPreviewRequest value, std::stop_token)
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override {
    ++m_observation->previews;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    if (value.mode == runtime::LocalPreviewMode::exact) {
      ++m_observation->exact;
      return runtime::LocalPreviewResult{value.token,
                                         value.relative_path,
                                         5,
                                         "hello",
                                         runtime::LocalPreviewState::complete,
                                         source(value.relative_path)};
    }
    return runtime::LocalPreviewResult{value.token,
                                       value.relative_path,
                                       5,
                                       "he",
                                       runtime::LocalPreviewState::prefix,
                                       std::nullopt};
  }
  auto revalidate(runtime::LocalRevalidateRequest value, std::stop_token)
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override {
    ++m_observation->reads;
    if (auto ready = check(); !ready) return std::unexpected(ready.error());
    return runtime::LocalReadResult{value.token, value.expected_source,
                                    m_observation->changed ? "jello" : "hello"};
  }

 private:
  auto check() -> std::expected<void, domain::LocalSourceError> {
    if (std::this_thread::get_id() == m_observation->owner)
      ++m_observation->owner_io;
    if (m_observation->read_gate) m_observation->read_gate->wait();
    if (m_revoked || m_observation->fail)
      return std::unexpected(
          domain::LocalSourceError{Code::io_failure, "secret /private/path"});
    return {};
  }
  runtime::LocalFolderGrantRequest m_request;
  std::shared_ptr<Observation> m_observation;
  domain::LocalRootIdentity m_root{root()};
  std::atomic<bool> m_revoked{};
};
class Factory final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Observation> observation{std::make_shared<Observation>()};
  bool trusted{true};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return trusted;
  }
  auto grant(const runtime::LocalFolderGrantRequest& value, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override {
    ++observation->grants;
    if (std::this_thread::get_id() == observation->owner)
      ++observation->owner_io;
    if (observation->grant_gate) observation->grant_gate->wait();
    return runtime::LocalFolderGrantResult{
        value.token, root(),
        std::make_shared<BrowserLease>(value, observation)};
  }
};
} // namespace chat_evidence_test
