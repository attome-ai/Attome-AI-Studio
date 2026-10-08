#if defined(ATM_MEDIA_FFMPEG)
// FFmpeg backend (F1 plan, ADR-025): LGPL FFmpeg, loaded as shared libraries; no GPL parts. The same contract as the
// Media Foundation backend: NV12 frames scaled to fit a box, 48 kHz stereo float audio, and H.264 + AAC in MP4 with
// the first encoder that opens (hardware first, then Cisco's OpenH264).

#include "atm/media/media.hpp"
#include "backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <tuple>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "atm/base/profiler.hpp"

namespace atm::media {
namespace {

constexpr AVRational kHns = {1, int(kHnsPerSecond)};

std::string av_text(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof buf);
  return buf;
}

tl::unexpected<Error> av_fail(ErrorCode code, std::string rule, std::string message, int err, std::string hint = {}) {
  if (err < 0)
    message += " (" + av_text(err) + ")";
  return fail(code, std::move(rule), std::move(message), {}, std::move(hint));
}

int even(int v) { return std::max(2, v & ~1); }

struct InputDeleter {
  void operator()(AVFormatContext *c) const { avformat_close_input(&c); }
};
struct CodecDeleter {
  void operator()(AVCodecContext *c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
  void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct PacketDeleter {
  void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct SwsDeleter {
  void operator()(SwsContext *s) const { sws_freeContext(s); }
};
struct SwrDeleter {
  void operator()(SwrContext *s) const { swr_free(&s); }
};
using Input = std::unique_ptr<AVFormatContext, InputDeleter>;
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Sws = std::unique_ptr<SwsContext, SwsDeleter>;
using Swr = std::unique_ptr<SwrContext, SwrDeleter>;

// FFmpeg reports progress notes on stderr by default; keep only real errors.
void quiet() {
  static const bool once = [] {
    av_log_set_level(AV_LOG_ERROR);
    return true;
  }();
  (void)once;
}

Result<Input> open_input(const std::string &path) {
  quiet();
  AVFormatContext *raw = nullptr;
  if (const int err = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); err < 0)
    return av_fail(ErrorCode::NotFound, "M_OPEN", "Cannot open \"" + path + "\".", err,
                   "Check that the file exists and is a video, image or sound file.");
  Input in(raw);
  if (const int err = avformat_find_stream_info(in.get(), nullptr); err < 0)
    return av_fail(ErrorCode::MediaDecodeFailed, "M_OPEN", "Cannot read the streams of \"" + path + "\".", err);
  return in;
}

// The picture stream: cover art in a sound file does not count.
int video_stream(AVFormatContext *in) {
  const int s = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  return s >= 0 && !(in->streams[s]->disposition & AV_DISPOSITION_ATTACHED_PIC) ? s : -1;
}

Result<Codec> open_decoder(AVStream *stream, const std::string &path) {
  const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec)
    return fail(ErrorCode::MediaDecodeFailed, "M_CODEC", "There is no decoder for the " +
                                                             std::string(avcodec_get_name(stream->codecpar->codec_id)) +
                                                             " stream of \"" + path + "\".");
  Codec ctx(avcodec_alloc_context3(codec));
  avcodec_parameters_to_context(ctx.get(), stream->codecpar);
  ctx->pkt_timebase = stream->time_base;
  ctx->thread_count = 0; // as many as useful
  if (const int err = avcodec_open2(ctx.get(), codec, nullptr); err < 0)
    return av_fail(ErrorCode::MediaDecodeFailed, "M_CODEC", "Cannot start the decoder for \"" + path + "\".", err);
  return ctx;
}

// Stream time to 100 ns units since the start of the file, as the Media Foundation backend reports it.
int64_t to_hns(int64_t ts, const AVStream *s, const AVFormatContext *in) {
  const int64_t start = in->start_time != AV_NOPTS_VALUE ? av_rescale_q(in->start_time, AV_TIME_BASE_Q, kHns) : 0;
  return av_rescale_q(ts, s->time_base, kHns) - start;
}

} // namespace

Result<MediaInfo> probe_av(const std::string &path) {
  ATM_PROFILE_SCOPE("media.open");
  ATM_TRY(Input in, open_input(path));
  MediaInfo info;
  if (const int v = video_stream(in.get()); v >= 0) {
    const AVStream *s = in->streams[v];
    info.has_video = s->codecpar->width > 0 && s->codecpar->height > 0;
    info.width = s->codecpar->width;
    info.height = s->codecpar->height;
    AVRational rate = s->avg_frame_rate.num > 0 ? s->avg_frame_rate : s->r_frame_rate;
    if (rate.num <= 0)
      rate = {30, 1};
    info.rate_num = rate.num;
    info.rate_den = rate.den;
  }
  if (const int a = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0); a >= 0) {
    info.has_audio = true;
    info.audio_rate = in->streams[a]->codecpar->sample_rate;
    info.audio_channels = in->streams[a]->codecpar->ch_layout.nb_channels;
  }
  if (in->duration != AV_NOPTS_VALUE)
    info.duration_hns = av_rescale_q(in->duration, AV_TIME_BASE_Q, kHns);
  if (!info.has_video && !info.has_audio)
    return fail(ErrorCode::MediaDecodeFailed, "M_NO_STREAMS", "\"" + path + "\" has no video or audio.");
  return info;
}

