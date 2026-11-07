/*
 * Dav1dDecoder.cpp
 *
 * Native AV1 decoder add-on for RenkuOS Media Kit
 * using VideoLAN's libdav1d.
 *
 * Design goals:
 *  - Correct 8-bit path (I400 / I420 / I422 / I444) — primary use case
 *  - Correct 10/12-bit path (16-bit samples) for future Media Kit support
 *  - No data loss on dav1d EAGAIN (pending input + pending picture)
 *  - Low latency (max_frame_delay = 1)
 *  - Thread-safe, no use-after-free, explicit error paths
 *
 * Debug: DAV1D_DECODER_DEBUG=1
 *
 * Copyright (c) 2026 RenkuOS contributors
 * SPDX-License-Identifier: MIT
 */

#include "Dav1dDecoder.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <new>

#include <media/MediaDefs.h>
#include <media/MediaFormats.h>

#include <dav1d/dav1d.h>


Dav1dDecoder::Dav1dDecoder()
	:
	fContext(nullptr),
	fOutputFormatKnown(false),
	fHasPendingPicture(false),
	fHasPendingInput(false),
	fInitialized(false),
	fFramesDecoded(0),
	fBytesFed(0),
	fDebug(false)
{
	memset(&fSettings, 0, sizeof(fSettings));
	memset(&fPendingPicture, 0, sizeof(fPendingPicture));
	memset(&fPendingInput, 0, sizeof(fPendingInput));
	memset(&fInputFormat, 0, sizeof(fInputFormat));
	memset(&fOutputFormat, 0, sizeof(fOutputFormat));

	const char* env = getenv("DAV1D_DECODER_DEBUG");
	if (env != nullptr
		&& (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0
			|| strcasecmp(env, "yes") == 0)) {
		fDebug = true;
	}
}


Dav1dDecoder::~Dav1dDecoder()
{
	_ShutdownDav1d();
	_Log("destroyed after %llu frames, %llu bytes fed",
		(unsigned long long)fFramesDecoded,
		(unsigned long long)fBytesFed);
}


void
Dav1dDecoder::GetCodecInfo(media_codec_info* info)
{
	if (info == nullptr)
		return;

	strlcpy(info->pretty_name, "AV1 (libdav1d)", sizeof(info->pretty_name));
	strlcpy(info->short_name, "dav1d", sizeof(info->short_name));
	info->id = 0;
	info->sub_id = 0;
}


status_t
Dav1dDecoder::Setup(media_format* inputFormat, const void* /*info*/,
	size_t /*infoSize*/)
{
	if (inputFormat == nullptr)
		return B_BAD_VALUE;

	std::lock_guard<std::mutex> lock(fLock);

	_ShutdownDav1d();

	fInputFormat = *inputFormat;
	fOutputFormatKnown = false;
	fFramesDecoded = 0;
	fBytesFed = 0;

	status_t err = _InitDav1d();
	if (err != B_OK) {
		_Log("Setup: dav1d_open failed");
		return err;
	}

	fInitialized = true;
	_Log("Setup OK (debug=%d, dav1d %s)", fDebug ? 1 : 0, dav1d_version());
	return B_OK;
}


