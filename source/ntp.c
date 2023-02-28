// Wii NTP client

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <getopt.h>
#include <time.h>
#include <sys/types.h>
#include <gctypes.h>
#include <network.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <ogc/video_types.h>
#include <ogc/gx_struct.h>
#include <ogc/consol.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/conf.h>
#include <ogc/exi.h>
#include <wiiuse/wpad.h>

#ifndef DEFAULT_NTP_SERVER
#define DEFAULT_NTP_SERVER "time.nist.gov"
#endif

// The Wii is big-endian.
#define htobe16(x)			x
#define be16toh(x)			x

#define htobe32(x)			x
#define be32toh(x)			x

#define NET_CONNECT_TIMEOUT		5000
#define NET_RECV_TIMEOUT		10000
#define NET_SEND_TIMEOUT		4000

#define NTP_PORT_NUMBER			123

#define NTP_MODE_CLIENT			3
#define NTP_MODE_SERVER			4
#define NTP_LEAP_NOTINSYNC		3

#define NTP_FIELD(l, v, m)		(((l) << 6) | ((v) << 3) | (m))
#define NTP_FIELD_LEAP(f)		(((f) >> 6) & 3)
#define NTP_FIELD_VERSION(f)		(((f) >> 3) & 7)
#define NTP_FIELD_MODE(f)		((f) & 7)

#define NTP_ROOT_DISTANCE_MAX		5

/*
 * "NTP timestamps are represented as a 64-bit unsigned fixed-point number,
 * in seconds relative to 0h on 1 January 1900."
 */
#define OFFSET_NTP_UNIX			UINT32_C(2208988800)
// GameCube and Wii time starts at 1 January 2000 00:00:00.
#define OFFSET_UNIX_WII			UINT32_C(946684800)

#define OFFSET_NTP_WII			(OFFSET_NTP_UNIX + OFFSET_UNIX_WII)

#define RTC_DIVISOR			(TB_TIMER_CLOCK * 1000)

struct ntp_ts {
	uint32_t sec;
	uint32_t frac;
} __attribute__((__packed__));

struct ntp_ts_short {
	uint16_t sec;
	uint16_t frac;
} __attribute__((__packed__));

struct ntp_msg {
	uint8_t field;
	uint8_t stratum;
	int8_t poll;
	int8_t precision;
	struct ntp_ts_short root_delay;
	struct ntp_ts_short root_dispersion;
	char refid[4];
	struct ntp_ts reference_time;
	struct ntp_ts origin_time;
	struct ntp_ts recv_time;
	struct ntp_ts trans_time;
} __attribute__((__packed__));

