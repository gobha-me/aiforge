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
                  const domain::ModelId& current) -> void;
  auto on_result(
      std::function<void(std::optional<domain::ModelId>)> callback) -> void;

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 10; }
  [[nodiscard]] auto content_cols() const -> int override { return 72; }
  auto layout_content(termforge::Rect area) -> void override;
  auto draw_content(termforge::Screen& screen) -> void override;
  auto on_escape() -> void override;

 private:
  struct Choice {
    domain::ModelId id;
    std::string label;
    std::string searchable;
    bool offline{};
  };

  auto apply_filter() -> void;
  auto choose(int index) -> void;

  termforge::TextInput m_filter;
  termforge::ListWidget m_list;
  std::vector<Choice> m_all;
  std::vector<std::size_t> m_visible;
  std::optional<domain::ModelId> m_current;
  std::function<void(std::optional<domain::ModelId>)> m_on_result;
};

}  // namespace aiforge::adapters
