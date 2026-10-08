#if defined(_WIN32) && !defined(ATM_MEDIA_FFMPEG)
// Media Foundation backend.

#include "atm/media/media.hpp"
#include "backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <codecapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include "atm/base/parallel.hpp"
#include "atm/base/profiler.hpp"

namespace atm::media {
namespace {

using Microsoft::WRL::ComPtr;

// COM on this thread and Media Foundation in this process. Never shut down: readers may live until exit.
void ensure_started() {
  thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com; // S_FALSE and RPC_E_CHANGED_MODE both mean COM is usable here
  static std::once_flag once;
  std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_FULL); });
}

std::wstring widen(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

tl::unexpected<Error> media_error(ErrorCode code, const char *rule, std::string message, HRESULT hr,
                                  std::string hint = {}) {
  Error e;
  e.code = code;
  e.rule = rule;
  e.message = std::move(message);
  e.hint = std::move(hint);
  char hex[16];
  std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(hr));
  e.details = {{"hresult", hex}};
  return tl::unexpected(std::move(e));
}

tl::unexpected<Error> open_error(const std::string &path, HRESULT hr) {
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
    return media_error(ErrorCode::NotFound, "M_NOT_FOUND", "The media file \"" + path + "\" does not exist.", hr,
                       "Check the path, or relink the clip to the file's new place.");
  return media_error(ErrorCode::MediaDecodeFailed, "M_OPEN", "Windows cannot decode \"" + path + "\".", hr,
                     "Use an MP4 or MOV file with H.264 or HEVC video, or install the codec for this format.");
}

enum class Gpu { decode, encode };
IMFDXGIDeviceManager *gpu_manager(Gpu use);

Result<ComPtr<IMFSourceReader>> open_reader(const std::string &path, bool video_processing, bool gpu = false) {
  ensure_started();
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 3);
  if (video_processing) {
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE); // colour conversion and scaling
    if (gpu && gpu_manager(Gpu::decode)) { // decode and scale on the GPU
      attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu_manager(Gpu::decode));
      attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
  }
  ComPtr<IMFSourceReader> reader;
  const HRESULT hr = MFCreateSourceReaderFromURL(widen(path).c_str(), attrs.Get(), &reader);
  if (FAILED(hr))
    return open_error(path, hr);
  return reader;
}

HRESULT seek(IMFSourceReader *reader, int64_t hns) {
  PROPVARIANT var;
  PropVariantInit(&var);
  var.vt = VT_I8;
  var.hVal.QuadPart = hns;
  return reader->SetCurrentPosition(GUID_NULL, var);
}

int even(int v) { return std::max(2, v & ~1); }

// The D3D11 device of the hardware encoder (and of the GPU decoder when it is switched on). With it the encoder
// works on GPU surfaces; without it the hardware encoder still runs, behind a slower system-memory path.
ComPtr<IMFDXGIDeviceManager> make_gpu_manager() {
  {
    ComPtr<IMFDXGIDeviceManager> m;
    ComPtr<ID3D11Device> device;
    UINT token = 0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, nullptr)))
      return m;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt)))
      mt->SetMultithreadProtected(TRUE);
    if (FAILED(MFCreateDXGIDeviceManager(&token, &m)) || FAILED(m->ResetDevice(device.Get(), token)))
      m.Reset();
    return m;
  }
}

IMFDXGIDeviceManager *gpu_manager(Gpu use) {
  (void)use; // one device: a second one for the decoder was measured slower (19 ms per 4K frame copy against 1.5-11)
  static const ComPtr<IMFDXGIDeviceManager> manager = make_gpu_manager();
  return manager.Get();
}

void tag_bt709(IMFMediaType *type) {
  type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

} // namespace

