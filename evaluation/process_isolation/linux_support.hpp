#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace aiforge::evaluation::process_isolation::linux_support {

inline constexpr std::size_t maximum_control_bytes = 64UZ * 1024UZ;

class Descriptor final {
 public:
  explicit Descriptor(int value = -1);
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&& other) noexcept;
  auto operator=(Descriptor&& other) noexcept -> Descriptor&;
  ~Descriptor();

  [[nodiscard]] auto get() const noexcept -> int;
  [[nodiscard]] auto release() noexcept -> int;
  auto reset(int value = -1) noexcept -> void;

 private:
  int m_value{-1};
};

[[nodiscard]] auto write_all(int descriptor, std::string_view value) -> bool;
[[nodiscard]] auto write_control(int directory, const char* name,
                                 std::string_view value) -> bool;
[[nodiscard]] auto read_control(int directory, const char* name,
                                std::size_t maximum = maximum_control_bytes)
    -> std::optional<std::string>;
[[nodiscard]] auto parse_processes(std::string_view document)
    -> std::optional<std::vector<pid_t>>;
[[nodiscard]] auto cgroup_is_populated(int directory) -> std::optional<bool>;

[[nodiscard]] auto pidfd_open(pid_t process) -> Descriptor;
[[nodiscard]] auto pidfd_kill(int descriptor) -> bool;
[[nodiscard]] auto pidfd_dead(int descriptor) -> bool;
[[nodiscard]] auto close_descriptors_from(unsigned int first) -> bool;

[[nodiscard]] auto safe_delegated_root_path(const std::filesystem::path& path)
    -> bool;
[[nodiscard]] auto open_pinned_directory(const std::filesystem::path& path)
    -> Descriptor;
[[nodiscard]] auto task_cgroup_owned_by(pid_t process, std::string_view prefix,
                                        std::string_view name) -> bool;

enum class BootstrapError {
  none,
  missing_delegation,
  missing_controller,
  cleanup_failed,
  internal_error,
};

class CgroupBootstrap final {
 public:
  explicit CgroupBootstrap(std::string task_prefix);
  CgroupBootstrap(const CgroupBootstrap&) = delete;
  auto operator=(const CgroupBootstrap&) -> CgroupBootstrap& = delete;
  ~CgroupBootstrap();

  [[nodiscard]] auto start(const std::filesystem::path& path) -> BootstrapError;
  [[nodiscard]] auto descriptor() const noexcept -> int;
  auto remember_task_owner(pid_t process) -> void;
  [[nodiscard]] auto cleanup_task_owner(pid_t process) -> bool;
  [[nodiscard]] auto cleanup() noexcept -> bool;

 private:
  [[nodiscard]] auto validate_root() const -> BootstrapError;
  [[nodiscard]] auto validate_controllers() const -> BootstrapError;
  [[nodiscard]] auto validate_root_ownership() const -> BootstrapError;
  [[nodiscard]] auto create_supervisor() -> BootstrapError;
  [[nodiscard]] auto migrate_to_supervisor() -> BootstrapError;
  [[nodiscard]] auto enable_controllers() -> BootstrapError;
  [[nodiscard]] auto fail(BootstrapError error) -> BootstrapError;

  std::string m_task_prefix;
  Descriptor m_root;
  Descriptor m_supervisor;
  std::string m_supervisor_name;
  bool m_created{};
  bool m_moved{};
  bool m_enabled{};
  bool m_ready{};
  bool m_cleaned{};
  bool m_cleanup_okay{true};
  std::vector<pid_t> m_task_owners;
};

enum class TaskCgroupError {
  missing_delegation,
  mechanism_absent,
  prerequisite_unavailable,
  internal_error,
};

class TaskCgroup final {
 public:
  [[nodiscard]] static auto create(std::string_view prefix,
                                   std::string_view suffix = {},
                                   int delegated_root_descriptor = 4)
      -> std::expected<TaskCgroup, TaskCgroupError>;

  TaskCgroup(const TaskCgroup&) = delete;
  auto operator=(const TaskCgroup&) -> TaskCgroup& = delete;
  TaskCgroup(TaskCgroup&& other) noexcept;
  auto operator=(TaskCgroup&& other) noexcept -> TaskCgroup&;
  ~TaskCgroup();

  [[nodiscard]] auto parent() const noexcept -> int;
  [[nodiscard]] auto child() const noexcept -> int;
  [[nodiscard]] auto name() const noexcept -> std::string_view;
  [[nodiscard]] auto has_required_controllers() const -> std::optional<bool>;
  [[nodiscard]] auto processes() const -> std::optional<std::vector<pid_t>>;
  [[nodiscard]] auto cleanup() -> bool;

 private:
  TaskCgroup(Descriptor parent, Descriptor child, std::string name);

  Descriptor m_parent;
  Descriptor m_child;
  std::string m_name;
  bool m_cleaned{};
};

#if defined(AIFORGE_PROCESS_ISOLATION_TEST_SUPPORT)
namespace test_support {

[[nodiscard]] auto scan_cgroup_directories(int directory)
    -> std::optional<std::vector<std::string>>;

} // namespace test_support
#endif

} // namespace aiforge::evaluation::process_isolation::linux_support
