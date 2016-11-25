#ifndef OSMLR_OUTPUT_TILES_HPP
#define OSMLR_OUTPUT_TILES_HPP

#include <osmlr/output/output.hpp>

namespace osmlr {
namespace output {

struct tiles : public output {
  tiles(std::string base_dir, valhalla::baldr::GraphReader &reader);
  virtual ~tiles();

  void add_path(const valhalla::baldr::merge::path &);
  void finish();
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_TILES_HPP */
