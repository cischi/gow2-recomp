#include <filesystem>
#include <iostream>
#include <sstream>

#include "run_extract.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: extract <iso> <out_dir>\n";
    return 2;
  }
  std::ostringstream log;
  int rc = RunExtract(std::filesystem::path(argv[1]), std::filesystem::path(argv[2]), log);
  (rc == 0 ? std::cout : std::cerr) << log.str();
  return rc;
}
