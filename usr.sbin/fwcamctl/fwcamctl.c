/*
 * Copyright (c) 2026 Abdelkader Boudih <freebsd@seuros.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * fwcamctl - control utility for fwcam(4) IIDC FireWire cameras
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dev/firewire/fwcam.h>

static const char *default_dev = "/dev/fwcam0";

static void
usage(void)
{
	fprintf(stderr,
	    "usage: fwcamctl [-f device] info\n"
	    "       fwcamctl [-f device] snap [-o output.ppm] [-s skip]\n"
	    "       fwcamctl [-f device] mode <format> <mode> <rate>\n"
	    "       fwcamctl [-f device] feat get <id>\n"
	    "       fwcamctl [-f device] feat set <id> <value> [value2]\n"
	    "\n"
	    "  Feature IDs: 0=brightness 1=auto_exposure 2=sharpness\n"
	    "               3=white_balance 4=hue 5=saturation 6=gamma\n"
	    "               7=shutter 8=gain 9=iris 10=focus 11=temperature\n"
	    "               12=trigger 13=zoom 14=pan 15=tilt\n");
	exit(1);
}

static inline int
clamp_u8(int v)
{
	if (v < 0)   return (0);
	if (v > 255) return (255);
	return (v);
}

/*
 * IIDC YUV 4:2:2 → RGB24
 * IIDC spec layout: U0 Y0 V0 Y1  (UYVY, 2 pixels per 4 bytes)
 */
static void
yuv422_to_rgb(const uint8_t *src, uint8_t *dst, int w, int h)
{
	int i, n = w * h / 2;

	for (i = 0; i < n; i++) {
		int cb = src[0] - 128;
		int y0 = src[1];
		int cr = src[2] - 128;
		int y1 = src[3];
		src += 4;

		/* pixel 0 */
		dst[0] = clamp_u8(y0 + (int)(1.402f * cr));
		dst[1] = clamp_u8(y0 - (int)(0.34414f * cb) - (int)(0.71414f * cr));
		dst[2] = clamp_u8(y0 + (int)(1.772f * cb));
		/* pixel 1 */
		dst[3] = clamp_u8(y1 + (int)(1.402f * cr));
		dst[4] = clamp_u8(y1 - (int)(0.34414f * cb) - (int)(0.71414f * cr));
		dst[5] = clamp_u8(y1 + (int)(1.772f * cb));
		dst += 6;
	}
}

/*
 * IIDC YUV 4:1:1 → RGB24
 * Layout: U0 Y0 Y1 V0 Y2 Y3  (4 pixels per 6 bytes)
 */
static void
yuv411_to_rgb(const uint8_t *src, uint8_t *dst, int w, int h)
{
	int i, j, n = w * h / 4;

	for (i = 0; i < n; i++) {
		int cb = src[0] - 128;
		int y[4];
		y[0] = src[1];
		y[1] = src[2];
		int cr = src[3] - 128;
		y[2] = src[4];
		y[3] = src[5];
		src += 6;

		for (j = 0; j < 4; j++) {
			*dst++ = clamp_u8(y[j] + (int)(1.402f * cr));
			*dst++ = clamp_u8(y[j] - (int)(0.34414f * cb) - (int)(0.71414f * cr));
			*dst++ = clamp_u8(y[j] + (int)(1.772f * cb));
		}
	}
}

/*
 * IIDC YUV 4:4:4 → RGB24
 * Layout: U Y V  (3 bytes/pixel)
 */
static void
yuv444_to_rgb(const uint8_t *src, uint8_t *dst, int w, int h)
{
	int i, n = w * h;

	for (i = 0; i < n; i++) {
		int cb = src[0] - 128;
		int y  = src[1];
		int cr = src[2] - 128;
		src += 3;

		*dst++ = clamp_u8(y + (int)(1.402f * cr));
		*dst++ = clamp_u8(y - (int)(0.34414f * cb) - (int)(0.71414f * cr));
		*dst++ = clamp_u8(y + (int)(1.772f * cb));
	}
}

