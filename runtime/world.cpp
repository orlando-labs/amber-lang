#include "runtime/world.h"

#include <utility>

namespace amber::runtime {

namespace {

RuntimeIoProviderStatus
unsupported_io_provider_operation(const std::string &operation) {
  RuntimeIoProviderStatus status;
  status.handled = false;
  status.ok = false;
  status.error_name = "UnsupportedOperationError";
  status.message = "recorded IO provider does not implement " + operation;
  return status;
}

} // namespace

RuntimeIoProviderStatus RuntimeIoProvider::fs_exists(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.exists?");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_file(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.file?");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_dir(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.dir?");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_metadata(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.metadata");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_read_bytes(const std::string &path,
                                 std::optional<std::size_t> limit) {
  (void)path;
  (void)limit;
  return unsupported_io_provider_operation("fs.read_bytes");
}

RuntimeIoProviderStatus
RuntimeIoProvider::fs_write_bytes(const std::string &path,
                                  const std::string &bytes, bool create,
                                  bool truncate, bool append) {
  (void)path;
  (void)bytes;
  (void)create;
  (void)truncate;
  (void)append;
  return unsupported_io_provider_operation("fs.write_bytes");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_mkdir(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.mkdir");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_mkdir_p(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.mkdir_p");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_remove(const std::string &path) {
  (void)path;
  return unsupported_io_provider_operation("fs.remove");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_rename(const std::string &from,
                                                     const std::string &to) {
  (void)from;
  (void)to;
  return unsupported_io_provider_operation("fs.rename");
}

RuntimeIoProviderStatus RuntimeIoProvider::fs_copy(const std::string &from,
                                                   const std::string &to) {
  (void)from;
  (void)to;
  return unsupported_io_provider_operation("fs.copy");
}

} // namespace amber::runtime