// ---- video ---------------------------------------------------------------------------------------------------

struct VideoReader::Impl {
  std::string path;
  Input in;
  Codec dec;
  int stream = -1;
  int width = 0, height = 0; // output, even
  Sws sws;
  Packet pkt{av_packet_alloc()};
  Frame cur{av_frame_alloc()}, pending{av_frame_alloc()};
  bool have_cur = false, have_pending = false, eof = false, draining = false, converted = false;
  int64_t cur_pts = 0, pending_pts = 0;
  std::vector<uint8_t> nv12;
  FrameView view;

  // The next decoded frame into `pending`; at the end of the stream sets `eof`.
  Result<void> read_next() {
    ATM_PROFILE_SCOPE("decode.frame");
    for (;;) {
      const int got = avcodec_receive_frame(dec.get(), pending.get());
      if (got == 0) {
        const int64_t ts = pending->best_effort_timestamp != AV_NOPTS_VALUE ? pending->best_effort_timestamp : pending->pts;
        pending_pts = ts == AV_NOPTS_VALUE ? (have_cur ? cur_pts : 0) : to_hns(ts, in->streams[stream], in.get());
        have_pending = true;
        return {};
      }
      if (got == AVERROR_EOF) {
        eof = true;
        return {};
      }
      if (got != AVERROR(EAGAIN))
        return av_fail(ErrorCode::MediaDecodeFailed, "M_DECODE", "Decoding \"" + path + "\" failed.", got);
      if (draining) {
        eof = true;
        return {};
      }
      // The decoder wants input: the next packet of our stream, or a flush at the end of the file.
      for (;;) {
        const int r = av_read_frame(in.get(), pkt.get());
        if (r < 0) {
          avcodec_send_packet(dec.get(), nullptr);
          draining = true;
          break;
        }
        const bool ours = pkt->stream_index == stream;
        if (ours)
          avcodec_send_packet(dec.get(), pkt.get());
        av_packet_unref(pkt.get());
        if (ours)
          break;
      }
    }
  }

  Result<void> seek(int64_t hns) {
    ATM_PROFILE_SCOPE("decode.seek");
    const AVStream *s = in->streams[stream];
    const int64_t start = in->start_time != AV_NOPTS_VALUE ? av_rescale_q(in->start_time, AV_TIME_BASE_Q, kHns) : 0;
    const int64_t ts = av_rescale_q(hns + start, kHns, s->time_base);
    if (const int err = av_seek_frame(in.get(), stream, ts, AVSEEK_FLAG_BACKWARD); err < 0)
      return av_fail(ErrorCode::MediaDecodeFailed, "M_SEEK", "Seeking in \"" + path + "\" failed.", err);
    avcodec_flush_buffers(dec.get());
    have_cur = have_pending = eof = draining = converted = false;
    return {};
  }