static int
write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
	FILE *f;

	if (strcmp(path, "-") == 0) {
		f = stdout;
	} else {
		f = fopen(path, "wb");
		if (f == NULL) {
			warn("fopen %s", path);
			return (-1);
		}
	}

	fprintf(f, "P6\n%d %d\n255\n", w, h);
	if (fwrite(rgb, 1, (size_t)(w * h * 3), f) != (size_t)(w * h * 3)) {
		warn("fwrite");
		if (f != stdout)
			fclose(f);
		return (-1);
	}

	if (f != stdout)
		fclose(f);
	return (0);
}

static int
do_snap(int fd, const char *outpath, int skip)
{
	struct fwcam_info info;
	uint8_t *frame_buf, *rgb_buf;
	uint32_t rgb_size;
	ssize_t n;
	int w, h, i;
	const char *pixfmt;

	if (ioctl(fd, FWCAM_GINFO, &info) < 0)
		err(1, "FWCAM_GINFO");

	if (info.frame_size == 0)
		errx(1, "frame_size is 0 — camera not yet probed?");

	if (info.cur_format != IIDC_FMT_VGA ||
	    info.cur_mode >= nitems(fwcam_fmt0_modes))
		errx(1, "unsupported format %d mode %d for snap "
		    "(only Format_0 VGA supported)",
		    info.cur_format, info.cur_mode);

	w      = fwcam_fmt0_modes[info.cur_mode].w;
	h      = fwcam_fmt0_modes[info.cur_mode].h;
	pixfmt = fwcam_fmt0_modes[info.cur_mode].pixfmt;

	if (strcmp(pixfmt, "Mono16") == 0)
		errx(1, "Mono16 not supported for PPM output");

	rgb_size  = (uint32_t)(w * h * 3);
	frame_buf = malloc(info.frame_size);
	rgb_buf   = malloc(rgb_size);
	if (frame_buf == NULL || rgb_buf == NULL)
		err(1, "malloc");

	/* Read frames; skip the first N to let AE/AWB settle */
	for (i = 0; i <= skip; i++) {
		n = read(fd, frame_buf, info.frame_size);
		if (n < 0)
			err(1, "read");
		if ((uint32_t)n < info.frame_size)
			errx(1, "short read: got %zd of %u bytes",
			    n, info.frame_size);
		if (i < skip)
			fprintf(stderr, "skipping frame %d/%d...\n", i + 1, skip);
	}

	/* Convert to RGB24 */
	if (strcmp(pixfmt, "YUV422") == 0) {
		yuv422_to_rgb(frame_buf, rgb_buf, w, h);
	} else if (strcmp(pixfmt, "YUV411") == 0) {
		yuv411_to_rgb(frame_buf, rgb_buf, w, h);
	} else if (strcmp(pixfmt, "YUV444") == 0) {
		yuv444_to_rgb(frame_buf, rgb_buf, w, h);
	} else if (strcmp(pixfmt, "RGB8") == 0) {
		memcpy(rgb_buf, frame_buf, rgb_size);
	} else if (strcmp(pixfmt, "Mono8") == 0) {
		for (i = 0; i < w * h; i++) {
			rgb_buf[i * 3 + 0] = frame_buf[i];
			rgb_buf[i * 3 + 1] = frame_buf[i];
			rgb_buf[i * 3 + 2] = frame_buf[i];
		}
	} else {
		errx(1, "no converter for pixel format %s", pixfmt);
	}

	if (write_ppm(outpath, rgb_buf, w, h) < 0) {
		free(frame_buf);
		free(rgb_buf);
		return (1);
	}

	fprintf(stderr, "saved %s (%dx%d %s, %u bytes raw)\n",
	    outpath, w, h, pixfmt, info.frame_size);

	free(frame_buf);
	free(rgb_buf);
	return (0);
}

