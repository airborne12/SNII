// SNII benchmark entry point (placeholder; real runner is filled in by the
// benchmark implementation). Confirms libsnii + CLucene link together.
#include <cstdio>

#include "CLucene.h"
#include "CLucene/store/RAMDirectory.h"
#include "snii/version.h"

int main() {
  auto* dir = _CLNEW lucene::store::RAMDirectory();
  const bool exists = dir->fileExists("none");
  _CLDECDELETE(dir);
  std::printf("snii_bench placeholder: version=%s, clucene_ok=%d\n",
              SNII_VERSION_STRING, static_cast<int>(!exists));
  return 0;
}