  Result<void> convert() {
    if (converted)
      return {};
    ATM_PROFILE_SCOPE("decode.convert");
    const AVFrame *f = cur.get();
    sws.reset(sws_getCachedContext(sws.release(), f->width, f->height, AVPixelFormat(f->format), width, height,
                                   AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws)
      return fail(ErrorCode::MediaDecodeFailed, "M_CONVERT", "Cannot convert the pictures of \"" + path + "\".");
    uint8_t *dst[4] = {nv12.data(), nv12.data() + size_t(width) * size_t(height), nullptr, nullptr};
    const int pitch[4] = {width, width, 0, 0};
    sws_scale(sws.get(), f->data, f->linesize, 0, f->height, dst, pitch);
    view = {dst[0], dst[1], width, width, width, height};
    converted = true;
    return {};
  }
};

VideoReader::~VideoReader() = default;

// The FFmpeg backend does not read compressed video for another decoder yet.
struct VideoStream::Impl {
  std::string codec;
  std::vector<uint8_t> header;
};
VideoStream::~VideoStream() = default;
Result<std::unique_ptr<VideoStream>> VideoStream::open(const std::string &) {
  return fail(ErrorCode::Unsupported, "M_NO_STREAM", "This build reads compressed video only with Media Foundation.");
}
const std::string &VideoStream::codec() const { return impl_->codec; }
const std::vector<uint8_t> &VideoStream::sequence_header() const { return impl_->header; }
Result<bool> VideoStream::next(Packet &) { return fail(ErrorCode::Unsupported, "M_NO_STREAM", "Not available in this build."); }
Result<void> VideoStream::seek(int64_t) { return fail(ErrorCode::Unsupported, "M_NO_STREAM", "Not available in this build."); }

Result<std::unique_ptr<VideoReader>> VideoReader::open(const std::string &path, int box_width, int box_height) {
  ATM_PROFILE_SCOPE("media.open");
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  ATM_TRY(Input in, open_input(path));
  impl->in = std::move(in);
  impl->stream = video_stream(impl->in.get());
  if (impl->stream < 0)
    return fail(ErrorCode::MediaDecodeFailed, "M_NO_VIDEO", "\"" + path + "\" has no video.");
  ATM_TRY(Codec dec, open_decoder(impl->in->streams[impl->stream], path));
  impl->dec = std::move(dec);
  int w = impl->dec->width, h = impl->dec->height;
  if (box_width > 0 && box_height > 0 && w > 0 && h > 0) // fit inside the box, keep the aspect ratio
    std::tie(w, h) = fit_inside(w, h, box_width, box_height);
  impl->width = even(w);
  impl->height = even(h);
  impl->nv12.resize(nv12_size(impl->width, impl->height));
  std::unique_ptr<VideoReader> reader(new VideoReader);
  reader->impl_ = std::move(impl);
  return reader;
}

Result<FrameView> VideoReader::frame_at(int64_t time) {
  Impl &m = *impl_;
  time = std::max<int64_t>(0, time) + kFrameTimeSlack;
  // Seek when the time is behind the held frame or far ahead of it; otherwise decode forward.
  if (!m.have_cur || time < m.cur_pts || time > m.cur_pts + 2 * kHnsPerSecond) {
    const bool restart = !m.have_cur && !m.have_pending && !m.eof && time < kHnsPerSecond / 2; // a fresh reader near 0
    if (!restart)
      ATM_CHECK(m.seek(time));
  }
  for (;;) {
    if (m.have_pending) {
      if (m.pending_pts > time && m.have_cur)
        break; // `cur` is the frame on screen at `time`
      std::swap(m.cur, m.pending);
      m.cur_pts = m.pending_pts;
      m.have_cur = true;
      m.have_pending = false;
      m.converted = false;
      if (m.cur_pts > time)
        break; // the stream starts after `time`: show its first frame
    } else if (m.eof) {
      break; // hold the last frame
    } else {
      ATM_CHECK(m.read_next());
    }
  }
  if (!m.have_cur)
    return fail(ErrorCode::MediaDecodeFailed, "M_NO_FRAME", "\"" + m.path + "\" has no frame at this time.");
  ATM_CHECK(m.convert());
  return m.view;
}

// ---- audio ---------------------------------------------------------------------------------------------------

Result<std::vector<float>> read_audio(const std::string &path, int64_t start, int64_t duration) {
  ATM_PROFILE_SCOPE("decode.audio");
  ATM_TRY(Input in, open_input(path));
  const int a = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (a < 0)
    return std::vector<float>{}; // no audio in this file
  AVStream *stream = in->streams[a];
  ATM_TRY(Codec dec, open_decoder(stream, path));
  // Everything is resampled to 48 kHz stereo float, interleaved, as the mixer wants it.
  SwrContext *raw = nullptr;
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  if (swr_alloc_set_opts2(&raw, &stereo, AV_SAMPLE_FMT_FLT, kAudioRate, &dec->ch_layout, dec->sample_fmt,
                          dec->sample_rate, 0, nullptr) < 0 ||
      swr_init(raw) < 0) {
    swr_free(&raw);
    return fail(ErrorCode::MediaDecodeFailed, "M_AUDIO", "Cannot convert the sound of \"" + path + "\".");
  }
  Swr swr(raw);
  if (start > 0) {
    const int64_t file_start = in->start_time != AV_NOPTS_VALUE ? av_rescale_q(in->start_time, AV_TIME_BASE_Q, kHns) : 0;
    av_seek_frame(in.get(), a, av_rescale_q(start + file_start, kHns, stream->time_base), AVSEEK_FLAG_BACKWARD);
  }
  const size_t out_frames = size_t(duration * kAudioRate / kHnsPerSecond);
  std::vector<float> out(out_frames * 2, 0.0f);
  Packet pkt(av_packet_alloc());
  Frame frame(av_frame_alloc());
  std::vector<float> buf;
  int64_t first_pts = -1; // hns of the first resampled sample
  size_t produced = 0;    // samples resampled so far
  bool done = false, draining = false;
  // Converted samples go to their place in `out`: sample 0 of `out` is the sound at `start`.
  const auto place = [&](const float *data, int n) {
    for (int i = 0; i < n; ++i, ++produced) {
      const int64_t at = int64_t(produced) + (first_pts - start) * kAudioRate / kHnsPerSecond;
      if (at >= int64_t(out_frames)) {
        done = true;
        return;
      }
      if (at >= 0) {
        out[size_t(at) * 2] = data[i * 2];
        out[size_t(at) * 2 + 1] = data[i * 2 + 1];
      }
    }
  };
  while (!done) {
    const int got = avcodec_receive_frame(dec.get(), frame.get());
    if (got == 0) {
      if (first_pts < 0) {
        const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        first_pts = ts == AV_NOPTS_VALUE ? start : to_hns(ts, stream, in.get());
      }
      const int cap = swr_get_out_samples(swr.get(), frame->nb_samples);
      buf.resize(size_t(std::max(cap, 0)) * 2);
      uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(buf.data())};
      const int n = swr_convert(swr.get(), dst, cap, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
      if (n > 0)
        place(buf.data(), n);
      av_frame_unref(frame.get());
      continue;
    }
    if (got == AVERROR_EOF || (got == AVERROR(EAGAIN) && draining))
      break;
    if (got != AVERROR(EAGAIN))
      return av_fail(ErrorCode::MediaDecodeFailed, "M_AUDIO", "Decoding the sound of \"" + path + "\" failed.", got);
    for (;;) {
      if (av_read_frame(in.get(), pkt.get()) < 0) {
        avcodec_send_packet(dec.get(), nullptr);
        draining = true;
        break;
      }
      const bool ours = pkt->stream_index == a;
      if (ours)
        avcodec_send_packet(dec.get(), pkt.get());
      av_packet_unref(pkt.get());
      if (ours)
        break;
    }
  }
  return out;
}

// ---- encode --------------------------------------------------------------------------------------------------

struct Encoder::Impl {
  EncodeSettings s;
  std::string name;
  AVFormatContext *out = nullptr;
  Codec video, audio;
  AVStream *vs = nullptr, *as = nullptr;
  Frame vframe{av_frame_alloc()}, aframe{av_frame_alloc()};
  Packet pkt{av_packet_alloc()};
  Sws sws; // NV12 -> the encoder's format, when it does not take NV12
  std::vector<float> audio_fifo; // interleaved stereo, not yet a full AAC frame
  int64_t audio_pts = 0;
  bool header = false, finished = false;

