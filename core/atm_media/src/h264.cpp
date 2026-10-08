// H.264 headers (ITU-T H.264, sections 7.3 and 8.2.1): the parts a GPU decoder needs from the program.
#include "atm/media/h264.hpp"

#include <algorithm>

namespace atm::media::h264 {
namespace {

// Reads bits of an RBSP, most significant first. Reading past the end gives zeros and marks the reader as overrun, so a
// damaged header is noticed once, at the end, not at every read.
class Bits {
public:
  explicit Bits(std::span<const uint8_t> data) : data_(data) {
    // The last 1 bit is the rbsp_stop_one_bit: what comes before it is the payload.
    for (size_t i = data_.size(); i-- > 0;)
      if (data_[i] != 0) {
        int b = 0;
        while (!((data_[i] >> b) & 1))
          ++b;
        stop_ = i * 8 + size_t(7 - b);
        break;
      }
  }
  uint32_t u(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i)
      v = (v << 1) | bit();
    return v;
  }
  bool flag() { return bit() != 0; }
  uint32_t ue() {
    int zeros = 0;
    while (bit() == 0) {
      if (++zeros > 31) {
        overrun_ = true;
        return 0;
      }
    }
    return zeros ? ((1u << zeros) - 1u + u(zeros)) : 0u;
  }
  int32_t se() {
    const uint32_t k = ue();
    return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2);
  }
  bool more_rbsp_data() const { return pos_ < stop_; }
  bool overrun() const { return overrun_; }

private:
  uint32_t bit() {
    if (pos_ >= data_.size() * 8) {
      overrun_ = true;
      return 0;
    }
    const uint32_t b = (data_[pos_ / 8] >> (7 - pos_ % 8)) & 1;
    ++pos_;
    return b;
  }
  std::span<const uint8_t> data_;
  size_t pos_ = 0, stop_ = 0;
  bool overrun_ = false;
};

template <size_t N>
void scaling_list(Bits &b, std::array<uint8_t, N> &list, bool &use_default) {
  int last = 8, next = 8;
  for (size_t j = 0; j < N; ++j) {
    if (next != 0) {
      const int delta = b.se();
      next = (last + delta + 256) % 256;
      use_default = j == 0 && next == 0;
    }
    list[j] = uint8_t(next == 0 ? last : next);
    last = list[j];
  }
}

// The scaling matrix of an SPS or a PPS: `count` lists, 4x4 first. A list that is not sent keeps 16 (flat) here; the GPU
// decoder applies the fall-back rules (A and B) itself from the "present" bits.
template <class T>
void scaling_matrix(Bits &b, T &owner, int count) {
  for (int i = 0; i < count; ++i) {
    if (!b.flag())
      continue;
    owner.scaling_list_present |= uint16_t(1u << i);
    bool use_default = false;
    if (i < 6)
      scaling_list(b, owner.scaling_4x4[size_t(i)], use_default);
    else
      scaling_list(b, owner.scaling_8x8[size_t(i - 6)], use_default);
    if (use_default)
      owner.use_default |= uint16_t(1u << i);
  }
}

void hrd_parameters(Bits &b) {
  const uint32_t cpb_cnt = b.ue() + 1;
  b.u(4); // bit_rate_scale
  b.u(4); // cpb_size_scale
  for (uint32_t i = 0; i < cpb_cnt && i < 32; ++i) {
    b.ue(); // bit_rate_value_minus1
    b.ue(); // cpb_size_value_minus1
    b.flag(); // cbr_flag
  }
  b.u(5); // initial_cpb_removal_delay_length_minus1
  b.u(5); // cpb_removal_delay_length_minus1
  b.u(5); // dpb_output_delay_length_minus1
  b.u(5); // time_offset_length
}

