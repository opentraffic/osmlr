#include "osmlr/util/tile_writer.hpp"

#include <boost/filesystem.hpp>
#include <valhalla/baldr/graphtile.h>
#include <valhalla/midgard/logging.h>
#include <unistd.h>

namespace bfs = boost::filesystem;
namespace vb = valhalla::baldr;

namespace osmlr {
namespace util {

tile_writer::tile_writer(std::string base_dir, std::string suffix, size_t max_fds)
  : m_base_dir(base_dir)
  , m_suffix(suffix)
  , m_max_fds(max_fds)
  , m_tile_hierarchy(m_base_dir) {
  if (bfs::exists(base_dir) && !bfs::is_empty(base_dir)) {
    LOG_WARN("Non-empty " + base_dir + " will be purged of data.");
    bfs::remove_all(base_dir);
  }
  bfs::create_directories(base_dir);
}

void tile_writer::write_to(vb::GraphId tile_id, const std::string &data) {
  const std::string tile_name = get_name_for_tile(tile_id);
  const int fd = get_fd_for(tile_id);

  assert(data.size() < std::numeric_limits<ssize_t>::max());
  ssize_t bytes_left = data.size();
  const char *ptr = data.data();

  while (bytes_left > 0) {
    ssize_t n = write(fd, ptr, bytes_left);

    if (n < 0) {
      std::string error(strerror(errno));
      throw std::runtime_error("Failed to write " + tile_name + " because: " + error);

    } else if (n == 0) {
      LOG_WARN("Making no progress writing " + tile_name);

    } else {
      bytes_left -= n;
      ptr += n;
    }
  }
}

void tile_writer::close_all() {
  while (m_fds.size() > 0) {
    evict_last_fd();
  }
}

std::string tile_writer::get_name_for_tile(vb::GraphId tile_id) {
  auto suffix = vb::GraphTile::FileSuffix(tile_id, m_tile_hierarchy);
  auto path = bfs::path(m_base_dir) / suffix;
  return path.replace_extension(m_suffix).string();
}

int tile_writer::get_fd_for(vb::GraphId tile_id) {
  auto itr = m_fds.find(tile_id);
  if (itr != m_fds.end()) {
    // update the LRU counter for this entry to make it the most recently used
    // item (unless it already is).
    if (itr->second.lru != m_max_lru) {
      m_max_lru += 1;
      itr->second.lru = m_max_lru;
    }
    return itr->second.fd;

  } else {
    return make_fd_for(tile_id);
  }
}

int tile_writer::make_fd_for(vb::GraphId tile_id) {
  while (m_fds.size() >= m_max_fds) {
    evict_last_fd();
  }
  const std::string tile_name = get_name_for_tile(tile_id);

  // first, assume that the file exists and try to open it.
  int fd = open(tile_name.c_str(), O_WRONLY | O_APPEND);

  // if it doesn't exist, then try to create it
  if (fd < 0 && errno == ENOENT) {
    bfs::path p(tile_name);
    bfs::create_directories(p.parent_path());
    fd = open(tile_name.c_str(), O_WRONLY | O_CREAT,
              S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
  }

  if (fd < 0) {
    std::string error(strerror(errno));
    throw std::runtime_error("Failed to open " + tile_name + " because: " + error);
  }

  m_fds.emplace(tile_id, lru_fd{fd, 0});
  return fd;
}

void tile_writer::evict_last_fd() {
  const auto end = m_fds.end();
  auto min_itr = end;
  for (auto itr = m_fds.begin(); itr != end; ++itr) {
    if (min_itr == end || itr->second.lru < min_itr->second.lru) {
      min_itr = itr;
    }
  }
  if (min_itr != end) {
    int fd = min_itr->second.fd;
    size_t lru = min_itr->second.lru;

    int status = close(fd);
    if (status < 0) {
      std::string error(strerror(errno));
      throw std::runtime_error("Failed to close fd " + std::to_string(fd) + " because: " + error);
    }
    m_fds.erase(min_itr);

    // subtract the minimum amount from each record to keep them from growing
    // without bound.
    for (auto &entry : m_fds) {
      entry.second.lru -= lru;
    }
  }
}

} // namespace util
} // namespace osmlr
