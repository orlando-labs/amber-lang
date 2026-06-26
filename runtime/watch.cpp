#include "runtime/watch.h"

#include <mutex>
#include <utility>

namespace amber::runtime {

class RuntimeWatchCell::Impl {
public:
  Impl(Value value, std::uint64_t cell_id, std::string target_name)
      : value_(std::move(value)), cell_id_(cell_id),
        target_name_(std::move(target_name)) {}

  Value read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  RuntimeWatchWriteResult write(Value value) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchWriteResult result;
    result.old_value = value_;
    result.new_value = value;
    result.old_revision = revision_;
    result.new_revision = revision_;
    if (watched_) {
      value_ = std::move(value);
      ++revision_;
      result.changed = true;
      result.new_revision = revision_;
      result.new_value = value_;
      return result;
    }
    value_ = std::move(value);
    result.new_value = value_;
    return result;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchCellSnapshot snapshot;
    snapshot.cell_id = cell_id_;
    snapshot.revision = revision_;
    snapshot.subscriber_count = subscriber_count_;
    snapshot.target_name = target_name_;
    snapshot.watched = watched_;
    snapshot.value = value_;
    return snapshot;
  }

  bool watched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watched_;
  }

  void enable_watch(std::string target_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_ = true;
    if (!target_name.empty()) {
      target_name_ = std::move(target_name);
    }
  }

  void subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_ = true;
    ++subscriber_count_;
  }

  void unsubscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  std::uint64_t cell_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_id_;
  }

private:
  mutable std::mutex mutex_;
  Value value_ = Value::null();
  std::uint64_t cell_id_ = 0;
  std::uint64_t revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::string target_name_;
  bool watched_ = false;
};

RuntimeWatchCell::RuntimeWatchCell(Value value, std::uint64_t cell_id,
                                   std::string target_name)
    : impl_(std::make_shared<Impl>(std::move(value), cell_id,
                                   std::move(target_name))) {}

Value RuntimeWatchCell::read() const { return impl_->read(); }

RuntimeWatchWriteResult RuntimeWatchCell::write(Value value) {
  return impl_->write(std::move(value));
}

RuntimeWatchCellSnapshot RuntimeWatchCell::snapshot() const {
  return impl_->snapshot();
}

bool RuntimeWatchCell::watched() const { return impl_->watched(); }

void RuntimeWatchCell::enable_watch(std::string target_name) {
  impl_->enable_watch(std::move(target_name));
}

void RuntimeWatchCell::subscribe() { impl_->subscribe(); }

void RuntimeWatchCell::unsubscribe() { impl_->unsubscribe(); }

std::uint64_t RuntimeWatchCell::cell_id() const { return impl_->cell_id(); }

class RuntimeWatchObjectState::Impl {
public:
  explicit Impl(std::uint64_t object_id) : object_id_(object_id) {}

  RuntimeWatchObjectStateSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchObjectStateSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.subscriber_count = subscriber_count_;
    for (const auto &[name, field] : fields_) {
      snapshot.field_revisions[name] = field.revision;
    }
    return snapshot;
  }

  RuntimeWatchIvarSnapshot snapshot_field(const std::string &field_name,
                                          Value current_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchIvarSnapshot snapshot;
    snapshot.object_id = object_id_;
    snapshot.object_revision = object_revision_;
    snapshot.field_name = field_name;
    snapshot.value = std::move(current_value);
    const auto found = fields_.find(field_name);
    if (found != fields_.end()) {
      snapshot.field_revision = found->second.revision;
      snapshot.subscriber_count = found->second.subscriber_count;
      snapshot.watched = found->second.watched;
    }
    return snapshot;
  }

  RuntimeWatchIvarWriteResult write_field(std::string field_name,
                                          Value old_value, Value new_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeWatchIvarWriteResult result;
    result.field_name = field_name;
    result.old_value = std::move(old_value);
    result.new_value = std::move(new_value);
    FieldState &field = fields_[field_name];
    result.old_revision = field.revision;
    result.new_revision = field.revision;
    result.old_object_revision = object_revision_;
    result.new_object_revision = object_revision_;
    if (!field.watched) {
      return result;
    }
    ++field.revision;
    ++object_revision_;
    result.changed = true;
    result.new_revision = field.revision;
    result.new_object_revision = object_revision_;
    return result;
  }

  void subscribe_field(std::string field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    FieldState &field = fields_[std::move(field_name)];
    field.watched = true;
    ++field.subscriber_count;
    ++subscriber_count_;
  }

  void unsubscribe_field(const std::string &field_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    if (found == fields_.end()) {
      return;
    }
    if (found->second.subscriber_count > 0) {
      --found->second.subscriber_count;
    }
    if (subscriber_count_ > 0) {
      --subscriber_count_;
    }
  }

  bool field_watched(const std::string &field_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fields_.find(field_name);
    return found != fields_.end() && found->second.watched;
  }

  std::uint64_t object_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return object_id_;
  }

