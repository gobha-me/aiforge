#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/backend/provider_character_catalog.hpp>
#include <aiforge/testing/scripted_provider_character_catalog_source.hpp>

namespace {

using namespace aiforge;

auto character_id(std::string value) -> domain::ProviderCharacterId {
  return domain::ProviderCharacterId::from(std::move(value)).value();
}

auto model_id(std::string value) -> domain::ModelId {
  return domain::ModelId::from(std::move(value)).value();
}

auto character(std::string id) -> backend::ProviderCharacterSummary {
  return backend::ProviderCharacterSummary{character_id(std::move(id))};
}

auto catalog(std::vector<backend::ProviderCharacterSummary> entries = {})
    -> backend::ProviderCharacterCatalog {
  return {std::move(entries), "venice"};
}

template <typename T>
concept HasAdultMember = requires(T value) { value.adult; };

static_assert(!HasAdultMember<backend::ProviderCharacterSummary>);
static_assert(!std::same_as<domain::ProviderCharacterId, domain::ModelId>);
static_assert(
    std::same_as<decltype(backend::ProviderCharacterSummary::featured), bool>);
static_assert(std::same_as<
              decltype(backend::ProviderCharacterSummary::web_enabled), bool>);

} // namespace

TEST_CASE("provider character validation rejects unusable limits",
          "[provider-characters][failure]") {
  const auto valid = catalog({character("helper")});

  auto no_entries = backend::ProviderCharacterLimits{};
  no_entries.maximum_entries = 0;
  auto no_text = backend::ProviderCharacterLimits{};
  no_text.maximum_text_bytes = 0;
  auto no_tags = backend::ProviderCharacterLimits{};
  no_tags.maximum_tags_per_entry = 0;
  auto no_total = backend::ProviderCharacterLimits{};
  no_total.maximum_total_text_bytes = 0;
  auto no_response = backend::ProviderCharacterLimits{};
  no_response.maximum_response_bytes = 0;

  for (const auto& limits :
       {no_entries, no_text, no_tags, no_total, no_response}) {
    const auto checked =
        backend::validate_provider_character_catalog(valid, limits);
    REQUIRE_FALSE(checked);
    REQUIRE(checked.error().code ==
            backend::ProviderCharacterErrorCode::invalid_request);
  }
}

TEST_CASE("provider character validation rejects empty display data",
          "[provider-characters][failure]") {
  auto empty_source = catalog({character("helper")});
  empty_source.source_id.clear();
  auto checked = backend::validate_provider_character_catalog(empty_source);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  auto empty_name = character("helper");
  empty_name.name = "";
  checked = backend::validate_provider_character_catalog(
      catalog({std::move(empty_name)}));
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  auto empty_description = character("helper");
  empty_description.description = "";
  checked = backend::validate_provider_character_catalog(
      catalog({std::move(empty_description)}));
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  auto empty_tag = character("helper");
  empty_tag.tags.emplace_back();
  checked = backend::validate_provider_character_catalog(
      catalog({std::move(empty_tag)}));
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);
}

TEST_CASE("provider character validation rejects unsafe external text",
          "[provider-characters][failure]") {
  const std::vector<std::string> unsafe_text{std::string{"\xc3", 1},
                                             std::string{"bad\x1btext", 8},
                                             std::string{"\xe2\x80\xae", 3}};

  for (const auto& unsafe : unsafe_text) {
    auto unsafe_source = catalog({character("helper")});
    unsafe_source.source_id = unsafe;
    REQUIRE_FALSE(backend::validate_provider_character_catalog(unsafe_source));

    auto unsafe_name = character("helper");
    unsafe_name.name = unsafe;
    REQUIRE_FALSE(backend::validate_provider_character_summary(unsafe_name));

    auto unsafe_description = character("helper");
    unsafe_description.description = unsafe;
    REQUIRE_FALSE(
        backend::validate_provider_character_summary(unsafe_description));

    auto unsafe_tag = character("helper");
    unsafe_tag.tags.push_back(unsafe);
    REQUIRE_FALSE(backend::validate_provider_character_summary(unsafe_tag));
  }

  for (const auto& unsafe_id_text :
       {std::string{"\xc3", 1}, std::string{"\xe2\x80\xae", 3}}) {
    auto unsafe_id = character(unsafe_id_text);
    REQUIRE_FALSE(backend::validate_provider_character_summary(unsafe_id));

    auto unsafe_model = character("helper");
    unsafe_model.model_id = model_id(unsafe_id_text);
    REQUIRE_FALSE(backend::validate_provider_character_summary(unsafe_model));
  }
}

TEST_CASE("provider character validation enforces every collection bound",
          "[provider-characters][failure]") {
  auto too_many_entries = catalog({character("one"), character("two")});
  auto checked = backend::validate_provider_character_catalog(
      too_many_entries, {.maximum_entries = 1});
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::too_large);

  auto oversized_text = character("i");
  oversized_text.name = "12345";
  checked = backend::validate_provider_character_catalog(
      {std::vector<backend::ProviderCharacterSummary>{oversized_text}, "s"},
      {.maximum_text_bytes = 4});
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::too_large);

  auto too_many_tags = character("i");
  too_many_tags.tags = {"one", "two"};
  checked = backend::validate_provider_character_summary(
      too_many_tags, {.maximum_tags_per_entry = 1});
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::too_large);

  auto exact = character("i");
  exact.name = "nn";
  checked = backend::validate_provider_character_catalog(
      {std::vector<backend::ProviderCharacterSummary>{exact}, "s"},
      {.maximum_total_text_bytes = 4});
  REQUIRE(checked);

  checked = backend::validate_provider_character_catalog(
      {std::vector<backend::ProviderCharacterSummary>{std::move(exact)}, "s"},
      {.maximum_total_text_bytes = 3});
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::too_large);
}

