# Overlay of vcpkg's x64-windows-static-md: everything static except FFmpeg and its codecs, which are built as DLLs.
# FFmpeg is LGPL (ADR-025); linking it dynamically keeps it replaceable by the user, as the LGPL asks.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT MATCHES "^(ffmpeg|openh264)$")
  set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
