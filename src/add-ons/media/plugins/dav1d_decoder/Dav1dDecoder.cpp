/*
 * Dav1dDecoder.cpp
 *
 * Native AV1 decoder add-on for RenkuOS Media Kit
 * using VideoLAN's libdav1d.
 *
 * Features:
 *  - Low-latency (max_frame_delay = 1)
 *  - Film grain synthesis
 *  - 8 / 10 / 12-bit support (planar)
 *  - Debug logging via DAV1D_DECODER_DEBUG=1
 *
 * Copyright (c) 2026 RenkuOS contributors
 * SPDX-License-Identifier: MIT
 */

#include "Dav1dDecoder.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <new>
#include <stdlib.h>

#include <media/MediaDefs.h>
#include <media/MediaFormats.h>

#include <dav1d/dav1d.h>

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Dav1dDecoder::Dav1dDecoder()
	:
	fContext(nullptr),
	fHasPendingPicture(false),
	fChunkData(nullptr),
	fChunkSize(0),
	fInitialized(false),
	fFramesDecoded(0),
	fBytesFed(0),
	fLastDecodeTime(0),
	fDebug(false)
{
	memset(&fSettings, 0, sizeof(fSettings));
	memset(&fPendingPicture, 0, sizeof(fPendingPicture));
	memset(&fInputFormat, 0, sizeof(fInputFormat));
	memset(&fOutputFormat, 0, sizeof(fOutputFormat));
	memset(&fChunkHeader, 0, sizeof(fChunkHeader));

	const char* env = getenv("DAV1D_DECODER_DEBUG");
	if (env != nullptr && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0))
		fDebug = true;
}


Dav1dDecoder::~Dav1dDecoder()
{
	_ShutdownDav1d();
	_Log("Dav1dDecoder destroyed after %llu frames, %llu bytes fed",
		fFramesDecoded, fBytesFed);
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
Dav1dDecoder::Setup(media_format* inputFormat, const void* info, size_t infoSize)
{
	if (inputFormat == nullptr)
		return B_BAD_VALUE;

	std::lock_guard<std::mutex> lock(fLock);

	_ShutdownDav1d();

	fInputFormat = *inputFormat;
	fHasPendingPicture = false;
	fChunkData = nullptr;
	fChunkSize = 0;
	fFramesDecoded = 0;
	fBytesFed = 0;

	status_t err = _InitDav1d();
	if (err != B_OK) {
		_Log("Setup failed: dav1d_open error");
		return err;
	}

	fInitialized = true;
	_Log("Setup OK (debug=%d)", fDebug ? 1 : 0);
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
		raw->last_active = in->output.display.line_count > 0
			? in->output.display.line_count - 1 : 0;
		raw->orientation = B_VIDEO_TOP_LEFT_RIGHT;
		raw->pixel_width_aspect = in->output.pixel_width_aspect;
		raw->pixel_height_aspect = in->output.pixel_height_aspect;
		raw->display.line_width = in->output.display.line_width;
		raw->display.line_count = in->output.display.line_count;
		raw->display.bytes_per_row = 0;
	}

	raw->display.format = B_YUV420;
	raw->display.bytes_per_row = raw->display.line_width;

	fOutputFormat = *outputFormat;
	_Log("NegotiateOutputFormat: %dx%d",
		raw->display.line_width, raw->display.line_count);
	return B_OK;
}


status_t
Dav1dDecoder::Decode(void* outBuffer, int64* frameCount,
	media_header* mh, media_decode_info* info)
{
	if (outBuffer == nullptr || frameCount == nullptr || mh == nullptr)
		return B_BAD_VALUE;

	if (!fInitialized)
		return B_NO_INIT;

	std::lock_guard<std::mutex> lock(fLock);

	*frameCount = 0;

	if (fHasPendingPicture) {
		_PictureToBuffer(fPendingPicture, outBuffer, mh);
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
		*frameCount = 1;
		fFramesDecoded++;
		_Log("Delivered pending frame #%llu", fFramesDecoded);
		return B_OK;
	}

	status_t err = B_OK;
	Dav1dPicture picture;
	memset(&picture, 0, sizeof(picture));

	while (true) {
		err = _GetPicture(&picture);
		if (err == B_OK) {
			_PictureToBuffer(picture, outBuffer, mh);
			dav1d_picture_unref(&picture);
			*frameCount = 1;
			fFramesDecoded++;
			_Log("Decoded frame #%llu (%dx%d bpc=%d)",
				fFramesDecoded, picture.p.w, picture.p.h, picture.p.bpc);
			return B_OK;
		}

		if (err != B_WOULD_BLOCK)
			return err;

		const void* chunkData = nullptr;
		size_t chunkSize = 0;
		media_header chunkHeader;
		memset(&chunkHeader, 0, sizeof(chunkHeader));

		status_t chunkErr = GetNextChunk(&chunkData, &chunkSize, &chunkHeader);
		if (chunkErr != B_OK) {
			if (chunkErr == B_LAST_BUFFER_ERROR || chunkErr == B_ERROR) {
				_Log("End of stream after %llu frames", fFramesDecoded);
				return B_LAST_BUFFER_ERROR;
			}
			_Log("GetNextChunk error: %s", strerror(chunkErr));
			return chunkErr;
		}

		if (chunkData == nullptr || chunkSize == 0)
			continue;

		err = _FeedData(chunkData, chunkSize,
			chunkHeader.start_time, chunkHeader.duration);
		if (err != B_OK && err != B_WOULD_BLOCK) {
			_Log("FeedData failed");
			return err;
		}
	}

	return B_ERROR;
}


