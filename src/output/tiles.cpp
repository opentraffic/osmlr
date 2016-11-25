#include "osmlr/output/tiles.hpp"
#include <stdexcept>

namespace osmlr {
namespace output {

tiles::tiles(std::string base_dir, valhalla::baldr::GraphReader &reader) {
  throw std::runtime_error("Unimplemented");
}

tiles::~tiles() {
}

void tiles::add_path(const valhalla::baldr::merge::path &p) {
  throw std::runtime_error("Unimplemented");
}

void tiles::finish() {
  throw std::runtime_error("Unimplemented");
}

} // namespace output
} // namespace osmlr
