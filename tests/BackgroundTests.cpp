#include "BackgroundTasks.h"
#include "IconLoader.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

int failures = 0;
void Check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << description << '\n';
  }
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct Gate {
  HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Gate() { if (handle) CloseHandle(handle); }
};

void TestBoundedWorkers() {
  auto gate = std::make_shared<Gate>();
  Check(gate->handle != nullptr, "worker gate can be created");
  if (!gate->handle) return;
  quickdial::BackgroundTasks tasks(nullptr, 0, 2, 4);
  std::atomic_int started = 0;
  int completed = 0;
  const DWORD owner = GetCurrentThreadId();
  for (int index = 0; index < 4; ++index) {
    Check(tasks.Submit([gate, &started, &completed, owner] {
      ++started;
      WaitForSingleObject(gate->handle, 5000);
      return [&completed, owner] {
        Check(GetCurrentThreadId() == owner, "completions run on the dispatching thread");
        ++completed;
      };
    }), "queue accepts work up to its capacity");
  }
  Check(WaitUntil([&] { return started == 2; }), "two workers start while the remaining work stays queued");
  Check(!tasks.Submit([] { return [] {}; }), "capacity includes active and queued work");
  Check(completed == 0, "workers never invoke UI completions themselves");
  SetEvent(gate->handle);
  Check(WaitUntil([&] { tasks.Dispatch(); return !tasks.HasPending(); }), "all accepted jobs complete");
  Check(completed == 4 && started == 4, "each accepted job completes exactly once");
}

void TestStopDuringBlockedWork() {
  auto gate = std::make_shared<Gate>();
  if (!gate->handle) { Check(false, "shutdown gate can be created"); return; }
  auto started = std::make_shared<std::atomic_bool>(false);
  auto returned = std::make_shared<std::atomic_bool>(false);
  auto lateCallback = std::make_shared<std::atomic_bool>(false);
  auto queuedRan = std::make_shared<std::atomic_bool>(false);
  auto tasks = std::make_unique<quickdial::BackgroundTasks>(nullptr, 0, 1, 2);
  Check(tasks->Submit([gate, started, returned, lateCallback] {
    *started = true;
    WaitForSingleObject(gate->handle, 5000);
    *returned = true;
    return [lateCallback] { *lateCallback = true; };
  }), "blocked work is accepted");
  Check(tasks->Submit([queuedRan] {
    *queuedRan = true;
    return [] {};
  }), "work can queue behind a stalled job");
  Check(WaitUntil([&] { return started->load(); }), "blocked job starts");
  const auto start = std::chrono::steady_clock::now();
  tasks.reset();
  Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
        "destroying a queue does not wait for a blocked shell operation");
  SetEvent(gate->handle);
  Check(WaitUntil([&] { return returned->load(); }), "a late worker returns safely after its owner is destroyed");
  Check(!*lateCallback && !*queuedRan, "shutdown discards pending work and late UI callbacks");
}

void TestWorkerFailuresAndHandles() {
  quickdial::BackgroundTasks tasks(nullptr, 0, 2, 8);
  Check(tasks.Submit([]() -> quickdial::BackgroundTasks::Completion {
    throw std::runtime_error("simulated provider failure");
  }), "throwing job is accepted");
  Check(WaitUntil([&] { return !tasks.HasPending(); }), "failed work releases its queue slot");
  Check(tasks.TakeFailure(), "worker exceptions are reported to the owner");
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial process handles can be sampled");
  int completed = 0;
  for (int index = 0; index < 200; ++index) {
    Check(tasks.Submit([&completed] { return [&completed] { ++completed; }; }), "stress job is accepted");
    Check(WaitUntil([&] { tasks.Dispatch(); return completed > index; }), "stress job completes");
  }
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "final process handles can be sampled");
  Check(after <= before + 4, "completed worker jobs do not accumulate thread handles");
  tasks.Stop();
  Check(!tasks.Submit([] { return [] {}; }), "stopped queue rejects new work");
}

void WriteBitmap(const std::filesystem::path& path, LONG width, LONG height) {
  BITMAPFILEHEADER header{};
  BITMAPINFOHEADER info{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(info);
  header.bfSize = header.bfOffBits + static_cast<DWORD>(width * height * 4);
  info.biSize = sizeof(info);
  info.biWidth = width;
  info.biHeight = height;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
  stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
  const std::vector<std::uint32_t> row(static_cast<std::size_t>(width), 0xff336699);
  for (LONG index = 0; index < height; ++index) {
    stream.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * 4));
  }
  Check(static_cast<bool>(stream), "custom icon fixture is written");
}

struct IconFrame {
  UINT size;
  std::vector<std::uint32_t> pixels;
};

void WriteIcon(const std::filesystem::path& path, const std::vector<IconFrame>& frames) {
  std::ofstream stream(path, std::ios::binary);
  const auto write = [&stream](const auto& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  write(WORD{0});
  write(WORD{1});
  write(static_cast<WORD>(frames.size()));
  DWORD offset = 6 + 16 * static_cast<DWORD>(frames.size());
  for (const auto& frame : frames) {
    const DWORD bytes = sizeof(BITMAPINFOHEADER) + frame.size * frame.size * 4 +
                        ((frame.size + 31) / 32) * 4 * frame.size;
    write(static_cast<BYTE>(frame.size));
    write(static_cast<BYTE>(frame.size));
    write(WORD{0});
    write(WORD{1});
    write(WORD{32});
    write(bytes);
    write(offset);
    offset += bytes;
  }
  for (const auto& frame : frames) {
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(frame.size);
    info.biHeight = static_cast<LONG>(frame.size * 2);
    info.biPlanes = 1;
    info.biBitCount = 32;
    write(info);
    stream.write(reinterpret_cast<const char*>(frame.pixels.data()), frame.pixels.size() * 4);
    const std::vector<char> mask(((frame.size + 31) / 32) * 4 * frame.size, 0);
    stream.write(mask.data(), mask.size());
  }
  Check(static_cast<bool>(stream), "multi-resolution icon fixture is written");
}

bool SolidColor(const quickdial::IconImage& image, std::uint32_t color) {
  if (image.pixels.empty()) return false;
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    if (image.pixels[index] != static_cast<std::uint8_t>(color >> (8 * (index % 4)))) return false;
  }
  return true;
}