TEST_CASE("provider character validation rejects duplicate identities and tags",
          "[provider-characters][failure]") {
  auto checked = backend::validate_provider_character_catalog(
      catalog({character("same"), character("same")}));
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  auto duplicate_tags = character("helper");
  duplicate_tags.tags = {"friendly", "friendly"};
  checked = backend::validate_provider_character_summary(duplicate_tags);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);
}

TEST_CASE("scripted provider character listing is bounded and cancellable",
          "[provider-characters][fake][failure]") {
  const auto value = catalog({character("helper")});
  testing::ScriptedProviderCharacterCatalogSource source{{value}};

  std::stop_source stop;
  stop.request_stop();
  auto result = source.list({}, stop.get_token());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::cancelled);
  REQUIRE(source.remaining_lists() == 1);
  REQUIRE(source.recorded_list_limits().empty());

  result = source.list({.maximum_entries = 0});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::invalid_request);
  REQUIRE(source.remaining_lists() == 1);
  REQUIRE(source.recorded_list_limits().empty());

  const auto limits = backend::ProviderCharacterLimits{.maximum_entries = 2};
  result = source.list(limits);
  REQUIRE(result);
  REQUIRE(*result == value);
  REQUIRE(source.remaining_lists() == 0);
  REQUIRE(source.recorded_list_limits() ==
          std::vector<backend::ProviderCharacterLimits>{limits});

  result = source.list();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::internal_failure);
}

TEST_CASE("scripted provider character errors are replayed exactly",
          "[provider-characters][fake][failure]") {
  const backend::ProviderCharacterError unavailable{
      backend::ProviderCharacterErrorCode::unavailable, "offline", true, 503};
  testing::ScriptedProviderCharacterCatalogSource source{{unavailable}};

  const auto result = source.list();
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == unavailable);
  REQUIRE(source.remaining_lists() == 0);

  const auto id = character_id("helper");
  testing::ScriptedProviderCharacterCatalogSource lookup_source{
      {}, {{id, unavailable}}};
  const auto lookup = lookup_source.lookup(id);
  REQUIRE_FALSE(lookup);
  REQUIRE(lookup.error() == unavailable);
  REQUIRE(lookup_source.remaining_lookups() == 0);
}

TEST_CASE("scripted provider character lookup preserves mismatched exchanges",
          "[provider-characters][fake][failure]") {
  const auto expected_id = character_id("helper");
  const auto value = character("helper");
  testing::ScriptedProviderCharacterCatalogSource source{
      {}, {{expected_id, value}}};

  std::stop_source stop;
  stop.request_stop();
  auto result = source.lookup(expected_id, {}, stop.get_token());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::cancelled);
  REQUIRE(source.recorded_lookups().empty());
  REQUIRE(source.remaining_lookups() == 1);

  result = source.lookup(expected_id, {.maximum_response_bytes = 0});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::invalid_request);
  REQUIRE(source.recorded_lookups().empty());
  REQUIRE(source.remaining_lookups() == 1);

  result = source.lookup(character_id("other"));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::invalid_request);
  REQUIRE(source.recorded_lookups().size() == 1);
  REQUIRE(source.remaining_lookups() == 1);

  const auto limits = backend::ProviderCharacterLimits{.maximum_entries = 1};
  result = source.lookup(expected_id, limits);
  REQUIRE(result);
  REQUIRE(*result == value);
  REQUIRE(source.recorded_lookups().back() ==
          testing::ProviderCharacterLookupRequest{expected_id, limits});
  REQUIRE(source.remaining_lookups() == 0);

  result = source.lookup(expected_id);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          backend::ProviderCharacterErrorCode::internal_failure);
}

TEST_CASE("provider character values retain neutral discovery fields",
          "[provider-characters][smoke]") {
  auto value = character("helper");
  value.name = "Helper";
  value.description = "A concise assistant";
  value.model_id = model_id("venice/model");
  value.featured = true;
  value.web_enabled = true;
  value.tags = {"featured", "general"};
  const auto discovered = catalog({value});

  REQUIRE(backend::validate_provider_character_catalog(discovered));
  REQUIRE(discovered.entries.front() == value);

  const backend::ProviderCharacterLimits defaults;
  REQUIRE(defaults.maximum_entries == 4096);
  REQUIRE(defaults.maximum_text_bytes == 4096);
  REQUIRE(defaults.maximum_tags_per_entry == 256);
  REQUIRE(defaults.maximum_total_text_bytes == 16U * 1024U * 1024U);
  REQUIRE(defaults.maximum_response_bytes == 4U * 1024U * 1024U);
}
