#include "atm/gen/provider.hpp"

namespace atm::gen {

std::string output_file(std::string_view port) {
  PortType type = PortType::latent;
  for (const KindDef &kind : kind_defs())
    for (const PortDef &out : kind.outputs)
      if (port == out.name)
        type = out.type;
  const char *ext = type == PortType::video   ? ".mp4"
                    : type == PortType::image ? ".jpg"
                    : type == PortType::audio ? ".wav"
                    : type == PortType::mask  ? ".png"
                    : type == PortType::number || type == PortType::integer || type == PortType::text || type == PortType::boolean ? ".txt"
                                              : ".bin";
  return std::string(port) + ext;
}

} // namespace atm::gen
