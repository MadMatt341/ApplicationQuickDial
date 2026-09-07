#include "BackgroundTasks.h"

#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace quickdial {

struct BackgroundTasks::State {
  std::mutex mutex;
  std::deque<Work> work;
  std::deque<Completion> completed;
  HWND window = nullptr;
  UINT message = 0;
  std::size_t limit = 0;
  std::size_t capacity = 0;
  std::size_t workers = 0;
  std::size_t outstanding = 0;
  bool stopped = false;
  bool failed = false;
};

BackgroundTasks::BackgroundTasks(HWND window, UINT message, std::size_t workers, std::size_t capacity)
    : state_(std::make_shared<State>()) {
  state_->window = window;
  state_->message = message;
  state_->limit = workers;
  state_->capacity = capacity;
}

BackgroundTasks::~BackgroundTasks() {
  Stop();
}

bool BackgroundTasks::Submit(Work work) {
  std::scoped_lock lock(state_->mutex);
  if (state_->stopped || state_->limit == 0 || state_->outstanding >= state_->capacity) {
    return false;
  }
  state_->work.emplace_back(std::move(work));
  ++state_->outstanding;
  if (state_->workers < state_->limit) {
    ++state_->workers;
    try {
      // The thread owns only shared queue state, never the queue or window owner.
      // Detaching closes its OS handle immediately; idle workers return naturally.
      std::thread(&BackgroundTasks::Run, state_).detach();
    } catch (...) {
      --state_->workers;
      --state_->outstanding;
      state_->work.pop_back();
      return false;
    }
  }
  return true;
}

void BackgroundTasks::Run(std::shared_ptr<State> state) {
  while (true) {
    Work work;
    {
      std::scoped_lock lock(state->mutex);
      if (state->stopped || state->work.empty()) {
        --state->workers;
        return;
      }
      work = std::move(state->work.front());
      state->work.pop_front();
    }

    Completion completion;
    bool failed = false;
    try {
      completion = work();
    } catch (...) {
      failed = true;
    }
    {
      std::scoped_lock lock(state->mutex);
      if (!state->stopped) {
        state->failed |= failed;
        if (completion) {
          state->completed.emplace_back(std::move(completion));
        } else {
          --state->outstanding;
        }
        // Stop uses this same gate, so no worker can notify a destroyed/reused HWND.
        // The owner's temporary timer also drains results if posting fails.
        if (state->window && !PostMessageW(state->window, state->message, 0, 0)) {
          continue;  // Retain the completion for the UI's fallback timer.
        }
      }
    }
  }
}

std::size_t BackgroundTasks::Dispatch() {
  std::deque<Completion> completed;
  {
    std::scoped_lock lock(state_->mutex);
    completed.swap(state_->completed);
    state_->outstanding -= completed.size();
  }
  std::size_t dispatched = 0;
  for (auto& completion : completed) {
    {
      std::scoped_lock lock(state_->mutex);
      if (state_->stopped) break;
    }
    completion();
    ++dispatched;
  }
  return dispatched;
}

bool BackgroundTasks::HasPending() const {
  std::scoped_lock lock(state_->mutex);
  return state_->outstanding != 0;
}

bool BackgroundTasks::TakeFailure() {
  std::scoped_lock lock(state_->mutex);
  return std::exchange(state_->failed, false);
}

void BackgroundTasks::Stop() {
  std::scoped_lock lock(state_->mutex);
  state_->stopped = true;
  state_->window = nullptr;
  state_->work.clear();
  state_->completed.clear();
  state_->outstanding = 0;
}

}  // namespace quickdial