static void
do_info(int fd)
{
	struct fwcam_info info;
	int i;

	if (ioctl(fd, FWCAM_GINFO, &info) < 0)
		err(1, "FWCAM_GINFO");

	printf("state:       %s\n",
	    info.state < nitems(fwcam_state_names) ?
	    fwcam_state_names[info.state] : "unknown");
	printf("cur format:  %d (%s)\n", info.cur_format,
	    info.cur_format < nitems(fwcam_fmt_names) ?
	    fwcam_fmt_names[info.cur_format] : "?");

	if (info.cur_format == IIDC_FMT_VGA &&
	    info.cur_mode < nitems(fwcam_fmt0_modes)) {
		printf("cur mode:    %d (%dx%d %s)\n", info.cur_mode,
		    fwcam_fmt0_modes[info.cur_mode].w,
		    fwcam_fmt0_modes[info.cur_mode].h,
		    fwcam_fmt0_modes[info.cur_mode].pixfmt);
	} else {
		printf("cur mode:    %d\n", info.cur_mode);
	}

	printf("cur rate:    %d (%s)\n", info.cur_framerate,
	    info.cur_framerate < nitems(fwcam_rate_names) ?
	    fwcam_rate_names[info.cur_framerate] : "?");
	printf("iso_channel: %u\n", info.iso_channel);
	printf("frame_size:  %u bytes\n", info.frame_size);
	printf("dropped:     %u frames\n", info.frame_dropped);

	printf("formats:     0x%08x  (", info.formats);
	for (i = 0; i < 8; i++) {
		if (info.formats & (1 << (31 - i)))
			printf("F%d ", i);
	}
	printf(")\n");

	printf("basic_func:  0x%08x", info.basic_func);
	if (info.basic_func & IIDC_CAM_POWER_CTRL) printf(" [power_ctrl]");
	if (info.basic_func & IIDC_ONE_SHOT_INQ)   printf(" [one_shot]");
	if (info.basic_func & IIDC_MULTI_SHOT_INQ) printf(" [multi_shot]");
	printf("\n");

	printf("features:    hi=0x%08x lo=0x%08x\n",
	    info.features_hi, info.features_lo);
	if (info.features_hi) {
		printf("             ");
		if (info.features_hi & IIDC_HAS_BRIGHTNESS)   printf("brightness ");
		if (info.features_hi & IIDC_HAS_AUTO_EXPOSURE) printf("auto_exp ");
		if (info.features_hi & IIDC_HAS_SHARPNESS)    printf("sharpness ");
		if (info.features_hi & IIDC_HAS_WHITE_BALANCE) printf("white_bal ");
		if (info.features_hi & IIDC_HAS_HUE)          printf("hue ");
		if (info.features_hi & IIDC_HAS_SATURATION)   printf("saturation ");
		if (info.features_hi & IIDC_HAS_GAMMA)        printf("gamma ");
		if (info.features_hi & IIDC_HAS_SHUTTER)      printf("shutter ");
		if (info.features_hi & IIDC_HAS_GAIN)         printf("gain ");
		if (info.features_hi & IIDC_HAS_IRIS)         printf("iris ");
		if (info.features_hi & IIDC_HAS_FOCUS)        printf("focus ");
		if (info.features_hi & IIDC_HAS_TEMPERATURE)  printf("temperature ");
		if (info.features_hi & IIDC_HAS_TRIGGER)      printf("trigger ");
		printf("\n");
	}
}

static void
do_mode(int fd, int argc, char **argv)
{
	struct fwcam_mode mode;

	if (argc < 3)
		usage();

	mode.format    = (uint8_t)atoi(argv[0]);
	mode.mode      = (uint8_t)atoi(argv[1]);
	mode.framerate = (uint8_t)atoi(argv[2]);
	mode.frame_size = 0;
	mode._pad = 0;

	if (ioctl(fd, FWCAM_SMODE, &mode) < 0)
		err(1, "FWCAM_SMODE");

	printf("mode set: format=%d mode=%d framerate=%d frame_size=%u\n",
	    mode.format, mode.mode, mode.framerate, mode.frame_size);
}