  ~Impl() {
    if (out) {
      if (header && !finished)
        av_write_trailer(out); // leave a readable file behind
      if (!(out->oformat->flags & AVFMT_NOFILE))
        avio_closep(&out->pb);
      avformat_free_context(out);
    }
  }

  Result<void> drain(AVCodecContext *enc, AVStream *st) {
    for (;;) {
      const int r = avcodec_receive_packet(enc, pkt.get());
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
        return {};
      if (r < 0)
        return av_fail(ErrorCode::EncoderUnavailable, "M_ENCODE", "Encoding \"" + s.path + "\" failed.", r);
      av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
      pkt->stream_index = st->index;
      if (const int w = av_interleaved_write_frame(out, pkt.get()); w < 0)
        return av_fail(ErrorCode::IoError, "M_WRITE", "Writing \"" + s.path + "\" failed.", w);
    }
  }

  Result<void> send_audio(int samples) {
    AVFrame *f = aframe.get();
    av_frame_unref(f);
    f->nb_samples = samples;
    f->format = audio->sample_fmt;
    av_channel_layout_copy(&f->ch_layout, &audio->ch_layout);
    f->sample_rate = audio->sample_rate;
    if (av_frame_get_buffer(f, 0) < 0)
      return fail(ErrorCode::Internal, "M_ENCODE", "Out of memory while encoding sound.");
    auto *l = reinterpret_cast<float *>(f->data[0]), *r = reinterpret_cast<float *>(f->data[1]); // planar float
    for (int i = 0; i < samples; ++i) {
      l[i] = audio_fifo[size_t(i) * 2];
      r[i] = audio_fifo[size_t(i) * 2 + 1];
    }
    audio_fifo.erase(audio_fifo.begin(), audio_fifo.begin() + ptrdiff_t(samples) * 2);
    f->pts = audio_pts;
    audio_pts += samples;
    if (const int e = avcodec_send_frame(audio.get(), f); e < 0)
      return av_fail(ErrorCode::EncoderUnavailable, "M_ENCODE", "The AAC encoder refused a frame.", e);
    return drain(audio.get(), as);
  }
};

Encoder::~Encoder() = default;

namespace {

// H.264 encoders to try, hardware first. OpenH264 (Cisco, BSD) is the software fallback everywhere.
const char *const kH264[] = {
#if defined(_WIN32)
    "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf",
#elif defined(__APPLE__)
    "h264_videotoolbox",
#else
    "h264_nvenc", "h264_amf",
#endif
    "libopenh264"};

AVPixelFormat pick_format(AVCodecContext *ctx, const AVCodec *codec) {
  const void *list = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || !list)
    return AV_PIX_FMT_YUV420P;
  const auto *fmts = static_cast<const AVPixelFormat *>(list);
  for (int i = 0; i < n; ++i)
    if (fmts[i] == AV_PIX_FMT_NV12)
      return AV_PIX_FMT_NV12;
  for (int i = 0; i < n; ++i)
    if (fmts[i] == AV_PIX_FMT_YUV420P)
      return AV_PIX_FMT_YUV420P;
  return fmts[0];
}

} // namespace