void vui(Bits &b, Sps &s) {
  if (b.flag()) { // aspect_ratio_info_present
    if (b.u(8) == 255) { // Extended_SAR
      b.u(16);
      b.u(16);
    }
  }
  if (b.flag()) // overscan_info_present
    b.flag();
  if (b.flag()) { // video_signal_type_present
    b.u(3);
    b.flag();
    if (b.flag()) { // colour_description_present
      b.u(8);
      b.u(8);
      b.u(8);
    }
  }
  if (b.flag()) { // chroma_loc_info_present
    b.ue();
    b.ue();
  }
  if (b.flag()) { // timing_info_present
    b.u(32);
    b.u(32);
    b.flag();
  }
  const bool nal_hrd = b.flag();
  if (nal_hrd)
    hrd_parameters(b);
  const bool vcl_hrd = b.flag();
  if (vcl_hrd)
    hrd_parameters(b);
  if (nal_hrd || vcl_hrd)
    b.flag(); // low_delay_hrd
  b.flag(); // pic_struct_present
  if (b.flag()) { // bitstream_restriction
    b.flag();
    b.ue();
    b.ue();
    b.ue();
    b.ue();
    s.num_reorder_frames = int(b.ue());
    s.max_dec_frame_buffering = int(b.ue());
  }
}

template <class T>
std::optional<T> failed(Error *error, const std::string &message) {
  if (error)
    error->message = message;
  return std::nullopt;
}

} // namespace

std::vector<Nal> split_annex_b(std::span<const uint8_t> data) {
  // Where each unit starts: just after a 00 00 01 (a 00 00 00 01 start code ends the same way).
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= data.size(); ++i)
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      starts.push_back(i + 3);
      i += 2;
    }
  std::vector<Nal> out;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t begin = starts[k];
    size_t end = k + 1 < starts.size() ? starts[k + 1] - 3 : data.size();
    while (end > begin && data[end - 1] == 0) // the zero of a 4-byte start code, and trailing zeros, are not the unit's
      --end;
    if (end <= begin)
      continue;
    Nal nal;
    nal.bytes = data.subspan(begin, end - begin);
    nal.type = nal.bytes[0] & 0x1F;
    nal.ref_idc = (nal.bytes[0] >> 5) & 3;
    out.push_back(nal);
  }
  return out;
}

std::vector<uint8_t> rbsp(std::span<const uint8_t> nal) {
  std::vector<uint8_t> out;
  out.reserve(nal.size());
  int zeros = 0;
  for (size_t i = 1; i < nal.size(); ++i) { // byte 0 is the NAL header
    const uint8_t c = nal[i];
    if (zeros >= 2 && c == 3) { // emulation_prevention_three_byte
      zeros = 0;
      continue;
    }
    out.push_back(c);
    zeros = c == 0 ? zeros + 1 : 0;
  }
  return out;
}