status_t
Dav1dDecoder::Seek(uint32 seekTo, int64 seekFrame, int64* frame, bigtime_t* time)
{
	std::lock_guard<std::mutex> lock(fLock);

	_Log("Seek requested (frame=%lld)", seekFrame);

	if (fContext != nullptr)
		dav1d_flush(fContext);

	if (fHasPendingPicture) {
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
	}

	fChunkData = nullptr;
	fChunkSize = 0;

	if (frame != nullptr)
		*frame = seekFrame;
	if (time != nullptr)
		*time = 0;

	return B_OK;
}


status_t
Dav1dDecoder::GetNextChunk(const void** chunkData, size_t* chunkSize,
	media_header* mediaHeader)
{
	return Decoder::GetNextChunk(chunkData, chunkSize, mediaHeader);
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

	_Log("dav1d context opened (version %s)", dav1d_version());
	return B_OK;
}


void
Dav1dDecoder::_ShutdownDav1d()
{
	if (fHasPendingPicture) {
		dav1d_picture_unref(&fPendingPicture);
		fHasPendingPicture = false;
	}

	if (fContext != nullptr) {
		dav1d_close(&fContext);
		fContext = nullptr;
	}

	fInitialized = false;
}


status_t
Dav1dDecoder::_FeedData(const void* data, size_t size,
	int64 startTime, int64 duration)
{
	if (fContext == nullptr || data == nullptr || size == 0)
		return B_BAD_VALUE;

	Dav1dData dav1dData;
	memset(&dav1dData, 0, sizeof(dav1dData));

	uint8_t* copy = (uint8_t*)malloc(size);
	if (copy == nullptr)
		return B_NO_MEMORY;

	memcpy(copy, data, size);

	int res = dav1d_data_wrap(&dav1dData, copy, size,
		[](const uint8_t* buf, void*) { free((void*)buf); }, nullptr);

	if (res < 0) {
		free(copy);
		_Log("dav1d_data_wrap failed (%d)", res);
		return B_ERROR;
	}

	dav1dData.m.timestamp = startTime;

	res = dav1d_send_data(fContext, &dav1dData);
	if (res < 0) {
		dav1d_data_unref(&dav1dData);
		if (res == DAV1D_ERR(EAGAIN))
			return B_WOULD_BLOCK;
		_Log("dav1d_send_data failed (%d)", res);
		return B_ERROR;
	}

	fBytesFed += size;
	return B_OK;
}


status_t
Dav1dDecoder::_GetPicture(Dav1dPicture* picture)
{
	if (fContext == nullptr || picture == nullptr)
		return B_BAD_VALUE;

	int res = dav1d_get_picture(fContext, picture);
	if (res < 0) {
		if (res == DAV1D_ERR(EAGAIN))
			return B_WOULD_BLOCK;
		_Log("dav1d_get_picture failed (%d)", res);
		return B_ERROR;
	}

	return B_OK;
}