int main(int argc, char **argv) {
	VIDEO_Init();

	PAD_Init();
	WPAD_Init();

	{
		GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
		{
			void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
			console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
			             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
			VIDEO_Configure(rmode);
			VIDEO_SetNextFramebuffer(xfb);
		}
		VIDEO_SetBlack(false);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		if (rmode->viTVMode & VI_NON_INTERLACE)
			VIDEO_WaitVSync();
	}

	{
		time_t current_time = time(NULL);

		printf("\x1b[2;0HCurrent system time: %s", ctime(&current_time));
	}

	const char *ntp_server = DEFAULT_NTP_SERVER;
	int timezone = 0;
	bool automatic = false;

	{
		struct option long_options[] = {
			{ "server",	required_argument,	0, 's' },
			{ "timezone",	required_argument,	0, 't' },
			{ "auto",	no_argument,		0, 'a' },
			{ "wait",	no_argument,		0, 'w' },
			{ 0, 0, 0, 0 }
		};

		while (true) {
			int option_index = 0;

			switch (getopt_long(argc, argv, "s:t:aw", long_options, &option_index)) {
				case -1:
					goto parsing_done;
				case 's':
					ntp_server = optarg;
					break;
				case 't': {
					char *end = NULL;
					timezone = strtol(optarg, &end, 10);
					if (end == optarg) {
						printf("Invalid UTC offset: '%s'\n", optarg);
						goto done;
					}
					break;
				}
				case 'a':
					automatic = true;
					break;
				case 'w':
					automatic = false;
					break;
				default:
					printf("Invalid argument: '%s'\n", optarg);
					goto done;
				}
		}
	}

parsing_done:
	if (automatic) {
		printf("Continuing in 5 seconds with UTC%s%d; press any button to stop\n",
		       timezone >= 0 ? "+" : "", timezone);
		u64 end_time = gettime() + secs_to_ticks(5);
		do {
			VIDEO_WaitVSync();

			PAD_ScanPads();
			for (unsigned char i = 0; i < 4; ++i) {
				if (PAD_ButtonsDown(i))
					goto tz_select;
			}

			WPAD_ScanPads();
			for (unsigned char i = 0; i < 4; ++i) {
				if (WPAD_ButtonsDown(i))
					goto tz_select;
			}
		} while (gettime() < end_time);
		goto tz_selected;
	}

tz_select:
	fputs("Use the D-pad to select your time zone, then press A to continue\n"
	      "Time zone: UTC\x1b[s", stdout);

	while (true) {
		printf("\x1b[u\x1b[K%s%d", timezone >= 0 ? "+" : "", timezone);

		while (true) {
			VIDEO_WaitVSync();

			PAD_ScanPads();
			WPAD_ScanPads();

			u32 pad_pressed = 0;
			u32 wpad_pressed = 0;
			for (unsigned char i = 0; i < 4; ++i)
				pad_pressed |= PAD_ButtonsDown(i);
			for (unsigned char i = 0; i < 4; ++i)
				wpad_pressed |= WPAD_ButtonsDown(i);

			if ((pad_pressed & PAD_BUTTON_START) || (wpad_pressed & WPAD_BUTTON_HOME)) {
				putchar('\n');
				goto done;
			}
			if ((pad_pressed & PAD_BUTTON_A) || (wpad_pressed & WPAD_BUTTON_A)) {
				putchar('\n');
				goto tz_selected;
			}
			if ((pad_pressed & PAD_BUTTON_LEFT) || (wpad_pressed & WPAD_BUTTON_LEFT)) {
				--timezone;
				break;
			}
			if ((pad_pressed & PAD_BUTTON_RIGHT) || (wpad_pressed & WPAD_BUTTON_RIGHT)) {
				++timezone;
				break;
			}
		}
	}

tz_selected:
	puts("Getting RTC bias");

	u32 rtc_bias = 0;
	if (CONF_GetCounterBias(&rtc_bias) < 0) {
		rtc_bias = 0;
		puts("Failed to get RTC bias; time may be significantly off");
	} else {
		printf("Got RTC bias: %u seconds\n", rtc_bias);
	}

	puts("Initializing network");
	if (net_init()) {
		puts("Failed to initialize network");
		goto done;
	}

	printf("Network initialized\nUsing NTP server %s\n", ntp_server);

	u32 **ip_addrs;
	struct hostent *hp = net_gethostbyname(ntp_server);
	if (!hp || hp->h_addrtype != AF_INET || !(ip_addrs = (u32 **)hp->h_addr_list)) {
		printf("net_gethostbyname() failed: %d\n", errno);
		goto done;
	}

	printf("Hostname: %s\nIPs:\n", hp->h_name);

	unsigned int total_ips = 0;
	{
		u32 *ip_addr_ptr;
		while ((ip_addr_ptr = ip_addrs[total_ips])) {
			++total_ips;

			u32 ip_addr = *ip_addr_ptr;

			printf("\t%u. %u.%u.%u.%u\n", total_ips,
			       (ip_addr >> 24)	& 0xFF,
			       (ip_addr >> 16)	& 0xFF,
			       (ip_addr >> 8)	& 0xFF,
			       (ip_addr)	& 0xFF);
		}
	}

	s32 socket_fd = net_socket(PF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0) {
		printf("net_socket() failed: %d\n", socket_fd);
		goto done;
	}

	{
		unsigned int ip_addrs_order[total_ips];

		for (unsigned int i = 0; i < total_ips; ++i)
			ip_addrs_order[i] = i;

		if (total_ips > 1) {
			for (unsigned int i = 0; i < total_ips; ++i) {
				unsigned int random_index = rand() % total_ips;
				unsigned int temp = ip_addrs_order[i];

				ip_addrs_order[i] = ip_addrs_order[random_index];
				ip_addrs_order[random_index] = temp;
			}
		}

		for (unsigned int i = 0; i < total_ips; i++) {
			unsigned int ip_addr_index = ip_addrs_order[i];

			u32 *ip_addr_ptr = ip_addrs[ip_addr_index];

			{
				u32 ip_addr = *ip_addr_ptr;
				printf("Attempting to connect server %u (%u.%u.%u.%u)\n",
				       ip_addr_index + 1,
				       (ip_addr >> 24)	& 0xFF,
				       (ip_addr >> 16)	& 0xFF,
				       (ip_addr >> 8)	& 0xFF,
				       (ip_addr)	& 0xFF);
			}

			struct sockaddr_in socketaddr = {
				.sin_family = AF_INET,
				.sin_len = sizeof (struct sockaddr_in),
				.sin_port = htons(NTP_PORT_NUMBER),
			};

			memcpy((char *)&socketaddr.sin_addr, ip_addr_ptr, hp->h_length);

			u64 connect_start = gettime();
			while (true) {
				s32 res = net_connect(socket_fd, (struct sockaddr *)&socketaddr,
				                      sizeof (struct sockaddr_in));

				if (res >= 0 || res == -EISCONN)
					goto connected;

				if (res != -EINPROGRESS && res != -EALREADY) {
					printf("net_connect() failed: %d\n", res);
					break;
				}

				if (ticks_to_millisecs(diff_ticks(connect_start, gettime()))
				    > NET_CONNECT_TIMEOUT) {
					puts("net_connect() timeout");
					break;
				}

				usleep(20 * 1000);
			}
		}
	}

	puts("Tried every server");
	goto done_close;

connected:
	puts("Connected; requesting time");

	u64 request_time_ticks = gettime();
	struct ntp_ts request_time = {
		.sec = htobe32(ticks_to_secs(request_time_ticks) + OFFSET_NTP_WII),
		.frac = htobe32(tick_nanosecs(request_time_ticks)),
	};

	{
		struct ntp_msg ntp_request = {
			/*
			 * "The client initializes the NTP message header, sends the request
			 * to the server, and strips the time of day from the Transmit
			 * Timestamp field of the reply.  For this purpose, all the NTP
			 * header fields are set to 0, except the Mode, VN, and optional
			 * Transmit Timestamp fields."
			 */
			.field = NTP_FIELD(0, 4, NTP_MODE_CLIENT),
			.trans_time = request_time,
		};

		const u8 *transmit_buf = (void *)&ntp_request;
		unsigned char remaining = sizeof (struct ntp_msg);

		u64 transmit_start = gettime();
		while (true) {
			s32 res = net_write(socket_fd, transmit_buf, remaining);

			if (res != -EAGAIN) {
				if (res < 0) {
					printf("net_write() failed: %d\n", res);
					goto done_close;
				}

				if (res >= remaining)
					break;

				remaining -= res;
				transmit_buf += res;
			}

			if (ticks_to_millisecs(diff_ticks(transmit_start, gettime()))
			    > NET_SEND_TIMEOUT) {
				puts("net_write() timeout");
				goto done_close;
			}

			usleep(20 * 1000);
		}
	}

	struct ntp_msg ntp_response = { 0 };

	{
		u8 *receive_buf = (u8 *)&ntp_response;
		unsigned char remaining = sizeof (struct ntp_msg);

		u64 receive_start = gettime();
		while (true) {
			s32 res = net_read(socket_fd, receive_buf, remaining);

			if (res != -EAGAIN) {
				if (res < 0) {
					printf("net_read() failed: %d\n", res);
					goto done_close;
				}

				if (res >= remaining)
					break;

				remaining -= res;
				receive_buf += res;
			}

			if (ticks_to_millisecs(diff_ticks(receive_start, gettime()))
			    > NET_RECV_TIMEOUT) {
				puts("net_read() timeout");
				goto done_close;
			}

			usleep(20 * 1000);
		}
	}

	if (NTP_FIELD_VERSION(ntp_response.field) != 3
	    && NTP_FIELD_VERSION(ntp_response.field) != 4) {
		printf("Server utilizing unsupported NTP version: NTPv%d\n",
		       NTP_FIELD_VERSION(ntp_response.field));
		goto done_close;
	}

	if (NTP_FIELD_MODE(ntp_response.field) != NTP_MODE_SERVER) {
		printf("Server utilizing unsupported mode: %d\n",
		       NTP_FIELD_MODE(ntp_response.field));
		goto done_close;
	}

	if (memcmp(&request_time, &ntp_response.origin_time, sizeof (struct ntp_ts))) {
		puts("Server returned origin time differing from our request's timestamp");
		goto done_close;
	}

	if (NTP_FIELD_LEAP(ntp_response.field) == NTP_LEAP_NOTINSYNC ||
	    ntp_response.stratum == 0 || ntp_response.stratum >= 16) {
		puts("Server not synchronized");
		goto done_close;
	}

	if (be32toh(ntp_response.recv_time.sec) < (OFFSET_NTP_WII + rtc_bias) ||
	    be32toh(ntp_response.trans_time.sec) < (OFFSET_NTP_WII + rtc_bias)) {
		puts("Server returned time before epoch");
		goto done_close;
	}

	if (ntp_response.root_delay.sec / 2 + ntp_response.root_dispersion.sec
	    > NTP_ROOT_DISTANCE_MAX) {
		puts("Server has too large of a root distance");
		goto done_close;
	}

	{
		unsigned int ntp_time = be32toh(ntp_response.trans_time.sec)
		                        - OFFSET_NTP_UNIX + (timezone * 60 * 60);

		{
			u64 ntp_time_wii = secs_to_ticks(ntp_time - OFFSET_UNIX_WII)
			                   + nanosecs_to_ticks(be32toh(ntp_response.trans_time.frac));
			settime(ntp_time_wii);
		}

		time_t ntp_time_long = ntp_time;
		printf("Got time: %s", ctime(&ntp_time_long));
	}

	net_close(socket_fd);
	net_deinit();

	puts("Setting RTC");

	if (!EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_1, NULL)) {
		puts("Failed to lock RTC");
		goto done;
	}
	if (!EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_1, EXI_SPEED8MHZ)) {
		puts("Failed to select RTC");
		EXI_Unlock(EXI_CHANNEL_0);
		goto done;
	}

	{
		u32 exi_status = 0;
		{
			u32 rtc_set_cmd = 0xA0000000;
			if (!EXI_Imm(EXI_CHANNEL_0, &rtc_set_cmd, 4, EXI_WRITE, NULL))
				exi_status |= 0x01;
		}
		if (!EXI_Sync(EXI_CHANNEL_0))
			exi_status |= 0x02;

		{
			u64 new_time = gettime();

			// The GameCube RTC can only be set in the precision of 1 second...
			u32 new_time_rtc = new_time / RTC_DIVISOR - rtc_bias;

			// ...so wait until the beginning of a second to do so.
			{
				long rtc_set_delay = tick_nanosecs(new_time);
				if (__builtin_expect(!!(rtc_set_delay), 1)) {
					++new_time_rtc;

					struct timespec rtc_set_delay_ts = {
						.tv_sec = 0,
						.tv_nsec = TB_NSPERSEC - rtc_set_delay,
					};

					nanosleep(&rtc_set_delay_ts, NULL);
				}
			}

			if (!EXI_Imm(EXI_CHANNEL_0, &new_time_rtc, 4, EXI_WRITE, NULL))
				exi_status |= 0x04;
		}
		if (!EXI_Sync(EXI_CHANNEL_0))
			exi_status |= 0x08;
		if (!EXI_Deselect(EXI_CHANNEL_0))
			exi_status |= 0x10;

		EXI_Unlock(EXI_CHANNEL_0);

		if (exi_status)
			puts("Failed to set RTC");
		else
			puts("RTC set");
	}

done:
	{
		time_t final_time = time(NULL);

		printf("System time now: %s"
		       "Exiting in 5 seconds\n", ctime(&final_time));
	}

	sleep(5);

	return 0;

done_close:
	net_close(socket_fd);
	net_deinit();

	goto done;
}
