#if defined(_WIN32)
// Still images for agents (see.frames, see.contact_sheet), written with the Windows Imaging Component.

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::media {

using Microsoft::WRL::ComPtr;

Result<void> write_jpeg(const std::string &path, const uint8_t *bgrx, int width, int height, float quality) {
  ATM_PROFILE_SCOPE("image.jpeg");
  thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com;
  const auto failed = [&](const char *step) {
    return fail(ErrorCode::IoError, "M_IMAGE_WRITE", std::string("Could not write the image \"") + path + "\" (" +
                                                         step + ").");
  };
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
    return failed("imaging factory");
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), int(path.size()), nullptr, 0);
  std::wstring wide(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.data(), int(path.size()), wide.data(), n);

  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)))
    return failed("open");
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, &options)))
    return failed("encoder");
  PROPBAG2 option{};
  option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_R4;
  value.fltVal = quality;
  options->Write(1, &option, &value);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
  if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(UINT(width), UINT(height))) ||
      FAILED(frame->SetPixelFormat(&format)))
    return failed("frame");
  // The JPEG encoder takes 24-bit BGR; WIC converts when the frame is written from a bitmap source.
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmapFromMemory(UINT(width), UINT(height), GUID_WICPixelFormat32bppBGR, UINT(width) * 4,
                                         UINT(width) * UINT(height) * 4, const_cast<BYTE *>(bgrx), &bitmap)) ||
      FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return failed("encode");
  return {};
}

} // namespace atm::media
#endif