Result<MediaInfo> probe_av(const std::string &path) {
  ATM_PROFILE_SCOPE("media.open");
  ATM_TRY(ComPtr<IMFSourceReader> reader, open_reader(path, false));
  MediaInfo info;
  ComPtr<IMFMediaType> type;
  if (SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &type))) {
    UINT32 w = 0, h = 0, n = 0, d = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d);
    info.has_video = w > 0 && h > 0;
    info.width = int(w);
    info.height = int(h);
    info.rate_num = n ? n : 30;
    info.rate_den = d ? d : 1;
  }
  type.Reset();
  if (SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &type))) {
    info.has_audio = true;
    info.audio_rate = int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
    info.audio_channels = int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
  }
  PROPVARIANT var;
  PropVariantInit(&var);
  if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
      var.vt == VT_UI8)
    info.duration_hns = int64_t(var.uhVal.QuadPart);
  PropVariantClear(&var);
  if (!info.has_video && !info.has_audio)
    return media_error(ErrorCode::MediaDecodeFailed, "M_NO_STREAMS", "\"" + path + "\" has no video or audio.", S_OK);
  return info;
}

// ---- video ---------------------------------------------------------------------------------------------------

struct VideoReader::Impl {
  std::string path;
  ComPtr<IMFSourceReader> reader;
  int width = 0, height = 0, stride = 0; // stride and plane height of the delivered buffers
  int surface_height = 0;
  ComPtr<IMFSample> cur, pending;
  int64_t cur_pts = 0, pending_pts = 0;
  bool eof = false;
  // The locked buffer of `cur`.
  ComPtr<IMFMediaBuffer> buffer;
  ComPtr<IMF2DBuffer> buffer2d;
  FrameView view;

  ~Impl() { unlock(); }

  void unlock() {
    if (buffer2d)
      buffer2d->Unlock2D();
    else if (buffer)
      buffer->Unlock();
    buffer.Reset();
    buffer2d.Reset();
    view = {};
  }

  void read_format() {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)))
      return;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
    surface_height = int(h);
    MFVideoArea area{}; // decoders pad 1080 to 1088; the picture is the aperture
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof area, nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
      w = UINT32(area.Area.cx);
      h = UINT32(area.Area.cy);
    }
    width = int(w) & ~1;
    height = int(h) & ~1;
    stride = int(INT32(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, w)));
  }

  Result<void> read_next() {
    ATM_PROFILE_SCOPE("decode.read");
    for (;;) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      const HRESULT hr =
          reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &sample);
      if (FAILED(hr))
        return media_error(ErrorCode::MediaDecodeFailed, "M_DECODE", "Decoding \"" + path + "\" failed.", hr);
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        read_format();
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        eof = true;
        return {};
      }
      if (sample) {
        pending = std::move(sample);
        pending_pts = ts;
        return {};
      }
    }
  }
};

VideoReader::~VideoReader() = default;

Result<std::unique_ptr<VideoReader>> VideoReader::open(const std::string &path, int box_width, int box_height) {
  ATM_PROFILE_SCOPE("decode.open");
  ATM_TRY(ComPtr<IMFSourceReader> reader, open_reader(path, true));
  ComPtr<IMFMediaType> native;
  HRESULT hr = reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
  if (FAILED(hr))
    return media_error(ErrorCode::MediaDecodeFailed, "M_NO_VIDEO", "\"" + path + "\" has no video.", hr);
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
  // The software decoder is the default. Measured with atm_bench --export (RTX 5090, 2 tracks): it gives a steady
  // 15x real time at 1080p and 3.4x at 4K. The GPU decoder reached 5.7x at 4K in some runs and 0.6-1x in others,
  // and 9x at 1080p, because every frame is copied back to system memory and that copy stalls on the GPU. GPU
  // compositing (F1 M12) removes the copy; until then ATTOME_DECODE=gpu switches the GPU decoder on to try it.
  const char *force = std::getenv("ATTOME_DECODE");
  const bool gpu = force && std::strcmp(force, "gpu") == 0;
  if (gpu) {
    native.Reset();
    ATM_TRY(ComPtr<IMFSourceReader> gpu_reader, open_reader(path, true, true));
    reader = std::move(gpu_reader);
  }
  if (box_width > 0 && box_height > 0 && w > 0 && h > 0) { // fit inside the box, keep the aspect ratio
    const auto [fw, fh] = fit_inside(int(w), int(h), box_width, box_height);
    w = UINT32(fw);
    h = UINT32(fh);
  }
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
  ComPtr<IMFMediaType> type;
  MFCreateMediaType(&type);
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  tag_bt709(type.Get());
  MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
  if (FAILED(hr))
    return open_error(path, hr);
  std::unique_ptr<VideoReader> out(new VideoReader);
  out->impl_ = std::make_unique<Impl>();
  out->impl_->path = path;
  out->impl_->reader = std::move(reader);
  out->impl_->read_format();
  return out;
}