void
Dav1dDecoder::_PictureToBuffer(const Dav1dPicture& picture,
	void* outBuffer, media_header* mh)
{
	if (outBuffer == nullptr || mh == nullptr)
		return;

	const int width  = picture.p.w;
	const int height = picture.p.h;
	const int bitDepth = picture.p.bpc;
	const Dav1dPixelLayout layout = picture.p.layout;
	const bool highBitDepth = (bitDepth > 8);

	color_space cs = _Dav1dToColorspace(layout, bitDepth);
	fOutputFormat.u.raw_video.display.format = cs;
	fOutputFormat.u.raw_video.display.line_width = width;
	fOutputFormat.u.raw_video.display.line_count = height;

	memset(mh, 0, sizeof(*mh));
	mh->type = B_MEDIA_RAW_VIDEO;
	mh->start_time = picture.m.timestamp;
	mh->data_offset = 0;
	mh->u.raw_video.field_sequence = fFramesDecoded;
	mh->u.raw_video.field_number = 0;
	mh->u.raw_video.first_active_line = 0;
	mh->u.raw_video.line_count = height;

	uint8* dst8 = static_cast<uint8*>(outBuffer);

	if (!highBitDepth) {
		if (layout == DAV1D_PIXEL_LAYOUT_I420) {
			_CopyPlane8(dst8, width, (const uint8*)picture.data[0], picture.stride[0], width, height);
			dst8 += width * height;
			_CopyPlane8(dst8, width / 2, (const uint8*)picture.data[1], picture.stride[1], width / 2, height / 2);
			dst8 += (width / 2) * (height / 2);
			_CopyPlane8(dst8, width / 2, (const uint8*)picture.data[2], picture.stride[2], width / 2, height / 2);
			mh->size_used = width * height * 3 / 2;
		} else if (layout == DAV1D_PIXEL_LAYOUT_I422) {
			_CopyPlane8(dst8, width, (const uint8*)picture.data[0], picture.stride[0], width, height);
			dst8 += width * height;
			_CopyPlane8(dst8, width / 2, (const uint8*)picture.data[1], picture.stride[1], width / 2, height);
			dst8 += (width / 2) * height;
			_CopyPlane8(dst8, width / 2, (const uint8*)picture.data[2], picture.stride[2], width / 2, height);
			mh->size_used = width * height * 2;
		} else if (layout == DAV1D_PIXEL_LAYOUT_I444) {
			_CopyPlane8(dst8, width, (const uint8*)picture.data[0], picture.stride[0], width, height);
			dst8 += width * height;
			_CopyPlane8(dst8, width, (const uint8*)picture.data[1], picture.stride[1], width, height);
			dst8 += width * height;
			_CopyPlane8(dst8, width, (const uint8*)picture.data[2], picture.stride[2], width, height);
			mh->size_used = width * height * 3;
		} else {
			_CopyPlane8(dst8, width, (const uint8*)picture.data[0], picture.stride[0], width, height);
			mh->size_used = width * height;
		}
	} else {
		uint16* dst16 = reinterpret_cast<uint16*>(outBuffer);
		const int strideY = picture.stride[0] / 2;

		if (layout == DAV1D_PIXEL_LAYOUT_I420) {
			_CopyPlane16(dst16, width, (const uint16*)picture.data[0], strideY, width, height);
			dst16 += width * height;
			_CopyPlane16(dst16, width / 2, (const uint16*)picture.data[1], picture.stride[1] / 2, width / 2, height / 2);
			dst16 += (width / 2) * (height / 2);
			_CopyPlane16(dst16, width / 2, (const uint16*)picture.data[2], picture.stride[2] / 2, width / 2, height / 2);
			mh->size_used = width * height * 3;
		} else if (layout == DAV1D_PIXEL_LAYOUT_I422) {
			_CopyPlane16(dst16, width, (const uint16*)picture.data[0], strideY, width, height);
			dst16 += width * height;
			_CopyPlane16(dst16, width / 2, (const uint16*)picture.data[1], picture.stride[1] / 2, width / 2, height);
			dst16 += (width / 2) * height;
			_CopyPlane16(dst16, width / 2, (const uint16*)picture.data[2], picture.stride[2] / 2, width / 2, height);
			mh->size_used = width * height * 4;
		} else if (layout == DAV1D_PIXEL_LAYOUT_I444) {
			_CopyPlane16(dst16, width, (const uint16*)picture.data[0], strideY, width, height);
			dst16 += width * height;
			_CopyPlane16(dst16, width, (const uint16*)picture.data[1], picture.stride[1] / 2, width, height);
			dst16 += width * height;
			_CopyPlane16(dst16, width, (const uint16*)picture.data[2], picture.stride[2] / 2, width, height);
			mh->size_used = width * height * 6;
		} else {
			_CopyPlane16(dst16, width, (const uint16*)picture.data[0], strideY, width, height);
			mh->size_used = width * height * 2;
		}
		_Log("High bit-depth frame delivered (bpc=%d, size=%u)", bitDepth, mh->size_used);
	}
}


color_space
Dav1dDecoder::_Dav1dToColorspace(Dav1dPixelLayout layout, int bitDepth) const
{
	switch (layout) {
		case DAV1D_PIXEL_LAYOUT_I400: return B_GRAY8;
		case DAV1D_PIXEL_LAYOUT_I420: return B_YUV420;
		case DAV1D_PIXEL_LAYOUT_I422: return B_YUV422;
		case DAV1D_PIXEL_LAYOUT_I444: return B_YUV444;
		default: return B_YUV420;
	}
}


void
Dav1dDecoder::_CopyPlane8(uint8* dst, int dstStride,
	const uint8* src, int srcStride, int width, int height)
{
	if (dstStride == srcStride && dstStride == width) {
		memcpy(dst, src, (size_t)width * height);
		return;
	}
	for (int y = 0; y < height; y++) {
		memcpy(dst, src, width);
		dst += dstStride;
		src += srcStride;
	}
}


void
Dav1dDecoder::_CopyPlane16(uint16* dst, int dstStride,
	const uint16* src, int srcStride, int width, int height)
{
	if (dstStride == srcStride && dstStride == width) {
		memcpy(dst, src, (size_t)width * height * sizeof(uint16));
		return;
	}
	for (int y = 0; y < height; y++) {
		memcpy(dst, src, width * sizeof(uint16));
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
