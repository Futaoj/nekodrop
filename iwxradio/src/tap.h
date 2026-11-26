// SPDX-License-Identifier: GPL-2.0-only
/* A TAP-Windows (tap0901) Ethernet adapter: how the station's traffic reaches
 * Windows. Windows runs DHCP, ARP and IPv6 on it as on any wired adapter; the
 * station uses the adapter's MAC address as its own on the air, so nothing in
 * the frames needs rewriting. */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TapAdapter {
public:
  ~TapAdapter();
  /* Opens the first tap0901 adapter, or the one whose name or GUID is given. */
  bool open(const std::string &wanted, std::string &error);
  /* 0: write on the caller (station RX) thread, the original path. 2: queue
   * and let two writer threads do the blocking TAP WriteFile calls. */
  void set_write_threads(unsigned threads) {
    _write_threads = threads > kWriterThreads ? kWriterThreads : threads;
  }
  bool is_open() const { return _handle != nullptr; }
  const std::array<uint8_t, 6> &mac() const { return _mac; }
  const std::string &name() const { return _name; }
  /* Marks the cable plugged in or out, which is what starts Windows' DHCP. */
  bool set_connected(bool up);
  /* Blocks up to `timeout_ms` for one Ethernet frame. Empty on timeout. */
  bool read(std::vector<uint8_t> &frame, unsigned timeout_ms);
  bool write(const uint8_t *frame, size_t len);
  /* Build the Ethernet header in the reusable write buffer and enqueue it.
   * A dedicated writer thread does the blocking TAP WriteFile call, so the
   * radio's receive thread never waits 40+ us per frame for the TAP driver. */
  bool write_eth(const uint8_t *da, const uint8_t *sa, uint16_t ethertype,
                 const uint8_t *payload, size_t len);
  struct WriteStats {
    uint64_t writes = 0, total_us = 0, max_us = 0, queued_max = 0, drops = 0, failed = 0;
  };
  const WriteStats &write_stats() const { return _write_stats; }

private:
  static constexpr size_t kMaxQueue = 4096;
  static constexpr unsigned kWriterThreads = 2;
  struct WriteCtx {
    alignas(8) unsigned char ov[64] = {};
    HANDLE event = nullptr;
  };
  bool enqueue_buffered();
  bool write_direct();
  void write_loop(unsigned index);
  bool write_one(const std::vector<uint8_t> &frame, WriteCtx &ctx);
  void *_handle = nullptr;
  void *_read_event = nullptr;
  bool _read_pending = false;
  std::vector<uint8_t> _read_buf;
  std::vector<uint8_t> _write_buf;
  std::array<uint8_t, 6> _mac{};
  std::string _name;
  alignas(8) unsigned char _read_ov[64] = {};
  unsigned _write_threads = 0;

  std::array<std::thread, kWriterThreads> _writers;
  std::array<WriteCtx, kWriterThreads> _write_ctx{};
  std::mutex _write_mutex;
  std::condition_variable _write_cv;
  std::deque<std::vector<uint8_t>> _write_queue;
  bool _write_stop = false;
  WriteStats _write_stats;
};




