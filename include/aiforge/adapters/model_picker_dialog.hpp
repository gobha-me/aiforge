#pragma once

#include <aiforge/model/catalog.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <termforge/widgets/dialog.hpp>
#include <termforge/widgets/list_widget.hpp>
#include <termforge/widgets/text_input.hpp>

namespace aiforge::adapters {

class ModelPickerDialog final : public termforge::Dialog {
 public:
  ModelPickerDialog();

  auto set_models(const model::CatalogSnapshot& snapshot,
                  std::optional<domain::ModelId> current = std::nullopt)
      -> void;
  auto on_result(std::function<void(std::optional<domain::ModelId>)> callback)
      -> void;
  auto on_event(const termforge::Event& event) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 18; }
  [[nodiscard]] auto content_cols() const -> int override { return 88; }
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  struct Choice {
    model::CatalogEntry entry;
    std::string label;
    std::string searchable;
    bool unavailable{};
  };

  auto apply_filter() -> void;
  auto choose(int index) -> void;
  [[nodiscard]] auto selected_entry() const noexcept
      -> const model::CatalogEntry*;
  [[nodiscard]] auto metadata_lines() const -> std::vector<std::string>;

  termforge::TextInput m_filter;
  termforge::ListWidget m_list;
  termforge::Rect m_metadata_area;
  std::vector<Choice> m_all;
  std::vector<std::size_t> m_visible;
  std::optional<domain::ModelId> m_current;
  model::CatalogOrigin m_origin{model::CatalogOrigin::live};
  std::string m_filter_error;
  std::function<void(std::optional<domain::ModelId>)> m_on_result;
};

} // namespace aiforge::adapters