Result<FrameView> VideoReader::frame_at(int64_t time) {
  Impl &m = *impl_;
  time = std::max<int64_t>(0, time) + kFrameTimeSlack;
  // Seek when the time is behind the held frame or far ahead of it; otherwise decode forward.
  if (!m.cur || time < m.cur_pts || time > m.cur_pts + 2 * kHnsPerSecond) {
    const bool restart = !m.cur && !m.pending && !m.eof && time < kHnsPerSecond / 2; // a fresh reader near 0
    if (!restart) {
      ATM_PROFILE_SCOPE("decode.seek");
      m.unlock();
      m.cur.Reset();
      m.pending.Reset();
      m.eof = false;
      if (const HRESULT hr = seek(m.reader.Get(), time); FAILED(hr))
        return media_error(ErrorCode::MediaDecodeFailed, "M_SEEK", "Seeking in \"" + m.path + "\" failed.", hr);
    }
  }
  for (;;) {
    if (m.pending) {
      if (m.pending_pts > time && m.cur)
        break; // `cur` is the frame on screen at `time`
      m.unlock();
      m.cur = std::move(m.pending);
      m.cur_pts = m.pending_pts;
      m.pending.Reset();
      if (m.cur_pts > time)
        break; // the stream starts after `time`: show its first frame
    } else if (m.eof) {
      break; // hold the last frame
    } else {
      ATM_CHECK(m.read_next());
    }
  }
  if (!m.cur)
    return media_error(ErrorCode::MediaDecodeFailed, "M_NO_FRAME", "\"" + m.path + "\" has no frame at this time.",
                       S_OK);
  if (!m.view.y) {
    ATM_PROFILE_SCOPE("decode.lock");
    HRESULT hr = m.cur->GetBufferByIndex(0, &m.buffer);
    if (FAILED(hr))
      return media_error(ErrorCode::MediaDecodeFailed, "M_DECODE", "Decoding \"" + m.path + "\" failed.", hr);
    BYTE *data = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    ComPtr<IMF2DBuffer2> buffer2;
    BYTE *start = nullptr;
    if (SUCCEEDED(m.buffer.As(&buffer2)) &&
        SUCCEEDED(buffer2->Lock2DSize(MF2DBuffer_LockFlags_Read, &data, &pitch, &start, &length))) {
      m.buffer2d = buffer2;
    } else {
      hr = m.buffer->Lock(&data, nullptr, &length);
      if (FAILED(hr))
        return media_error(ErrorCode::MediaDecodeFailed, "M_DECODE", "Decoding \"" + m.path + "\" failed.", hr);
      pitch = m.stride;
    }
    if (pitch <= 0)
      return media_error(ErrorCode::MediaDecodeFailed, "M_LAYOUT", "\"" + m.path + "\" decodes to an unexpected layout.", S_OK);
    // The UV plane follows the Y plane, whose height may be padded (1088 rows for a 1080-row picture).
    int plane_rows = std::max(m.surface_height, m.height);
    if (length > 0 && int(length / DWORD(pitch)) * 2 / 3 > plane_rows && int(length / DWORD(pitch)) * 2 / 3 < plane_rows + 64)
      plane_rows = int(length / DWORD(pitch)) * 2 / 3;
    m.view = FrameView{data, data + size_t(pitch) * size_t(plane_rows), int(pitch), int(pitch), m.width, m.height};
  }
  return m.view;
}