static void
do_feat_get(int fd, int feat_id)
{
	struct fwcam_feature feat;

	if (feat_id < 0 || feat_id >= FWCAM_FEAT_MAX)
		errx(1, "invalid feature id %d (0-%d)", feat_id, FWCAM_FEAT_MAX - 1);

	memset(&feat, 0, sizeof(feat));
	feat.id = (uint32_t)feat_id;

	if (ioctl(fd, FWCAM_GFEAT, &feat) < 0)
		err(1, "FWCAM_GFEAT");

	printf("feature %d (%s):\n", feat_id,
	    fwcam_feat_names[feat_id] != NULL ? fwcam_feat_names[feat_id] : "?");
	printf("  present: %s\n",
	    (feat.flags & FWCAM_FEATF_PRESENT) ? "yes" : "no");
	printf("  flags:   0x%x", feat.flags);
	if (feat.flags & FWCAM_FEATF_ONOFF)  printf(" [on/off]");
	if (feat.flags & FWCAM_FEATF_AUTO)   printf(" [auto]");
	if (feat.flags & FWCAM_FEATF_MANUAL) printf(" [manual]");
	printf("\n");
	printf("  range:   %u - %u\n", feat.min, feat.max);

	if (feat_id == FWCAM_FEAT_WHITE_BALANCE)
		printf("  value:   U/B=%u V/R=%u\n", feat.value, feat.value2);
	else
		printf("  value:   %u\n", feat.value);
}

static void
do_feat_set(int fd, int feat_id, uint32_t value, uint32_t value2)
{
	struct fwcam_feature feat;

	if (feat_id < 0 || feat_id >= FWCAM_FEAT_MAX)
		errx(1, "invalid feature id %d (0-%d)", feat_id, FWCAM_FEAT_MAX - 1);

	memset(&feat, 0, sizeof(feat));
	feat.id     = (uint32_t)feat_id;
	feat.value  = value;
	feat.value2 = value2;

	if (ioctl(fd, FWCAM_SFEAT, &feat) < 0)
		err(1, "FWCAM_SFEAT");

	printf("feature %d (%s) set to %u\n", feat_id,
	    fwcam_feat_names[feat_id] != NULL ? fwcam_feat_names[feat_id] : "?", value);
}

int
main(int argc, char *argv[])
{
	const char *devpath = default_dev;
	int fd, ch;

	while ((ch = getopt(argc, argv, "f:")) != -1) {
		switch (ch) {
		case 'f':
			devpath = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	fd = open(devpath, O_RDWR);
	if (fd < 0)
		err(1, "open %s", devpath);

	if (strcmp(argv[0], "info") == 0) {
		do_info(fd);

	} else if (strcmp(argv[0], "snap") == 0) {
		const char *out = "snap.ppm";
		int skip = 5;

		/* re-parse the snap subcommand's own flags */
		optreset = 1;
		optind = 1;
		while ((ch = getopt(argc, argv, "o:s:")) != -1) {
			switch (ch) {
			case 'o':
				out = optarg;
				break;
			case 's':
				skip = atoi(optarg);
				break;
			default:
				usage();
			}
		}
		do_snap(fd, out, skip);

	} else if (strcmp(argv[0], "mode") == 0) {
		do_mode(fd, argc - 1, argv + 1);

	} else if (strcmp(argv[0], "feat") == 0) {
		if (argc < 3)
			usage();

		int feat_id = atoi(argv[2]);

		if (strcmp(argv[1], "get") == 0) {
			do_feat_get(fd, feat_id);
		} else if (strcmp(argv[1], "set") == 0) {
			if (argc < 4)
				usage();
			uint32_t val  = (uint32_t)atoi(argv[3]);
			uint32_t val2 = (argc > 4) ? (uint32_t)atoi(argv[4]) : 0;
			do_feat_set(fd, feat_id, val, val2);
		} else {
			usage();
		}

	} else {
		warnx("unknown command: %s", argv[0]);
		usage();
	}

	close(fd);
	return (0);
}
