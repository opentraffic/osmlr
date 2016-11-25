#ifndef OSMLR_OUTPUT_TILES_HPP
#define OSMLR_OUTPUT_TILES_HPP

#include <osmlr/output/output.hpp>
#include <osmlr/util/tile_writer.hpp>

namespace osmlr {
namespace output {

struct tiles : public output {
  tiles(valhalla::baldr::GraphReader &reader, std::string base_dir, size_t max_fds, uint32_t max_length = 15000);
  virtual ~tiles();

  void add_path(const valhalla::baldr::merge::path &);
  void finish();

private:
  valhalla::baldr::GraphReader &m_reader;
  util::tile_writer m_writer;
  uint32_t m_max_length;
};

} // namespace output
} // namespace osmlr

#endif /* OSMLR_OUTPUT_TILES_HPP */
