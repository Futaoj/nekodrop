/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows replacement for OWL's daemon/io.c. The "WLAN device" is the radio
 * daemon reached over its pipe, and the host device is a Wintun adapter. */
#include "io.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>
#include <errno.h>
#include <string.h>

#include <ev.h>

#include "frame_fixup.h"
#include "host_frame.h"
#include "host_if.h"
#include "log.h"
#include "netutils.h"
#include "radio_client.h"
#include "radio_link.h"
#include "service_bridge.h"
#include "tcp_merge.h"
#include "win_internal.h"

/* core.c hands io_state.wlan_handle to pcap_dispatch, so that is where the
 * radio connection lives. */
struct pcap {
	struct radio_client *radio;
};

static struct pcap wlan;
/* NULL when the adapter could not be created; data frames are then dropped. */
static struct host_if *host;

struct radio_client *winawdl_radio(void) {
	return wlan.radio;
}

/* However long frames keep coming, joined data reaches the host this soon. */
#define JOIN_MAX_HOLD_US 3000
#define RX_BATCH_BUDGET_US 1000

static struct {
	int wait_us; /* 0: segments are not joined */
	uint64_t since_us;
	uint64_t deadline_us;
	ev_timer delivery;
	unsigned long long joined;
} joining;
static struct tcp_merge merge;

unsigned long long winawdl_segments_joined(void) {
	return joining.joined;
}

static uint64_t now_us(void) {
	static LARGE_INTEGER frequency;
	LARGE_INTEGER ticks;
	if (frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&ticks);
	return (uint64_t) (ticks.QuadPart / frequency.QuadPart) * 1000000u +
	       (uint64_t) (ticks.QuadPart % frequency.QuadPart) * 1000000u /
		       (uint64_t) frequency.QuadPart;
}

static void deliver_joined(void) {
	static uint8_t packet[TCP_MERGE_MAX_PACKET];
	if (joining.delivery.active)
		ev_timer_stop(ev_default_loop(), &joining.delivery);
	int length = tcp_merge_take(&merge, packet);
	if (length && host && host_if_write(host, packet, length) < 0) {
		static unsigned long long dropped;
		dropped++;
		if ((dropped & (dropped - 1)) == 0)
			log_warn("Wintun rejected joined TCP packets: %llu", dropped);
	}
}

static void join_timeout(struct ev_loop *loop, ev_timer *timer, int revents) {
	(void) loop;
	(void) timer;
	(void) revents;
	deliver_joined();
}

void winawdl_join_segments(int wait_us) {
	joining.wait_us = wait_us > 0 ? wait_us : 0;
	ev_timer_init(&joining.delivery, join_timeout, 0., 0.);
}

static void on_radio_disconnect(void) {
	ev_win_stop(ev_default_loop());
}

/* A fresh locally administered unicast address per run, as Apple devices use
 * for AWDL. */