Result<std::unique_ptr<Encoder>> Encoder::create(const EncodeSettings &requested) {
  ATM_PROFILE_SCOPE("encode.open");
  auto m = std::make_unique<Impl>();
  m->s = requested;
  m->s.width = even(m->s.width);
  m->s.height = even(m->s.height);
  const EncodeSettings &s = m->s;
  if (const int err = avformat_alloc_output_context2(&m->out, nullptr, "mp4", s.path.c_str()); err < 0 || !m->out)
    return av_fail(ErrorCode::EncoderUnavailable, "M_ENCODER", "Cannot create \"" + s.path + "\".", err);

  // The first H.264 encoder that opens with these settings. ATTOME_H264_ENCODER names one to use instead (e.g.
  // libopenh264, to test the software path on a machine with a hardware encoder).
  quiet();
  std::vector<const char *> order(std::begin(kH264), std::end(kH264));
  const char *forced = std::getenv("ATTOME_H264_ENCODER");
  if (forced && *forced)
    order = {forced};
  std::string tried;
  for (const char *name : order) {
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec)
      continue;
    Codec ctx(avcodec_alloc_context3(codec));
    ctx->width = s.width;
    ctx->height = s.height;
    ctx->time_base = {int(s.rate_den), int(s.rate_num)};
    ctx->framerate = {int(s.rate_num), int(s.rate_den)};
    ctx->bit_rate = s.bitrate;
    ctx->gop_size = int(2 * s.rate_num / std::max<int64_t>(1, s.rate_den));
    ctx->max_b_frames = 0;
    ctx->pix_fmt = pick_format(ctx.get(), codec);
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (m->out->oformat->flags & AVFMT_GLOBALHEADER)
      ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx.get(), codec, nullptr) == 0) {
      m->video = std::move(ctx);
      m->name = name;
      break;
    }
    tried += std::string(tried.empty() ? "" : ", ") + name;
  }
  if (!m->video)
    return fail(ErrorCode::EncoderUnavailable, "ENCODER_UNAVAILABLE",
                "No H.264 encoder could start" + (tried.empty() ? std::string(".") : " (tried " + tried + ")."), {},
                "Install the GPU driver, or check that the FFmpeg build includes OpenH264.");
  m->vs = avformat_new_stream(m->out, nullptr);
  avcodec_parameters_from_context(m->vs->codecpar, m->video.get());
  m->vs->time_base = m->video->time_base;
  m->vs->avg_frame_rate = m->video->framerate;
  if (m->video->pix_fmt != AV_PIX_FMT_NV12)
    m->sws.reset(sws_getContext(s.width, s.height, AV_PIX_FMT_NV12, s.width, s.height, m->video->pix_fmt, SWS_POINT,
                                nullptr, nullptr, nullptr));

  if (s.audio) {
    const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!aac)
      return fail(ErrorCode::EncoderUnavailable, "ENCODER_UNAVAILABLE", "There is no AAC encoder in this FFmpeg build.");
    Codec ctx(avcodec_alloc_context3(aac));
    ctx->sample_rate = kAudioRate;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx->ch_layout, &stereo);
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    ctx->bit_rate = 192'000;
    ctx->time_base = {1, kAudioRate};
    if (m->out->oformat->flags & AVFMT_GLOBALHEADER)
      ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (const int err = avcodec_open2(ctx.get(), aac, nullptr); err < 0)
      return av_fail(ErrorCode::EncoderUnavailable, "ENCODER_UNAVAILABLE", "The AAC encoder did not start.", err);
    m->audio = std::move(ctx);
    m->as = avformat_new_stream(m->out, nullptr);
    avcodec_parameters_from_context(m->as->codecpar, m->audio.get());
    m->as->time_base = m->audio->time_base;
  }

  if (!(m->out->oformat->flags & AVFMT_NOFILE))
    if (const int err = avio_open(&m->out->pb, s.path.c_str(), AVIO_FLAG_WRITE); err < 0)
      return av_fail(ErrorCode::IoError, "M_WRITE", "Cannot write \"" + s.path + "\".", err,
                     "Check that the folder exists and is writable.");
  if (const int err = avformat_write_header(m->out, nullptr); err < 0)
    return av_fail(ErrorCode::IoError, "M_WRITE", "Cannot write the header of \"" + s.path + "\".", err);
  m->header = true;

  AVFrame *f = m->vframe.get();
  f->format = m->video->pix_fmt;
  f->width = s.width;
  f->height = s.height;
  if (av_frame_get_buffer(f, 0) < 0)
    return fail(ErrorCode::Internal, "M_ENCODE", "Out of memory while starting the encoder.");
  std::unique_ptr<Encoder> enc(new Encoder);
  enc->impl_ = std::move(m);
  return enc;
}

