#pragma once

#include <aiforge/backend/provider_character_catalog.hpp>
#include <aiforge/model/catalog.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <termforge/widgets/dialog.hpp>
#include <termforge/widgets/list_widget.hpp>
#include <termforge/widgets/text_input.hpp>

namespace aiforge::adapters {

struct ProviderCharacterPickerResult {
  // No selection means the explicit provider-default/off choice. Cancellation
  // is the outer nullopt supplied to the callback.
  std::optional<backend::ProviderCharacterSummary> selection;
  auto operator==(const ProviderCharacterPickerResult&) const -> bool = default;
};

class ProviderCharacterPickerDialog final : public termforge::Dialog {
 public:
  ProviderCharacterPickerDialog();

  auto set_characters(const backend::ProviderCharacterCatalog& characters,
                      const model::CatalogSnapshot& models,
                      const domain::ModelId& current_model,
                      std::optional<domain::ProviderCharacterId>
                          current_character = std::nullopt) -> void;
  auto on_result(
      std::function<void(std::optional<ProviderCharacterPickerResult>)>
          callback) -> void;
  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 18; }
  [[nodiscard]] auto content_cols() const -> int override { return 88; }
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  struct Choice {
    std::optional<backend::ProviderCharacterSummary> entry;
    std::string label;
    std::string searchable;
    std::string unavailable_reason;
  };

  auto apply_filter() -> void;
  auto choose(int index) -> void;
  [[nodiscard]] auto selected_choice() const noexcept -> const Choice*;
  [[nodiscard]] auto metadata_lines() const -> std::vector<std::string>;

  termforge::TextInput m_filter;
  termforge::ListWidget m_list;
  termforge::Rect m_metadata_area;
  std::vector<Choice> m_all;
  std::vector<std::size_t> m_visible;
  std::optional<domain::ProviderCharacterId> m_current_character;
  domain::ModelId m_current_model{domain::ModelId::from("unknown").value()};
  std::string m_source_id;
  std::string m_filter_error;
  std::function<void(std::optional<ProviderCharacterPickerResult>)> m_on_result;
};

} // namespace aiforge::adapters
