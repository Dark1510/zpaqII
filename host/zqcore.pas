{ zpaqII - bindings to the C core (include/zq.h).

  Only opaque pointers cross the boundary: the host never depends on struct
  layouts except for the Reader/Writer callback records, which must match the
  C ABI (three pointers each).  Every call that can fail returns -1 and leaves
  the message in zq_last_error(). }

unit zqcore;
{$mode objfpc}{$H+}

interface

type
  TZqGet   = function(ctx: Pointer): LongInt; cdecl;
  TZqRead  = function(ctx: Pointer; buf: PAnsiChar; n: LongInt): LongInt; cdecl;
  TZqPut   = procedure(ctx: Pointer; c: LongInt); cdecl;
  TZqWrite = procedure(ctx: Pointer; buf: PAnsiChar; n: LongInt); cdecl;

  TZqReader = record
    get: TZqGet;
    read: TZqRead;
    ctx: Pointer;
  end;

  TZqWriter = record
    put: TZqPut;
    write: TZqWrite;
    ctx: Pointer;
  end;

  PZqReader = ^TZqReader;
  PZqWriter = ^TZqWriter;

{$LinkLib zqcore}

type
  TZqBuf = type Pointer;
  TZqSha1 = type Pointer;
  TZqAes = type Pointer;
  TZqDecompresser = type Pointer;

{ --- errors --- }
function zq_last_error: PAnsiChar; cdecl; external;

{ --- buffers --- }
function zq_buf_new(init: SizeUInt): TZqBuf; cdecl; external;
procedure zq_buf_free(b: TZqBuf); cdecl; external;
procedure zq_buf_put(b: TZqBuf; c: LongInt); cdecl; external;
procedure zq_buf_write(b: TZqBuf; p: PAnsiChar; n: LongInt); cdecl; external;
procedure zq_buf_resize(b: TZqBuf; n: SizeUInt); cdecl; external;
function zq_buf_size(b: TZqBuf): SizeUInt; cdecl; external;
function zq_buf_data(b: TZqBuf): PAnsiChar; cdecl; external;
procedure zq_buf_reset(b: TZqBuf); cdecl; external;
procedure zq_buf_seek_read(b: TZqBuf; pos: SizeUInt); cdecl; external;
function zq_buf_get(b: TZqBuf): LongInt; cdecl; external;
procedure zq_buf_set_limit(b: TZqBuf; n: SizeUInt); cdecl; external;
function zq_buf_read(b: TZqBuf; p: PAnsiChar; n: LongInt): LongInt; cdecl; external;
function zq_buf_reader(b: TZqBuf): TZqReader; cdecl; external;
function zq_buf_writer(b: TZqBuf): TZqWriter; cdecl; external;

{ --- SHA-1 --- }
type
  TZqSha256 = Pointer;

function zq_sha256_new: TZqSha256; cdecl; external;
procedure zq_sha256_free(s: TZqSha256); cdecl; external;
procedure zq_sha256_put(s: TZqSha256; c: LongInt); cdecl; external;
procedure zq_sha256_write(s: TZqSha256; buf: PAnsiChar; n: Int64); cdecl; external;
function zq_sha256_result(s: TZqSha256): PAnsiChar; cdecl; external;

function zq_sha1_new: TZqSha1; cdecl; external;
procedure zq_sha1_free(s: TZqSha1); cdecl; external;
procedure zq_sha1_init(s: TZqSha1); cdecl; external;
procedure zq_sha1_put(s: TZqSha1; c: LongInt); cdecl; external;
procedure zq_sha1_write(s: TZqSha1; buf: PAnsiChar; n: Int64); cdecl; external;
function zq_sha1_result(s: TZqSha1): PAnsiChar; cdecl; external;
function zq_sha1_usize(s: TZqSha1): QWord; cdecl; external;

{ --- extra digests (integrity lists) --- }
type
  TZqCrc32 = record
    crc: Cardinal;
  end;
  PZqCrc32 = ^TZqCrc32;

  { Matches struct zq_xxh64 in include/zq.h byte for byte. }
  TZqXxh64 = record
    v1, v2, v3, v4: QWord;
    total: QWord;
    seed: QWord;
    n: LongInt;
    started: LongInt;
    buf: array[0..31] of Byte;
  end;
  PZqXxh64 = ^TZqXxh64;

