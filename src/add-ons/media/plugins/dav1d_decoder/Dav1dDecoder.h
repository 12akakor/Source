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

	// Decoder interface
	virtual void				GetCodecInfo(media_codec_info* info);
	virtual status_t			Setup(media_format* inputFormat,
									const void* info, size_t infoSize);
	virtual status_t			NegotiateOutputFormat(media_format* outputFormat);
	virtual status_t			Decode(void* outBuffer, int64* frameCount,
									media_header* mh, media_decode_info* info);
	virtual status_t			Seek(uint32 seekTo, int64 seekFrame,
									int64* frame, bigtime_t* time);
	virtual status_t			GetNextChunk(const void** chunkData,
									size_t* chunkSize, media_header* mediaHeader);

private:
	status_t					_InitDav1d();
	void						_ShutdownDav1d();
	status_t					_FeedData(const void* data, size_t size,
									int64 startTime, int64 duration);
	status_t					_GetPicture(Dav1dPicture* picture);
	void						_PictureToBuffer(const Dav1dPicture& picture,
									void* outBuffer, media_header* mh);
	color_space					_Dav1dToColorspace(Dav1dPixelLayout layout,
									int bitDepth) const;
	void						_CopyPlane(uint8* dst, int dstStride,
									const uint8* src, int srcStride,
									int width, int height);

	Dav1dContext*				fContext;
	Dav1dSettings				fSettings;

	media_format				fInputFormat;
	media_format				fOutputFormat;

	// Current picture waiting to be consumed
	Dav1dPicture				fPendingPicture;
	bool						fHasPendingPicture;

	// Input buffer management
	const void*					fChunkData;
	size_t						fChunkSize;
	media_header				fChunkHeader;

	std::mutex					fLock;
	std::atomic<bool>			fInitialized;

	// Performance / diagnostics
	uint64						fFramesDecoded;
	bigtime_t					fLastDecodeTime;
};

#endif // DAV1D_DECODER_H
