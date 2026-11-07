/*
 * test_dav1d_decoder.cpp
 *
 * Minimal stand-alone test for the dav1d Media Kit decoder add-on.
 * This does not require the full Media Kit; it only exercises the
 * libdav1d path that the add-on uses.
 *
 * Build (on RenkuOS / Haiku):
 *   g++ -O2 -o test_dav1d_decoder test_dav1d_decoder.cpp -ldav1d
 *
 * Run with a raw .obu or .ivf AV1 file:
 *   ./test_dav1d_decoder sample.obu
 *
 * Copyright (c) 2026 RenkuOS contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <dav1d/dav1d.h>

static void
usage(const char* prog)
{
	fprintf(stderr, "Usage: %s <av1-file.obu|.ivf>\n", prog);
	fprintf(stderr, "Simple libdav1d smoke test used by the RenkuOS Media Kit add-on.\n");
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	const char* path = argv[1];
	FILE* f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0) {
		fprintf(stderr, "Empty file\n");
		fclose(f);
		return 1;
	}

	uint8_t* data = (uint8_t*)malloc(size);
	if (!data) {
		fclose(f);
		return 1;
	}
	if (fread(data, 1, size, f) != (size_t)size) {
		fprintf(stderr, "Read error\n");
		free(data);
		fclose(f);
		return 1;
	}
	fclose(f);

	printf("Loaded %ld bytes from %s\n", size, path);
	printf("dav1d version: %s\n", dav1d_version());

	Dav1dSettings settings;
	dav1d_default_settings(&settings);
	settings.n_threads = 0;
	settings.max_frame_delay = 1;
	settings.apply_grain = 1;

	Dav1dContext* ctx = nullptr;
	int res = dav1d_open(&ctx, &settings);
	if (res < 0) {
		fprintf(stderr, "dav1d_open failed: %d\n", res);
		free(data);
		return 1;
	}

	Dav1dData dav1dData;
	memset(&dav1dData, 0, sizeof(dav1dData));

	res = dav1d_data_wrap(&dav1dData, data, size,
		[](const uint8_t* buf, void*) { free((void*)buf); }, nullptr);
	if (res < 0) {
		fprintf(stderr, "dav1d_data_wrap failed: %d\n", res);
		dav1d_close(&ctx);
		return 1;
	}

	res = dav1d_send_data(ctx, &dav1dData);
	if (res < 0 && res != DAV1D_ERR(EAGAIN)) {
		fprintf(stderr, "dav1d_send_data failed: %d\n", res);
		dav1d_data_unref(&dav1dData);
		dav1d_close(&ctx);
		return 1;
	}

	int frames = 0;
	while (true) {
		Dav1dPicture pic;
		memset(&pic, 0, sizeof(pic));
		res = dav1d_get_picture(ctx, &pic);
		if (res == DAV1D_ERR(EAGAIN))
			break;
		if (res < 0) {
			fprintf(stderr, "dav1d_get_picture failed: %d\n", res);
			break;
		}

		frames++;
		printf("Frame %d: %dx%d  bpc=%d  layout=%d\n",
			frames, pic.p.w, pic.p.h, pic.p.bpc, (int)pic.p.layout);

		dav1d_picture_unref(&pic);
	}

	dav1d_close(&ctx);

	printf("Decoded %d frame(s). Test %s.\n",
		frames, frames > 0 ? "PASSED" : "FAILED (no frames)");
	return frames > 0 ? 0 : 1;
}
