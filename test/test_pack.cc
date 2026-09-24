#include "esp32_git.h"
#include "pack.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
  namespace fs = std::filesystem;
  const fs::path fixture = "build/fixtures/pack-delta";
  fs::remove_all(fixture);
  fs::create_directories(fixture / "source");
  std::vector<uint8_t> base(15000);
  for (size_t i = 0; i < base.size(); ++i) base[i] = (uint8_t)((i * 17 + 31) % 256);
  std::vector<uint8_t> variant = base;
  memset(variant.data() + 3000, 'X', 3000);
  std::ofstream(fixture / "source/base.bin", std::ios::binary)
      .write((const char *)base.data(), base.size());
  std::ofstream(fixture / "source/variant.bin", std::ios::binary)
      .write((const char *)variant.data(), variant.size());
  if (system("git -C build/fixtures/pack-delta/source init -q && "
             "git -C build/fixtures/pack-delta/source add base.bin variant.bin && "
             "git -C build/fixtures/pack-delta/source -c user.name=test "
             "-c user.email=test@example.com commit -qm fixture && "
             "git -C build/fixtures/pack-delta/source "
             "-c pack.useDeltaBaseOffset=false repack -adf") != 0) return 1;

  fs::path pack, idx;
  for (const auto &entry : fs::directory_iterator(fixture / "source/.git/objects/pack")) {
    if (entry.path().extension() == ".pack") pack = entry.path();
    if (entry.path().extension() == ".idx") idx = entry.path();
  }
  if (pack.empty() || idx.empty()) return 1;
  const std::string inspect = "git verify-pack -v " + idx.string();
  FILE *verification = popen(inspect.c_str(), "r");
  if (!verification) return 1;
  bool has_delta = false;
  char line[256];
  while (fgets(line, sizeof(line), verification)) {
    has_delta |= strstr(line, "chain length = 1:") != nullptr;
  }
  pclose(verification);
  if (!has_delta) return 1;

  const std::string scratch = (fixture / "device").string();
  if (esp32git_init(scratch.c_str()) != ESP32GIT_OK) return 1;
  size_t objects = 0;
  bool valid = true;
  const bool parsed = e32g::pack_read_file(
      pack.string(), 0, fs::file_size(pack), scratch,
      [&](const e32g::PackEntry &entry) {
        const char *type = entry.type == e32g::PACK_COMMIT ? "commit"
                           : entry.type == e32g::PACK_TREE ? "tree" : "blob";
        char sha[41];
        valid &= esp32git_object_write(scratch.c_str(), type,
                                      entry.data.data(), entry.data.size(), sha) == ESP32GIT_OK &&
                 entry.sha == sha;
        ++objects;
      });
  if (!parsed || !valid || objects != 4) {
    fprintf(stderr, "delta pack failed: parsed=%d valid=%d objects=%zu\n",
            parsed, valid, objects);
    return 1;
  }
  puts("delta pack passed");
  return 0;
}
