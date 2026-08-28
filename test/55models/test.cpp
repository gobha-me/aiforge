#include <aiforge/model/catalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace aiforge;
using namespace std::chrono_literals;

namespace {

auto model_id(const std::string& value) -> domain::ModelId {
  return domain::ModelId::from(value).value();
}

auto entry(std::string id, std::string type = "text",
           const std::uint64_t context = 8192) -> model::CatalogEntry {
  model::CatalogEntry result{model_id(id), std::move(type)};
  if (context != 0) result.context_window_tokens = context;
  result.maximum_output_tokens = 1024;
  return result;
}

auto snapshot(const std::chrono::milliseconds timestamp,
              std::vector<model::CatalogEntry> entries)
    -> model::CatalogSnapshot {
  return {std::chrono::sys_time<std::chrono::milliseconds>{timestamp},
          std::move(entries)};
}

class FakeSource final : public model::CatalogSource {
 public:
  auto fetch(std::stop_token stop_token)
      -> std::expected<model::CatalogSnapshot, model::CatalogError> override {
    ++calls;
    if (stop_token.stop_requested()) {
      return std::unexpected(model::CatalogError{
          model::CatalogErrorCode::cancelled, "cancelled", false});
    }
    if (failure) return std::unexpected(*failure);
    return value;
  }

  model::CatalogSnapshot value{
      std::chrono::sys_time<std::chrono::milliseconds>{}};
  std::optional<model::CatalogError> failure;
  int calls{};
};

class FakeCache final : public model::CatalogCache {
 public:
  auto load(std::stop_token stop_token)
      -> std::expected<std::optional<model::CatalogSnapshot>,
                       model::CatalogError> override {
    ++loads;
    if (stop_token.stop_requested()) {
      return std::unexpected(model::CatalogError{
          model::CatalogErrorCode::cancelled, "cancelled", false});
    }
    if (load_failure) return std::unexpected(*load_failure);
    return value;
  }

  auto store(const model::CatalogSnapshot& snapshot, std::stop_token stop_token)
      -> std::expected<void, model::CatalogError> override {
    ++stores;
    if (stop_token.stop_requested()) {
      return std::unexpected(model::CatalogError{
          model::CatalogErrorCode::cancelled, "cancelled", false});
    }
    if (store_failure) return std::unexpected(*store_failure);
    stored = snapshot;
    return {};
  }

  std::optional<model::CatalogSnapshot> value;
  std::optional<model::CatalogSnapshot> stored;
  std::optional<model::CatalogError> load_failure;
  std::optional<model::CatalogError> store_failure;
  int loads{};
  int stores{};
};

} // namespace

TEST_CASE("catalog validation rejects malformed, duplicate, and bounded data",
          "[models][failure]") {
  auto duplicate = snapshot(1ms, {entry("same"), entry("same")});
  auto checked = model::validate_catalog(duplicate);
  REQUIRE_FALSE(checked);
  REQUIRE(checked.error().code == model::CatalogErrorCode::invalid_data);

  auto control = entry("control");
  control.type = "te\x1bxt";
  checked = model::validate_catalog(snapshot(1ms, {std::move(control)}));
  REQUIRE_FALSE(checked);

  checked = model::validate_catalog(snapshot(1ms, {entry("one"), entry("two")}),
                                    {.maximum_entries = 1});
  REQUIRE_FALSE(checked);
}

TEST_CASE("fresh cache avoids live fetch and one snapshot is reused",
          "[models][cache]") {
  FakeSource source;
  source.value = snapshot(1000ms, {entry("live")});
  FakeCache cache;
  cache.value = snapshot(950ms, {entry("cached")});
  model::CatalogService service{
      source, &cache, 24h,
      [] { return std::chrono::sys_time<std::chrono::milliseconds>{1000ms}; }};

  auto first = service.snapshot();
  REQUIRE(first);
  REQUIRE(first->get().origin == model::CatalogOrigin::fresh_cache);
  REQUIRE(first->get().entries.front().id == model_id("cached"));
  REQUIRE(source.calls == 0);
  REQUIRE(cache.loads == 1);

  REQUIRE(service.snapshot());
  REQUIRE(cache.loads == 1);
  REQUIRE(source.calls == 0);
}

