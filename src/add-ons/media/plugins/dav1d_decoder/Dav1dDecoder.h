/*
 * Dav1dDecoder.h
 *
 * Native AV1 decoder add-on for RenkuOS Media Kit
 * using VideoLAN's libdav1d.
 *
 * Copyright (c) 2026 RenkuOS contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef DAV1D_DECODER_H
#define DAV1D_DECODER_H

#include <media/DecoderPlugin.h>
#include <media/MediaDefs.h>
#include <media/MediaFormats.h>
#include <SupportDefs.h>

#include <dav1d/dav1d.h>

#include <atomic>
#include <mutex>

class Dav1dDecoder : public Decoder {
public:
								Dav1dDecoder();
	virtual						~Dav1dDecoder();

	virtual void				GetCodecInfo(media_codec_info* info);
	virtual status_t			Setup(media_format* inputFormat,
									const void* info, size_t infoSize);
	virtual status_t			NegotiateOutputFormat(media_format* outputFormat);
	virtual status_t			Decode(void* outBuffer, int64* frameCount,
									media_header* mh, media_decode_info* info);
	virtual status_t			Seek(uint32 seekTo, int64 seekFrame,
									int64* frame, bigtime_t* time);

private:
	status_t					_InitDav1d();
	void						_ShutdownDav1d();
	void						_FlushInput();
	status_t					_FeedInput(const void* data, size_t size,
									int64 startTime);
	status_t					_DrainPicture(Dav1dPicture* outPicture);
	status_t					_PictureToBuffer(const Dav1dPicture& picture,
									void* outBuffer, media_header* mh);
	color_space					_ColorspaceFor(Dav1dPixelLayout layout,
									int bitDepth) const;
	static void					_CopyPlane8(uint8* dst, ptrdiff_t dstStride,
									const uint8* src, ptrdiff_t srcStride,
									int width, int height);
	static void					_CopyPlane16(uint16* dst, ptrdiff_t dstStride,
									const uint16* src, ptrdiff_t srcStride,
									int width, int height);
	void						_Log(const char* fmt, ...) const;

	Dav1dContext*				fContext;
	Dav1dSettings				fSettings;

	media_format				fInputFormat;
	media_format				fOutputFormat;
	bool						fOutputFormatKnown;

	Dav1dPicture				fPendingPicture;
	bool						fHasPendingPicture;

	Dav1dData					fPendingInput;
	bool						fHasPendingInput;

	std::mutex					fLock;
	std::atomic<bool>			fInitialized;

	uint64						fFramesDecoded;
	uint64						fBytesFed;
	bool						fDebug;
};

#endif // DAV1D_DECODER_H
