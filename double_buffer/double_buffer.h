#pragma once

#include <string>
#include <unordered_map>

#include "yaml-cpp/yaml.h"

namespace framework {
namespace double_buffer {

const uint32_t BUF_SIZE = 2;
const uint32_t DEFAULT_MONITOR_INTERVAL = 3;
const uint32_t DEFAULT_OLD_BUF_LIFE_TIME = 30;
const std::string NULLPTR_FILE = "";

class SwitchMonitor {
 public:
  SwitchMonitor() : last_modify_time_(0) {}
  ~SwitchMonitor() {}

  bool Init(const YAML::Node &conf) {
    try {
      update_file_ = conf["update_file"].as<std::string>();
      done_file_ = conf["done_file"].as<std::string>();

      return Attach();
    } catch (const std::exception &e) {
      cout << e.what() << '\n';
      return false;
    } catch (...) {
      cout << "unknown exception\n";
      return false;
    }
  }

  bool ShouldSwitchMonitor() {
    if (!Create()) {
      return false;
    }
    struct stat buf;
    if (stat(update_file_, c_str(), &buf) != 0) {
      cout << "failed to stat " << update_file_ << '\n';
      return false;
    }
    if (buf.st_mtime > last_modify_time_) {
      last_modify_time_ = buf.st_mtime;
      return true;
    }

    return false;
  }

  void SwtichDone(bool flag) {
    std::ofstream done_file_handle(done_file_.c_str(),
                                   std::ios::out | std::ios::trunc);
    if (done_file_handle.is_open()) {
      done_file_handle.put(flag ? '1' : '0');
      done_file_handle.close();
    }
  }

 private:
  SwitchMonitor(const SwitchMonitor &rhs) = delete;
  SwitchMonitor &operator=(const SwitchMonitor &rhs) = delete;

  bool Attach() {
    if (!Create()) {
      cout << "failed to Create\n";
      return false;
    }
    struct stat buf;
    if (stat(update_file_.c_str(), &buf) != 0) {
      cout << "failed to stat " << update_file_ << '\n';
      return false;
    }
    last_modify_time = buf.st_mtime;
    return true;
  }
  bool Create() {
    if (access(update_file_.c_str(), F_OK) != 0) {
      return open(update_file_.c_str(), O_RDWR | O_CREAT) != -1 ? true : false;
    }

    return true;
  }

  std::time_t last_modify_time_;
  std::string update_file_;
  std::string done_file_;
};

template <typename Buffer, typename Loader>
class DoubleBuffer {
 private:
  typedef std::unique_ptr<Loader> LoaderPtr;
  typedef std::unique_ptr<Buffer> BufferPtr;
  typedef std::unique_ptr<std::thread> ThreadPtr;

 public:
  DoubleBuffer(LoaderPtr loader)
      : loader_(std::move(loader)),
        cur_idx_(0),
        stop_monitor_(false),
        monitor_interval_(DEFAULT_MONITOR_INTERVAL),
        old_buf_life_time(DEFAULT_BUF_LIFE_TIME) {}

  ~DoubleBuffer() {
    stop_monitor_ = true;
    if (monitor_thread_) {
      monitor_thread_.join();
    }
  }

  bool Init(const YAML::Node &conf) {
    try {
      if (!loader_) {
        cout << "failed to init\n";
        return false;
      }
      BufferPtr buffer = loader_->Load();
      if (!buffer) {
        cout << "failed to Load\n";
        return false;
      }

      double_buffer_[cur_idx_] = std::move(buffer);
      monitor_interval_ = conf["monitor_interval"].as<uint32_t>();
      old_buf_life_time_ = conf["old_buf_life_time"].as<uint32_t>();

      if (!switch_monitor_.Init(conf["switch_monitor"])) {
        cout << "failed to init monitor\n";
        return false;
      }
      monitor_thread_.reset(new std::thread(MonitorUpdate, this));
    } catch (const std::exception &e) {
      cout << e.what() << '\n';
      return false;
    } catch (...) {
      cout << "unknown exception\n";
      return false;
    }

    return true;
  }

  const BufferPtr &get_buffer() const { return double_buffer_[cur_idx_]; }

 private:
  DoubleBuffer(const DoubleBuffer &rhs) = delete;
  DoubleBuffer &operator=(const DoubleBuffer &rhs) = delete;

  static void MonitorUpdate(DoubleBuffer *buf) {
    if (buf == nullptr) {
      cout << "buf is nullptr\n";
      return;
    }
    while (!buf->stop_monitor_) {
      sleep(buf->monitor_interval_);

      if (!buf->switch_monitor_.ShouldSwitchMonitor()) {
        continue;
      }

      uint32_t unused_idx = 1 - buf.cur_idx_;

      buf->double_buffer_[unused_idx] = nullptr;

      BufferPtr buffer = buf->loader_->load();
      if (!buffer) {
        buf->switch_monitor_.SwitchMonitor(false);
        cout << "buffer is nullptr\n";
        continue;
      }
      buf->double_buffer[unused_idx] = std::move(buffer);

      buf->cur_idx_ = unused_idx;

      buf->switch_monitor_.SwitchDone(true);

      if (buf->old_buf_life_time_ > 0) {
        sleep(buf->old_buf_file_time_);
        buf->double_buffer[1 - buf->cur_idx_] = nullptr;
      }
    }
  }
  SwitchMonitor switch_monitor_;
  BufferPtr double_buffer_[BUF_SIZE];
  LoaderPtr loader_;
  ThreadPtr monitor_thread_;
  std::atomic<uint32_t> cur_idx_;
  std::atomic<bool> stop_monitor_;
  uint32_t monitor_interval_;
  uint32_t old_buf_life_time_;
};

class DoubleBufferConfigureManager {
 public:
  static DoubleBufferConfigureManager &instance() {
    static DoubleBufferConfigureManager inst;
    return inst;
  }

  ~DoubleBufferConfigureManager() {}

  bool Init(const std::string &conf_name) {
    try {
      YAML::Node conf = YAML::LoadFile(conf_name);
      for (uint32_t i = 0; i < conf.size(); ++i) {
        table_.insert(
            std::make_pair(conf[i]["command_key"].as<std::string>(), conf[i]));
      }
      return true;
    } catch (const std::exception &e) {
      cout << e.what() << '\n';
      return false;
    } catch (...) {
      cout << "unknown exception\n";
      return false;
    }
  }

  const YAML::Node &GetConfigureNode(const std::string &key) const {
    const auto &it = table_.find(key);
    if (it == table_.end()) return nullptr_node_;

    return it->second;
  }

  std::string GetMonitorFile(const std::string &key) const {
    try {
      const auto &it = table_.find(key);
      if (it == table_.end()) {
        return NULLPTR_FILE;
      }
      return it->second["switch_monitor"]["update_file"].as<std::string>();
    } catch (const std::exception &e) {
      cout << e.what() << '\n';
      return NULLPTR_FILE;
    } catch (...) {
      cout << "unknown exception\n";
      return NULLPTR_FILE;
    }
  }

 private:
  DoubleBufferConfigureMananger() = default;
  typedef std::unordered_map<std::string, YAML::Node> ConfigTable;
  ConfigTable table_;

  YAML::Node nullptr_node_;
};

}  // namespace double_buffer
}  // namespace framework