// ---- compressed video, for a decoder of our own -----------------------------------------------------------------

struct VideoStream::Impl {
  std::string path, codec;
  std::vector<uint8_t> header;
  ComPtr<IMFSourceReader> reader;
};

VideoStream::~VideoStream() = default;

Result<std::unique_ptr<VideoStream>> VideoStream::open(const std::string &path) {
  ATM_PROFILE_SCOPE("stream.open");
  ATM_TRY(ComPtr<IMFSourceReader> reader, open_reader(path, false));
  const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  ComPtr<IMFMediaType> native;
  HRESULT hr = reader->GetNativeMediaType(video, 0, &native);
  if (FAILED(hr))
    return media_error(ErrorCode::MediaDecodeFailed, "M_NO_VIDEO", "\"" + path + "\" has no video.", hr);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(video, TRUE);
  hr = reader->SetCurrentMediaType(video, nullptr, native.Get()); // as stored: no decoder in between
  if (FAILED(hr))
    return open_error(path, hr);
  GUID subtype{};
  native->GetGUID(MF_MT_SUBTYPE, &subtype);
  std::unique_ptr<VideoStream> out(new VideoStream);
  out->impl_ = std::make_unique<Impl>();
  Impl &m = *out->impl_;
  m.path = path;
  m.reader = std::move(reader);
  m.codec = subtype == MFVideoFormat_H264 ? "h264" : subtype == MFVideoFormat_HEVC ? "hevc" : "other";
  UINT32 size = 0;
  if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
    m.header.resize(size);
    native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m.header.data(), size, nullptr);
  }
  return out;
}

const std::string &VideoStream::codec() const { return impl_->codec; }
const std::vector<uint8_t> &VideoStream::sequence_header() const { return impl_->header; }

Result<bool> VideoStream::next(Packet &packet) {
  ATM_PROFILE_SCOPE("stream.read");
  Impl &m = *impl_;
  for (;;) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    const HRESULT hr = m.reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &sample);
    if (FAILED(hr))
      return media_error(ErrorCode::MediaDecodeFailed, "M_READ", "Reading \"" + m.path + "\" failed.", hr);
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
      return false;
    if (!sample)
      continue;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
      continue;
    BYTE *data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)))
      continue;
    packet.data.assign(data, data + length);
    buffer->Unlock();
    packet.pts = ts;
    packet.key = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != 0;
    return true;
  }
}

Result<void> VideoStream::seek(int64_t time) {
  if (const HRESULT hr = ::atm::media::seek(impl_->reader.Get(), std::max<int64_t>(0, time)); FAILED(hr))
    return media_error(ErrorCode::MediaDecodeFailed, "M_SEEK", "Seeking in \"" + impl_->path + "\" failed.", hr);
  return {};
}

// ---- audio ---------------------------------------------------------------------------------------------------