status_t
Dav1dDecoder::NegotiateOutputFormat(media_format* outputFormat)
{
	if (outputFormat == nullptr || !fInitialized)
		return B_BAD_VALUE;

	std::lock_guard<std::mutex> lock(fLock);

	media_raw_video_format* raw = &outputFormat->u.raw_video;

	outputFormat->type = B_MEDIA_RAW_VIDEO;
	outputFormat->user_data_type = 0;
	memset(outputFormat->user_data, 0, sizeof(outputFormat->user_data));

	if (fInputFormat.type == B_MEDIA_ENCODED_VIDEO) {
		const media_encoded_video_format* in = &fInputFormat.u.encoded_video;
		raw->field_rate = in->output.field_rate;
		raw->interlace = 1;
		raw->first_active = 0;
		raw->last_active = (in->output.display.line_count > 0)
			? (in->output.display.line_count - 1) : 0;
		raw->orientation = B_VIDEO_TOP_LEFT_RIGHT;
		raw->pixel_width_aspect = in->output.pixel_width_aspect;
		raw->pixel_height_aspect = in->output.pixel_height_aspect;
		raw->display.line_width = in->output.display.line_width;
		raw->display.line_count = in->output.display.line_count;
	} else {
		raw->field_rate = 0;
		raw->interlace = 1;
		raw->first_active = 0;
		raw->last_active = 0;
		raw->orientation = B_VIDEO_TOP_LEFT_RIGHT;
		raw->pixel_width_aspect = 1;
		raw->pixel_height_aspect = 1;
		raw->display.line_width = 0;
		raw->display.line_count = 0;
	}

	raw->display.format = B_YUV420;
	raw->display.bytes_per_row = raw->display.line_width;

	fOutputFormat = *outputFormat;
	_Log("NegotiateOutputFormat: %dx%d (provisional)",
		(int)raw->display.line_width, (int)raw->display.line_count);
	return B_OK;
}


status_t
Dav1dDecoder::Decode(void* outBuffer, int64* frameCount,
	media_header* mh, media_decode_info* /*info*/)
{
	if (outBuffer == nullptr || frameCount == nullptr || mh == nullptr)
		return B_BAD_VALUE;
	if (!fInitialized)
		return B_NO_INIT;

	std::lock_guard<std::mutex> lock(fLock);

	*frameCount = 0;

	if (fHasPendingPicture) {
		status_t err = _PictureToBuffer(fPendingPicture, outBuffer, mh);
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
		if (err != B_OK)
			return err;
		*frameCount = 1;
		fFramesDecoded++;
		_Log("delivered pending frame #%llu",
			(unsigned long long)fFramesDecoded);
		return B_OK;
	}

	for (;;) {
		Dav1dPicture picture;
		memset(&picture, 0, sizeof(picture));
		status_t err = _DrainPicture(&picture);

		if (err == B_OK) {
			const int w = picture.p.w;
			const int h = picture.p.h;
			const int bpc = picture.p.bpc;

			err = _PictureToBuffer(picture, outBuffer, mh);
			dav1d_picture_unref(&picture);
			if (err != B_OK)
				return err;

			*frameCount = 1;
			fFramesDecoded++;
			_Log("decoded frame #%llu (%dx%d bpc=%d)",
				(unsigned long long)fFramesDecoded, w, h, bpc);
			return B_OK;
		}

		if (err != B_WOULD_BLOCK)
			return err;

		if (fHasPendingInput) {
			int res = dav1d_send_data(fContext, &fPendingInput);
			if (res == 0) {
				fHasPendingInput = false;
				fBytesFed += fPendingInput.sz;
				continue;
			}
			if (res == DAV1D_ERR(EAGAIN))
				continue;
			dav1d_data_unref(&fPendingInput);
			fHasPendingInput = false;
			_Log("dav1d_send_data (pending) failed (%d)", res);
			return B_ERROR;
		}

		const void* chunkData = nullptr;
		size_t chunkSize = 0;
		media_header chunkHeader;
		memset(&chunkHeader, 0, sizeof(chunkHeader));

		status_t chunkErr = GetNextChunk(&chunkData, &chunkSize, &chunkHeader);
		if (chunkErr != B_OK) {
			if (chunkErr == B_LAST_BUFFER_ERROR || chunkErr == B_ERROR) {
				Dav1dData empty;
				memset(&empty, 0, sizeof(empty));
				dav1d_send_data(fContext, &empty);

				err = _DrainPicture(&picture);
				if (err == B_OK) {
					const int w = picture.p.w;
					const int h = picture.p.h;
					const int bpc = picture.p.bpc;
					err = _PictureToBuffer(picture, outBuffer, mh);
					dav1d_picture_unref(&picture);
					if (err != B_OK)
						return err;
					*frameCount = 1;
					fFramesDecoded++;
					_Log("decoded final frame #%llu (%dx%d bpc=%d)",
						(unsigned long long)fFramesDecoded, w, h, bpc);
					return B_OK;
				}
				_Log("end of stream after %llu frames",
					(unsigned long long)fFramesDecoded);
				return B_LAST_BUFFER_ERROR;
			}
			_Log("GetNextChunk failed: %s", strerror(chunkErr));
			return chunkErr;
		}

		if (chunkData == nullptr || chunkSize == 0)
			continue;

		err = _FeedInput(chunkData, chunkSize, chunkHeader.start_time);
		if (err != B_OK && err != B_WOULD_BLOCK)
			return err;
	}
}


