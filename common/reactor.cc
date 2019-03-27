/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "reactor.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "base/logging.h"

namespace {

// Use at most sizeof(epoll_event) * kEpollMaxEvents kernel memory
constexpr int kEpollMaxEvents = 64;

}  // namespace

namespace bluetooth {
namespace common {

class Reactor::Reactable {
 public:
  Reactable(int fd, Closure on_read_ready, Closure on_write_ready)
      : fd_(fd),
        on_read_ready_(std::move(on_read_ready)),
        on_write_ready_(std::move(on_write_ready)),
        is_executing_(false) {}
  const int fd_;
  Closure on_read_ready_;
  Closure on_write_ready_;
  bool is_executing_;
  std::recursive_mutex lock_;
};

Reactor::Reactor()
  : epoll_fd_(0),
    control_fd_(0),
    is_running_(false),
    reactable_removed_(false) {
  RUN_NO_INTR(epoll_fd_ = epoll_create1(EPOLL_CLOEXEC));
  CHECK_NE(epoll_fd_, -1) << __func__ << ": cannot create epoll_fd: " << strerror(errno);

  control_fd_ = eventfd(0, EFD_NONBLOCK);
  CHECK_NE(control_fd_, -1) << __func__ << ": cannot create control_fd: " << strerror(errno);

  epoll_event control_epoll_event = {EPOLLIN, {.ptr = nullptr}};
  int result;
  RUN_NO_INTR(result = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, control_fd_, &control_epoll_event));
  CHECK_NE(result, -1) << __func__ << ": cannot register control_fd: " << strerror(errno);
}

Reactor::~Reactor() {
  int result;
  RUN_NO_INTR(result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, control_fd_, nullptr));
  CHECK_NE(result, -1) << __func__ << ": cannot unregister control_fd: " << strerror(errno);

  RUN_NO_INTR(result = close(control_fd_));
  CHECK_NE(result, -1) << __func__ << ": cannot close control_fd: " << strerror(errno);

  RUN_NO_INTR(result = close(epoll_fd_));
  CHECK_NE(result, -1) << __func__ << ": cannot close epoll_fd: " << strerror(errno);
}

void Reactor::Run() {
  bool previously_running = is_running_.exchange(true);
  CHECK_EQ(previously_running, false) << __func__ << ": already running";
  LOG(INFO) << __func__ << ": started";

  for (;;) {
    invalidation_list_.clear();
    epoll_event events[kEpollMaxEvents];
    int count;
    RUN_NO_INTR(count = epoll_wait(epoll_fd_, events, kEpollMaxEvents, -1));
    CHECK_NE(count, -1) << __func__ << ": Error polling for fds: " << strerror(errno);

    for (int i = 0; i < count; ++i) {
      auto event = events[i];
      CHECK_NE(event.events, 0u) << __func__ << ": no result in epoll result";

      // If the ptr stored in epoll_event.data is nullptr, it means the control fd triggered
      if (event.data.ptr == nullptr) {
        uint64_t value;
        eventfd_read(control_fd_, &value);
        LOG(INFO) << __func__ << ": stopped";
        is_running_ = false;
        return;
      }
      auto* reactable = static_cast<Reactor::Reactable*>(event.data.ptr);
      {
        std::unique_lock<std::mutex> lock(mutex_);
        // See if this reactable has been removed in the meantime.
        if (std::find(invalidation_list_.begin(), invalidation_list_.end(), reactable) != invalidation_list_.end()) {
          continue;
        }

        std::lock_guard<std::recursive_mutex> reactable_lock(reactable->lock_);
        lock.unlock();
        reactable_removed_ = false;
        reactable->is_executing_ = true;
        if (event.events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR) && reactable->on_read_ready_ != nullptr) {
          reactable->on_read_ready_();
        }
        if (!reactable_removed_ && event.events & EPOLLOUT && reactable->on_write_ready_ != nullptr) {
          reactable->on_write_ready_();
        }
        reactable->is_executing_ = false;
      }
      if (reactable_removed_) {
        delete reactable;
      }
    }
  }
}

void Reactor::Stop() {
  if (!is_running_) {
    LOG(WARNING) << __func__ << ": not running, will stop once it's started";
  }
  auto control = eventfd_write(control_fd_, 1);
  CHECK_NE(control, -1) << __func__ << ": failed: " << strerror(errno);
}

Reactor::Reactable* Reactor::Register(int fd, Closure on_read_ready, Closure on_write_ready) {
  uint32_t poll_event_type = 0;
  if (on_read_ready != nullptr) {
    poll_event_type |= (EPOLLIN | EPOLLRDHUP);
  }
  if (on_write_ready != nullptr) {
    poll_event_type |= EPOLLOUT;
  }
  auto* reactable = new Reactable(fd, on_read_ready, on_write_ready);
  epoll_event event = {
      .events = poll_event_type,
      {.ptr = reactable}
  };
  int register_fd;
  RUN_NO_INTR(register_fd = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event));
  CHECK_NE(register_fd, -1) << __func__ << ": failed: " << strerror(errno);
  return reactable;
}

void Reactor::Unregister(Reactor::Reactable* reactable) {
  CHECK_NE(reactable, nullptr);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    invalidation_list_.push_back(reactable);
  }
  {
    int result;
    std::lock_guard<std::recursive_mutex> reactable_lock(reactable->lock_);
    RUN_NO_INTR(result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, reactable->fd_, nullptr));
    if (result == -1 && errno == ENOENT) {
      LOG(INFO) << __func__ << ": reactable is invalid or unregistered";
    } else if (result == -1) {
      PLOG(FATAL) << __func__ << ": failed";
    }
    // If we are unregistering during the callback event from this reactable, we delete it after the callback is executed.
    // reactable->is_executing_ is protected by reactable->lock_, so it's thread safe.
    if (reactable->is_executing_) {
      reactable_removed_ = true;
    }
  }
  // If we are unregistering outside of the callback event from this reactable, we delete it now
  if (!reactable_removed_) {
    delete reactable;
  }
}

void Reactor::ModifyRegistration(Reactor::Reactable* reactable, Closure on_read_ready, Closure on_write_ready) {
  CHECK_NE(reactable, nullptr);

  uint32_t poll_event_type = 0;
  if (on_read_ready != nullptr) {
    poll_event_type |= (EPOLLIN | EPOLLRDHUP);
  }
  if (on_write_ready != nullptr) {
    poll_event_type |= EPOLLOUT;
  }
  {
    std::lock_guard<std::recursive_mutex> reactable_lock(reactable->lock_);
    reactable->on_read_ready_ = std::move(on_read_ready);
    reactable->on_write_ready_ = std::move(on_write_ready);
  }
  epoll_event event = {
      .events = poll_event_type,
      {.ptr = reactable}
  };
  int modify_fd;
  RUN_NO_INTR(modify_fd = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, reactable->fd_, &event));
  CHECK_NE(modify_fd, -1) << __func__ << ": failed: " << strerror(errno);
}

}  // namespace common
}  // namespace bluetooth