Result<std::vector<float>> read_audio(const std::string &path, int64_t start, int64_t duration) {
  ATM_PROFILE_SCOPE("decode.audio");
  ATM_TRY(ComPtr<IMFSourceReader> reader, open_reader(path, false));
  const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
  ComPtr<IMFMediaType> type;
  if (FAILED(reader->GetNativeMediaType(stream, 0, &type)))
    return std::vector<float>{}; // no audio in this file
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(stream, TRUE);
  type.Reset();
  MFCreateMediaType(&type);
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  HRESULT hr = reader->SetCurrentMediaType(stream, nullptr, type.Get());
  type.Reset();
  if (SUCCEEDED(hr))
    hr = reader->GetCurrentMediaType(stream, &type);
  if (FAILED(hr))
    return media_error(ErrorCode::MediaDecodeFailed, "M_AUDIO", "Windows cannot decode the audio of \"" + path + "\".",
                       hr);
  const int rate = int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
  const int channels = std::max(1, int(MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2)));
  if (start > 0)
    seek(reader.Get(), start);

  // Decoded PCM of the range, in the file's own rate, as stereo float.
  std::vector<float> pcm;
  pcm.reserve(size_t(duration * rate / kHnsPerSecond + rate) * 2);
  int64_t first_pts = -1;
  for (;;) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
      break;
    if (!sample)
      continue;
    if (first_pts < 0)
      first_pts = ts;
    if (ts >= start + duration)
      break;
    ComPtr<IMFMediaBuffer> buffer;
    BYTE *data = nullptr;
    DWORD bytes = 0;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &bytes)))
      break;
    const auto *in = reinterpret_cast<const int16_t *>(data);
    const size_t frames = bytes / (2 * size_t(channels));
    for (size_t i = 0; i < frames; ++i) {
      const float l = float(in[i * size_t(channels)]) / 32768.0f;
      const float r = channels > 1 ? float(in[i * size_t(channels) + 1]) / 32768.0f : l;
      pcm.push_back(l);
      pcm.push_back(r);
    }
    buffer->Unlock();
  }
  if (first_pts < 0)
    return std::vector<float>{};

  // Linear resample to 48 kHz, aligned so that output frame 0 is the sound at `start`.
  const size_t in_frames = pcm.size() / 2;
  const size_t out_frames = size_t(duration * kAudioRate / kHnsPerSecond);
  const double offset = double(start - first_pts) * rate / double(kHnsPerSecond); // input frames to skip
  const double step = double(rate) / double(kAudioRate);
  std::vector<float> out(out_frames * 2, 0.0f);
  for (size_t i = 0; i < out_frames; ++i) {
    const double pos = offset + double(i) * step;
    if (pos < 0.0)
      continue;
    const size_t a = size_t(pos);
    if (a + 1 >= in_frames)
      break;
    const float t = float(pos - double(a));
    out[i * 2] = pcm[a * 2] + (pcm[a * 2 + 2] - pcm[a * 2]) * t;
    out[i * 2 + 1] = pcm[a * 2 + 1] + (pcm[a * 2 + 3] - pcm[a * 2 + 1]) * t;
  }
  return out;
}

// ---- encode --------------------------------------------------------------------------------------------------


struct Encoder::Impl {
  EncodeSettings settings;
  ComPtr<IMFSinkWriter> writer;
  // Frame buffers are reused: a fresh 3-12 MB allocation per frame costs page faults every frame.
  std::vector<ComPtr<IMFMediaBuffer>> pool;

  IMFMediaBuffer *free_buffer(DWORD bytes) {
    for (const auto &b : pool) { // the encoder holds a reference while it still reads a buffer
      b->AddRef();
      if (b->Release() == 1)
        return b.Get();
    }
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(MFCreateMemoryBuffer(bytes, &b)))
      return nullptr;
    pool.push_back(b);
    return b.Get();
  }
  DWORD video_stream = 0, audio_stream = 0;
  int64_t audio_frames = 0;
  bool finished = false;
  std::string name;
};

Encoder::~Encoder() = default;