static int random_ether_addr(struct ether_addr *addr) {
	if (BCryptGenRandom(NULL, addr->ether_addr_octet, ETHER_ADDR_LEN,
			    BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return -EIO;
	addr->ether_addr_octet[0] = (uint8_t) ((addr->ether_addr_octet[0] | 0x02) & 0xFE);
	return 0;
}

static int open_host_interface(struct io_state *state) {
	struct in6_addr address;
	char error[128];
	rfc4291_addr(&state->if_ether_addr, &address);
	host = host_if_open(address.s6_addr, error, sizeof(error));
	if (!host) {
		log_error("cannot open the AWDL host interface: %s", error);
		return -ENODEV;
	}
	state->host_fd = ev_win_fd_for_handle(host_if_read_event(host));
	state->host_ifindex = (int) host_if_index(host);
	strncpy(state->host_ifname, HOST_IF_NAME, sizeof(state->host_ifname) - 1);
	return 0;
}

int io_state_init(struct io_state *state, const char *wlan_name, const char *host_name,
		  const struct ether_addr *bssid_filter) {
	memset(state, 0, sizeof(*state));

	wlan.radio = radio_client_open(on_radio_disconnect);
	if (!wlan.radio)
		return -ENODEV;
	state->wlan_handle = &wlan;
	state->wlan_fd = ev_win_fd_for_handle(radio_client_rx_event(wlan.radio));
	state->wlan_ifindex = 1;
	strncpy(state->wlan_ifname, wlan_name, sizeof(state->wlan_ifname) - 1);

	if (radio_client_send(wlan.radio, RADIO_LINK_SET_BSSID_FILTER,
			      bssid_filter->ether_addr_octet, ETHER_ADDR_LEN) < 0)
		return -EIO;
	if (random_ether_addr(&state->if_ether_addr) < 0 ||
	    radio_client_send(wlan.radio, RADIO_LINK_SET_ADDRESS,
			      state->if_ether_addr.ether_addr_octet, ETHER_ADDR_LEN) < 0)
		return -EIO;
	if (host_name[0])
		return open_host_interface(state);
	return 0;
}

void io_state_free(struct io_state *state) {
	if (merge.length)
		deliver_joined();
	host_if_close(host);
	host = NULL;
	radio_client_close(wlan.radio);
	wlan.radio = NULL;
	state->wlan_handle = NULL;
}

int pcap_dispatch(pcap_t *handle, int count, pcap_handler callback, unsigned char *user) {
	static uint8_t frame[RADIO_CLIENT_MAX_FRAME];
	static uint8_t announced[SERVICE_BRIDGE_MAX_PACKET];
	const uint64_t deadline = now_us() + RX_BATCH_BUDGET_US;
	int handled = 0;
	do {
		int length = radio_client_pop(handle->radio, frame);
		if (length == 0)
			break;
		length = frame_fixup_received(frame, length);
		int announced_len = service_bridge_packet(frame, length, announced);
		if (announced_len && host)
			host_if_write(host, announced, announced_len);
		struct pcap_pkthdr header = {.caplen = (uint32_t) length, .len = (uint32_t) length};
		callback(user, &header, frame);
		handled++;
	} while ((count <= 0 || handled < count) && now_us() < deadline);
	return handled;
}

int wlan_send(const struct io_state *state, const uint8_t *buf, int len) {
	static uint8_t frame[RADIO_CLIENT_MAX_FRAME];
	if (!state || !state->wlan_handle || len < 0 || len > (int) sizeof(frame))
		return -EINVAL;
	memcpy(frame, buf, (size_t) len);
	len = frame_fixup_sent(frame, len, sizeof(frame));
	return radio_client_send(state->wlan_handle->radio, RADIO_LINK_TX, frame, (uint32_t) len);
}

int host_send(const struct io_state *state, const uint8_t *buf, int len) {
	(void) state;
	int packet_len = 0;
	const uint8_t *packet = host_frame_ip6(buf, len, &packet_len);
	if (!host || !packet)
		return 0;
	if (joining.wait_us) {
		uint64_t now = now_us();
		/* Timer callbacks run between RX batches. Enforce the current
		 * deadline here as well if an individual batch spans it. */
		if (merge.length && now >= joining.deadline_us)
			deliver_joined();
		int continued = merge.length != 0;
		if (!tcp_merge_offer(&merge, packet, packet_len)) {
			/* What is held was received first and goes first. */
			deliver_joined();
			continued = 0;
			if (!tcp_merge_offer(&merge, packet, packet_len))
				return host_if_write(host, packet, packet_len) < 0 ? -ENOBUFS : 0;
		}
		if (continued)
			joining.joined++;
		else
			joining.since_us = now;
		if (merge.closed) {
			deliver_joined();
		} else {
			uint64_t quiet = now + (uint64_t) joining.wait_us;
			uint64_t latest = joining.since_us + JOIN_MAX_HOLD_US;
			uint64_t due = quiet < latest ? quiet : latest;
			joining.deadline_us = due;
			/* A true one-shot on the AWDL event loop also fires when no more
			 * radio packets arrive. Only TCP segments extend the quiet time. */
			ev_timer_set(&joining.delivery, (due - now) / 1e6, 0.);
			ev_timer_start(ev_default_loop(), &joining.delivery);
		}
		return 0;
	}
	return host_if_write(host, packet, packet_len) < 0 ? -ENOBUFS : 0;
}

int host_recv(const struct io_state *state, uint8_t *buf, int *len) {
	static uint8_t packet[HOST_IF_MAX_PACKET];
	if (!host)
		return -EWOULDBLOCK;
	/* Skip what AWDL cannot carry instead of reporting it: the read event
	 * fires again only once the adapter's queue has been seen empty. */
	for (;;) {
		int packet_len = host_if_read(host, packet);
		if (packet_len == 0)
			return -EWOULDBLOCK;
		int frame_len = host_frame_from_ip6(
			packet, packet_len, state->if_ether_addr.ether_addr_octet, buf, *len);
		if (frame_len > 0) {
			*len = frame_len;
			return 0;
		}
	}
}
