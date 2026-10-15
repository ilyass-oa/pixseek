#include "pixseek/catalog.hpp"
#include "pixseek/imgdist.h"
#include "pixseek/net.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace pixseek {
std::vector<CatalogEntry> load_catalog(const std::string &directory) {
  std::vector<std::string> files;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_symlink() || !entry.is_regular_file())
      continue;
    auto extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (extension == ".bmp") {
      files.push_back(entry.path().string());
      if (files.size() > 10000)
        throw std::runtime_error("Catalog exceeds 10000 images");
    }
  }
  std::sort(files.begin(), files.end());
  std::vector<CatalogEntry> result;
  for (const auto &path : files) {
    try {
      auto bytes = read_image(path);
      uint64_t hash;
      if (!PHashRaw(bytes.data(), static_cast<unsigned int>(bytes.size()),
                    &hash))
        throw std::runtime_error("Invalid BMP: " + path);
      result.push_back({path, hash});
    } catch (const std::exception &error) {
      std::cerr << "Skipping " << error.what() << '\n';
    }
  }
  if (result.empty())
    throw std::runtime_error("Catalog contains no supported BMP images");
  return result;
}
std::string search(const std::vector<CatalogEntry> &catalog,
                   const std::vector<char> &data) {
  uint64_t hash;
  if (!PHashRaw(data.data(), static_cast<unsigned int>(data.size()), &hash))
    return "No similar image found (no comparison could be performed "
           "successfully).";
  unsigned int best = 65;
  const CatalogEntry *match = nullptr;
  for (const auto &entry : catalog) {
    auto distance = DistancePHash(hash, entry.hash);
    if (distance < best) {
      best = distance;
      match = &entry;
    }
  }
  if (!match)
    return "No similar image found (no comparison could be performed "
           "successfully).";
  std::string reply = "Most similar image found: '" + match->path +
                      "' with a distance of " + std::to_string(best) + ".";
  if (reply.size() >= max_reply_bytes)
    return "Error: result path is too long.";
  return reply;
}
} // namespace pixseek