Result<std::unique_ptr<Encoder>> Encoder::create(const EncodeSettings &requested) {
  ATM_PROFILE_SCOPE("encode.open");
  ensure_started();
  EncodeSettings s = requested;
  s.width = even(s.width);
  s.height = even(s.height);
  const std::wstring path = widen(s.path);
  const auto fail_hr = [&](HRESULT hr) {
    return media_error(ErrorCode::EncoderUnavailable, "M_ENCODER",
                       "Could not start the H.264 encoder for \"" + s.path + "\".", hr,
                       "Check that the folder exists and is writable, and that the size is at most 4096 x 2304.");
  };

  std::unique_ptr<Encoder> out(new Encoder);
  out->impl_ = std::make_unique<Impl>();
  Impl &m = *out->impl_;
  m.settings = s;
  HRESULT hr = E_FAIL;
  for (const BOOL hardware : {TRUE, FALSE}) { // GPU encoder first, the software encoder when there is none
    if (hardware && s.b_frames > 0)
      continue; // the GPU encoders ignore the B-frame count
    m.writer.Reset();
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (hardware && gpu_manager(Gpu::encode))
      attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, gpu_manager(Gpu::encode));
    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &m.writer);
    if (FAILED(hr))
      continue;

    ComPtr<IMFMediaType> vout, vin;
    MFCreateMediaType(&vout);
    vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vout->SetUINT32(MF_MT_AVG_BITRATE, UINT32(s.bitrate));
    vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (s.b_frames > 0) // Baseline, the default, has no B-frames
      vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    tag_bt709(vout.Get());
    MFSetAttributeSize(vout.Get(), MF_MT_FRAME_SIZE, UINT32(s.width), UINT32(s.height));
    MFSetAttributeRatio(vout.Get(), MF_MT_FRAME_RATE, UINT32(s.rate_num), UINT32(s.rate_den));
    MFSetAttributeRatio(vout.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m.writer->AddStream(vout.Get(), &m.video_stream);
    if (FAILED(hr))
      continue;
    MFCreateMediaType(&vin);
    vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); // the encoder's own format: no converter in between
    vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vin->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(s.width));
    tag_bt709(vin.Get());
    MFSetAttributeSize(vin.Get(), MF_MT_FRAME_SIZE, UINT32(s.width), UINT32(s.height));
    MFSetAttributeRatio(vin.Get(), MF_MT_FRAME_RATE, UINT32(s.rate_num), UINT32(s.rate_den));
    MFSetAttributeRatio(vin.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // Ask the encoder for its fastest setting; at export bitrates the quality difference is small.
    ComPtr<IMFAttributes> params;
    MFCreateAttributes(&params, 3);
    params->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, UINT32(s.quality_vs_speed));
    if (s.b_frames > 0)
      params->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, UINT32(s.b_frames));
    params->SetUINT32(CODECAPI_AVEncMPVGOPSize, UINT32(std::max<int64_t>(1, s.rate_num / s.rate_den) * 2));
    hr = m.writer->SetInputMediaType(m.video_stream, vin.Get(), params.Get());
    if (FAILED(hr))
      continue;

    if (s.audio) {
      ComPtr<IMFMediaType> aout, ain;
      MFCreateMediaType(&aout);
      aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
      aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
      aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
      hr = m.writer->AddStream(aout.Get(), &m.audio_stream);
      if (FAILED(hr))
        continue;
      MFCreateMediaType(&ain);
      ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
      ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
      ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
      ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
      ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
      ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
      ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * 4);
      hr = m.writer->SetInputMediaType(m.audio_stream, ain.Get(), nullptr);
      if (FAILED(hr))
        continue;
    }
    hr = m.writer->BeginWriting();
    if (SUCCEEDED(hr))
      break;
  }
  if (SUCCEEDED(hr)) { // which encoder runs, for the job report and the profiler
    ComPtr<IMFSinkWriterEx> ex;
    if (SUCCEEDED(m.writer.As(&ex)))
      for (DWORD i = 0;; ++i) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(ex->GetTransformForStream(m.video_stream, i, &category, &transform)))
          break;
        ComPtr<IMFAttributes> attributes;
        wchar_t friendly[256] = {};
        if (category == MFT_CATEGORY_VIDEO_ENCODER && SUCCEEDED(transform->GetAttributes(&attributes)) &&
            SUCCEEDED(attributes->GetString(MFT_FRIENDLY_NAME_Attribute, friendly, 256, nullptr))) {
          char utf8[512] = {};
          WideCharToMultiByte(CP_UTF8, 0, friendly, -1, utf8, sizeof utf8, nullptr, nullptr);
          m.name = utf8;
        }
      }
    if (m.name.empty())
      m.name = "Microsoft H.264 encoder";
  }
  if (FAILED(hr))
    return fail_hr(hr);
  return out;
}

