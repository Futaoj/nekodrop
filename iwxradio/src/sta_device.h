// SPDX-License-Identifier: GPL-2.0-only
/*
 * What the Wi-Fi station (WifiStation, wifi_sta.h) needs from a radio: the
 * sta_* calls IwxDevice always had, as an interface, so another backend can
 * run the same join, WPA2/WPA3 and TAP bridge. IwxDevice itself is
 * unchanged (the kernel driver builds iwx.h): radio_main wraps it in
 * StaDeviceOf<IwxDevice>. The MediaTek radio (mtradio/) implements it with
 * host-side CCMP.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "radio_backend.h"

/* RxFrame::status bits WifiStation reads for a protected frame: decrypted,
 * MIC good (the Intel values, IWX_RX_MPDU_RES_STATUS_DEC_DONE and _MIC_OK).
 * A backend that decrypts on the host sets both for the frames it decrypted,
 * leaves the CCMP header in place and sets mic_stripped. */
constexpr uint16_t kStaRxMicOk = 1u << 6;
constexpr uint16_t kStaRxDecDone = 1u << 11;

class StaDevice {
public:
  virtual bool sta_join(const StaParams &p) = 0;
  virtual bool sta_associated(uint16_t aid, uint8_t dtim_period) = 0;
  /* What association agreed on (HT/VHT/width), before sta_associated. */
  virtual void sta_join_update(const StaParams &p) = 0;
  virtual bool sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) = 0;
  virtual bool sta_set_mfp(bool on) = 0;
  virtual bool sta_set_igtk(unsigned key_id, const uint8_t key[16], uint64_t rsc) = 0;
  /* Firmware rate control for the AP; afterwards sta_tlc() is true and
   * frames sent with TxRate::Auto use it. */
  virtual bool sta_rate_control() = 0;
  virtual bool sta_tlc() const = 0;
  /* `encrypt`: the keys are installed and the frame carries the protected
   * bit; the backend adds the CCMP header and MIC. */
  virtual int sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) = 0;
  virtual bool sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) = 0;
  virtual bool sta_remove_rx_ba(unsigned tid) = 0;
  /* Optional Station TX A-MPDU / Block Ack on TID 0 (gated by StaConfig::block_ack). */
  virtual bool sta_tx_agg_setup() { return false; }
  virtual bool sta_tx_agg_enable(bool /*on*/) { return false; }
  virtual bool sta_tx_agg_flush() { return false; }
  virtual bool sta_tx_agg_ready() const { return false; }
  virtual uint16_t sta_tx_agg_seq() const { return 0; }
  virtual unsigned sta_tx_agg_queued() const { return 0; }
  virtual unsigned sta_tx_agg_cap() const { return 0; }
  virtual int sta_transmit_agg(const uint8_t * /*frame*/, size_t /*len*/, bool /*encrypt*/, bool /*kick*/ = true) {
    return -1;
  }
  virtual void sta_kick_tx_agg() {}
  virtual void sta_leave() = 0;
  virtual const StaParams &sta_params() const = 0;
  /* Experimental Wi-Fi 6 station: the card and firmware can join as HE. */
  virtual bool sta_he_capable() const { return false; }
  /* Takes the associated link off the air (true) and back (false), so the
   * radio stays on AWDL's channel; sta_transmit fails meanwhile. False:
   * the backend cannot, or the firmware refused. */
  virtual bool sta_pause(bool) { return false; }

protected:
  ~StaDevice() = default;
};

/* A backend that already has the sta_* calls (IwxDevice), as a StaDevice. */
template <class Dev>
class StaDeviceOf final : public StaDevice {
public:
  explicit StaDeviceOf(Dev &dev) : _dev(dev) {}
  bool sta_join(const StaParams &p) override { return _dev.sta_join(p); }
  bool sta_associated(uint16_t aid, uint8_t dtim_period) override { return _dev.sta_associated(aid, dtim_period); }
  void sta_join_update(const StaParams &p) override { _dev.sta_join_update(p); }
  bool sta_set_key(bool group, unsigned key_index, const uint8_t key[16]) override {
    return _dev.sta_set_key(group, key_index, key);
  }
  bool sta_set_mfp(bool on) override { return _dev.sta_set_mfp(on); }
  bool sta_set_igtk(unsigned key_id, const uint8_t key[16], uint64_t rsc) override {
    return _dev.sta_set_igtk(key_id, key, rsc);
  }
  bool sta_rate_control() override { return _dev.sta_rate_control(); }
  bool sta_tlc() const override { return _dev.sta_tlc(); }
  int sta_transmit(const uint8_t *frame, size_t len, const TxRate &rate, bool encrypt) override {
    return _dev.sta_transmit(frame, len, rate, encrypt);
  }
  bool sta_add_rx_ba(unsigned tid, uint16_t ssn, uint16_t window) override {
    return _dev.sta_add_rx_ba(tid, ssn, window);
  }
  bool sta_remove_rx_ba(unsigned tid) override { return _dev.sta_remove_rx_ba(tid); }
  bool sta_tx_agg_setup() override { return _dev.sta_tx_agg_setup(); }
  bool sta_tx_agg_enable(bool on) override { return _dev.sta_tx_agg_enable(on); }
  bool sta_tx_agg_flush() override { return _dev.sta_tx_agg_flush(); }
  bool sta_tx_agg_ready() const override { return _dev.sta_tx_agg_ready(); }
  uint16_t sta_tx_agg_seq() const override { return _dev.sta_tx_agg_seq(); }
  unsigned sta_tx_agg_queued() const override { return _dev.sta_tx_agg_queued(); }
  unsigned sta_tx_agg_cap() const override { return _dev.sta_tx_agg_cap(); }
  int sta_transmit_agg(const uint8_t *frame, size_t len, bool encrypt, bool kick = true) override {
    return _dev.sta_transmit_agg(frame, len, encrypt, kick);
  }
  void sta_kick_tx_agg() override { _dev.sta_kick_tx_agg(); }
  void sta_leave() override { _dev.sta_leave(); }
  const StaParams &sta_params() const override { return _dev.sta_params(); }
  bool sta_he_capable() const override { return _dev.sta_he_capable(); }
  bool sta_pause(bool pause) override { return _dev.sta_pause(pause); }

private:
  Dev &_dev;
};