Result<void> Encoder::video(const uint8_t *nv12, int64_t frame) {
  ATM_PROFILE_SCOPE("encode.video");
  Impl &m = *impl_;
  AVFrame *f = m.vframe.get();
  if (av_frame_make_writable(f) < 0)
    return fail(ErrorCode::Internal, "M_ENCODE", "Out of memory while encoding.");
  const int w = m.s.width, h = m.s.height;
  if (m.sws) {
    const uint8_t *src[4] = {nv12, nv12 + size_t(w) * size_t(h), nullptr, nullptr};
    const int pitch[4] = {w, w, 0, 0};
    sws_scale(m.sws.get(), src, pitch, 0, h, f->data, f->linesize);
  } else {
    for (int y = 0; y < h; ++y)
      std::memcpy(f->data[0] + size_t(y) * size_t(f->linesize[0]), nv12 + size_t(y) * size_t(w), size_t(w));
    const uint8_t *uv = nv12 + size_t(w) * size_t(h);
    for (int y = 0; y < h / 2; ++y)
      std::memcpy(f->data[1] + size_t(y) * size_t(f->linesize[1]), uv + size_t(y) * size_t(w), size_t(w));
  }
  f->pts = frame;
  if (const int e = avcodec_send_frame(m.video.get(), f); e < 0)
    return av_fail(ErrorCode::EncoderUnavailable, "M_ENCODE", "The H.264 encoder refused frame " + std::to_string(frame) + ".", e);
  return m.drain(m.video.get(), m.vs);
}

