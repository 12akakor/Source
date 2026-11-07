/*
 * dav1d_decoder.cpp
 *
 * Media Kit add-on entry point for the libdav1d AV1 decoder.
 *
 * Copyright (c) 2026 RenkuOS contributors
 * SPDX-License-Identifier: MIT
 */

#include "Dav1dDecoder.h"

#include <media/DecoderPlugin.h>
#include <media/MediaFormats.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Media add-on class
// ---------------------------------------------------------------------------

class Dav1dDecoderAddon : public DecoderAddon {
public:
								Dav1dDecoderAddon(image_id image);
	virtual						~Dav1dDecoderAddon();

	virtual	status_t			GetCodecInfo(media_codec_info* info, int32 index);
	virtual	Decoder*			InstantiateDecoder();
	virtual	status_t			RegisterFormats();
};


Dav1dDecoderAddon::Dav1dDecoderAddon(image_id image)
	:
	DecoderAddon(image)
{
}


Dav1dDecoderAddon::~Dav1dDecoderAddon()
{
}


status_t
Dav1dDecoderAddon::GetCodecInfo(media_codec_info* info, int32 index)
{
	if (info == nullptr || index != 0)
		return B_BAD_INDEX;

	strlcpy(info->pretty_name, "AV1 (libdav1d)", sizeof(info->pretty_name));
	strlcpy(info->short_name, "dav1d", sizeof(info->short_name));
	info->id = 0;
	info->sub_id = 0;

	return B_OK;
}


Decoder*
Dav1dDecoderAddon::InstantiateDecoder()
{
	return new (std::nothrow) Dav1dDecoder();
}


status_t
Dav1dDecoderAddon::RegisterFormats()
{
	// Register the formats we can decode.
	// The Media Kit will call this during add-on loading.

	media_format format;
	media_format_description description;

	// --- video/av1 ---
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_ENCODED_VIDEO;
	format.u.encoded_video = media_encoded_video_format::wildcard;

	memset(&description, 0, sizeof(description));
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'av01';
	description.u.misc.codec = 'av01';

	BMediaFormats mediaFormats;
	status_t err = mediaFormats.MakeFormatFor(description, 1, &format);
	if (err != B_OK) {
		fprintf(stderr, "Dav1dDecoderAddon: MakeFormatFor(av01) failed: %s\n",
			strerror(err));
	}

	// Also register common MIME types via the encoding family
	// (the system will map video/av1 and video/x-av1 to the above)

	return B_OK;
}


// ---------------------------------------------------------------------------
// Add-on entry point (required by Media Kit)
// ---------------------------------------------------------------------------

MediaAddon*
make_media_addon(image_id image)
{
	return new (std::nothrow) Dav1dDecoderAddon(image);
}