void TestIconScaling() {
  const auto directory = std::filesystem::temp_directory_path() /
      (L"QuickDialIconTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(directory);
  const auto bitmap = directory / L"large.bmp";
  WriteBitmap(bitmap, 2048, 1024);
  auto image = quickdial::LoadApplicationIcon(L"", bitmap.wstring(), 32);
  Check(image.width == 32 && image.height == 16 && image.pixels.size() == 2048,
        "large custom icons are downscaled before returning pixels to the UI");
  image = quickdial::LoadApplicationIcon(L"", bitmap.wstring(), 512);
  Check(image.width == 96 && image.height == 48 && image.pixels.size() == 18432,
        "icon requests are capped even at excessive display sizes");
  image = quickdial::LoadApplicationIcon(L"", bitmap.wstring(), 41);
  Check(image.width == 41 && image.height == 21,
        "aspect ratio rounding retains the requested longest edge at fractional ratios");

  const auto icon = directory / L"resolutions.ico";
  WriteIcon(icon, {{16, std::vector<std::uint32_t>(16 * 16, 0xffff0000)},
                   {64, std::vector<std::uint32_t>(64 * 64, 0xff0000ff)},
                   {32, std::vector<std::uint32_t>(32 * 32, 0xff00ff00)}});
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 28);
  Check(image.width == 28 && image.height == 28 && SolidColor(image, 0xff00ff00),
        "ICO decoding selects the smallest sufficient resolution despite unordered frames");
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 32);
  Check(image.width == 32 && SolidColor(image, 0xff00ff00),
        "an exact-size ICO frame is preserved without resampling");
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 40);
  Check(image.width == 40 && SolidColor(image, 0xff0000ff),
        "higher DPI requests select the larger ICO frame before downscaling");
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 96);
  Check(image.width == 64 && SolidColor(image, 0xff0000ff),
        "the largest ICO frame is retained when every frame is smaller than requested");
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 16);
  Check(image.width == 16 && SolidColor(image, 0xffff0000),
        "small requests can still select the native small ICO frame");

  WriteIcon(icon, {{2, {0x00ff0000, 0xff00ff00, 0x00ff0000, 0xff00ff00}}});
  image = quickdial::LoadApplicationIcon(L"", icon.wstring(), 1);
  Check(image.width == 1 && image.height == 1 && image.pixels[0] == 0 && image.pixels[2] == 0 &&
            image.pixels[1] >= 127 && image.pixels[1] <= 128 && image.pixels[3] == image.pixels[1],
        "downsampling premultiplies alpha before filtering to avoid transparent color fringes");

  std::vector<std::uint32_t> shellPixels(32 * 32, 0);
  for (UINT y = 4; y < 28; ++y) {
    for (UINT x = 4; x < 28; ++x) {
      shellPixels[y * 32 + x] = (x >= 8 && x < 24 && y >= 8 && y < 24) ? 0xff00ff00 : 0x8000ff00;
    }
  }
  WriteIcon(icon, {{32, std::move(shellPixels)}});
  for (const UINT size : {32U, 28U}) {
    // Use the ICO as the target, not as the custom image: this exercises the
    // actual Shell HBITMAP import, including its straight-alpha edge pixels.
    image = quickdial::LoadApplicationIcon(icon.wstring(), std::nullopt, size);
    bool premultiplied = !image.pixels.empty();
    bool partialAlpha = false;
    for (std::size_t pixel = 0; pixel < image.pixels.size(); pixel += 4) {
      const auto alpha = image.pixels[pixel + 3];
      partialAlpha |= alpha > 0 && alpha < 255;
      premultiplied &= std::max({image.pixels[pixel], image.pixels[pixel + 1], image.pixels[pixel + 2]}) <= alpha;
    }
    Check(partialAlpha, "shell icon fixture retains partially transparent edges");
    Check(premultiplied, "shell icons premultiply edge colors before native-size display or downscaling");
  }

  wchar_t executable[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  Check(length != 0 && length < MAX_PATH, "shell icon test resolves its executable");
  if (length != 0 && length < MAX_PATH) {
    image = quickdial::LoadApplicationIcon(executable, (directory / L"missing.png").wstring(), 28);
    Check(image.width > 0 && image.height > 0 && image.width <= 28 && image.height <= 28,
          "an invalid custom image falls back to a bounded executable shell icon");
  }
  WriteBitmap(bitmap, 8193, 1);
  image = quickdial::LoadApplicationIcon(L"", bitmap.wstring(), 32);
  Check(image.pixels.empty(), "unreasonable source dimensions are rejected");
  std::filesystem::remove(icon);
  std::filesystem::remove(bitmap);
  std::filesystem::remove(directory);
}

}  // namespace

int main() {
  TestBoundedWorkers();
  TestStopDuringBlockedWork();
  TestWorkerFailuresAndHandles();
  TestIconScaling();
  if (failures == 0) std::cout << "All quickdial background tests passed.\n";
  return failures == 0 ? 0 : 1;
}