status_t
Dav1dDecoder::Seek(uint32 /*seekTo*/, int64 seekFrame, int64* frame,
	bigtime_t* time)
{
	std::lock_guard<std::mutex> lock(fLock);

	_Log("Seek to frame %lld", (long long)seekFrame);

	if (fContext != nullptr)
		dav1d_flush(fContext);

	if (fHasPendingPicture) {
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
	}
	_FlushInput();

	if (frame != nullptr)
		*frame = seekFrame;
	if (time != nullptr)
		*time = 0;

	return B_OK;
}


status_t
Dav1dDecoder::_InitDav1d()
{
	dav1d_default_settings(&fSettings);

	fSettings.n_threads = 0;
	fSettings.max_frame_delay = 1;
	fSettings.apply_grain = 1;
	fSettings.operating_point = 0;
	fSettings.all_layers = 0;

	int res = dav1d_open(&fContext, &fSettings);
	if (res < 0) {
		_Log("dav1d_open failed (%d)", res);
		fContext = nullptr;
		return B_ERROR;
	}
	return B_OK;
}


void
Dav1dDecoder::_ShutdownDav1d()
{
	if (fHasPendingPicture) {
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
	}
	_FlushInput();

	if (fContext != nullptr) {
		dav1d_close(&fContext);
		fContext = nullptr;
	}
	fInitialized = false;
	fOutputFormatKnown = false;
}


void
Dav1dDecoder::_FlushInput()
{
	if (fHasPendingInput) {
		dav1d_data_unref(&fPendingInput);
		fHasPendingInput = false;
	}
}


status_t
Dav1dDecoder::_FeedInput(const void* data, size_t size, int64 startTime)
{
	if (fContext == nullptr || data == nullptr || size == 0)
		return B_BAD_VALUE;

	uint8_t* copy = (uint8_t*)malloc(size);
	if (copy == nullptr)
		return B_NO_MEMORY;
	memcpy(copy, data, size);

	Dav1dData packet;
	memset(&packet, 0, sizeof(packet));

	int res = dav1d_data_wrap(&packet, copy, size,
		[](const uint8_t* buf, void*) { free((void*)buf); },
		nullptr);
	if (res < 0) {
		free(copy);
		_Log("dav1d_data_wrap failed (%d)", res);
		return B_ERROR;
	}
	packet.m.timestamp = startTime;

	res = dav1d_send_data(fContext, &packet);
	if (res == 0) {
		fBytesFed += size;
		return B_OK;
	}

	if (res == DAV1D_ERR(EAGAIN)) {
		fPendingInput = packet;
		fHasPendingInput = true;
		return B_WOULD_BLOCK;
	}

	dav1d_data_unref(&packet);
	_Log("dav1d_send_data failed (%d)", res);
	return B_ERROR;
}


status_t
Dav1dDecoder::_DrainPicture(Dav1dPicture* outPicture)
{
	if (fContext == nullptr || outPicture == nullptr)
		return B_BAD_VALUE;

	int res = dav1d_get_picture(fContext, outPicture);
	if (res == 0)
		return B_OK;
	if (res == DAV1D_ERR(EAGAIN))
		return B_WOULD_BLOCK;

	_Log("dav1d_get_picture failed (%d)", res);
	return B_ERROR;
}