std::optional<Sps> parse_sps(std::span<const uint8_t> data, Error *error) {
  Bits b(data);
  Sps s;
  s.profile_idc = int(b.u(8));
  s.constraint_flags = int(b.u(8));
  s.level_idc = int(b.u(8));
  s.id = int(b.ue());
  if (s.id > 31)
    return failed<Sps>(error, "SPS id out of range");
  static constexpr int kHigh[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
  if (std::find(std::begin(kHigh), std::end(kHigh), s.profile_idc) != std::end(kHigh)) {
    s.chroma_format_idc = int(b.ue());
    if (s.chroma_format_idc == 3)
      s.separate_colour_plane = b.flag();
    s.bit_depth_luma = int(b.ue()) + 8;
    s.bit_depth_chroma = int(b.ue()) + 8;
    s.qpprime_y_zero_transform_bypass = b.flag();
    s.scaling_matrix_present = b.flag();
    if (s.scaling_matrix_present)
      scaling_matrix(b, s, s.chroma_format_idc != 3 ? 8 : 12);
  }
  s.log2_max_frame_num = int(b.ue()) + 4;
  s.pic_order_cnt_type = int(b.ue());
  if (s.pic_order_cnt_type == 0) {
    s.log2_max_pic_order_cnt_lsb = int(b.ue()) + 4;
  } else if (s.pic_order_cnt_type == 1) {
    s.delta_pic_order_always_zero = b.flag();
    s.offset_for_non_ref_pic = b.se();
    s.offset_for_top_to_bottom_field = b.se();
    const uint32_t cycle = b.ue();
    if (cycle > 255)
      return failed<Sps>(error, "SPS has too many reference frames in its POC cycle");
    for (uint32_t i = 0; i < cycle; ++i)
      s.offset_for_ref_frame.push_back(b.se());
  } else if (s.pic_order_cnt_type != 2) {
    return failed<Sps>(error, "SPS has an unknown picture order count type");
  }
  s.max_num_ref_frames = int(b.ue());
  s.gaps_in_frame_num_allowed = b.flag();
  s.pic_width_in_mbs = int(b.ue()) + 1;
  s.pic_height_in_map_units = int(b.ue()) + 1;
  s.frame_mbs_only = b.flag();
  if (!s.frame_mbs_only)
    s.mb_adaptive_frame_field = b.flag();
  s.direct_8x8_inference = b.flag();
  s.frame_cropping = b.flag();
  if (s.frame_cropping) {
    s.crop_left = int(b.ue());
    s.crop_right = int(b.ue());
    s.crop_top = int(b.ue());
    s.crop_bottom = int(b.ue());
  }
  s.vui_present = b.flag();
  if (s.vui_present)
    vui(b, s);
  if (b.overrun())
    return failed<Sps>(error, "SPS ends early");
  return s;
}

std::optional<Pps> parse_pps(std::span<const uint8_t> data, const std::vector<Sps> &sps, Error *error) {
  Bits b(data);
  Pps p;
  p.id = int(b.ue());
  p.sps_id = int(b.ue());
  if (p.id > 255 || p.sps_id > 31)
    return failed<Pps>(error, "PPS id out of range");
  const auto s = std::find_if(sps.begin(), sps.end(), [&](const Sps &x) { return x.id == p.sps_id; });
  if (s == sps.end())
    return failed<Pps>(error, "PPS refers to an SPS that was not sent");
  p.entropy_coding_mode = b.flag();
  p.bottom_field_pic_order_in_frame_present = b.flag();
  p.num_slice_groups = int(b.ue()) + 1;
  if (p.num_slice_groups > 1)
    return failed<Pps>(error, "PPS uses slice groups (FMO), which a GPU decoder does not take");
  p.num_ref_idx_l0_default_active = int(b.ue()) + 1;
  p.num_ref_idx_l1_default_active = int(b.ue()) + 1;
  p.weighted_pred = b.flag();
  p.weighted_bipred_idc = int(b.u(2));
  p.pic_init_qp = 26 + b.se();
  p.pic_init_qs = 26 + b.se();
  p.chroma_qp_index_offset = b.se();
  p.deblocking_filter_control_present = b.flag();
  p.constrained_intra_pred = b.flag();
  p.redundant_pic_cnt_present = b.flag();
  p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
  if (b.more_rbsp_data()) {
    p.transform_8x8_mode = b.flag();
    p.scaling_matrix_present = b.flag();
    if (p.scaling_matrix_present)
      scaling_matrix(b, p, 6 + (s->chroma_format_idc != 3 ? 2 : 6) * (p.transform_8x8_mode ? 1 : 0));
    p.second_chroma_qp_index_offset = b.se();
  }
  if (b.overrun())
    return failed<Pps>(error, "PPS ends early");
  return p;
}

std::optional<SliceHeader> parse_slice_header(const Nal &nal, const std::vector<Sps> &sps, const std::vector<Pps> &pps, Error *error) {
  const std::vector<uint8_t> data = rbsp(nal.bytes);
  Bits b(data);
  SliceHeader h;
  h.nal_type = nal.type;
  h.nal_ref_idc = nal.ref_idc;
  h.first_mb_in_slice = int(b.ue());
  h.slice_type = int(b.ue());
  h.pps_id = int(b.ue());
  const auto p = std::find_if(pps.begin(), pps.end(), [&](const Pps &x) { return x.id == h.pps_id; });
  if (p == pps.end())
    return failed<SliceHeader>(error, "slice refers to a PPS that was not sent");
  const auto s = std::find_if(sps.begin(), sps.end(), [&](const Sps &x) { return x.id == p->sps_id; });
  if (s == sps.end())
    return failed<SliceHeader>(error, "slice's PPS refers to an SPS that was not sent");
  if (s->separate_colour_plane)
    h.colour_plane_id = int(b.u(2));
  h.frame_num = int(b.u(s->log2_max_frame_num));
  if (!s->frame_mbs_only) {
    h.field_pic = b.flag();
    if (h.field_pic)
      h.bottom_field = b.flag();
  }
  if (h.idr())
    h.idr_pic_id = int(b.ue());
  if (s->pic_order_cnt_type == 0) {
    h.pic_order_cnt_lsb = int(b.u(s->log2_max_pic_order_cnt_lsb));
    if (p->bottom_field_pic_order_in_frame_present && !h.field_pic)
      h.delta_pic_order_cnt_bottom = b.se();
  }
  if (s->pic_order_cnt_type == 1 && !s->delta_pic_order_always_zero) {
    h.delta_pic_order_cnt[0] = b.se();
    if (p->bottom_field_pic_order_in_frame_present && !h.field_pic)
      h.delta_pic_order_cnt[1] = b.se();
  }
  if (p->redundant_pic_cnt_present)
    b.ue(); // redundant_pic_cnt
  const int type = h.type();
  if (type == 1)
    b.flag(); // direct_spatial_mv_pred
  int l0 = p->num_ref_idx_l0_default_active, l1 = p->num_ref_idx_l1_default_active;
  if (type == 0 || type == 3 || type == 1) {
    if (b.flag()) { // num_ref_idx_active_override
      l0 = int(b.ue()) + 1;
      if (type == 1)
        l1 = int(b.ue()) + 1;
    }
  }
  // ref_pic_list_modification: read past it.
  const auto modification = [&]() {
    if (b.flag()) {
      for (int guard = 0; guard < 100; ++guard) {
        const uint32_t idc = b.ue();
        if (idc == 3)
          break;
        b.ue(); // abs_diff_pic_num_minus1 or long_term_pic_num
        if (b.overrun())
          break;
      }
    }
  };
  if (type != 2 && type != 4)
    modification();
  if (type == 1)
    modification();
  // pred_weight_table: read past it.
  if ((p->weighted_pred && (type == 0 || type == 3)) || (p->weighted_bipred_idc == 1 && type == 1)) {
    const int chroma_array_type = s->separate_colour_plane ? 0 : s->chroma_format_idc;
    b.ue(); // luma_log2_weight_denom
    if (chroma_array_type != 0)
      b.ue();
    const auto weights = [&](int count) {
      for (int i = 0; i < count; ++i) {
        if (b.flag()) {
          b.se();
          b.se();
        }
        if (chroma_array_type != 0 && b.flag())
          for (int j = 0; j < 2; ++j) {
            b.se();
            b.se();
          }
      }
    };
    weights(l0);
    if (type == 1)
      weights(l1);
  }
  if (h.reference()) { // dec_ref_pic_marking
    if (h.idr()) {
      h.no_output_of_prior_pics = b.flag();
      h.long_term_reference = b.flag();
    } else {
      h.adaptive_ref_pic_marking = b.flag();
      if (h.adaptive_ref_pic_marking)
        for (int guard = 0; guard < 100; ++guard) {
          Mmco m;
          m.op = int(b.ue());
          if (m.op == 0 || b.overrun())
            break;
          if (m.op == 1 || m.op == 3)
            m.difference_of_pic_nums = int(b.ue()) + 1;
          if (m.op == 2)
            m.long_term_pic_num = int(b.ue());
          if (m.op == 3 || m.op == 6)
            m.long_term_frame_idx = int(b.ue());
          if (m.op == 4)
            m.max_long_term_frame_idx_plus1 = int(b.ue());
          h.mmco.push_back(m);
        }
    }
  }
  if (b.overrun())
    return failed<SliceHeader>(error, "slice header ends early");
  return h;
}

std::string gpu_unsupported(const Sps &sps) {
  if (!sps.frame_mbs_only)
    return "interlaced (field) video";
  if (sps.chroma_format_idc != 1)
    return "colour other than 4:2:0";
  if (sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8)
    return "more than 8 bits per sample";
  if (sps.pic_width_in_mbs <= 0 || sps.pic_height_in_map_units <= 0)
    return "no picture size";
  return {};
}

int PocCounter::next(const Sps &sps, const SliceHeader &h) {
  const bool mmco5 = std::any_of(h.mmco.begin(), h.mmco.end(), [](const Mmco &m) { return m.op == 5; });
  int top = 0, bottom = 0;
  if (sps.pic_order_cnt_type == 0) {
    int prev_msb = prev_poc_msb_, prev_lsb = prev_poc_lsb_;
    if (h.idr()) {
      prev_msb = 0;
      prev_lsb = 0;
    } else if (prev_had_mmco5_) {
      prev_msb = 0;
      prev_lsb = prev_poc_after_mmco5_;
    }
    const int max_lsb = 1 << sps.log2_max_pic_order_cnt_lsb;
    const int lsb = h.pic_order_cnt_lsb;
    int msb = prev_msb;
    if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2)
      msb = prev_msb + max_lsb;
    else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2)
      msb = prev_msb - max_lsb;
    top = msb + lsb;
    bottom = top + h.delta_pic_order_cnt_bottom;
    if (h.reference()) { // the previous reference picture is what the next one counts from
      prev_poc_msb_ = msb;
      prev_poc_lsb_ = lsb;
      prev_had_mmco5_ = mmco5;
      prev_poc_after_mmco5_ = top - std::min(top, bottom);
    }
  } else {
    const int max_frame_num = 1 << sps.log2_max_frame_num;
    const int prev_offset = prev_had_mmco5_ ? 0 : prev_frame_num_offset_;
    const int prev_num = prev_had_mmco5_ ? 0 : prev_frame_num_;
    int offset = 0;
    if (!h.idr())
      offset = prev_num > h.frame_num ? prev_offset + max_frame_num : prev_offset;
    if (sps.pic_order_cnt_type == 1) {
      const int cycle = int(sps.offset_for_ref_frame.size());
      int abs_frame_num = cycle != 0 ? offset + h.frame_num : 0;
      if (!h.reference() && abs_frame_num > 0)
        --abs_frame_num;
      int expected = 0;
      if (abs_frame_num > 0) {
        int per_cycle = 0;
        for (const int d : sps.offset_for_ref_frame)
          per_cycle += d;
        const int cycle_count = (abs_frame_num - 1) / cycle, in_cycle = (abs_frame_num - 1) % cycle;
        expected = cycle_count * per_cycle;
        for (int i = 0; i <= in_cycle; ++i)
          expected += sps.offset_for_ref_frame[size_t(i)];
      }
      if (!h.reference())
        expected += sps.offset_for_non_ref_pic;
      top = expected + h.delta_pic_order_cnt[0];
      bottom = top + sps.offset_for_top_to_bottom_field + h.delta_pic_order_cnt[1];
    } else {
      const int temp = h.idr() ? 0 : (h.reference() ? 2 * (offset + h.frame_num) : 2 * (offset + h.frame_num) - 1);
      top = bottom = temp;
    }
    prev_frame_num_offset_ = offset;
    prev_frame_num_ = h.frame_num;
    prev_had_mmco5_ = mmco5;
  }
  const int poc = std::min(top, bottom);
  return mmco5 ? 0 : poc; // a picture that resets the counts orders as 0 among those that follow
}

} // namespace atm::media::h264