TEST_CASE("stale cache refreshes or degrades explicitly on network failure",
          "[models][cache][failure]") {
  FakeSource source;
  source.value = snapshot(1000ms, {entry("live")});
  FakeCache cache;
  cache.value = snapshot(1ms, {entry("stale")});
  model::CatalogService refreshed{
      source, &cache, 1h,
      [] { return std::chrono::sys_time<std::chrono::milliseconds>{48h}; }};
  auto live = refreshed.snapshot();
  REQUIRE(live);
  REQUIRE(live->get().origin == model::CatalogOrigin::live);
  REQUIRE(cache.stores == 1);

  source.calls = 0;
  source.failure = model::CatalogError{model::CatalogErrorCode::unavailable,
                                       "offline", true};
  cache.stores = 0;
  model::CatalogService fallback{
      source, &cache, 1h,
      [] { return std::chrono::sys_time<std::chrono::milliseconds>{48h}; }};
  auto stale = fallback.snapshot();
  REQUIRE(stale);
  REQUIRE(stale->get().origin == model::CatalogOrigin::stale_cache);
  REQUIRE_FALSE(stale->get().warnings.empty());
  REQUIRE(cache.stores == 0);
}

TEST_CASE("cancellation and absent fallback never become stale success",
          "[models][failure][cancel]") {
  FakeSource source;
  FakeCache cache;
  source.failure = model::CatalogError{model::CatalogErrorCode::unavailable,
                                       "offline", true};
  model::CatalogService unavailable{source, &cache};
  auto result = unavailable.snapshot();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == model::CatalogErrorCode::unavailable);

  std::stop_source stop;
  stop.request_stop();
  model::CatalogService cancelled{source, &cache};
  result = cancelled.snapshot(stop.get_token());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == model::CatalogErrorCode::cancelled);
}

TEST_CASE("lookup rejects missing offline and capacity-less text models",
          "[models][lookup][failure]") {
  FakeSource source;
  auto offline = entry("offline");
  offline.offline = true;
  auto empty = entry("empty", "text", 0);
  source.value = snapshot(1ms, {entry("ready"), std::move(offline),
                                std::move(empty), entry("image", "image")});
  model::CatalogService service{source};

  auto found = service.lookup(model_id("ready"), {});
  REQUIRE(found);
  REQUIRE(found->context_window_tokens == 8192);

  REQUIRE_FALSE(service.lookup(model_id("missing"), {}));
  REQUIRE_FALSE(service.lookup(model_id("offline"), {}));
  REQUIRE_FALSE(service.lookup(model_id("empty"), {}));
  REQUIRE_FALSE(service.lookup(model_id("image"), {}));
}

TEST_CASE("lookup returns provenance-rich decimal pricing observations",
          "[models][pricing]") {
  FakeSource source;
  auto priced = entry("priced");
  model::Pricing rates;
  rates.base.input = model::Price{domain::DecimalAmount::from("1.42").value(),
                                  domain::DecimalAmount::from("2.5").value()};
  rates.base.output =
      model::Price{domain::DecimalAmount::from("2.83").value(), std::nullopt};
  priced.pricing = std::move(rates);
  source.value = snapshot(123ms, {std::move(priced)});
  source.value.source_id = "test.models";
  source.value.source_revision = "revision-7";
  model::CatalogService service{source};

  const auto found = service.lookup(model_id("priced"), {});
  REQUIRE(found);
  REQUIRE(found->pricing_observation);
  REQUIRE(found->pricing_observation->model_id == model_id("priced"));
  REQUIRE(found->pricing_observation->source_id == "test.models");
  REQUIRE(found->pricing_observation->source_revision == "revision-7");
  REQUIRE(found->pricing_observation->fetched_at ==
          std::chrono::sys_time<std::chrono::milliseconds>{123ms});
  REQUIRE(found->pricing_observation->origin ==
          domain::PricingCatalogOrigin::live);
  REQUIRE(found->pricing_observation->pricing.base.input->usd->to_string() ==
          "1.42");
  REQUIRE(domain::validate_pricing_observation(*found->pricing_observation));
}

TEST_CASE("model suggestions are text-only deterministic and bounded",
          "[models][suggestions]") {
  auto image = entry("alpha-image", "image");
  auto offline = entry("alpha-offline");
  offline.offline = true;
  auto catalog = snapshot(1ms, {entry("alpha-chat"), entry("alpine-chat"),
                                std::move(image), std::move(offline)});
  const std::vector<std::string> expected{"alpha-chat", "alpine-chat"};
  REQUIRE(model::suggest_models(catalog, "alpa-chat", 3) == expected);
}
