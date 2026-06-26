#pragma once

#include "runtime/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace amber::runtime {

struct RuntimeWatchCellSnapshot {
  std::uint64_t cell_id = 0;
  std::uint64_t revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string target_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchWriteResult {
  bool changed = false;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
};

struct RuntimeWatchObjectStateSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::unordered_map<std::string, std::uint64_t> field_revisions;
};

struct RuntimeWatchIvarSnapshot {
  std::uint64_t object_id = 0;
  std::uint64_t object_revision = 0;
  std::uint64_t field_revision = 0;
  std::uint64_t subscriber_count = 0;
  std::string field_name;
  bool watched = false;
  Value value = Value::null();
};

struct RuntimeWatchIvarWriteResult {
  bool changed = false;
  std::string field_name;
  Value old_value = Value::null();
  Value new_value = Value::null();
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
};

struct RuntimeWatchEvent {
  std::string kind;
  std::uint64_t watch_epoch = 0;
  std::uint64_t cell_id = 0;
  std::uint64_t handle_id = 0;
  std::uint64_t object_id = 0;
  std::uint64_t old_object_revision = 0;
  std::uint64_t new_object_revision = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t old_revision = 0;
  std::uint64_t new_revision = 0;
  Value old_value = Value::null();
  Value new_value = Value::null();
};

enum class RuntimeDependencyKind { Binding, Ivar, Object };

struct RuntimeDependency {
  RuntimeDependencyKind kind = RuntimeDependencyKind::Binding;
  std::uint64_t cell_id = 0;
  std::uint64_t object_id = 0;
  std::string target_name;
  std::string field_name;
  std::uint64_t revision = 0;
  std::uint64_t object_revision = 0;
};

struct RuntimeDependencySet {
  std::uint64_t notebook_cell_id = 0;
  std::vector<RuntimeDependency> dependencies;
};

class RuntimeWatchCell {
public:
  explicit RuntimeWatchCell(Value value = Value::null(),
                            std::uint64_t cell_id = 0,
                            std::string target_name = {});

  Value read() const;
  RuntimeWatchWriteResult write(Value value);
  RuntimeWatchCellSnapshot snapshot() const;
  bool watched() const;
  void enable_watch(std::string target_name = {});
  void subscribe();
  void unsubscribe();
  std::uint64_t cell_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchObjectState {
public:
  explicit RuntimeWatchObjectState(std::uint64_t object_id = 0);

  RuntimeWatchObjectStateSnapshot snapshot() const;
  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const;
  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value);
  void subscribe_field(std::string field_name);
  void unsubscribe_field(const std::string &field_name);
  bool field_watched(const std::string &field_name) const;
  std::uint64_t object_id() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class RuntimeWatchHandle {
public:
  RuntimeWatchHandle();
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                     std::uint64_t handle_id, std::string target_name);
  RuntimeWatchHandle(std::shared_ptr<RuntimeWatchObjectState> object_state,
                     std::uint64_t handle_id, std::string target_name,
                     std::string field_name);
  RuntimeWatchHandle(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle &operator=(const RuntimeWatchHandle &) = delete;
  RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept;
  RuntimeWatchHandle &operator=(RuntimeWatchHandle &&) noexcept;
  ~RuntimeWatchHandle();

  bool active() const;
  bool unwatch();
  std::uint64_t handle_id() const;
  std::shared_ptr<RuntimeWatchCell> cell() const;
  RuntimeWatchCellSnapshot snapshot() const;

private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace amber::runtime