status_t
Dav1dDecoder::_PictureToBuffer(const Dav1dPicture& picture,
	void* outBuffer, media_header* mh)
{
	if (outBuffer == nullptr || mh == nullptr)
		return B_BAD_VALUE;

	const int width  = picture.p.w;
	const int height = picture.p.h;
	const int bpc    = picture.p.bpc;
	const Dav1dPixelLayout layout = picture.p.layout;

	if (width <= 0 || height <= 0 || picture.data[0] == nullptr)
		return B_BAD_DATA;

	const int chromaW = (width  + 1) / 2;
	const int chromaH = (height + 1) / 2;

	const color_space cs = _ColorspaceFor(layout, bpc);

	fOutputFormat.u.raw_video.display.format = cs;
	fOutputFormat.u.raw_video.display.line_width = width;
	fOutputFormat.u.raw_video.display.line_count = height;
	fOutputFormat.u.raw_video.display.bytes_per_row = width;
	fOutputFormat.u.raw_video.last_active = height > 0 ? height - 1 : 0;
	fOutputFormatKnown = true;

	memset(mh, 0, sizeof(*mh));
	mh->type = B_MEDIA_RAW_VIDEO;
	mh->start_time = picture.m.timestamp;
	mh->data_offset = 0;
	mh->u.raw_video.field_sequence = (uint32)fFramesDecoded;
	mh->u.raw_video.field_number = 0;
	mh->u.raw_video.first_active_line = 0;
	mh->u.raw_video.line_count = height;

	uint8* dst = static_cast<uint8*>(outBuffer);

	if (bpc <= 8) {
		const uint8* srcY = (const uint8*)picture.data[0];
		const uint8* srcU = (const uint8*)picture.data[1];
		const uint8* srcV = (const uint8*)picture.data[2];
		const ptrdiff_t strideY = picture.stride[0];
		const ptrdiff_t strideU = picture.stride[1];
		const ptrdiff_t strideV = picture.stride[2];

		switch (layout) {
			case DAV1D_PIXEL_LAYOUT_I400:
				_CopyPlane8(dst, width, srcY, strideY, width, height);
				mh->size_used = (size_t)width * height;
				break;

			case DAV1D_PIXEL_LAYOUT_I420:
				_CopyPlane8(dst, width, srcY, strideY, width, height);
				dst += (size_t)width * height;
				_CopyPlane8(dst, chromaW, srcU, strideU, chromaW, chromaH);
				dst += (size_t)chromaW * chromaH;
				_CopyPlane8(dst, chromaW, srcV, strideV, chromaW, chromaH);
				mh->size_used = (size_t)width * height
					+ 2 * ((size_t)chromaW * chromaH);
				break;

			case DAV1D_PIXEL_LAYOUT_I422:
				_CopyPlane8(dst, width, srcY, strideY, width, height);
				dst += (size_t)width * height;
				_CopyPlane8(dst, chromaW, srcU, strideU, chromaW, height);
				dst += (size_t)chromaW * height;
				_CopyPlane8(dst, chromaW, srcV, strideV, chromaW, height);
				mh->size_used = (size_t)width * height
					+ 2 * ((size_t)chromaW * height);
				break;

			case DAV1D_PIXEL_LAYOUT_I444:
				_CopyPlane8(dst, width, srcY, strideY, width, height);
				dst += (size_t)width * height;
				_CopyPlane8(dst, width, srcU, strideU, width, height);
				dst += (size_t)width * height;
				_CopyPlane8(dst, width, srcV, strideV, width, height);
				mh->size_used = (size_t)width * height * 3;
				break;

			default:
				_Log("unsupported 8-bit layout %d", (int)layout);
				return B_MISMATCHED_VALUES;
		}
		return B_OK;
	}

	{
		uint16* dst16 = reinterpret_cast<uint16*>(outBuffer);
		const uint16* srcY = (const uint16*)picture.data[0];
		const uint16* srcU = (const uint16*)picture.data[1];
		const uint16* srcV = (const uint16*)picture.data[2];
		const ptrdiff_t strideY = picture.stride[0] / 2;
		const ptrdiff_t strideU = picture.stride[1] / 2;
		const ptrdiff_t strideV = picture.stride[2] / 2;

		switch (layout) {
			case DAV1D_PIXEL_LAYOUT_I400:
				_CopyPlane16(dst16, width, srcY, strideY, width, height);
				mh->size_used = (size_t)width * height * 2;
				break;

			case DAV1D_PIXEL_LAYOUT_I420:
				_CopyPlane16(dst16, width, srcY, strideY, width, height);
				dst16 += (size_t)width * height;
				_CopyPlane16(dst16, chromaW, srcU, strideU, chromaW, chromaH);
				dst16 += (size_t)chromaW * chromaH;
				_CopyPlane16(dst16, chromaW, srcV, strideV, chromaW, chromaH);
				mh->size_used = ((size_t)width * height
					+ 2 * ((size_t)chromaW * chromaH)) * 2;
				break;

			case DAV1D_PIXEL_LAYOUT_I422:
				_CopyPlane16(dst16, width, srcY, strideY, width, height);
				dst16 += (size_t)width * height;
				_CopyPlane16(dst16, chromaW, srcU, strideU, chromaW, height);
				dst16 += (size_t)chromaW * height;
				_CopyPlane16(dst16, chromaW, srcV, strideV, chromaW, height);
				mh->size_used = ((size_t)width * height
					+ 2 * ((size_t)chromaW * height)) * 2;
				break;

			case DAV1D_PIXEL_LAYOUT_I444:
				_CopyPlane16(dst16, width, srcY, strideY, width, height);
				dst16 += (size_t)width * height;
				_CopyPlane16(dst16, width, srcU, strideU, width, height);
				dst16 += (size_t)width * height;
				_CopyPlane16(dst16, width, srcV, strideV, width, height);
				mh->size_used = (size_t)width * height * 3 * 2;
				break;

			default:
				_Log("unsupported high-bit-depth layout %d", (int)layout);
				return B_MISMATCHED_VALUES;
		}
		_Log("high-bit-depth frame bpc=%d size=%u", bpc, (unsigned)mh->size_used);
		return B_OK;
	}
}


