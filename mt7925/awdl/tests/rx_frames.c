/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio_client.h"
#include "raw_rx.h"
#include "rx.h"
#include "wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "RX check failed at line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static const struct ether_addr sender = {{2, 1, 2, 3, 4, 5}};
static const struct ether_addr receiver = {{2, 6, 7, 8, 9, 10}};
static const uint8_t llc_awdl[16] = {0xaa, 0xaa, 3, 0, 0, 0, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd};
static uint8_t dma_packet[16383], record[8 + RADIO_CLIENT_MAX_FRAME];

static void check_receive(struct awdl_state *state, int count, int ip_bytes, int aggregate,
			  int format, int ht_control, int output_capacity) {
	/* An RXD with no optional groups, followed by the three-address QoS header. */
	memset(dma_packet, 0, sizeof(dma_packet));
	uint8_t *mac = dma_packet + 32;
	mac[0] = 0x88;
	mac[1] = ht_control ? 0x80 : 0;
	memcpy(mac + 4, &receiver, 6);
	memcpy(mac + 10, &sender, 6);
	memcpy(mac + 16, &AWDL_BSSID, 6);
	mac[24] = aggregate ? 0x80 : 0;
	size_t pos = 32 + 26 + (ht_control ? 4 : 0) + (format ? 2 : 0);
	for (int i = 0; i < count; i++) {
		size_t subframe = pos;
		CHECK(pos + 14 + sizeof(llc_awdl) + ip_bytes + 3 <= sizeof(dma_packet));
		if (aggregate) {
			memcpy(dma_packet + pos, &receiver, 6);
			memcpy(dma_packet + pos + 6, &sender, 6);
			unsigned len = (unsigned)sizeof(llc_awdl) + ip_bytes;
			dma_packet[pos + 12] = (uint8_t)(len >> 8);
			dma_packet[pos + 13] = (uint8_t)len;
			pos += 14;
		}
		memcpy(dma_packet + pos, llc_awdl, sizeof(llc_awdl));
		pos += sizeof(llc_awdl);
		memset(dma_packet + pos, 0, 40);
		dma_packet[pos] = 0x60;
		dma_packet[pos + 4] = (uint8_t)((ip_bytes - 40) >> 8);
		dma_packet[pos + 5] = (uint8_t)(ip_bytes - 40);
		memset(dma_packet + pos + 40, i + 1, ip_bytes - 40);
		pos += ip_bytes;
		if (aggregate && i + 1 < count)
			pos += (4 - (pos - subframe) % 4) % 4;
	}
	dma_packet[0] = (uint8_t)pos;
	dma_packet[1] = (uint8_t)(pos >> 8);
	dma_packet[3] = 2 << 3; /* normal frame */
	dma_packet[13] = 6;     /* RX channel */
	dma_packet[16] = (uint8_t)format;
	uint32_t record_bytes = 0;
	CHECK(mt_rx_radio_record(dma_packet, (uint32_t)pos, 0, record, sizeof(record), &record_bytes) == MT_OK);
	CHECK(record_bytes > 8 && record_bytes - 8 <= RADIO_CLIENT_MAX_FRAME);
	if (count > 16)
		CHECK(record_bytes - 8 > 4096);
	const struct buf *frame = buf_new_const(record + 8, (int)record_bytes - 8);
	CHECK(frame);
	struct buf *packets[32];
	CHECK(output_capacity <= 32);
	struct awdl_rx_frames out = {packets, packets + output_capacity};
	int result = awdl_rx(frame, &out, state);
	CHECK(result == (output_capacity < count ? RX_UNEXPECTED_VALUE : RX_OK));
	int decoded = (int)(out.next - packets);
	CHECK(decoded == (output_capacity < count ? output_capacity : count));
	for (int i = 0; i < decoded; i++) {
		CHECK(buf_len(packets[i]) == 14 + ip_bytes);
		const uint8_t *eth = buf_data(packets[i]);
		CHECK(!memcmp(eth, &receiver, 6) && !memcmp(eth + 6, &sender, 6));
		CHECK(eth[12] == 0x86 && eth[13] == 0xdd && eth[14] == 0x60);
		for (int j = 40; j < ip_bytes; j++)
			CHECK(eth[14 + j] == (uint8_t)(i + 1));
		buf_free(packets[i]);
	}
	buf_free(frame);
}

int main(void) {
	struct awdl_state state = {0};
	state.self_address = receiver;
	awdl_peer_state_init(&state.peers);
	CHECK(awdl_peer_add(state.peers.peers, &sender, 1, NULL, NULL) >= 0);
	check_receive(&state, 1, 1500, 0, 0, 0, 1);   /* Ordinary QoS data */
	check_receive(&state, 1, 1450, 1, 1, 0, 1);   /* One hardware-split record, with RXD pad */
	check_receive(&state, 20, 515, 1, 0, 1, 20);  /* Complete large A-MSDU and HT Control */
	check_receive(&state, 20, 515, 1, 0, 0, 19);  /* Bounded output batch */
	awdl_peers_free(state.peers.peers);
	puts("MT7925 receive frame checks passed");
	return 0;
}