Result<void> Encoder::video(const uint8_t *nv12, int64_t frame) {
  ATM_PROFILE_SCOPE("encode.video");
  Impl &m = *impl_;
  const EncodeSettings &s = m.settings;
  const DWORD bytes = DWORD(nv12_size(s.width, s.height));
  IMFMediaBuffer *buffer = m.free_buffer(bytes);
  ComPtr<IMFSample> sample;
  BYTE *data = nullptr;
  HRESULT hr = buffer ? buffer->Lock(&data, nullptr, nullptr) : E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    ATM_PROFILE_SCOPE("encode.copy");
    parallel_for(bytes / 65536 + 1, 1, [&](int64_t first, int64_t last) {
      const size_t a = size_t(first) * 65536, b = std::min<size_t>(size_t(last) * 65536, bytes);
      std::memcpy(data + a, nv12 + a, b - a);
    });
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    hr = MFCreateSample(&sample);
  }
  if (SUCCEEDED(hr)) {
    sample->AddBuffer(buffer);
    const int64_t t0 = frame * s.rate_den * kHnsPerSecond / s.rate_num;
    const int64_t t1 = (frame + 1) * s.rate_den * kHnsPerSecond / s.rate_num;
    sample->SetSampleTime(t0);
    sample->SetSampleDuration(t1 - t0);
    hr = m.writer->WriteSample(m.video_stream, sample.Get());
  }
  if (FAILED(hr))
    return media_error(ErrorCode::EncoderUnavailable, "M_ENCODE", "Encoding a video frame failed.", hr);
  return {};
}

Result<void> Encoder::audio(const float *stereo, size_t frames) {
  ATM_PROFILE_SCOPE("encode.audio");
  Impl &m = *impl_;
  if (!m.settings.audio || frames == 0)
    return {};
  const DWORD bytes = DWORD(frames * 4);
  ComPtr<IMFMediaBuffer> buffer;
  ComPtr<IMFSample> sample;
  BYTE *data = nullptr;
  HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
  if (SUCCEEDED(hr))
    hr = buffer->Lock(&data, nullptr, nullptr);
  if (SUCCEEDED(hr)) {
    auto *pcm = reinterpret_cast<int16_t *>(data);
    for (size_t i = 0; i < frames * 2; ++i)
      pcm[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    hr = MFCreateSample(&sample);
  }
  if (SUCCEEDED(hr)) {
    sample->AddBuffer(buffer.Get());
    const int64_t t0 = m.audio_frames * kHnsPerSecond / kAudioRate;
    const int64_t t1 = (m.audio_frames + int64_t(frames)) * kHnsPerSecond / kAudioRate;
    sample->SetSampleTime(t0);
    sample->SetSampleDuration(t1 - t0);
    hr = m.writer->WriteSample(m.audio_stream, sample.Get());
    m.audio_frames += int64_t(frames);
  }
  if (FAILED(hr))
    return media_error(ErrorCode::EncoderUnavailable, "M_ENCODE", "Encoding audio failed.", hr);
  return {};
}

const std::string &Encoder::name() const { return impl_->name; }

Result<void> Encoder::finish() {
  ATM_PROFILE_SCOPE("encode.finish");
  Impl &m = *impl_;
  if (m.finished)
    return {};
  m.finished = true;
  if (const HRESULT hr = m.writer->Finalize(); FAILED(hr))
    return media_error(ErrorCode::EncoderUnavailable, "M_ENCODE", "Finishing the video file failed.", hr);
  return {};
}

} // namespace atm::media

#endif // _WIN32
