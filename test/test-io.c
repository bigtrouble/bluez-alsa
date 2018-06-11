/*
 * test-io.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include "inc/a2dp.inc"
#include "inc/sine.inc"
#include "inc/test.inc"
#include "../src/at.c"
#include "../src/bluealsa.c"
#include "../src/ctl.c"
#include "../src/io.c"
#include "../src/rfcomm.c"
#include "../src/transport.c"
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/rt.c"

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static const a2dp_aac_t config_aac_44100_stereo = {
	.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
	AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100)
	.channels = AAC_CHANNELS_2,
	.vbr = 1,
	AAC_INIT_BITRATE(0xFFFF)
};

static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info.vendor_id = APTX_VENDOR_ID,
	.info.codec_id = APTX_CODEC_ID,
	.frequency = APTX_SAMPLING_FREQ_44100,
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

/**
 * Helper function for timed thread join.
 *
 * This function takes the timeout value in milliseconds. */
static int pthread_timedjoin(pthread_t thread, void **retval, useconds_t usec) {

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)usec * 1000;

	/* normalize timespec structure */
	ts.tv_sec += ts.tv_nsec / (long)1e9;
	ts.tv_nsec = ts.tv_nsec % (long)1e9;

	return pthread_timedjoin_np(thread, retval, &ts);
}

static int test_a2dp_encoding(struct ba_transport *t, void *(*cb)(void *)) {

	int bt_fds[2];
	int pcm_fds[2];

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_fds) == 0);

	t->state = TRANSPORT_ACTIVE;
	t->bt_fd = bt_fds[0];
	t->a2dp.pcm.fd = pcm_fds[1];

	pthread_t thread;
	int16_t buffer[1024 * 10];
	size_t i;

	pthread_create(&thread, NULL, cb, t);

	snd_pcm_sine_s16le(buffer, sizeof(buffer) / sizeof(int16_t), 2, 0, 0.01);
	assert(write(pcm_fds[0], buffer, sizeof(buffer)) == sizeof(buffer));
	sleep(1);

	for (i = 0; i < 5; i++) {
		uint8_t *p = (uint8_t *)buffer;
		ssize_t len = read(bt_fds[1], p, t->mtu_write);
		fprintf(stderr, "BT data [len: %3zd]:", len);
		while (len--)
			fprintf(stderr, " %02x", *p++ & 0xFF);
		fprintf(stderr, "\n");
	}

	assert(pthread_cancel(thread) == 0);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);

	close(pcm_fds[0]);
	close(bt_fds[1]);
	return 0;
}

int test_a2dp_sbc_invalid_setup(void) {

	const uint8_t codec[] = { 0xff, 0xff, 0xff, 0xff };
	struct ba_transport transport = {
		.profile = BLUETOOTH_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.a2dp = {
			.cconfig = (uint8_t *)&codec,
			.cconfig_size = sizeof(a2dp_sbc_t),
		},
		.state = TRANSPORT_IDLE,
		.bt_fd = -1,
	};

	pthread_t thread;

	pthread_create(&thread, NULL, io_thread_a2dp_sink_sbc, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 1);
	assert(strcmp(test_error_msg, "Invalid BT socket: -1") == 0);

	transport.bt_fd = 0;

	pthread_create(&thread, NULL, io_thread_a2dp_sink_sbc, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 2);
	assert(strcmp(test_error_msg, "Invalid reading MTU: 0") == 0);

	transport.mtu_read = 475;

	pthread_create(&thread, NULL, io_thread_a2dp_sink_sbc, &transport);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 3);
	assert(strcmp(test_error_msg, "Couldn't initialize SBC codec: Invalid argument") == 0);

	transport.a2dp.cconfig = (uint8_t *)&config_sbc_44100_stereo;
	*test_error_msg = '\0';

	pthread_create(&thread, NULL, io_thread_a2dp_sink_sbc, &transport);
	assert(pthread_cancel(thread) == 0);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_error_count == 3);
	assert(strcmp(test_error_msg, "") == 0);

	return 0;
}

int test_a2dp_sbc_decoding(void) {

	int bt_fds[2];
	int pcm_fds[2];

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, bt_fds) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_fds) == 0);

	struct ba_transport transport = {
		.profile = BLUETOOTH_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.a2dp = {
			.cconfig = (uint8_t *)&config_sbc_44100_stereo,
			.cconfig_size = sizeof(a2dp_sbc_t),
			.pcm = { .fd = pcm_fds[0] },
		},
		.state = TRANSPORT_ACTIVE,
		.bt_fd = bt_fds[1],
		.mtu_read = 475,
	};

	pthread_t thread;
	int16_t buffer[1024 * 2];

	pthread_create(&thread, NULL, io_thread_a2dp_sink_sbc, &transport);

	snd_pcm_sine_s16le(buffer, sizeof(buffer) / sizeof(int16_t), 2, 0, 0.01);
	assert(a2dp_write_sbc(bt_fds[0], &config_sbc_44100_stereo, buffer, sizeof(buffer)) == 0);

	assert(pthread_cancel(thread) == 0);
	assert(pthread_timedjoin(thread, NULL, 1e6) == 0);
	assert(test_warn_count == 0 && test_error_count == 0);

	close(pcm_fds[1]);
	close(bt_fds[0]);
	return 0;
}

int test_a2dp_sbc_encoding(void) {

	struct ba_transport transport = {
		.profile = BLUETOOTH_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC,
		.a2dp = {
			.cconfig = (uint8_t *)&config_sbc_44100_stereo,
			.cconfig_size = sizeof(config_sbc_44100_stereo),
		},
	};

	transport.mtu_write = 153 * 3,
	assert(test_a2dp_encoding(&transport, io_thread_a2dp_source_sbc) == 0);
	assert(test_warn_count == 0 && test_error_count == 0);

	return 0;
}

#if ENABLE_AAC
int test_a2dp_aac_encoding(void) {

	struct ba_transport transport = {
		.profile = BLUETOOTH_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG24,
		.a2dp = {
			.cconfig = (uint8_t *)&config_aac_44100_stereo,
			.cconfig_size = sizeof(config_aac_44100_stereo),
		},
	};

	transport.mtu_write = 64;
	assert(test_a2dp_encoding(&transport, io_thread_a2dp_source_aac) == 0);
	assert(test_warn_count == 0 && test_error_count == 0);

	return 0;
}
#endif

#if ENABLE_APTX
int test_a2dp_aptx_encoding(void) {

	struct ba_transport transport = {
		.profile = BLUETOOTH_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_APTX,
		.a2dp = {
			.cconfig = (uint8_t *)&config_aptx_44100_stereo,
			.cconfig_size = sizeof(config_aptx_44100_stereo),
		},
	};

	transport.mtu_write = 40;
	assert(test_a2dp_encoding(&transport, io_thread_a2dp_source_aptx) == 0);
	assert(test_warn_count == 0 && test_error_count == 0);

	return 0;
}
#endif

int main(void) {
	test_run(test_a2dp_sbc_invalid_setup);
	test_run(test_a2dp_sbc_decoding);
	test_run(test_a2dp_sbc_encoding);
#if ENABLE_AAC
	test_run(test_a2dp_aac_encoding);
#endif
#if ENABLE_APTX
	test_run(test_a2dp_aptx_encoding);
#endif
	return 0;
}