procedure zq_crc32_init(var s: TZqCrc32); cdecl; external;
procedure zq_crc32_write(var s: TZqCrc32; p: Pointer; n: SizeInt); cdecl; external;
function zq_crc32_result(s: PZqCrc32): Cardinal; cdecl; external;
procedure zq_xxh64_init(var s: TZqXxh64; seed: QWord); cdecl; external;
procedure zq_xxh64_write(var s: TZqXxh64; p: Pointer; n: SizeInt); cdecl; external;
function zq_xxh64_result(s: PZqXxh64): QWord; cdecl; external;

{ --- fragment scanner: the cut loop of add() --- }
type
  { Must match struct zq_fragscan in include/zq.h byte for byte. }
  TZqFragscan = record
    sz: Int64;
    h: Cardinal;
    hits: Cardinal;
    c1: LongInt;
    o1: array[0..255] of Byte;
  end;
  PZqFragscan = ^TZqFragscan;

procedure zq_fragscan_init(st: PZqFragscan); cdecl; external;
function zq_frag_scan(buf: PAnsiChar; n: Int64; st: PZqFragscan;
                      maxfrag, minfrag: Int64; fragment: LongInt;
                      cut: PInteger): Int64; cdecl; external;

{ A complete fragment of a scanned chain (parallel fragmenter).  Must match
  struct zq_fragcut in include/zq.h byte for byte. }
type
  TZqFragcut = record
    start: Int64;
    len: Int64;
    hits: Cardinal;
    o1: array[0..255] of Byte;
    sha1: array[0..19] of Byte;
  end;
  PZqFragcut = ^TZqFragcut;

function zq_frag_chain(base: PAnsiChar; from_, fine, stop_at: Int64;
                       dopo: LongInt; maxfrag, minfrag: Int64;
                       fragment: LongInt; outbuf: PZqFragcut; cap: LongInt;
                       done: PInteger; open: PZqFragcut): LongInt; cdecl;
  external;

{ --- AES / key stretching / random --- }
function zq_aes_new(key: PAnsiChar; keylen: LongInt; iv: PAnsiChar): TZqAes; cdecl; external;
procedure zq_aes_free(a: TZqAes); cdecl; external;
procedure zq_aes_encrypt(a: TZqAes; buf: PAnsiChar; n: LongInt; offset: QWord); cdecl; external;
procedure zq_stretch_key(outkey, inkey, salt: PAnsiChar); cdecl; external;
procedure zq_random(buf: PAnsiChar; n: LongInt); cdecl; external;

{ --- decompressor --- }
function zq_d_new: TZqDecompresser; cdecl; external;
procedure zq_d_free(d: TZqDecompresser); cdecl; external;
procedure zq_d_set_input(d: TZqDecompresser; var r: TZqReader); cdecl; external;
procedure zq_d_set_output(d: TZqDecompresser; var w: TZqWriter); cdecl; external;
procedure zq_d_set_sha1(d: TZqDecompresser; s: TZqSha1); cdecl; external;
function zq_d_find_block(d: TZqDecompresser; memptr: PDouble): LongInt; cdecl; external;
function zq_d_find_filename(d: TZqDecompresser; w: PZqWriter): LongInt; cdecl; external;
function zq_d_read_comment(d: TZqDecompresser; w: PZqWriter): LongInt; cdecl; external;
function zq_d_decompress(d: TZqDecompresser; n: LongInt): LongInt; cdecl; external;
function zq_d_read_segment_end(d: TZqDecompresser; sha1str: PAnsiChar): LongInt; cdecl; external;
function zq_d_buffered(d: TZqDecompresser): LongInt; cdecl; external;

{ --- compressor --- }
function zq_compress_block(b: TZqBuf; var w: TZqWriter; method, filename, comment: PAnsiChar; dosha1: LongInt): LongInt; cdecl; external;

implementation

end.
