// SPDX-License-Identifier: GPL-2.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>

#include <cstring>
#include <chrono>

#include "tap.h"

namespace {

/* tap-windows6's control codes (tap-windows.h). */
constexpr DWORD tap_ctl(DWORD n) { return CTL_CODE(FILE_DEVICE_UNKNOWN, n, METHOD_BUFFERED, FILE_ANY_ACCESS); }
const DWORD kTapGetMac = tap_ctl(1);
const DWORD kTapSetMediaStatus = tap_ctl(6);

const char kAdapterClass[] =
    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
const char kConnections[] =
    "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}";

std::string reg_string(HKEY key, const char *value) {
  char buf[256] = {};
  DWORD size = sizeof(buf) - 1, type = 0;
  if (RegQueryValueExA(key, value, nullptr, &type, reinterpret_cast<BYTE *>(buf), &size) != ERROR_SUCCESS ||
      type != REG_SZ)
    return {};
  return buf;
}

std::string connection_name(const std::string &guid) {
  HKEY key;
  const std::string path = std::string(kConnections) + "\\" + guid + "\\Connection";
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
  std::string name = reg_string(key, "Name");
  RegCloseKey(key);
  return name;
}

/* DeviceIoControl on the overlapped handle, waited for (Codex review #22:
 * a null OVERLAPPED on an overlapped handle is outside the API contract). */
bool ioctl_wait(HANDLE h, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len, DWORD *got) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  DWORD n = 0;
  bool ok = DeviceIoControl(h, code, in, in_len, out, out_len, &n, &ov) != 0;
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(h, &ov, &n, FALSE) != 0;
    } else {
      CancelIoEx(h, &ov);
      GetOverlappedResult(h, &ov, &n, TRUE);
      ok = false;
    }
  }
  CloseHandle(ov.hEvent);
  if (got) *got = n;
  return ok;
}

} // namespace

TapAdapter::~TapAdapter() {
  if (_handle) {
    {
      std::lock_guard<std::mutex> lock(_write_mutex);
      _write_stop = true;
    }
    _write_cv.notify_all();
    CancelIoEx(_handle, nullptr);
    for (auto &writer : _writers) {
      if (writer.joinable()) writer.join();
    }
    /* The read may have been started by another thread: cancel it by its
     * OVERLAPPED and wait before its buffer and event go (Codex review #21). */
    if (_read_pending) {
      auto *ov = reinterpret_cast<OVERLAPPED *>(_read_ov);
      DWORD n = 0;
      CancelIoEx(_handle, ov);
      GetOverlappedResult(_handle, ov, &n, TRUE);
      _read_pending = false;
    }
    CloseHandle(_handle);
  }
  if (_read_event) CloseHandle(_read_event);
  for (auto &ctx : _write_ctx) ctx.op.reset();
}

