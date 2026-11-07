# dav1d Media Kit Decoder for RenkuOS

Native AV1 video decoder add-on for the RenkuOS Media Kit, powered by [libdav1d](https://code.videolan.org/videolan/dav1d).

## Features

- Full AV1 support (all profiles, bit depths 8/10/12, 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4)
- Low-latency configuration (`max_frame_delay = 1`)
- Film grain synthesis enabled
- Clean integration with `BMediaDecoder` / Media Kit
- Proper seeking (flush + keyframe restart)
- Thread-safe

## Requirements

- RenkuOS (or compatible Haiku-derived system)
- `libdav1d` and `libdav1d_devel` packages installed
- Media Kit development headers

## Building

```bash
# From the RenkuOS source tree
cd src/add-ons/media/plugins/dav1d_decoder
jam -q dav1d_decoder
```

Or integrate into the main build:

```bash
jam -q add-ons/media/plugins/dav1d_decoder
```

## Installation

The add-on is installed to:

```
/boot/system/add-ons/media/plugins/dav1d_decoder
```

After installation, restart the media_server (or reboot):

```bash
kill -9 media_server   # or use the Media preference panel
```

## Supported MIME types / formats

- `video/av1`
- `video/x-av1`
- FourCC `av01`

## Architecture notes

The decoder inherits from the Media Kit `Decoder` base class and implements:

- `Setup()`
- `NegotiateOutputFormat()`
- `Decode()`
- `Seek()`

Encoded data is obtained via `GetNextChunk()`. Decoded frames are delivered as planar YUV (`B_YUV420` / `B_YUV422` / `B_YUV444`).

## Future improvements

- Zero-copy path when possible
- Proper 10-bit / 12-bit Media Kit support
- Hardware acceleration fallback (when available on RenkuOS)
- Better timestamp handling from container

## License

MIT (the add-on code).  
libdav1d itself is BSD-2-Clause.
