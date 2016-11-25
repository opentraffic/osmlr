#ifndef OSMLR_UTIL_TILE_WRITER_HPP
#define OSMLR_UTIL_TILE_WRITER_HPP

#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <string>

namespace osmlr {
namespace util {

/**
 * Writes data into tile files.
 *
 * In order to write data into many tiles in an unordered fashion, this class
 * will manage a set of open files in order to not run out of file descriptors.
 */
struct tile_writer {
  tile_writer(std::string base_dir, std::string suffix, size_t max_fds);
  void write_to(valhalla::baldr::GraphId tile_id, const std::string &data);
  void close_all();

private:
  std::string get_name_for_tile(valhalla::baldr::GraphId tile_id);
  int get_fd_for(valhalla::baldr::GraphId tile_id);
  int make_fd_for(valhalla::baldr::GraphId tile_id);
  void evict_last_fd();

  const std::string m_base_dir, m_suffix;
  const size_t m_max_fds;
  const valhalla::baldr::TileHierarchy m_tile_hierarchy;

  struct lru_fd {
    // the file descriptor itself
    int fd;
    // the least-recently-used index. this is set to the max on use, and
    // reduced by the minimum when the minimum is evicted.
    size_t lru;
  };

  size_t m_max_lru;
  std::unordered_map<valhalla::baldr::GraphId, lru_fd> m_fds;
};

} // namespace util
} // namespace osmlr

#endif /* OSMLR_UTIL_TILE_WRITER_HPP */
