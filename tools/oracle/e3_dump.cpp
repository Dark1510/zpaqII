// E3 oracle: predictor trace (p per bit), encoder output and decoder round
// trip for a set of ZPAQL models, using libzpaq 7.15 built with -DNOJIT
// (the trace interleaves predict/update so it does not depend on the JIT).
// Usage: e3_oracle <manifest> <results>
#include "libzpaq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace libzpaq {
std::string makeConfig(const char *method, int args[]);
}

void libzpaq::error(const char *msg) {
  fprintf(stderr, "oracle error: %s\n", msg);
  exit(1);
}

struct MemWriter : public libzpaq::Writer {
  std::string s;
  void put(int c) { s += char(c); }
  void write(const char *p, int n) { s.append(p, n); }
};

struct Case {
  std::string config;
  int args[9];
};

static void hex(const char *p, int n, std::string &out) {
  static const char *d = "0123456789abcdef";
  for (int i = 0; i < n; ++i) {
    out += d[(unsigned char)p[i] >> 4];
    out += d[(unsigned char)p[i] & 15];
  }
}

static std::string sha1hex(const char *p, int n) {
  libzpaq::SHA1 s;
  s.write(p, n);
  std::string out;
  hex(s.result(), 20, out);
  return out;
}

static std::string escape(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') o += "\\\\";
    else if (s[i] == '\n') o += "\\n";
    else o += s[i];
  }
  return o;
}

static unsigned lcg = 1;
static unsigned nextrand() {
  lcg = lcg * 1103515245u + 12345u;
  return lcg >> 8;
}

/* Data with periodic repeats so MATCH/AVG have something to chew on. */
static void make_data(unsigned char *b, int n) {
  for (int i = 0; i < n; ++i) {
    if (i >= 64 && (i % 64) >= 32)
      b[i] = b[i - 32];
    else
      b[i] = (unsigned char)(nextrand() & 255);
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: e3_oracle <manifest> <results>\n");
    return 2;
  }
  FILE *mf = fopen(argv[1], "w");
  FILE *rf = fopen(argv[2], "w");
  if (!mf || !rf) {
    fprintf(stderr, "cannot open files\n");
    return 2;
  }

  std::vector<Case> cases;
  static const char *methods[] = {
      "x4.0c256ci1,1,1,1,1,1,2a",
      "x4.0ci1,1,1,1,2awm",
      "x4.0t0",
      "x4.0s8",
      "x4.0cm16",
      "x4.0icm16",
      "x4.0a24",
      "x4.0m8",
      "x4.0c0,0,511i2",
      "x4.0w2c0,1010,255i1",
      "x4.3ci1mm8ts8t0",
      0};
  for (int i = 0; methods[i]; ++i) {
    Case c;
    memset(c.args, 0, sizeof(c.args));
    c.config = libzpaq::makeConfig(methods[i], c.args);
    cases.push_back(c);
  }

  /* Synthetic: AVG + SSE with a non-default start, and MATCH next to CM. */
  {
    Case c;
    memset(c.args, 0, sizeof(c.args));
    c.config =
        "comp 9 16 0 0 4 0 cm 16 255 1 cm 20 255 2 avg 0 1 128 3 sse 8 2 3 32\n"
        "hcomp c-- *c=a a+= 255 d=a *d=c halt\n"
        "end\n";
    c.args[0] = 4;  /* block size arg, unused by the model */
    cases.push_back(c);
  }
  /* Synthetic: unmodeled (store) block. */
  {
    Case c;
    memset(c.args, 0, sizeof(c.args));
    c.config = "comp 0 0 0 0 0 hcomp end\n";
    cases.push_back(c);
  }

  const int N = 512;
  static unsigned char data[N];
  lcg = 12345u;
  make_data(data, N);

  for (size_t i = 0; i < cases.size(); ++i) {
    fprintf(mf, "CASE %zu\n", i);
    fprintf(mf, "CONFIG %s\n", escape(cases[i].config).c_str());
    fprintf(mf, "ARGS");
    for (int j = 0; j < 9; ++j) fprintf(mf, " %d", cases[i].args[j]);
    fprintf(mf, "\n");

    libzpaq::ZPAQL z, pz;
    MemWriter pcmd;
    libzpaq::Compiler(cases[i].config.c_str(), cases[i].args, z, pz, &pcmd);

    fprintf(rf, "RESULT %zu\n", i);

    /* Predictor trace (only when the config has model components) */
    if (z.header[6] > 0) {
      libzpaq::Predictor pr(z);
      pr.init();
      for (int k = 0; k < N; ++k) {
        const int c = data[k];
        /* predict and update each bit in turn, the way the coder does:
         * calling predict() several times in a row is not equivalent
         * because update() re-reads the bit history it just wrote. */
        fprintf(rf, "P %d", k);
        for (int j = 7; j >= 0; --j) {
          fprintf(rf, " %d", pr.predict());
          pr.update((c >> j) & 1);
        }
        fprintf(rf, "\n");
      }
    }

    /* Encoder */
    std::string enc;
    {
      MemWriter w;
      libzpaq::Encoder e(z);
      e.out = &w;
      e.init();
      for (int k = 0; k < N; ++k) e.compress(data[k]);
      e.compress(-1);
      for (int k = 0; k < 4; ++k) w.put(0);
      enc = w.s;
      std::string h;
      hex(enc.data(), (int)enc.size(), h);
      fprintf(rf, "ENC %d %s\n", (int)enc.size(), h.c_str());
    }

    /* Decoder round trip */
    {
      libzpaq::StringBuffer sb;
      sb.write(enc.data(), (int)enc.size());
      libzpaq::Decoder d(z);
      d.in = &sb;
      d.init();
      std::string out;
      int c;
      int count = 0;
      while ((c = d.decompress()) >= 0) {
        out += char(c);
        ++count;
      }
      fprintf(rf, "DEC %d %s\n", count, sha1hex(out.data(), (int)out.size()).c_str());
    }
    fflush(mf);
    fflush(rf);
  }

  fclose(mf);
  fclose(rf);
  return 0;
}