private:
  struct FieldState {
    std::uint64_t revision = 0;
    std::uint64_t subscriber_count = 0;
    bool watched = false;
  };

  mutable std::mutex mutex_;
  std::uint64_t object_id_ = 0;
  std::uint64_t object_revision_ = 0;
  std::uint64_t subscriber_count_ = 0;
  std::unordered_map<std::string, FieldState> fields_;
};

RuntimeWatchObjectState::RuntimeWatchObjectState(std::uint64_t object_id)
    : impl_(std::make_shared<Impl>(object_id)) {}

RuntimeWatchObjectStateSnapshot RuntimeWatchObjectState::snapshot() const {
  return impl_->snapshot();
}

RuntimeWatchIvarSnapshot
RuntimeWatchObjectState::snapshot_field(const std::string &field_name,
                                        Value current_value) const {
  return impl_->snapshot_field(field_name, std::move(current_value));
}

RuntimeWatchIvarWriteResult
RuntimeWatchObjectState::write_field(std::string field_name, Value old_value,
                                     Value new_value) {
  return impl_->write_field(std::move(field_name), std::move(old_value),
                            std::move(new_value));
}

void RuntimeWatchObjectState::subscribe_field(std::string field_name) {
  impl_->subscribe_field(std::move(field_name));
}

void RuntimeWatchObjectState::unsubscribe_field(const std::string &field_name) {
  impl_->unsubscribe_field(field_name);
}

bool RuntimeWatchObjectState::field_watched(
    const std::string &field_name) const {
  return impl_->field_watched(field_name);
}

std::uint64_t RuntimeWatchObjectState::object_id() const {
  return impl_->object_id();
}

class RuntimeWatchHandle::Impl {
public:
  Impl(std::shared_ptr<RuntimeWatchCell> cell, std::uint64_t handle_id,
       std::string target_name)
      : cell_(std::move(cell)), handle_id_(handle_id),
        target_name_(std::move(target_name)) {
    if (cell_ != nullptr) {
      cell_->enable_watch(target_name_);
      cell_->subscribe();
      active_ = true;
    }
  }

  Impl(std::shared_ptr<RuntimeWatchObjectState> object_state,
       std::uint64_t handle_id, std::string target_name, std::string field_name)
      : object_state_(std::move(object_state)), handle_id_(handle_id),
        target_name_(std::move(target_name)),
        field_name_(std::move(field_name)) {
    if (object_state_ != nullptr) {
      object_state_->subscribe_field(field_name_);
      active_ = true;
    }
  }

  ~Impl() { unwatch(); }

  bool active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
  }

  bool unwatch() {
    std::shared_ptr<RuntimeWatchCell> cell;
    std::shared_ptr<RuntimeWatchObjectState> object_state;
    std::string field_name;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_) {
        return false;
      }
      active_ = false;
      cell = cell_;
      object_state = object_state_;
      field_name = field_name_;
    }
    if (cell != nullptr) {
      cell->unsubscribe();
    }
    if (object_state != nullptr) {
      object_state->unsubscribe_field(field_name);
    }
    return true;
  }

  std::uint64_t handle_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handle_id_;
  }

  std::shared_ptr<RuntimeWatchCell> cell() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cell_;
  }

  RuntimeWatchCellSnapshot snapshot() const {
    std::shared_ptr<RuntimeWatchCell> cell;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cell = cell_;
    }
    return cell == nullptr ? RuntimeWatchCellSnapshot{} : cell->snapshot();
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<RuntimeWatchCell> cell_;
  std::shared_ptr<RuntimeWatchObjectState> object_state_;
  std::uint64_t handle_id_ = 0;
  std::string target_name_;
  std::string field_name_;
  bool active_ = false;
};

RuntimeWatchHandle::RuntimeWatchHandle()
    : impl_(std::make_shared<Impl>(nullptr, 0, "")) {}

RuntimeWatchHandle::RuntimeWatchHandle(std::shared_ptr<RuntimeWatchCell> cell,
                                       std::uint64_t handle_id,
                                       std::string target_name)
    : impl_(std::make_shared<Impl>(std::move(cell), handle_id,
                                   std::move(target_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(
    std::shared_ptr<RuntimeWatchObjectState> object_state,
    std::uint64_t handle_id, std::string target_name, std::string field_name)
    : impl_(std::make_shared<Impl>(std::move(object_state), handle_id,
                                   std::move(target_name),
                                   std::move(field_name))) {}

RuntimeWatchHandle::RuntimeWatchHandle(RuntimeWatchHandle &&) noexcept =
    default;

RuntimeWatchHandle &
RuntimeWatchHandle::operator=(RuntimeWatchHandle &&) noexcept = default;

RuntimeWatchHandle::~RuntimeWatchHandle() = default;

bool RuntimeWatchHandle::active() const { return impl_->active(); }

bool RuntimeWatchHandle::unwatch() { return impl_->unwatch(); }

std::uint64_t RuntimeWatchHandle::handle_id() const {
  return impl_->handle_id();
}

std::shared_ptr<RuntimeWatchCell> RuntimeWatchHandle::cell() const {
  return impl_->cell();
}

RuntimeWatchCellSnapshot RuntimeWatchHandle::snapshot() const {
  return impl_->snapshot();
}

} // namespace amber::runtime
