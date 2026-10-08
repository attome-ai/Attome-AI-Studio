#pragma once
// H.264 headers, as a GPU decoder needs them (Vulkan Video): the decoder decodes the slices itself, but the program gives it
// the sequence and picture parameter sets, the picture's order count, and keeps its reference pictures. Pure C++: no
// platform, no library. Only what decoding progressive 4:2:0 8-bit video needs is kept; what a GPU decoder cannot take
// (fields, 4:2:2, 10-bit) is reported, so the caller decodes those on the CPU.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atm::media::h264 {

// The NAL units of a buffer in Annex B form (start codes: what Media Foundation gives).
struct Nal {
  int type = 0;     // nal_unit_type: 1 slice, 5 IDR slice, 7 SPS, 8 PPS, ...
  int ref_idc = 0;  // nal_ref_idc
  std::span<const uint8_t> bytes; // the whole unit, header included, emulation prevention still in
};
std::vector<Nal> split_annex_b(std::span<const uint8_t> data);

// The payload of a NAL unit without its header byte and with the emulation-prevention bytes (00 00 03) taken out.
std::vector<uint8_t> rbsp(std::span<const uint8_t> nal);

struct Sps {
  int profile_idc = 0, level_idc = 0, constraint_flags = 0;
  int id = 0;
  int chroma_format_idc = 1, bit_depth_luma = 8, bit_depth_chroma = 8;
  bool separate_colour_plane = false, qpprime_y_zero_transform_bypass = false;
  bool scaling_matrix_present = false;
  // Scaling lists: 6 of 4x4 and 6 of 8x8 (only the first two used for 4:2:0), in the order they are sent, with whether each
  // was sent and whether it uses the default.
  std::array<std::array<uint8_t, 16>, 6> scaling_4x4{};
  std::array<std::array<uint8_t, 64>, 6> scaling_8x8{};
  uint16_t scaling_list_present = 0, use_default = 0; // bits 0..5: 4x4 lists, 6..11: 8x8 lists
  int log2_max_frame_num = 4;
  int pic_order_cnt_type = 0;
  int log2_max_pic_order_cnt_lsb = 4;
  bool delta_pic_order_always_zero = false;
  int offset_for_non_ref_pic = 0, offset_for_top_to_bottom_field = 0;
  std::vector<int> offset_for_ref_frame;
  int max_num_ref_frames = 0;
  bool gaps_in_frame_num_allowed = false;
  int pic_width_in_mbs = 0, pic_height_in_map_units = 0;
  bool frame_mbs_only = true, mb_adaptive_frame_field = false, direct_8x8_inference = false;
  bool frame_cropping = false;
  int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0; // in crop units (2 luma samples for 4:2:0)
  bool vui_present = false;
  int max_dec_frame_buffering = -1; // from the VUI's bitstream restriction, -1 when not sent
  int num_reorder_frames = -1;
  // The picture: coded size and what is shown (after the crop).
  int coded_width() const { return pic_width_in_mbs * 16; }
  int coded_height() const { return pic_height_in_map_units * 16 * (frame_mbs_only ? 1 : 2); }
  int width() const { return coded_width() - 2 * (crop_left + crop_right); }
  int height() const { return coded_height() - 2 * (frame_mbs_only ? 1 : 2) * (crop_top + crop_bottom); }
};

struct Pps {
  int id = 0, sps_id = 0;
  bool entropy_coding_mode = false, bottom_field_pic_order_in_frame_present = false;
  int num_slice_groups = 1;
  int num_ref_idx_l0_default_active = 1, num_ref_idx_l1_default_active = 1;
  bool weighted_pred = false;
  int weighted_bipred_idc = 0;
  int pic_init_qp = 26, pic_init_qs = 26, chroma_qp_index_offset = 0;
  bool deblocking_filter_control_present = false, constrained_intra_pred = false, redundant_pic_cnt_present = false;
  bool transform_8x8_mode = false, scaling_matrix_present = false;
  std::array<std::array<uint8_t, 16>, 6> scaling_4x4{};
  std::array<std::array<uint8_t, 64>, 6> scaling_8x8{};
  uint16_t scaling_list_present = 0, use_default = 0;
  int second_chroma_qp_index_offset = 0;
};

// The decoded reference picture marking of a slice (what to forget, what to keep long-term).
struct Mmco {
  int op = 0; // 1: forget a short-term picture, 2: forget a long-term one, 3: short-term to long-term, 4: max long-term index, 5: forget all, 6: this picture long-term
  int difference_of_pic_nums = 0, long_term_pic_num = 0, long_term_frame_idx = 0, max_long_term_frame_idx_plus1 = 0;
};

struct SliceHeader {
  int nal_type = 0, nal_ref_idc = 0;
  int first_mb_in_slice = 0, slice_type = 0, pps_id = 0;
  int colour_plane_id = 0;
  int frame_num = 0;
  bool field_pic = false, bottom_field = false;
  int idr_pic_id = 0;
  int pic_order_cnt_lsb = 0, delta_pic_order_cnt_bottom = 0;
  int delta_pic_order_cnt[2] = {0, 0};
  // dec_ref_pic_marking
  bool no_output_of_prior_pics = false, long_term_reference = false, adaptive_ref_pic_marking = false;
  std::vector<Mmco> mmco;
  bool idr() const { return nal_type == 5; }
  bool reference() const { return nal_ref_idc != 0; }
  int type() const { return slice_type % 5; } // 0 P, 1 B, 2 I, 3 SP, 4 SI
};

struct Error {
  std::string message;
};

// Parse the RBSP of an SPS, a PPS, or a slice's NAL unit (a slice needs the parameter sets it refers to).
std::optional<Sps> parse_sps(std::span<const uint8_t> rbsp, Error *error = nullptr);
std::optional<Pps> parse_pps(std::span<const uint8_t> rbsp, const std::vector<Sps> &sps, Error *error = nullptr);
std::optional<SliceHeader> parse_slice_header(const Nal &nal, const std::vector<Sps> &sps, const std::vector<Pps> &pps, Error *error = nullptr);

// Why a GPU decoder would not take this sequence (empty: it would). Fields, 4:2:2/4:4:4, more than 8 bits, or no frame
// size.
std::string gpu_unsupported(const Sps &sps);

// The picture order count of each frame (8.2.1), kept across frames: give it every frame's first slice in decoding order.
class PocCounter {
public:
  // The frame's order count (frames only: the smaller of top and bottom, which are equal for a progressive frame).
  int next(const Sps &sps, const SliceHeader &slice);

private:
  int prev_poc_msb_ = 0, prev_poc_lsb_ = 0;   // type 0
  int prev_frame_num_ = 0, prev_frame_num_offset_ = 0; // types 1 and 2
  bool prev_had_mmco5_ = false;
  int prev_poc_after_mmco5_ = 0; // a picture with memory_management_control_operation 5 counts as order 0 for the next
};

} // namespace atm::media::h264