Result<void> Encoder::audio(const float *stereo, size_t frames) {
  ATM_PROFILE_SCOPE("encode.audio");
  Impl &m = *impl_;
  if (!m.audio)
    return {};
  m.audio_fifo.insert(m.audio_fifo.end(), stereo, stereo + frames * 2);
  const int size = m.audio->frame_size > 0 ? m.audio->frame_size : 1024;
  while (m.audio_fifo.size() >= size_t(size) * 2)
    ATM_CHECK(m.send_audio(size));
  return {};
}

Result<void> Encoder::finish() {
  ATM_PROFILE_SCOPE("encode.finish");
  Impl &m = *impl_;
  if (m.audio) {
    if (!m.audio_fifo.empty()) // the last, shorter frame
      ATM_CHECK(m.send_audio(int(m.audio_fifo.size() / 2)));
    avcodec_send_frame(m.audio.get(), nullptr);
    ATM_CHECK(m.drain(m.audio.get(), m.as));
  }
  avcodec_send_frame(m.video.get(), nullptr);
  ATM_CHECK(m.drain(m.video.get(), m.vs));
  if (const int e = av_write_trailer(m.out); e < 0)
    return av_fail(ErrorCode::IoError, "M_WRITE", "Finishing \"" + m.s.path + "\" failed.", e);
  m.finished = true;
  return {};
}

const std::string &Encoder::name() const { return impl_->name; }

} // namespace atm::media
#endif
