#ifndef OSMLR_OUTPUT_OUTPUT_HPP
#define OSMLR_OUTPUT_OUTPUT_HPP

#include <valhalla/baldr/merge.h>

namespace osmlr {
namespace output {

/**
 * Abstract interface to various different kinds of output.
 */
struct output {
  virtual ~output();

  virtual void add_path(const valhalla::baldr::merge::path &) = 0;
  virtual void finish() = 0;
  virtual void update_tiles(const std::vector<std::string>& osmlr_tiles) = 0;
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_OUTPUT_HPP */