bool TapAdapter::open(const std::string &wanted, std::string &error) {
  HKEY cls;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kAdapterClass, 0, KEY_READ, &cls) != ERROR_SUCCESS) {
    error = "cannot read the network adapter class";
    return false;
  }
  /* Try every matching tap0901 entry: a removed phantom adapter can leave a
   * stale registry entry ahead of a live one and fail with ERROR_FILE_NOT_FOUND
   * (issue #20). */
  HANDLE h = INVALID_HANDLE_VALUE;
  std::string last_path;
  DWORD last_err = 0;
  for (DWORD i = 0;; i++) {
    char sub[256];
    DWORD len = sizeof(sub);
    if (RegEnumKeyExA(cls, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY key;
    if (RegOpenKeyExA(cls, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
    const std::string component = reg_string(key, "ComponentId");
    const std::string id = reg_string(key, "NetCfgInstanceId");
    RegCloseKey(key);
    if (component != "tap0901" || id.empty()) continue;
    const std::string name = connection_name(id);
    if (!wanted.empty() && wanted != id && wanted != name) continue;
    const std::string path = "\\\\.\\Global\\" + id + ".tap";
    HANDLE cand = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, nullptr);
    if (cand != INVALID_HANDLE_VALUE) {
      h = cand;
      _name = name;
      break;
    }
    last_path = path;
    last_err = GetLastError();
  }
  RegCloseKey(cls);
  if (h == INVALID_HANDLE_VALUE) {
    if (last_path.empty()) {
      error = "no TAP-Windows (tap0901) adapter found: install tap-windows6";
    } else if (last_err == ERROR_FILE_NOT_FOUND) {
      error = "cannot open " + last_path +
              " (error 2: TAP adapter is not present; reinstall nekodrop to recreate it)";
    } else {
      error = "cannot open " + last_path + " (error " + std::to_string(last_err) +
              "): is another program using the TAP adapter?";
    }
    return false;
  }
  DWORD got = 0;
  if (!ioctl_wait(h, kTapGetMac, _mac.data(), 6, _mac.data(), 6, &got) || got != 6) {
    CloseHandle(h);
    error = "the TAP adapter did not give its MAC address";
    return false;
  }
  _handle = h;
  _read_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  for (unsigned i = 0; i < kWriterThreads; i++) {
    auto op = std::make_unique<PendingOp>();
    op->event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    op->buf.reserve(2048);
    _write_ctx[i].op = std::move(op);
    if (i < _write_threads) _writers[i] = std::thread(&TapAdapter::write_loop, this, i);
  }
  _read_buf.resize(2048);
  _write_buf.reserve(2048);
  return true;
}

bool TapAdapter::set_connected(bool up) {
  ULONG status = up ? 1 : 0;
  DWORD got = 0;
  return _handle && ioctl_wait(_handle, kTapSetMediaStatus, &status, sizeof(status), &status,
                               sizeof(status), &got);
}

bool TapAdapter::read(std::vector<uint8_t> &frame, unsigned timeout_ms) {
  frame.clear();
  if (!_handle) return false;
  auto *ov = reinterpret_cast<OVERLAPPED *>(_read_ov);
  static_assert(sizeof(OVERLAPPED) <= sizeof(_read_ov), "overlapped storage");
  if (!_read_pending) {
    std::memset(ov, 0, sizeof(*ov));
    ov->hEvent = _read_event;
    ResetEvent(_read_event);
    DWORD got = 0;
    if (ReadFile(_handle, _read_buf.data(), static_cast<DWORD>(_read_buf.size()), &got, ov)) {
      frame.assign(_read_buf.begin(), _read_buf.begin() + got);
      return true;
    }
    if (GetLastError() != ERROR_IO_PENDING) return false;
    _read_pending = true;
  }
  if (WaitForSingleObject(_read_event, timeout_ms) != WAIT_OBJECT_0) return true;
  DWORD got = 0;
  _read_pending = false;
  if (!GetOverlappedResult(_handle, ov, &got, FALSE)) return false;
  frame.assign(_read_buf.begin(), _read_buf.begin() + got);
  return true;
}

bool TapAdapter::write(const uint8_t *frame, size_t len) {
  if (!_handle) return false;
  _write_buf.assign(frame, frame + len);
  return _write_threads ? enqueue_buffered() : write_direct();
}

bool TapAdapter::write_eth(const uint8_t *da, const uint8_t *sa, uint16_t ethertype,
                           const uint8_t *payload, size_t len) {
  if (!_handle) return false;
  _write_buf.resize(14 + len);
  std::memcpy(_write_buf.data(), da, 6);
  std::memcpy(_write_buf.data() + 6, sa, 6);
  _write_buf[12] = static_cast<uint8_t>(ethertype >> 8);
  _write_buf[13] = static_cast<uint8_t>(ethertype);
  if (len) std::memcpy(_write_buf.data() + 14, payload, len);
  return _write_threads ? enqueue_buffered() : write_direct();
}

bool TapAdapter::write_direct() {
  std::vector<uint8_t> tmp;
  tmp.swap(_write_buf);
  const bool ok = write_one(std::move(tmp), _write_ctx[0]);
  return ok;
}

bool TapAdapter::enqueue_buffered() {
  if (!_handle) return false;
  {
    std::lock_guard<std::mutex> lock(_write_mutex);
    if (_write_stop) return false;
    if (_write_queue.size() >= kMaxQueue) {
      _stat_drops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _write_queue.emplace_back(_write_buf);
    const uint64_t qsz = static_cast<uint64_t>(_write_queue.size());
    uint64_t prev = _stat_queued_max.load(std::memory_order_relaxed);
    while (qsz > prev && !_stat_queued_max.compare_exchange_weak(prev, qsz, std::memory_order_relaxed)) {
    }
  }
  _write_cv.notify_one();
  return true;
}

void TapAdapter::write_loop(unsigned index) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  WriteCtx &ctx = _write_ctx[index];
  for (;;) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock<std::mutex> lock(_write_mutex);
      _write_cv.wait(lock, [this] { return _write_stop || !_write_queue.empty(); });
      if (_write_stop && _write_queue.empty()) break;
      frame = std::move(_write_queue.front());
      _write_queue.pop_front();
    }
    write_one(std::move(frame), ctx);
  }
}

bool TapAdapter::write_one(std::vector<uint8_t> &&frame, WriteCtx &ctx) {
  if (!ctx.op) {
    ctx.op = std::make_unique<PendingOp>();
    ctx.op->event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  }
  PendingOp *op = ctx.op.get();
  if (!op || !op->event) return false;
  const auto started = std::chrono::steady_clock::now();
  op->buf.swap(frame);
  std::memset(&op->ov, 0, sizeof(op->ov));
  op->ov.hEvent = op->event;
  ResetEvent(op->event);
  DWORD put = 0;
  bool ok = false;
  bool leaked = false;
  if (WriteFile(_handle, op->buf.data(), static_cast<DWORD>(op->buf.size()), &put, &op->ov)) {
    ok = true;
  } else if (GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(op->event, 500) == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(_handle, &op->ov, &put, FALSE) != 0;
    } else {
      /* Bounded cancel wait (Codex review #8 / round 2 #14): if the kernel
       * still holds the IRP after 1 s, detach and leak op so we neither UAF
       * nor deadlock shutdown. */
      CancelIoEx(_handle, &op->ov);
      if (WaitForSingleObject(op->event, 1000) == WAIT_OBJECT_0) {
        GetOverlappedResult(_handle, &op->ov, &put, FALSE);
      } else {
        ctx.op.release();
        leaked = true;
      }
    }
  }
  if (!leaked) frame.swap(op->buf);
  const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count());
  _stat_writes.fetch_add(1, std::memory_order_relaxed);
  _stat_total_us.fetch_add(us, std::memory_order_relaxed);
  uint64_t prev_max = _stat_max_us.load(std::memory_order_relaxed);
  while (us > prev_max && !_stat_max_us.compare_exchange_weak(prev_max, us, std::memory_order_relaxed)) {
  }
  if (!ok) _stat_failed.fetch_add(1, std::memory_order_relaxed);
  return ok;
}
