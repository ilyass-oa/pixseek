#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace pixseek {
struct CatalogEntry {
  std::string path;
  uint64_t hash;
};
std::vector<CatalogEntry> load_catalog(const std::string &directory);
std::string search(const std::vector<CatalogEntry> &catalog,
                   const std::vector<char> &data);
} // namespace pixseek
