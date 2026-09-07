#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>

namespace quickdial {

// Work runs on bounded background threads. Only Dispatch invokes completions,
// on the caller's UI thread. Stop discards queued work and all late completions.
class BackgroundTasks {
 public:
  using Completion = std::function<void()>;
  using Work = std::function<Completion()>;

  BackgroundTasks(HWND window, UINT message, std::size_t workers, std::size_t capacity);
  ~BackgroundTasks();
  BackgroundTasks(const BackgroundTasks&) = delete;
  BackgroundTasks& operator=(const BackgroundTasks&) = delete;

  bool Submit(Work work);
  std::size_t Dispatch();
  bool HasPending() const;
  bool TakeFailure();
  void Stop();

 private:
  struct State;
  static void Run(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

}  // namespace quickdial