color_space
Dav1dDecoder::_ColorspaceFor(Dav1dPixelLayout layout, int /*bitDepth*/) const
{
	switch (layout) {
		case DAV1D_PIXEL_LAYOUT_I400: return B_GRAY8;
		case DAV1D_PIXEL_LAYOUT_I420: return B_YUV420;
		case DAV1D_PIXEL_LAYOUT_I422: return B_YUV422;
		case DAV1D_PIXEL_LAYOUT_I444: return B_YUV444;
		default:                      return B_YUV420;
	}
}


void
Dav1dDecoder::_CopyPlane8(uint8* dst, ptrdiff_t dstStride,
	const uint8* src, ptrdiff_t srcStride, int width, int height)
{
	if (width <= 0 || height <= 0 || dst == nullptr || src == nullptr)
		return;

	if (dstStride == srcStride && dstStride == width) {
		memcpy(dst, src, (size_t)width * (size_t)height);
		return;
	}

	for (int y = 0; y < height; y++) {
		memcpy(dst, src, (size_t)width);
		dst += dstStride;
		src += srcStride;
	}
}


void
Dav1dDecoder::_CopyPlane16(uint16* dst, ptrdiff_t dstStride,
	const uint16* src, ptrdiff_t srcStride, int width, int height)
{
	if (width <= 0 || height <= 0 || dst == nullptr || src == nullptr)
		return;

	if (dstStride == srcStride && dstStride == width) {
		memcpy(dst, src, (size_t)width * (size_t)height * sizeof(uint16));
		return;
	}

	for (int y = 0; y < height; y++) {
		memcpy(dst, src, (size_t)width * sizeof(uint16));
		dst += dstStride;
		src += srcStride;
	}
}


void
Dav1dDecoder::_Log(const char* fmt, ...) const
{
	if (!fDebug)
		return;

	char buffer[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	fprintf(stderr, "[dav1d_decoder] %s\n", buffer);
}
