# video_echo
Video frame capture tool using V4L2 Linux API. Useful for debugging camera interfaces.

## Features:
- Enumerate formats, frame sizes and framerates.
- Set/try given format. Works both for single and multi plane formats.
- Capture using given format. Only single plane formats supported at the moment.
- Display image to LCD using framebuffer API. Convert YUV to RGB on the fly (quite slow).
