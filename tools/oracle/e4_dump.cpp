// E4b oracle: compress one block with libzpaq::compressBlock.
// Usage: e4_oracle <in> <out> <method>
#include "libzpaq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void libzpaq::error(const char *msg) {
  fprintf(stderr, "oracle error: %s\n", msg);
  exit(1);
}

struct FileWriter : public libzpaq::Writer {
  FILE *f;
  void put(int c) { fputc(c, f); }
  void write(const char *p, int n) { fwrite(p, 1, (size_t)n, f); }
};

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: e4_oracle <in> <out> <method>\n");
    return 2;
  }
  FILE *in = fopen(argv[1], "rb");
  FILE *out = fopen(argv[2], "wb");
  if (!in || !out) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }
  libzpaq::StringBuffer sb;
  {
    char buf[1 << 16];
    int nr;
    while ((nr = (int)fread(buf, 1, sizeof(buf), in)) > 0)
      sb.write(buf, nr);
  }
  fclose(in);
  FileWriter w;
  w.f = out;
  libzpaq::compressBlock(&sb, &w, argv[3], "data.bin", 0, true);
  fclose(out);
  return 0;
}
