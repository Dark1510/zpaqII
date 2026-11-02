{ zpaqII - journaling archive (JIDAC) reader, lister and extractor.
  Port of zpaq 7.15 (Unlicense, Matt Mahoney), unix side. }

unit zqjidac;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes, BaseUnix, SyncObjs, zqcore, zqfile, zqarchive;

const
  ZQ_RED_DT: array[0..255] of Byte = (
    160, 80, 53, 40, 32, 26, 22, 20, 17, 16, 14, 13, 12, 11, 10, 10,
    9, 8, 8, 8, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5,
    4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  );

type
  THT = record
    sha1: array[0..19] of Byte;
    usize: Int64;
    csize: Int64;
  end;

  TBlockInfo = record
    start: Cardinal;
    offset: Int64;
    usize: Int64;
    bsize: Int64;
    frags: Cardinal;
  end;

  TVer = record
    firstFragment: Cardinal;
    offset: Int64;
    data_offset: Int64;
    date: Int64;
    lastdate: Int64;
    csize: Int64;
    updates: Integer;
    deletes: Integer;
  end;

  TDT = class
  public
    date: Int64;
    size: Int64;
    attr: Int64;
    data: Integer;     { 0 = not present in the update, 1 = keep }
    ptr: array of Cardinal;
  end;

  TJidac = class
  private
    function GetDT(const fn: string): TDT;
    function LoadBlock(const arcName, password: string; nb: Integer;
                       osb: TZqBuf; out bad: Boolean): Boolean;
    procedure ScanInto(edt: TStringList; const name: string);
  public
    fBlockMem: Double;       { memory used by the last block decompressed }
    fBlockFiles: array of array of Integer;
    fNeedCount: array of Cardinal;
    archive: string;
    password: string;
    version: Int64;
    cmdDate: Int64;   { update date for streaming entries }
    ht: array of THT;
    blocks: array of TBlockInfo;
    ver: array of TVer;
    dt: TStringList;
    files: TStringList;      { selection prefixes, may be empty }
    tofiles: TStringList;    { -to: rename targets (files[i] -> tofiles[i]) }
    onlyfiles: TStringList;  { -only: include only these prefixes }
    notfiles: TStringList;   { -not: exclude these prefixes }
    nottype: string;         { -not =...: list chars to suppress }
    fragment: Integer;       { -fragment N: 2^N KiB average fragment size }
    all: Integer;            { -all N: list/extract versions in N digit dirs }
    summary: Integer;        { -summary N: top N by size, -1 = fragment ids }
    dotest: Boolean;         { -test: verify but do not write files }
    noattributes: Boolean;   { -noattributes: ignore/save attributes }
    repack: string;          { -repack F: write the archive to F }
    newpassword: string;     { -repack F [X]: new key digest ('' = none) }
    methodSet: Boolean;      { -method given (rejected with -repack -all) }
    threads: Integer;        { -tN: compression threads, 0 = one per CPU }
    indexName: string;       { -index F: index archive used by add/extract }
    dhsize: Int64;           { total size of D blocks according to H blocks }
    dcsize: Int64;           { total size of D blocks according to C blocks }
    lastOffset: Int64;       { end of the last block scanned (upstream csize) }
    nfilesSeen: Cardinal;    { files found while scanning }
    isJidac: Boolean;
    constructor Create(const arc, pass: string; ver_: Int64);
    destructor Destroy; override;
    function IsSelected(const fn: string; rn: Boolean): Boolean;
    function Rename(const name: string): string;
    function WriteIndex(const indexFile: string; force: Boolean): Integer;
    function ReadArchive(out errors: Integer; out nfiles: Cardinal;
                        const src: string = ''): Int64;
    function List(force: Boolean): Integer;
    function Equal(dtr: TDT; const fn: string): Boolean;
    function Extract(force: Boolean; testOnly: Boolean = False): Integer;
    function RepackWhole(force: Boolean): Integer;
    function RepackSelected(force: Boolean): Integer;
    function Add(const addFiles: TStringList; const method: string; date: Int64;
                 force: Boolean; out errors: Integer): Integer;
    function FindBlockFor(frag: Cardinal): Integer;
    procedure PrepareExtract(force, testOnly: Boolean;
                             out totalFiles: Integer; out totalSize: Int64);
  end;

{ -summary progress prefix: percentage and ETA since the program started }
procedure PrintProgress(ts, td: Int64; sum: Integer);
var ZqStartTime: Int64 = 0;

const
  DEFAULT_VERSION = Int64(99999999999999);

implementation

procedure PrintProgress(ts, td: Int64; sum: Integer);
var
  eta: Double;
begin
  if td > ts then td := ts;
  if td >= 1000000 then
  begin
    eta := (MTime - ZqStartTime) * (ts - td) / (td + 1.0);
    Write(Format('%5.2f%% %d:%.2d:%.2d ',
                 [td * 100.0 / (ts + 0.5), Trunc(eta / 3600000),
                  Trunc(eta / 60000) mod 60, Trunc(eta / 1000) mod 60]));
    if sum > 0 then
    begin
      Write(#13);
      Flush(Output);
    end;
  end;
end;

type
  TNameSink = class
    s: AnsiString;
  end;

  TOutSink = class
    buf: TZqBuf;
  end;

  { Writes every byte to two archives (archive + index, upstream WriterPair). }
  TWriterPair = class
    a, b: TOutputArchive;
  end;

  { ---- Parallel block compression (upstream CompressJob) ----------------
    The main thread splits the input into blocks exactly as before (that is
    what decides the archive bytes), hands each block to a pool of worker
    threads and writes the compressed blocks to the archive in submission
    order.  The output is therefore independent of the thread count. }

  TCompSlot = class
  public
    inp, outb: TZqBuf;          { uncompressed input, compressed output }
    sink: TOutSink;
    meth, fname, comment: AnsiString;
    ev: TEvent;                 { set by the worker when done }
    err: Boolean;
    errmsg: string;
  end;

  { ---- Parallel block decompression (upstream ExtractJob) -------------
    Blocks that a selected file needs are decompressed ahead of time, in
    ascending order, into a small ring of slot buffers. }

  TDecSlot = class
  public
    nb: Integer;              { block number, -1 = free }
    buf: TZqBuf;
    ok, bad, done, pinned: Boolean;
    ev: TEvent;
  end;

  TDecPool = class;

  TDecWorker = class(TThread)
  private
    fPool: TDecPool;
  protected
    procedure Execute; override;
  public
    constructor Create(p: TDecPool);
  end;

  TDecPool = class
  private
    fOwner: TJidac;
    fSlots: array of TDecSlot;
    fWorkers: array of TDecWorker;
    fNeeded: array of Integer;
    fNext: Integer;           { next index in fNeeded to prefetch }
    fStop: Integer;
    fSync: TZqBuf;            { out-of-order fallback }
    fLock: TCriticalSection;
    fCurrent: Integer;        { slot the main thread is reading, -1 = none }
    function Find(nb: Integer): TDecSlot;
    function FindIdx(nb: Integer): Integer;
    function TakeWork(var nb: Integer): TDecSlot;
  public
    constructor Create(owner: TJidac; const needed: array of Integer;
                       nthreads: Integer);
    destructor Destroy; override;
    procedure Start;
    function Get(nb: Integer; out bad: Boolean; out size: Int64): PAnsiChar;
  end;

  TCompPool = class;

  TCompWorker = class(TThread)
  private
    fPool: TCompPool;
  protected
    procedure Execute; override;
  public
    constructor Create(p: TCompPool);
  end;

  TCompPool = class
  private
    fSlots: array of TCompSlot;
    fWorkers: array of TCompWorker;
    fLock: TCriticalSection;
    fWork: TEvent;
    fNextSubmit, fNextCompress, fNextFlush, fQueued, fStop: Integer;
    procedure CompressOne(s: TCompSlot);
    function Take: TCompSlot;
    function Stopped: Boolean;
  public
    constructor Create(nthreads: Integer);
    destructor Destroy; override;
    function Slots: Integer;
    function Queued: Integer;
    procedure Start;
    procedure Submit(var buf: TZqBuf; const meth, fname, comment: string);
    function FlushOne(const w: TZqWriter): Int64;
  end;



procedure PairPut(ctx: Pointer; c: LongInt); cdecl;
var
  p: TWriterPair;
begin
  p := TWriterPair(ctx);
  p.a.Put(c);
  if p.b <> nil then p.b.Put(c);
end;

procedure PairWrite(ctx: Pointer; buf: PAnsiChar; n: LongInt); cdecl;
var
  p: TWriterPair;
begin
  p := TWriterPair(ctx);
  p.a.WriteBuf(buf, n);
  if p.b <> nil then p.b.WriteBuf(buf, n);
end;

function PairWriter(p: TWriterPair): TZqWriter;
begin
  Result.put := @PairPut;
  Result.write := @PairWrite;
  Result.ctx := Pointer(p);
end;

procedure NameSinkPut(ctx: Pointer; c: LongInt); cdecl;
begin
  TNameSink(ctx).s := TNameSink(ctx).s + AnsiChar(c);
end;

function NameSinkWriter(k: TNameSink): TZqWriter;
begin
  Result.put := @NameSinkPut;
  Result.write := nil;
  Result.ctx := Pointer(k);
end;

procedure OutSinkPut(ctx: Pointer; c: LongInt); cdecl;
begin
  zq_buf_put(TOutSink(ctx).buf, c);
end;

procedure OutSinkWrite(ctx: Pointer; p: PAnsiChar; n: LongInt); cdecl;
begin
  zq_buf_write(TOutSink(ctx).buf, p, n);
end;

function OutSinkWriter(k: TOutSink): TZqWriter;
begin
  Result.put := @OutSinkPut;
  Result.write := @OutSinkWrite;
  Result.ctx := Pointer(k);
end;

constructor TCompWorker.Create(p: TCompPool);
begin
  inherited Create(True);       { suspended until Start }
  FreeOnTerminate := False;
  fPool := p;
end;

procedure TCompWorker.Execute;
var
  s: TCompSlot;
begin
  while True do
  begin
    fPool.fWork.WaitFor(INFINITE);
    s := fPool.Take;          { resets the event when there is no work left }
    if fPool.Stopped then Exit;
    if s <> nil then fPool.CompressOne(s);
  end;
end;

constructor TCompPool.Create(nthreads: Integer);
var
  i: Integer;
begin
  inherited Create;
  if nthreads < 1 then nthreads := 1;
  SetLength(fSlots, nthreads);
  for i := 0 to nthreads - 1 do
  begin
    fSlots[i] := TCompSlot.Create;
    fSlots[i].inp := zq_buf_new(0);
    fSlots[i].outb := zq_buf_new(1 shl 16);
    fSlots[i].sink := TOutSink.Create;
    fSlots[i].sink.buf := fSlots[i].outb;
    fSlots[i].ev := TEvent.Create(nil, False, False, '');
  end;
  fLock := TCriticalSection.Create;
  fWork := TEvent.Create(nil, True, False, '');   { manual reset: see Take }
  SetLength(fWorkers, nthreads);
  for i := 0 to nthreads - 1 do fWorkers[i] := TCompWorker.Create(Self);
end;

destructor TCompPool.Destroy;
var
  i: Integer;
begin
  fLock.Acquire;
  try
    fStop := 1;
  finally
    fLock.Release;
  end;
  for i := 0 to High(fWorkers) do fWork.SetEvent;
  for i := 0 to High(fWorkers) do
  begin
    fWorkers[i].WaitFor;
    fWorkers[i].Free;
  end;
  for i := 0 to High(fSlots) do
  begin
    zq_buf_free(fSlots[i].inp);
    zq_buf_free(fSlots[i].outb);
    fSlots[i].sink.Free;
    fSlots[i].ev.Free;
    fSlots[i].Free;
  end;
  fWork.Free;
  fLock.Free;
  inherited Destroy;
end;

procedure TCompPool.Start;
var
  i: Integer;
begin
  for i := 0 to High(fWorkers) do fWorkers[i].Start;
end;

function TCompPool.Slots: Integer;
begin
  Result := Length(fSlots);
end;

function TCompPool.Queued: Integer;
begin
  Result := fQueued;
end;

function TCompPool.Take: TCompSlot;
begin
  Result := nil;
  fLock.Acquire;
  try
    if fStop <> 0 then Exit;
    if fNextCompress >= fNextSubmit then
    begin
      { No queued work: clear the event under the same lock that Submit uses,
        so a job queued right now cannot lose its wakeup. }
      fWork.ResetEvent;
      Exit;
    end;
    Result := fSlots[fNextCompress mod Length(fSlots)];
    Inc(fNextCompress);
  finally
    fLock.Release;
  end;
end;

function TCompPool.Stopped: Boolean;
begin
  Result := fStop <> 0;
end;

procedure TCompPool.CompressOne(s: TCompSlot);
var
  wr: TZqWriter;
begin
  zq_buf_resize(s.outb, 0);
  wr := OutSinkWriter(s.sink);
  s.err := zq_compress_block(s.inp, wr, PAnsiChar(s.meth), PAnsiChar(s.fname),
                             PAnsiChar(s.comment), 1) < 0;
  if s.err then s.errmsg := string(zq_last_error) else s.errmsg := '';
  zq_buf_resize(s.inp, 0);
  s.ev.SetEvent;
end;

procedure TCompPool.Submit(var buf: TZqBuf; const meth, fname, comment: string);
var
  s: TCompSlot;
  tmp: TZqBuf;
begin
  fLock.Acquire;
  try
    s := fSlots[fNextSubmit mod Length(fSlots)];
    tmp := s.inp;               { swap: no copy of the block data }
    s.inp := buf;
    buf := tmp;
    zq_buf_resize(buf, 0);
    s.meth := AnsiString(meth);
    s.fname := AnsiString(fname);
    s.comment := AnsiString(comment);
    s.err := False;
    s.errmsg := '';
    Inc(fNextSubmit);
    Inc(fQueued);
    fWork.SetEvent;
  finally
    fLock.Release;
  end;
end;

function TCompPool.FlushOne(const w: TZqWriter): Int64;
var
  s: TCompSlot;
  n, left: Int64;
  p: PAnsiChar;
begin
  if fQueued <= 0 then Exit(0);
  s := fSlots[fNextFlush mod Length(fSlots)];
  s.ev.WaitFor(INFINITE);
  if s.err then
    raise Exception.Create('compression failed: ' + s.errmsg);
  n := Int64(zq_buf_size(s.outb));
  p := zq_buf_data(s.outb);
  left := n;
  while left > 0 do
  begin
    if Assigned(w.write) then
    begin
      if left > MaxInt then
      begin
        w.write(w.ctx, p, MaxInt);
        Dec(left, MaxInt);
      end
      else
      begin
        w.write(w.ctx, p, LongInt(left));
        left := 0;
      end;
    end
    else
    begin
      w.put(w.ctx, Ord(p^));
      Inc(p);
      Dec(left);
    end;
  end;
  zq_buf_resize(s.outb, 0);
  fLock.Acquire;
  try
    Inc(fNextFlush);
    Dec(fQueued);
  finally
    fLock.Release;
  end;
  Result := n;
end;



{ ---- parallel block decompression ---------------------------------- }

constructor TDecWorker.Create(p: TDecPool);
begin
  inherited Create(True);
  FreeOnTerminate := False;
  fPool := p;
end;

procedure TDecWorker.Execute;
var
  nb: Integer;
  s: TDecSlot;
  ok, bad: Boolean;
begin
  while True do
  begin
    s := fPool.TakeWork(nb);
    if fPool.fStop <> 0 then Exit;
    if s = nil then
    begin
      Sleep(1);                 { window full; retry }
      Continue;
    end;
    bad := False;
    ok := fPool.fOwner.LoadBlock(fPool.fOwner.archive, fPool.fOwner.password,
                                 nb, s.buf, bad);
    fPool.fLock.Acquire;
    try
      s.ok := ok;
      s.bad := bad;
      s.done := True;
    finally
      fPool.fLock.Release;
    end;
    s.ev.SetEvent;
  end;
end;

constructor TDecPool.Create(owner: TJidac; const needed: array of Integer;
                            nthreads: Integer);
var
  i: Integer;
begin
  inherited Create;
  fOwner := owner;
  SetLength(fNeeded, Length(needed));
  for i := 0 to High(needed) do fNeeded[i] := needed[i];
  fNext := 0;
  fStop := 0;
  fCurrent := -1;
  fSync := zq_buf_new(1 shl 16);
  fLock := TCriticalSection.Create;
  if nthreads < 1 then nthreads := 1;
  SetLength(fSlots, nthreads + 1);
  for i := 0 to High(fSlots) do
  begin
    fSlots[i] := TDecSlot.Create;
    fSlots[i].nb := -1;
    fSlots[i].buf := zq_buf_new(1 shl 16);
    fSlots[i].done := False;
    fSlots[i].pinned := False;
    fSlots[i].ev := TEvent.Create(nil, False, False, '');
  end;
  SetLength(fWorkers, nthreads);
  for i := 0 to nthreads - 1 do fWorkers[i] := TDecWorker.Create(Self);
end;

destructor TDecPool.Destroy;
var
  i: Integer;
begin
  fStop := 1;
  for i := 0 to High(fWorkers) do
  begin
    fWorkers[i].WaitFor;
    fWorkers[i].Free;
  end;
  for i := 0 to High(fSlots) do
  begin
    zq_buf_free(fSlots[i].buf);
    fSlots[i].ev.Free;
    fSlots[i].Free;
  end;
  zq_buf_free(fSync);
  fLock.Free;
  inherited Destroy;
end;

procedure TDecPool.Start;
var
  i: Integer;
begin
  for i := 0 to High(fWorkers) do fWorkers[i].Start;
end;

function TDecPool.Find(nb: Integer): TDecSlot;
var
  i: Integer;
begin
  Result := nil;
  for i := 0 to High(fSlots) do
    if fSlots[i].nb = nb then Exit(fSlots[i]);
end;

function TDecPool.FindIdx(nb: Integer): Integer;
var
  i: Integer;
begin
  Result := -1;
  for i := 0 to High(fSlots) do
    if fSlots[i].nb = nb then Exit(i);
end;

{ Reserve a slot for the next block to prefetch, evicting the lowest numbered
  finished block if the ring is full.  Called by workers. }
function TDecPool.TakeWork(var nb: Integer): TDecSlot;
var
  i, best: Integer;
begin
  Result := nil;
  nb := -1;
  fLock.Acquire;
  try
    if (fStop <> 0) or (fNext >= Length(fNeeded)) then Exit;
    best := -1;
    for i := 0 to High(fSlots) do
    begin
      if fSlots[i].pinned then Continue;
      if fSlots[i].nb < 0 then
      begin
        best := i;
        Break;
      end;
      if fSlots[i].done and ((best < 0) or (fSlots[i].nb < fSlots[best].nb)) then
        best := i;
    end;
    if best < 0 then Exit;            { all busy: caller retries }
    nb := fNeeded[fNext];
    Inc(fNext);
    Result := fSlots[best];
    Result.nb := nb;
    Result.done := False;
    Result.ok := False;
    Result.bad := False;
    Result.ev.ResetEvent;
  finally
    fLock.Release;
  end;
end;

function TDecPool.Get(nb: Integer; out bad: Boolean; out size: Int64): PAnsiChar;
var
  s: TDecSlot;
  found: Boolean;
begin
  bad := False;
  size := 0;
  while True do
  begin
    fLock.Acquire;
    try
      if fCurrent >= 0 then
      begin
        fSlots[fCurrent].pinned := False;
        fCurrent := -1;
      end;
      s := Find(nb);
      found := s <> nil;
      if found and s.done then
      begin
        s.pinned := True;
        fCurrent := FindIdx(nb);
      end;
    finally
      fLock.Release;
    end;
    if not found then
    begin
      { Not in the prefetch window: decompress it here. }
      zq_buf_resize(fSync, 0);
      if not fOwner.LoadBlock(fOwner.archive, fOwner.password, nb, fSync, bad)
      then
        Exit(nil);
      size := Int64(zq_buf_size(fSync));
      Exit(zq_buf_data(fSync));
    end;
    if s.done then
    begin
      bad := s.bad;
      if not s.ok then Exit(nil);
      size := Int64(zq_buf_size(s.buf));
      Exit(zq_buf_data(s.buf));
    end;
    s.ev.WaitFor(INFINITE);
  end;
end;

function BToI(var s: PAnsiChar): Cardinal;
begin
  Result := Ord(s[0]) or (Ord(s[1]) shl 8) or (Ord(s[2]) shl 16) or
            (Cardinal(Ord(s[3])) shl 24);
  Inc(s, 4);
end;

function BToL(var s: PAnsiChar): Int64;
var
  lo, hi: Cardinal;
begin
  lo := BToI(s);
  hi := BToI(s);
  Result := Int64(lo) or (Int64(hi) shl 32);
end;

function IsDigitChar(c: AnsiChar): Boolean;
begin
  Result := (c >= '0') and (c <= '9');
end;

constructor TJidac.Create(const arc, pass: string; ver_: Int64);
begin
  inherited Create;
  archive := arc;
  password := pass;
  version := ver_;
  cmdDate := 0;
  SetLength(ht, 1);   { element 0 is not used, like upstream }
  FillChar(ht[0], SizeOf(THT), 0);
  SetLength(blocks, 0);
  SetLength(ver, 1);
  ver[0].firstFragment := 0;
  ver[0].offset := 0;
  ver[0].data_offset := 0;
  ver[0].date := 0;
  ver[0].lastdate := 0;
  ver[0].csize := 0;
  ver[0].updates := 0;
  ver[0].deletes := 0;
  dt := TStringList.Create;
  dt.Sorted := True;
  dt.CaseSensitive := True;
  dt.Duplicates := dupIgnore;
  files := TStringList.Create;
  files.CaseSensitive := True;
  tofiles := TStringList.Create;
  tofiles.CaseSensitive := True;
  onlyfiles := TStringList.Create;
  onlyfiles.CaseSensitive := True;
  notfiles := TStringList.Create;
  notfiles.CaseSensitive := True;
  nottype := '';
  fragment := 6;
  all := 0;
  summary := 0;
  dotest := False;
  noattributes := False;
  repack := '';
  newpassword := '';
  methodSet := False;
  threads := 0;
  indexName := '';
  dhsize := 0;
  dcsize := 0;
  lastOffset := 0;
  nfilesSeen := 0;
  isJidac := False;
end;

destructor TJidac.Destroy;
var
  i: Integer;
begin
  for i := 0 to dt.Count - 1 do dt.Objects[i].Free;
  dt.Free;
  files.Free;
  tofiles.Free;
  onlyfiles.Free;
  notfiles.Free;
  inherited;
end;

{ Case-insensitive path prefix match with * and ? wildcards (zpaq ispath). }
function IsPath(a, b: PAnsiChar): Boolean;
var
  ca, cb: Integer;
begin
  while a^ <> #0 do
  begin
    ca := Ord(a^);
    cb := Ord(b^);
    if (ca >= Ord('A')) and (ca <= Ord('Z')) then Inc(ca, 32);
    if (cb >= Ord('A')) and (cb <= Ord('Z')) then Inc(cb, 32);
    if ca = Ord('*') then
    begin
      while True do
      begin
        if IsPath(a + 1, b) then Exit(True);
        if b^ = #0 then Exit(False);
        Inc(b);
      end;
    end
    else if ca = Ord('?') then
    begin
      if b^ = #0 then Exit(False);
    end
    else if (ca = cb) and (ca = Ord('/')) and
            (PAnsiChar(a + 1)^ = #0) then
      Exit(True)
    else if ca <> cb then
      Exit(False);
    Inc(a);
    Inc(b);
  end;
  Result := (b^ = #0) or (b^ = '/');
end;

{ Test whether filename is selected by files, -only and -not.  When rn is
  true the renamed name (via -to) is used for the files[] comparison, which is
  what upstream does for list and add. }
function TJidac.IsSelected(const fn: string; rn: Boolean): Boolean;
var
  i: Integer;
  matched: Boolean;
  p: AnsiString;
begin
  p := AnsiString(fn);
  matched := True;
  if files.Count > 0 then
  begin
    matched := False;
    i := 0;
    while (i < files.Count) and (not matched) do
    begin
      if rn and (i < tofiles.Count) then
      begin
        if IsPath(PAnsiChar(AnsiString(tofiles[i])), PAnsiChar(p)) then
          matched := True;
      end
      else if IsPath(PAnsiChar(AnsiString(files[i])), PAnsiChar(p)) then
        matched := True;
      Inc(i);
    end;
  end;
  if not matched then Exit(False);
  if onlyfiles.Count > 0 then
  begin
    matched := False;
    for i := 0 to onlyfiles.Count - 1 do
      if IsPath(PAnsiChar(AnsiString(onlyfiles[i])), PAnsiChar(p)) then
      begin
        matched := True;
        Break;
      end;
  end;
  if not matched then Exit(False);
  for i := 0 to notfiles.Count - 1 do
    if IsPath(PAnsiChar(AnsiString(notfiles[i])), PAnsiChar(p)) then
      Exit(False);
  Result := True;
end;

{ a + '/' + b with a single separator (upstream append_path). }
function AppendPath(const a, b: string): string;
var
  x, y: string;
begin
  x := a;
  y := b;
  if (Length(y) > 0) and (y[1] = '/') then Delete(y, 1, 1);
  if (Length(x) > 0) and (x[Length(x)] = '/') then
    SetLength(x, Length(x) - 1);
  Result := x + '/' + y;
end;

{ Rename a stored name using tofiles[] (upstream Jidac::rename). }
function TJidac.Rename(const name: string): string;
var
  i, n, fl: Integer;
begin
  if (files.Count = 0) and (tofiles.Count > 0) then
    Exit(AppendPath(tofiles[0], name));
  n := Length(name);
  for i := 0 to files.Count - 1 do
  begin
    if i >= tofiles.Count then Break;
    fl := Length(files[i]);
    if (fl <= n) and (files[i] = Copy(name, 1, fl)) then
      Exit(tofiles[i] + Copy(name, fl + 1, MaxInt));
  end;
  Result := name;
end;

function TJidac.GetDT(const fn: string): TDT;
var
  idx: Integer;
begin
  idx := dt.IndexOf(fn);
  if idx >= 0 then
    Result := TDT(dt.Objects[idx])
  else
  begin
    Result := TDT.Create;
    Result.date := 0;
    Result.size := 0;
    Result.attr := 0;
    Result.data := 0;
    SetLength(Result.ptr, 0);
    dt.AddObject(fn, Result);
  end;
end;

function TJidac.FindBlockFor(frag: Cardinal): Integer;
var
  lo, hi, mid: Integer;
begin
  lo := 0;
  hi := High(blocks);
  Result := -1;
  while lo <= hi do
  begin
    mid := (lo + hi) div 2;
    if blocks[mid].start <= frag then
    begin
      Result := mid;
      lo := mid + 1;
    end
    else
      hi := mid - 1;
  end;
  if (Result >= 0) and
     (frag >= blocks[Result].start + blocks[Result].frags) then
    Result := -1;
end;

{ ------------------------------------------------------------------ }
{ index creation (extract -index F)                                    }
{ ------------------------------------------------------------------ }

type
  TByte34 = array[0..33] of Byte;

{ The C block of a transaction is rewritten with a data size of 0 so the
  index can be read without the D blocks: 8 zero bytes (csize), the 4 byte
  compressed size terminator, the SHA-1 marker, SHA-1 of 8 zero bytes and the
  end of block marker. }
const
  INDEX_CBLOCK_PATCH: TByte34 = (
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,
    $fd,
    $05, $fe, $40, $57, $53, $16, $6f, $12, $55, $59, $e7, $c9, $ac, $55,
    $86, $54, $f1, $07, $c7, $e9,
    $ff);

function ReadFully(a: TInputArchive; buf: PAnsiChar; n: Int64): Boolean;
var
  got, want: LongInt;
begin
  while n > 0 do
  begin
    if n > $40000 then want := $40000 else want := LongInt(n);
    got := a.Read(buf, want);
    if got <> want then Exit(False);
    Inc(buf, got);
    Dec(n, got);
  end;
  Result := True;
end;

function CopyBytes(a: TInputArchive; o: TOutputArchive; n: Int64): Boolean;
var
  buf: array[0..65535] of AnsiChar;
  got, want: LongInt;
begin
  while n > 0 do
  begin
    if n > SizeOf(buf) then want := SizeOf(buf) else want := LongInt(n);
    got := a.Read(@buf[0], want);
    if got <> want then Exit(False);
    o.WriteBuf(@buf[0], got);
    Dec(n, got);
  end;
  Result := True;
end;

{ Write an index of this archive to indexFile: every transaction keeps its C,
  H and I blocks and loses the D blocks (upstream's "extract -index F"). }
function TJidac.WriteIndex(const indexFile: string; force: Boolean): Integer;
var
  i: Integer;
  inArc: TInputArchive;
  outArc: TOutputArchive;
  hdr: array[0..255] of AnsiChar;
  hsize: Integer;
  endPos, n: Int64;
  salt: array[0..31] of AnsiChar;
  saltPtr: PAnsiChar;
  fs: TFileStream;
begin
  if Length(ver) < 2 then
    raise Exception.Create('no journaling data');
  if force and FileExistsUTF8(indexFile) then DeleteFileUTF8(indexFile);
  if FileExistsUTF8(indexFile) then
    raise Exception.Create('index file exists');

  saltPtr := nil;
  if ver[1].offset = 32 then   { encrypted archive: reuse its salt }
  begin
    if not FileExistsUTF8(SubPart(archive, 1)) then
      raise Exception.Create('cannot read part 1');
    fs := TFileStream.Create(SubPart(archive, 1), fmOpenRead or fmShareDenyNone);
    try
      if fs.Read(salt, 32) <> 32 then
        raise Exception.Create('cannot read salt');
    finally
      fs.Free;
    end;
    salt[0] := AnsiChar(Ord(salt[0]) xor (Ord('7') xor Ord('z')));
    saltPtr := @salt[0];
  end;

  inArc := TInputArchive.Create(archive, password);
  outArc := TOutputArchive.Create(indexFile, password, saltPtr, 0);
  try
    for i := 1 to High(ver) do
    begin
      if inArc.Tell() <> ver[i].offset then
        raise Exception.Create('I''m lost');
      hsize := Integer(ver[i].data_offset - ver[i].offset);
      if (hsize < 70) or (hsize > 255) then
        raise Exception.Create('bad C block size');
      if not ReadFully(inArc, @hdr[0], hsize) then
        raise Exception.Create('EOF in header');
      if (hdr[hsize - 36] <> #9) or (hdr[hsize - 22] <> #$fd) or
         (hdr[hsize - 1] <> #$ff) then
        raise Exception.Create('C block in weird format');
      Move(INDEX_CBLOCK_PATCH, hdr[hsize - 34], 34);
      outArc.WriteBuf(@hdr[0], hsize);
      inArc.Seek(ver[i].csize, 1);      { skip the D blocks }
      if i < High(ver) then endPos := ver[i + 1].offset
      else endPos := lastOffset;
      n := endPos - inArc.Tell();
      if (n < 0) or (not CopyBytes(inArc, outArc, n)) then
        raise Exception.Create('EOF');
    end;
    Write(indexFile, ' -> ', Format('%1.0f', [outArc.Tell + 0.0]));
    WriteLn;
    Result := 0;
  finally
    outArc.Free;
    inArc.Free;
  end;
end;

{ ------------------------------------------------------------------ }
{ read_archive                                                        }
{ ------------------------------------------------------------------ }

function TJidac.ReadArchive(out errors: Integer; out nfiles: Cardinal;
                            const src: string): Int64;
var
  inArc: TInputArchive;
  d: TZqDecompresser;
  rd: TZqReader;
  osb: TZqBuf;
  filesSeen: Cardinal;
  lastfile: string;
  block_offset, data_offset: Int64;
  arcName, allname: string;
  found_data, first, done, skip, restart: Boolean;
  mem: Double;
  rc: Integer;
  osink: TOutSink;
  fnameSink, commentSink: TNameSink;
  segs: Integer;
  nw: TZqWriter;
  sha: TZqSha1;
  sha1result: array[0..20] of AnsiChar;
  usizeBuf: Int64;
  fdate, num: Int64;
  i: Integer;
  ch: AnsiChar;
  jidacBlock: Boolean;
  s: PAnsiChar;
  send: PAnsiChar;
  nFrags: Cardinal;
  bsize: Int64;
  na, ni, j, f: Cardinal;
  dtr: TDT;
  len: Cardinal;
  blk: TBlockInfo;
  vr: TVer;
  jmp: Int64;
  fn: string;
  sbuf: array[0..3] of AnsiChar;
  nrd: LongInt;
  k: Cardinal;
  e: Exception;
  existing: TDT;
begin
  errors := 0;
  filesSeen := 0;
  SetLength(ht, 1);
  ht[0].usize := 0;
  ht[0].csize := 0;
  SetLength(blocks, 0);
  SetLength(ver, 1);
  ver[0].firstFragment := 0;
  ver[0].offset := 0;
  ver[0].data_offset := 0;
  ver[0].date := 0;
  ver[0].lastdate := 0;
  ver[0].csize := 0;
  ver[0].updates := 0;
  ver[0].deletes := 0;

  dcsize := 0;
  dhsize := 0;
  if src <> '' then arcName := src else arcName := archive;
  lastfile := arcName;
  if Copy(lastfile, Length(lastfile) - 4, 5) = '.zpaq' then
    SetLength(lastfile, Length(lastfile) - 5);
  if not FileExistsUTF8(SubPart(arcName, 1)) then
  begin
    { upstream's InputArchive::open() calls ioerr(): perror + throw }
    WriteLn(StdErr, SubPart(arcName, 1), ': ',
            SysErrorMessage(ESysENOENT));
    raise Exception.Create(SubPart(arcName, 1));
  end;
  Write(arcName);
  if version = DEFAULT_VERSION then Write(': ')
  else Write(' -until ', Format('%1.0f', [version + 0.0]), ': ');

  block_offset := 32 * Ord(password <> '');
  data_offset := block_offset;
  found_data := False;
  first := True;

  try
    inArc := TInputArchive.Create(arcName, password);
  except
    WriteLn(StdErr, arcName, ' not found.');
    errors := 1;
    Result := 0;
    Exit;
  end;

  osb := zq_buf_new(32832);
  sha := zq_sha1_new;
  osink := TOutSink.Create;
  osink.buf := osb;
  fnameSink := TNameSink.Create;
  commentSink := TNameSink.Create;
  d := nil;

  rd := InputReader(inArc);
  nrd := inArc.Read(@sbuf[0], 4);
  if (nrd > 0) and ((not CompareMem(@sbuf[0], PAnsiChar('7kSt'), 4)) and
     ((not CompareMem(@sbuf[0], PAnsiChar('zPQ'), 3)) or (sbuf[3] < #1))) then
  begin
    zq_buf_free(osb);
    zq_sha1_free(sha);
    osink.Free;
    fnameSink.Free;
    commentSink.Free;
    inArc.Free;
    raise Exception.Create('password incorrect');
  end;
  inArc.Seek(-nrd, 1);

  done := False;
  while not done do
  begin
    if d <> nil then zq_d_free(d);
    d := zq_d_new;
    rd := InputReader(inArc);
    zq_d_set_input(d, rd);
    skip := False;
    restart := False;
    segs := 0;
    try
      while True do
      begin
        mem := 0;
        rc := zq_d_find_block(d, @mem);
        if rc < 0 then raise Exception.Create(string(zq_last_error));
        if rc = 0 then Break;
        found_data := True;

        while True do
        begin
          fnameSink.s := '';
          nw := NameSinkWriter(fnameSink);
          rc := zq_d_find_filename(d, @nw);
          if rc < 0 then raise Exception.Create(string(zq_last_error));
          if rc = 0 then Break;
          if Length(fnameSink.s) > 0 then
          begin
            for i := 1 to Length(fnameSink.s) do
              if fnameSink.s[i] = '\' then fnameSink.s[i] := '/';
            lastfile := string(fnameSink.s);
          end;
          commentSink.s := '';
          nw := NameSinkWriter(commentSink);
          if zq_d_read_comment(d, @nw) < 0 then
            raise Exception.Create(string(zq_last_error));

          jidacBlock := (Length(commentSink.s) >= 4) and
                     (Copy(string(commentSink.s), Length(commentSink.s) - 3, 4) =
                      'jDC' + #1);
          if jidacBlock then
          begin
            isJidac := True;
            if (Length(fnameSink.s) <> 28) or
               (Copy(string(fnameSink.s), 1, 3) <> 'jDC') then
              raise Exception.Create('bad journaling block name');
            if skip then
              raise Exception.Create('mixed journaling and streaming block');

            usizeBuf := 0;
            i := 1;
            while (i <= Length(commentSink.s)) and
                  IsDigitChar(commentSink.s[i]) do
            begin
              usizeBuf := usizeBuf * 10 + Ord(commentSink.s[i]) - Ord('0');
              if usizeBuf > $ffffffff then
                raise Exception.Create('journaling block too big');
              Inc(i);
            end;

            fdate := 0;
            num := 0;
            i := 4;
            while (i <= 17) and IsDigitChar(fnameSink.s[i]) do
            begin
              fdate := fdate * 10 + Ord(fnameSink.s[i]) - Ord('0');
              Inc(i);
            end;
            if (i <> 18) or (fdate < 19000000000000) or
               (fdate >= 30000000000000) then
              raise Exception.Create('bad date');
            i := 19;
            while (i <= 28) and IsDigitChar(fnameSink.s[i]) do
            begin
              num := num * 10 + Ord(fnameSink.s[i]) - Ord('0');
              Inc(i);
            end;
            if (i <> 29) or (num > $ffffffff) then
              raise Exception.Create('bad fragment');

            ch := fnameSink.s[18];
            if (ch = 'c') or (ch = 'h') or (ch = 'i') then
            begin
              if mem > 1.5e9 then
                raise Exception.Create('index block requires too much memory');
              zq_buf_resize(osb, 0);
              zq_buf_set_limit(osb, SizeUInt(usizeBuf));
              nw := OutSinkWriter(osink);
              zq_d_set_output(d, nw);
              zq_sha1_init(sha);
              zq_d_set_sha1(d, sha);
              rc := zq_d_decompress(d, -1);
              if rc < 0 then raise Exception.Create(string(zq_last_error));
              sha1result[0] := #0;
              if zq_d_read_segment_end(d, @sha1result[0]) < 0 then
                raise Exception.Create(string(zq_last_error));
              if Int64(zq_buf_size(osb)) <> usizeBuf then
                raise Exception.Create('bad block size');
              if usizeBuf <> Int64(zq_sha1_usize(sha)) then
                raise Exception.Create('bad checksum size');
              if (sha1result[0] <> #0) and
                 (not CompareMem(@sha1result[1], zq_sha1_result(sha), 20)) then
                raise Exception.Create('bad checksum');
            end
            else
            begin
              if zq_d_read_segment_end(d, nil) < 0 then
                raise Exception.Create(string(zq_last_error));
            end;

            if ch = 'c' then
            begin
              if zq_buf_size(osb) < 8 then
                raise Exception.Create('c block too small');
              data_offset := inArc.Tell() + 1 - zq_d_buffered(d);
              s := zq_buf_data(osb);
              jmp := BToL(s);
              if jmp < 0 then
                WriteLn('Incomplete transaction ignored');
              if (jmp < 0) or
                 ((version < 19000000000000) and (Int64(Length(ver)) > version)) or
                 ((version >= 19000000000000) and (version < fdate)) then
              begin
                done := True;
                Break;
              end
              else
              begin
                Inc(dcsize, jmp);
                if jmp <> 0 then inArc.Seek(data_offset + jmp, 0);
                SetLength(ver, Length(ver) + 1);
                vr := Default(TVer);
                vr.firstFragment := Cardinal(Length(ht));
                vr.offset := block_offset;
                vr.data_offset := data_offset;
                vr.date := fdate;
                vr.lastdate := fdate;
                vr.csize := jmp;
                ver[High(ver)] := vr;
                if all > 0 then
                begin
                  allname := Itos(Int64(Length(ver)) - 1, all) + '/';
                  if dt.IndexOf(allname) < 0 then
                  begin
                    dtr := GetDT(allname);
                    dtr.date := fdate;
                  end;
                end;
                if jmp <> 0 then
                begin
                  restart := True;
                  Break;
                end;
              end;
            end
            else if ch = 'h' then
            begin
              if (zq_buf_size(osb) mod 24) <> 4 then
                raise Exception.Create('bad h block size');
              nFrags := Cardinal((zq_buf_size(osb) - 4) div 24);
              if (num < 1) or (num + nFrags > $ffffffff) then
                raise Exception.Create('bad h fragment');
              s := zq_buf_data(osb);
              bsize := BToI(s);
              Inc(dhsize, bsize);
              blk := Default(TBlockInfo);
              blk.start := Cardinal(num);
              blk.offset := data_offset;
              blk.usize := 8;
              blk.bsize := bsize;
              blk.frags := Cardinal(zq_buf_size(osb) div 24);
              if nFrags > 0 then
              begin
                SetLength(blocks, Length(blocks) + 1);
                blocks[High(blocks)] := blk;
              end;
              for j := 0 to nFrags - 1 do
              begin
                while Int64(Length(ht)) <= Int64(num + j) do
                begin
                  SetLength(ht, Length(ht) + 1);
                  ht[High(ht)].usize := 0;
                  ht[High(ht)].csize := 0;
                end;
                Move(s^, ht[num + j].sha1[0], 20);
                Inc(s, 20);
                f := BToI(s);
                if f > $7fffffff then
                  raise Exception.Create('fragment too big');
                ht[num + j].usize := f;
                if Length(blocks) > 0 then
                  Inc(blocks[High(blocks)].usize, Int64(f) + 4);
              end;
              Inc(data_offset, bsize);
            end
            else if ch = 'i' then
            begin
              if fdate > ver[High(ver)].lastdate then
                ver[High(ver)].lastdate := fdate;
              s := zq_buf_data(osb);
              send := s + zq_buf_size(osb);
              while (s + 9) <= send do
              begin
                dtr := TDT.Create;
                dtr.date := BToL(s);
                if dtr.date <> 0 then
                  Inc(ver[High(ver)].updates)
                else
                  Inc(ver[High(ver)].deletes);
                len := StrLen(s);
                fn := string(AnsiString(s));
                Inc(s, len + 1);
                if s > send then
                begin
                  dtr.Free;
                  raise Exception.Create('filename too long');
                end;
                if dtr.date <> 0 then
                begin
                  Inc(filesSeen);
                  na := BToI(s);
                  j := 0;
                  while j < na do
                  begin
                    if j < 8 then
                      Inc(dtr.attr, Int64(Ord(s^)) shl (j * 8));
                    Inc(s);
                    Inc(j);
                  end;
                  ni := BToI(s);
                  SetLength(dtr.ptr, ni);
                  j := 0;
                  while j < ni do
                  begin
                    dtr.ptr[j] := BToI(s);
                    Inc(j);
                  end;
                end;
                if all > 0 then
                  fn := Itos(Int64(Length(ver)) - 1, all) + '/' + fn;
                if IsSelected(fn, False) then
                begin
                  existing := GetDT(fn);
                  existing.date := dtr.date;
                  existing.size := 0;
                  if noattributes then existing.attr := 0
                  else existing.attr := dtr.attr;
                  existing.ptr := dtr.ptr;
                  existing.data := 0;
                end;
                dtr.Free;
              end;
            end
            else
            begin
              WriteLn('Skipping ', fnameSink.s, ' ', commentSink.s);
              raise Exception.Create('Unexpected journaling block');
            end;
          end
          else
          begin
            if Length(ver) = 1 then
            begin
              if version < 1 then
              begin
                done := True;
                Break;
              end;
              SetLength(ver, Length(ver) + 1);
              vr := Default(TVer);
              vr.firstFragment := Cardinal(Length(ht));
              vr.offset := block_offset;
              vr.csize := -1;
              ver[High(ver)] := vr;
            end;
            sha1result[0] := #0;
            if zq_d_read_segment_end(d, @sha1result[0]) < 0 then
              raise Exception.Create(string(zq_last_error));
            skip := True;
            fn := lastfile;
            if all > 0 then
              fn := Itos(Int64(Length(ver)) - 1, all) + '/' + fn;
            { Upstream only counts and records streaming segments that the
              selection matches (dt is the working set of this command). }
            if IsSelected(fn, False) then
            begin
            dtr := GetDT(fn);
            if (Length(fnameSink.s) > 0) or first then
            begin
              Inc(filesSeen);
              dtr.date := cmdDate;
              dtr.attr := 0;
              SetLength(dtr.ptr, 0);
              Inc(ver[High(ver)].updates);
            end;
            SetLength(dtr.ptr, Length(dtr.ptr) + 1);
            dtr.ptr[High(dtr.ptr)] := Cardinal(Length(ht));
            end;
            if (segs = 0) or (Length(blocks) = 0) then
            begin
              blk := Default(TBlockInfo);
              blk.start := Cardinal(Length(ht));
              blk.offset := block_offset;
              blk.usize := -1;   { streaming: uncompressed size unknown }
              SetLength(blocks, Length(blocks) + 1);
              blocks[High(blocks)] := blk;
            end;
            SetLength(ht, Length(ht) + 1);
            Move(sha1result[1], ht[High(ht)].sha1[0], 20);
            ht[High(ht)].usize := -1;
            ht[High(ht)].csize := 0;
          end;
          Inc(segs);
          first := False;
        end;
        if done or restart then Break;
        block_offset := inArc.Tell() - zq_d_buffered(d);
      end;
      if restart then Continue;
      if not done then done := True;
    except
      on e: Exception do
      begin
        inArc.Seek(-zq_d_buffered(d), 1);
        WriteLn(StdErr, 'Skipping block at ', Format('%1.0f', [block_offset + 0.0]),
                ': ', e.Message);
        Inc(errors);
      end;
    end;
  end;

  if (inArc.Tell() > 32 * Ord(password <> '')) and (not found_data) then
    raise Exception.Create('archive contains no data');
  lastOffset := block_offset;
  nfilesSeen := filesSeen;
  WriteLn(Format('%d versions, %d files, %d fragments, %1.6f MB',
    [Length(ver) - 1, filesSeen, Length(ht) - 1,
     block_offset / 1000000.0]));

  for i := 0 to dt.Count - 1 do
  begin
    dtr := TDT(dt.Objects[i]);
    dtr.size := 0;
    j := 0;
    while j < Cardinal(Length(dtr.ptr)) do
    begin
      if (dtr.ptr[j] > 0) and (dtr.ptr[j] < Cardinal(Length(ht))) then
      begin
        if ht[dtr.ptr[j]].usize >= 0 then
          Inc(dtr.size, ht[dtr.ptr[j]].usize)
        else
          dtr.size := -1;
      end;
      Inc(j);
    end;
  end;

  zq_d_free(d);
  zq_buf_free(osb);
  zq_sha1_free(sha);
  osink.Free;
  fnameSink.Free;
  commentSink.Free;
  inArc.Free;
  nfiles := filesSeen;
  Result := block_offset;
end;

{ ------------------------------------------------------------------ }
{ list                                                                }
{ ------------------------------------------------------------------ }


{ ------------------------------------------------------------------ }
{ extract                                                             }
{ ------------------------------------------------------------------ }

function TJidac.LoadBlock(const arcName, password: string; nb: Integer;
                          osb: TZqBuf; out bad: Boolean): Boolean;
var
  inArc: TInputArchive;
  d: TZqDecompresser;
  rd: TZqReader;
  nw: TZqWriter;
  osink: TOutSink;
  fnameSink, commentSink: TNameSink;
  sha: TZqSha1;
  sha1result: array[0..20] of AnsiChar;
  rc: Integer;
begin
  Result := False;
  bad := False;
  inArc := TInputArchive.Create(arcName, password);
  d := zq_d_new;
  sha := zq_sha1_new;
  fnameSink := TNameSink.Create;
  commentSink := TNameSink.Create;
  osink := TOutSink.Create;
  osink.buf := osb;
  try
    inArc.Seek(blocks[nb].offset, 0);
    rd := InputReader(inArc);
    zq_d_set_input(d, rd);
    fBlockMem := 0;
    rc := zq_d_find_block(d, @fBlockMem);
    if rc <= 0 then Exit;
    nw := NameSinkWriter(fnameSink);
    rc := zq_d_find_filename(d, @nw);
    if rc <= 0 then Exit;
    nw := NameSinkWriter(commentSink);
    if zq_d_read_comment(d, @nw) < 0 then Exit;
    zq_buf_resize(osb, 0);
    zq_buf_set_limit(osb, SizeUInt(blocks[nb].usize));
    nw := OutSinkWriter(osink);
    zq_d_set_output(d, nw);
    zq_sha1_init(sha);
    zq_d_set_sha1(d, sha);
    if zq_d_decompress(d, -1) < 0 then Exit;
    sha1result[0] := #0;
    if zq_d_read_segment_end(d, @sha1result[0]) < 0 then Exit;
    if (sha1result[0] <> #0) and
       (not CompareMem(@sha1result[1], zq_sha1_result(sha), 20)) then
      bad := True;
    Result := True;
  finally
    zq_d_free(d);
    zq_sha1_free(sha);
    fnameSink.Free;
    commentSink.Free;
    osink.Free;
    inArc.Free;
  end;
end;

{ ------------------------------------------------------------------ }
{ repack (extract -repack F [X])                                      }
{ ------------------------------------------------------------------ }

procedure PutI(b: TZqBuf; x: Int64; n: Integer); forward;

{ -repack with -all: re-encrypt the whole archive into a new file.  The
  block contents are not touched, only the key (upstream's copy(in, out)). }
function TJidac.RepackWhole(force: Boolean): Integer;
var
  inArc: TInputArchive;
  outArc: TOutputArchive;
  salt: array[0..31] of AnsiChar;
  buf: array[0..4095] of AnsiChar;
  nr: LongInt;
  total: Int64;
begin
  if (files.Count > 0) or (tofiles.Count > 0) or (onlyfiles.Count > 0) or
     noattributes or (version <> DEFAULT_VERSION) or methodSet then
    raise Exception.Create('-repack -all does not allow partial copy');
  if force then DeleteFile(repack);
  if FileExists(repack) then
    raise Exception.Create('output file exists');

  inArc := TInputArchive.Create(archive, password);
  try
    FillChar(salt, SizeOf(salt), 0);
    if newpassword <> '' then zq_random(@salt[0], 32);
    if newpassword <> '' then
      outArc := TOutputArchive.Create(repack, newpassword, @salt[0], 0)
    else
      outArc := TOutputArchive.Create(repack, '', nil, 0);
    try
      total := 0;
      while True do
      begin
        nr := inArc.Read(@buf[0], SizeOf(buf));
        if nr < 1 then Break;
        outArc.WriteBuf(@buf[0], nr);
        Inc(total, nr);
      end;
      Write(archive, ' ', Format('%1.0f', [total + 0.0]), ' ', repack,
            ' -> ', Format('%1.0f', [outArc.Tell() + 0.0]));
      WriteLn;
    finally
      outArc.Free;
    end;
  finally
    inArc.Free;
  end;
  Result := 0;
end;

{ -repack F [X]: build a new archive with only the D blocks that the files
  need, regenerate the H blocks and write the I blocks again.  The blocks
  themselves are copied verbatim, so the result is deterministic. }
function TJidac.RepackSelected(force: Boolean): Integer;
var
  i, j, nb, dtcount: Integer;
  dtr: TDT;
  inArc: TInputArchive;
  outArc: TOutputArchive;
  w: TZqWriter;
  isb: TZqBuf;
  salt: array[0..31] of AnsiChar;
  cstart, dstart, csize: Int64;
  need, keep: array of Boolean;
  fn, cfn, comment: string;
  frag: Cardinal;
  k: Cardinal;
  badfrag: Boolean;
begin
  if Length(ver) < 2 then
    raise Exception.Create('cannot repack streaming archive');

  SetLength(need, Length(blocks));
  SetLength(keep, dt.Count);
  dtcount := 0;

  { Label the files and the blocks they need.  Upstream does not apply the
    file selection to a repack: every entry with a date is kept. }
  for i := 0 to dt.Count - 1 do
  begin
    dtr := TDT(dt.Objects[i]);
    keep[i] := False;
    if (dtr.date = 0) or (dt[i] = '') then Continue;
    if dt[i][Length(dt[i])] = '/' then
    begin
      keep[i] := True;
      Continue;
    end;
    if Length(blocks) = 0 then Continue;
    badfrag := False;
    for j := 0 to High(dtr.ptr) do
    begin
      frag := dtr.ptr[j];
      if (frag = 0) or (frag >= Cardinal(Length(ht))) or (ht[frag].usize < -1) then
      begin
        WriteLn(StdErr, dt[i], ': bad frag IDs, skipping...');
        badfrag := True;
        Break;
      end;
      nb := FindBlockFor(frag);
      if nb >= 0 then need[nb] := True;
    end;
    if not badfrag then keep[i] := True;
  end;

  { Total compressed size of the D blocks we keep. }
  csize := 0;
  for i := 0 to High(blocks) do
  begin
    if blocks[i].bsize < 1 then
      raise Exception.Create('empty block');
    if need[i] then Inc(csize, blocks[i].bsize);
  end;

  inArc := nil;
  outArc := nil;
  isb := zq_buf_new(64);
  try
    inArc := TInputArchive.Create(archive, password);
    if (not force) and FileExists(repack) then
      raise Exception.Create('repack output exists');
    DeleteFile(repack);
    FillChar(salt, SizeOf(salt), 0);
    if newpassword <> '' then zq_random(@salt[0], 32);
    if newpassword <> '' then
      outArc := TOutputArchive.Create(repack, newpassword, @salt[0], 0)
    else
      outArc := TOutputArchive.Create(repack, '', nil, 0);
    w := OutputWriter(outArc);
    comment := 'jDC' + #1;
    cstart := outArc.Tell();

    { C block: data size unknown for now (-1), one version }
    zq_buf_resize(isb, 0);
    PutI(isb, -1, 8);
    cfn := 'jDC' + Itos(ver[1].date, 14) + 'c' + Itos(1, 10);
    if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(cfn)),
                         PAnsiChar(comment), 1) < 0 then
      raise Exception.Create(string(zq_last_error));
    dstart := outArc.Tell();

    { D blocks, verbatim }
    for i := 0 to High(blocks) do
      if need[i] then
      begin
        inArc.Seek(blocks[i].offset, 0);
        if not CopyBytes(inArc, outArc, blocks[i].bsize) then
          raise Exception.Create('EOF');
      end;
    Write('Data ', Format('%1.0f', [csize + 0.0]), ' -> ');
    csize := outArc.Tell() - dstart;
    WriteLn(Format('%1.0f', [csize + 0.0]));

    { H blocks, dated like the last version }
    for i := 0 to High(blocks) do
      if need[i] then
      begin
        zq_buf_resize(isb, 0);
        PutI(isb, blocks[i].bsize, 4);
        for j := 0 to Integer(blocks[i].frags) - 1 do
        begin
          k := blocks[i].start + Cardinal(j);
          zq_buf_write(isb, @ht[k].sha1[0], 20);
          PutI(isb, ht[k].usize, 4);
        end;
        fn := 'jDC' + Itos(ver[High(ver)].date, 14) + 'h' +
              Itos(blocks[i].start, 10);
        if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(fn)),
                             PAnsiChar(comment), 1) < 0 then
          raise Exception.Create(string(zq_last_error));
      end;

    { I blocks with the selected file list }
    zq_buf_resize(isb, 0);
    i := 0;
    while True do
    begin
      if (i < dt.Count) and keep[i] then
      begin
        dtr := TDT(dt.Objects[i]);
        fn := Rename(dt[i]);
        PutI(isb, dtr.date, 8);
        zq_buf_write(isb, PAnsiChar(AnsiString(fn)), Length(fn));
        zq_buf_write(isb, #0, 1);
        if (dtr.attr and 255) = Ord('u') then
        begin
          PutI(isb, 3, 4);
          PutI(isb, dtr.attr, 3);
        end
        else if (dtr.attr and 255) = Ord('w') then
        begin
          PutI(isb, 5, 4);
          PutI(isb, dtr.attr, 5);
        end
        else
          PutI(isb, 0, 4);
        PutI(isb, Length(dtr.ptr), 4);
        for j := 0 to High(dtr.ptr) do PutI(isb, dtr.ptr[j], 4);
      end;
      if (zq_buf_size(isb) > 16000) or
         ((zq_buf_size(isb) > 0) and (i >= dt.Count)) then
      begin
        Inc(dtcount);
        fn := 'jDC' + Itos(ver[High(ver)].date) + 'i' + Itos(dtcount, 10);
        if zq_compress_block(isb, w, '1', PAnsiChar(AnsiString(fn)),
                             PAnsiChar(comment), 1) < 0 then
          raise Exception.Create(string(zq_last_error));
        zq_buf_resize(isb, 0);
      end;
      if i >= dt.Count then Break;
      Inc(i);
    end;

    Write(archive, ' ', Format('%1.0f', [lastOffset + 0.0]), ' -> ',
          repack, ' ', Format('%1.0f', [outArc.Tell() + 0.0]));
    WriteLn;

    { Rewrite the C block now that the data size is known }
    outArc.Seek(cstart, 0);
    zq_buf_resize(isb, 0);
    PutI(isb, csize, 8);
    if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(cfn)),
                         PAnsiChar(comment), 1) < 0 then
      raise Exception.Create(string(zq_last_error));
    outArc.CloseArchive;
  finally
    outArc.Free;
    inArc.Free;
    zq_buf_free(isb);
  end;
  Result := 0;
end;

{ Open an input file like upstream's fopen(RB): on failure print
  "<path>: <error>" (printerr) and return nil. }
function OpenIn(const path: string): TStream;
var
  h: THandle;
begin
  Result := nil;
  h := fpOpen(PChar(path), O_RDONLY, 0);
  if h < 0 then
  begin
    WriteLn(StdErr, path, ': ', SysErrorMessage(fpGetErrno));
    Exit;
  end;
  Result := THandleStream.Create(h);
end;

{ Open an output file like upstream's fopen: on failure print "<path>: <error>"
  (printerr) and return nil.  create=True truncates, otherwise it must exist. }
function OpenOut(const path: string; create: Boolean): TStream;
var
  h: THandle;
begin
  Result := nil;
  if create then
    h := fpOpen(PChar(path), O_RDWR or O_CREAT or O_TRUNC, &666)
  else
    h := fpOpen(PChar(path), O_RDWR, 0);
  if h < 0 then
  begin
    WriteLn(StdErr, path, ': ', SysErrorMessage(fpGetErrno));
    Exit;
  end;
  Result := THandleStream.Create(h);
end;

{ Label the entries to extract (upstream's loop): decides dt[].data (-1 skip,
  0 extract) and prints the "="/"?" lines and the skipped count. }
procedure TJidac.PrepareExtract(force, testOnly: Boolean;
                                out totalFiles: Integer; out totalSize: Int64);
var
  i, j, k, nb: Integer;
  dtr: TDT;
  fn: string;
  isdir: Boolean;
  skipped: Integer;
begin
  totalFiles := 0;
  totalSize := 0;
  skipped := 0;
  { ---- Label the files and find the blocks they need (upstream) ---- }
  SetLength(fBlockFiles, Length(blocks));
  SetLength(fNeedCount, Length(blocks));
    for i := 0 to dt.Count - 1 do
    begin
      dtr := TDT(dt.Objects[i]);
      dtr.data := -1;
      if (dtr.date = 0) or (dt[i] = '') then Continue;
      fn := Rename(dt[i]);
      isdir := dt[i][Length(dt[i])] = '/';
      if (not testOnly) and force and (not isdir) and Equal(dtr, fn) then
      begin
        if summary <= 0 then WriteLn('= ', fn);
        CloseFileAttrs(fn, dtr.date, dtr.attr);
        Inc(skipped);
      end
      else if (not testOnly) and (not force) and FileExistsUTF8(fn) then
      begin
        if summary <= 0 then WriteLn('? ', fn);
        Inc(skipped);
      end
      else if isdir then
        dtr.data := 0
      else if Length(blocks) > 0 then
      begin
        dtr.data := 0;
        Inc(totalFiles);
        Inc(totalSize, dtr.size);
        for j := 0 to High(dtr.ptr) do
        begin
          k := dtr.ptr[j];
          if (k = 0) or (k >= Cardinal(Length(ht))) or (ht[k].usize < -1) then
          begin
            WriteLn(StdErr, dt[i], ': bad frag IDs, skipping...');
            dtr.data := -1;
            Break;
          end;
          nb := FindBlockFor(k);
          if nb < 0 then Continue;
          if (Length(fBlockFiles[nb]) = 0) or
             (fBlockFiles[nb][High(fBlockFiles[nb])] <> i) then
          begin
            SetLength(fBlockFiles[nb], Length(fBlockFiles[nb]) + 1);
            fBlockFiles[nb][High(fBlockFiles[nb])] := i;
          end;
          if Cardinal(k - blocks[nb].start + 1) > fNeedCount[nb] then
            fNeedCount[nb] := k - blocks[nb].start + 1;
        end;
      end;
    end;
  if (not force) and (skipped > 0) then
    WriteLn(skipped, ' ?existing files skipped (-force overwrites).');
  if force and (skipped > 0) then
    WriteLn(skipped, ' =identical files skipped.');
end;


function TJidac.Extract(force: Boolean; testOnly: Boolean): Integer;
var
  i, j, nb, b, k, idx: Integer;
  dtr: TDT;
  fn: string;
  isdir: Boolean;
  skipped, totalFiles, extracted, errors, nt: Integer;
  totalSize, totalDone, maxMem: Int64;
  blockFiles: array of array of Integer;
  needCount: array of Cardinal;
  boff: array of Int64;
  bdata: PAnsiChar;
  curFile: TStream;
  curIdx: Integer;
  mm: Double;
  offs, q, usize2: Int64;
  bad: Boolean;
  osb: TZqBuf;
  pool: TDecPool;
  needed: array of Integer;
  needFlags: array of Boolean;
begin
  Result := 0;
  errors := 0;
  skipped := 0;
  totalFiles := 0;
  totalSize := 0;
  totalDone := 0;
  curFile := nil;
  curIdx := -1;
  osb := zq_buf_new(1 shl 16);
  pool := nil;
  try
    PrepareExtract(force, testOnly, totalFiles, totalSize);

    nt := threads;
    if nt < 1 then nt := CPUCount;
    WriteLn('Extracting ', Format('%1.6f', [totalSize / 1000000.0]),
            ' MB in ', totalFiles, ' files -threads ', nt);

    { ---- Prefetch the blocks that the selected files need ---- }
    SetLength(needFlags, Length(blocks));
    SetLength(needed, 0);
    for b := 0 to High(blocks) do
      if Length(fBlockFiles[b]) > 0 then
      begin
        needFlags[b] := True;
        SetLength(needed, Length(needed) + 1);
        needed[High(needed)] := b;
      end;
    if (nt > 1) and (Length(needed) > 1) then
    begin
      pool := TDecPool.Create(Self, needed, nt);
      pool.Start;
    end;

    { ---- Decompress block by block and write the files ---- }
    for b := 0 to High(blocks) do
    begin
      if Length(fBlockFiles[b]) = 0 then Continue;
      if fNeedCount[b] < 1 then Continue;
      if pool <> nil then
      begin
        bdata := pool.Get(b, bad, usize2);
        if bdata <> nil then q := usize2;
      end
      else
      begin
        if not LoadBlock(archive, password, b, osb, bad) then
        begin
          WriteLn(StdErr, 'cannot decompress block ', b);
          Inc(errors);
          Continue;
        end;
        bdata := zq_buf_data(osb);
        q := Int64(zq_buf_size(osb));
      end;
      if bdata = nil then
      begin
        WriteLn(StdErr, 'cannot decompress block ', b);
        Inc(errors);
        Continue;
      end;
      if fBlockMem > mm then mm := fBlockMem;
      if bad then
      begin
        WriteLn(StdErr, 'bad checksum in block ', b);
        Inc(errors);
      end;

      { Offset of each fragment inside the decompressed block }
      SetLength(boff, fNeedCount[b]);
      offs := 0;
      for k := 0 to Int64(fNeedCount[b]) - 1 do
      begin
        boff[k] := offs;
        if ht[blocks[b].start + Cardinal(k)].usize > 0 then
          Inc(offs, ht[blocks[b].start + Cardinal(k)].usize);
      end;
      { Upstream prints the bytes the decompresser produced: it pulls 16 KiB
        chunks until it has the bytes the selected files need, so a partially
        used block stops at the next 16 KiB boundary (or at the end). }
      if offs < q then
      begin
        j := Int64(((offs + 16383) div 16384) * 16384);
        if j > q then j := q;
        q := j;
      end;
      PrintProgress(totalSize, totalDone, summary);
      if summary <= 0 then
        WriteLn('[', blocks[b].start, '..',
                Int64(blocks[b].start) + Int64(fNeedCount[b]) - 1,
                '] -> ', q);

      for idx := 0 to High(fBlockFiles[b]) do
      begin
        i := fBlockFiles[b][idx];
        dtr := TDT(dt.Objects[i]);
        if (dtr.date = 0) or (dtr.data < 0) or
           (dtr.data >= Length(dtr.ptr)) then Continue;
        offs := 0;
        for j := 0 to High(dtr.ptr) do
        begin
          k := dtr.ptr[j];
          if Cardinal(k) < blocks[b].start then
          begin
            if ht[k].usize > 0 then Inc(offs, ht[k].usize);
            Continue;
          end;
          if Cardinal(k) >= blocks[b].start + fNeedCount[b] then Break;

          { Open (or reopen) the output file }
          if (curIdx <> i) or (curFile = nil) then
          begin
            if curFile <> nil then
            begin
              curFile.Free;
              curFile := nil;
            end;
            if dtr.data = 0 then
            begin
              if summary <= 0 then
              begin
                PrintProgress(totalSize, totalDone, summary);
                WriteLn('> ', Rename(dt[i]));
              end;
              if not testOnly then
              begin
                MakePath(Rename(dt[i]));
                curFile := OpenOut(Rename(dt[i]), True);
                if curFile = nil then
                begin
                  bad := True;   { reported: skip this file, keep going }
                  curIdx := i;
                  Break;
                end;
              end;
            end
            else if not testOnly then
            begin
              curFile := OpenOut(Rename(dt[i]), False);
              if curFile = nil then
              begin
                bad := True;
                curIdx := i;
                Break;
              end;
            end;
            curIdx := i;
          end;

          usize2 := ht[k].usize;
          if (not testOnly) and (curFile <> nil) and (usize2 > 0) then
          begin
            curFile.Seek(offs, soBeginning);
            curFile.Write((bdata + boff[k - blocks[b].start])^, usize2);
          end;
          Inc(dtr.data);
          if usize2 > 0 then
          begin
            Inc(offs, usize2);
            Inc(totalDone, usize2);
          end;
        end;

        { Last fragment: close and set the date and attributes }
        if (dtr.data >= Length(dtr.ptr)) and (curIdx = i) then
        begin
          if curFile <> nil then
          begin
            curFile.Free;
            curFile := nil;
          end;
          curIdx := -1;
          if not testOnly then
            CloseFileAttrs(Rename(dt[i]), dtr.date, dtr.attr);
        end;
      end;
    end;

    { ---- Directories and read-only attributes ---- }
    if not testOnly then
    begin
      for i := 0 to dt.Count - 1 do
      begin
        dtr := TDT(dt.Objects[i]);
        if (dtr.data >= 0) and (dtr.date <> 0) and (dt[i] <> '') then
        begin
          fn := Rename(dt[i]);
          if dt[i][Length(dt[i])] = '/' then
            MakePath(fn, dtr.date, dtr.attr)
          else if (dtr.attr and $1ff) = Ord('w') + 256 then
            CloseFileAttrs(fn, 0, dtr.attr);
        end;
      end;
    end;

    { ---- Report failed extractions (upstream) ---- }
    extracted := 0;
    errors := 0;
    for i := 0 to dt.Count - 1 do
    begin
      dtr := TDT(dt.Objects[i]);
      fn := Rename(dt[i]);
      if (dtr.data >= 0) and (dtr.date <> 0) and (fn <> '') and
         (fn[Length(fn)] <> '/') then
      begin
        Inc(extracted);
        if Cardinal(dtr.data) <> Cardinal(Length(dtr.ptr)) then
        begin
          if errors = 0 then
          begin
            WriteLn(StdErr);
            WriteLn(StdErr, 'Failed (extracted/total fragments, file):');
          end;
          Inc(errors);
          WriteLn(StdErr, dtr.data, '/', Length(dtr.ptr), ' ', fn);
        end;
      end;
    end;
    if errors > 0 then
    begin
      WriteLn(StdErr);
      WriteLn(StdErr, 'Extracted ', extracted - errors, ' of ', extracted,
              ' files OK (', errors, ' errors) using ',
              Format('%1.3f', [mm / 1000000.0]), ' MB x ', nt, ' threads');
    end;
  finally
    if curFile <> nil then curFile.Free;
    if pool <> nil then pool.Free;
    zq_buf_free(osb);
  end;
  Result := Ord(errors > 0);
end;

{ ------------------------------------------------------------------ }
{ add (journaling)                                                    }
{ ------------------------------------------------------------------ }

type
  TEdtEntry = class
    date, size, attr, sortkey: Int64;
    data: Integer;          { 0 = not compressed in this update, 1 = added }
    ptr: array of Cardinal;
  end;

  THTIndex = class
  public
    t: array of Cardinal;
    htsize: Cardinal;
    constructor Create(n: SizeInt);
    function HashOf(p: PAnsiChar): Cardinal;
    function Find(const htArr: array of THT; p: PAnsiChar): Cardinal;
    procedure Update(const htArr: array of THT);
  end;

constructor THTIndex.Create(n: SizeInt);
var
  b: Integer;
begin
  htsize := 1;
  b := 1;
  while (n * 3) shr b <> 0 do Inc(b);
  if b - 1 < 1 then b := 2;
  SetLength(t, SizeInt(1) shl (b - 1));
  FillChar(t[0], Length(t) * SizeOf(Cardinal), 0);
end;

function THTIndex.HashOf(p: PAnsiChar): Cardinal;
var
  v: Cardinal;
begin
  Move(p^, v, 4);
  Result := v and Cardinal(Length(t) - 1);
end;

function THTIndex.Find(const htArr: array of THT; p: PAnsiChar): Cardinal;
var
  h: Cardinal;
  i: SizeInt;
  idx: Cardinal;
begin
  h := HashOf(p);
  for i := 0 to High(t) do
  begin
    idx := t[(h + Cardinal(i)) and Cardinal(Length(t) - 1)];
    if idx = 0 then Exit(0);
    if CompareMem(p, @htArr[idx].sha1[0], 20) then Exit(idx);
  end;
  Result := 0;
end;

procedure THTIndex.Update(const htArr: array of THT);
var
  zero: array[0..19] of AnsiChar;
  h, ph: Cardinal;
  i: SizeInt;
begin
  FillChar(zero, 20, 0);
  while htsize < Cardinal(Length(htArr)) do
  begin
    if htsize >= Cardinal(Length(t)) div 4 * 3 then
    begin
      SetLength(t, Length(t) * 2);
      htsize := 1;
    end;
    if (htArr[htsize].usize >= 0) and
       (not CompareMem(@htArr[htsize].sha1[0], @zero[0], 20)) then
    begin
      h := HashOf(@htArr[htsize].sha1[0]);
      for i := 0 to High(t) do
      begin
        ph := (h + Cardinal(i)) and Cardinal(Length(t) - 1);
        if t[ph] = 0 then
        begin
          t[ph] := htsize;
          Break;
        end;
      end;
    end;
    Inc(htsize);
  end;
end;

procedure PutI(b: TZqBuf; x: Int64; n: Integer);
var
  i: Integer;
  c: AnsiChar;
begin
  for i := 0 to n - 1 do
  begin
    c := AnsiChar(x and 255);
    zq_buf_write(b, @c, 1);
    x := x shr 8;
  end;
end;

procedure TJidac.ScanInto(edt: TStringList; const name: string);
var
  date, size, attr: Int64;
  isDir: Boolean;
  idx: Integer;
  e: TEdtEntry;
  sr: TSearchRec;
  fn, sub, nm: string;
begin
  FileInfo(name, date, size, attr, isDir);
  { upstream's scandir: only regular files (S_IFREG) and directories (S_IFDIR)
    are recorded; a symbolic link or a device is skipped without a message }
  if (date <> 0) and (((attr shr 8) and $F000) <> $8000) and
     (((attr shr 8) and $F000) <> $4000) then
    Exit;
  if date = 0 then
  begin
    { upstream's scandir: perror() for a file that cannot be stat'ed }
    WriteLn(StdErr, name, ': ', SysErrorMessage(fpGetErrno));
    Exit;
  end;
  nm := name;
  while (Length(nm) > 1) and (nm[Length(nm)] = '/') do
    SetLength(nm, Length(nm) - 1);
  if isDir then
  begin
    fn := nm + '/';
    { upstream's addfile(): an entry is only recorded when selected; the
      directory itself is always traversed. }
    if IsSelected(fn, False) then
    begin
      idx := edt.IndexOf(fn);
      if idx >= 0 then e := TEdtEntry(edt.Objects[idx])
      else
      begin
        e := TEdtEntry.Create;
        edt.AddObject(fn, e);
      end;
      e.date := date;
      e.size := 0;
      e.attr := attr;
      e.sortkey := 0;
      e.data := 0;
      SetLength(e.ptr, 0);
    end;
    if FindFirst(fn + '*', faAnyFile, sr) = 0 then
    begin
      repeat
        if (sr.Name <> '.') and (sr.Name <> '..') then
        begin
          sub := fn + sr.Name;
          ScanInto(edt, sub);
        end;
      until FindNext(sr) <> 0;
      FindClose(sr);
    end;
  end
  else if IsSelected(nm, False) then
  begin
    idx := edt.IndexOf(nm);
    if idx >= 0 then e := TEdtEntry(edt.Objects[idx])
    else
    begin
      e := TEdtEntry.Create;
      edt.AddObject(nm, e);
    end;
    e.date := date;
    e.size := size;
    e.attr := attr;
    e.sortkey := 0;
    e.data := 0;
    SetLength(e.ptr, 0);
  end;
end;

{ True if the archive entry has the same contents as external file fn. }
function TJidac.Equal(dtr: TDT; const fn: string): Boolean;
var
  fs: TFileStream;
  buf: array[0..65535] of AnsiChar;
  k: Integer;
  frag: Cardinal;
  n, chunk, total: Int64;
  sha: TZqSha1;
begin
  Result := False;
  { upstream: a file that cannot be opened is simply not equal }
  if not FileExistsUTF8(fn) then Exit;
  { fragment sizes and hashes must be known, and the size must match }
  total := 0;
  for k := 0 to High(dtr.ptr) do
  begin
    frag := dtr.ptr[k];
    if (frag = 0) or (frag >= Cardinal(Length(ht))) or (ht[frag].usize < 0) then
      Exit;
    Inc(total, ht[frag].usize);
  end;
  try
    fs := TFileStream.Create(fn, fmOpenRead or fmShareDenyNone);
  except
    Exit;
  end;
  sha := nil;
  try
    if fs.Size <> total then Exit;
    { compare the SHA-1 of every fragment with the one in the archive, which is
      what upstream does: the stored bytes may be preprocessed (LZ77, E8E9) }
    sha := zq_sha1_new;
    for k := 0 to High(dtr.ptr) do
    begin
      frag := dtr.ptr[k];
      zq_sha1_init(sha);
      n := ht[frag].usize;
      while n > 0 do
      begin
        if n > SizeOf(buf) then chunk := SizeOf(buf) else chunk := n;
        if fs.Read(buf, LongInt(chunk)) <> chunk then Exit;
        zq_sha1_write(sha, @buf[0], chunk);
        Dec(n, chunk);
      end;
      if not CompareMem(zq_sha1_result(sha), @ht[frag].sha1[0], 20) then Exit;
    end;
    Result := True;
  finally
    if sha <> nil then zq_sha1_free(sha);
    fs.Free;
  end;
end;
{ List archive contents, comparing with the file system when files... are
  given (upstream list).  Status: '-' internal, '+' external, '=' same,
  '#' different, '^' duplicate. }
function TJidac.List(force: Boolean): Integer;
var
  i, j, idx, n: Integer;
  dtr, dir: TDT;
  edt: TStringList;
  e: TEdtEntry;
  names: array of string;
  ldate, lsize, lattr: array of Int64;
  lstat: array of AnsiChar;
  lref: array of Integer;
  lskip: array of Boolean;
  used: array of Boolean;
  usize, allsize, ddsize: Int64;
  nfiles, refs, nfrags, unknown, total: Cardinal;
  refsSeen: array of Boolean;
  matches, mismatches, internal, external, dups: Cardinal;
  same, hyphen: Boolean;
  st: AnsiChar;
  ii: Integer;
  order: array of Integer;

  procedure AddEntry(const nm: string; adate, asize, aattr: Int64;
                     ast: AnsiChar; aref: Integer);
  begin
    SetLength(names, Length(names) + 1);
    SetLength(ldate, Length(ldate) + 1);
    SetLength(lsize, Length(lsize) + 1);
    SetLength(lattr, Length(lattr) + 1);
    SetLength(lstat, Length(lstat) + 1);
    SetLength(lref, Length(lref) + 1);
    SetLength(lskip, Length(lskip) + 1);
    names[High(names)] := nm;
    ldate[High(ldate)] := adate;
    lsize[High(lsize)] := asize;
    lattr[High(lattr)] := aattr;
    lstat[High(lstat)] := ast;
    lref[High(lref)] := aref;
    lskip[High(lskip)] := False;
  end;

  { Fragment list of entry i (external files have none). }
  function RefPtr(i: Integer): TDT;
  begin
    if lref[i] >= 0 then Result := TDT(dt.Objects[lref[i]]) else Result := nil;
  end;

  { Compare two fragment lists like std::vector's operator<. }
  function PtrCmp(pa, pb: TDT): Integer;
  var
    x, y, k: Integer;
  begin
    x := 0;
    y := 0;
    if pa <> nil then x := Length(pa.ptr);
    if pb <> nil then y := Length(pb.ptr);
    k := 0;
    while (k < x) and (k < y) do
    begin
      if pa.ptr[k] <> pb.ptr[k] then
      begin
        if pa.ptr[k] < pb.ptr[k] then Exit(-1) else Exit(1);
      end;
      Inc(k);
    end;
    if x < y then Exit(-1);
    if x > y then Exit(1);
    Result := 0;
  end;

  { compareFragmentList(): size desc, fragment list, status, name. }
  function LessEntry(a, b: Integer): Boolean;
  var
    r: Integer;
  begin
    if lsize[a] <> lsize[b] then Exit(lsize[a] > lsize[b]);
    r := PtrCmp(RefPtr(a), RefPtr(b));
    if r <> 0 then Exit(r < 0);
    if lstat[a] <> lstat[b] then Exit(lstat[a] < lstat[b]);
    Result := names[a] < names[b];
  end;

  procedure DirSizes(list: TStringList);
  var
    a, b, k: Integer;
    dd: TDT;
  begin
    for a := 0 to list.Count - 1 do
    begin
      dd := TDT(list.Objects[a]);
      if (Length(list[a]) > 0) and (list[a][Length(list[a])] <> '/') then
        for k := 1 to Length(list[a]) do
          if list[a][k] = '/' then
          begin
            b := list.IndexOf(Copy(list[a], 1, k));
            if b >= 0 then Inc(TDT(list.Objects[b]).size, dd.size);
          end;
    end;
  end;

begin
  matches := 0;
  mismatches := 0;
  internal := 0;
  external := 0;
  dups := 0;

  { External files named on the command line. }
  edt := TStringList.Create;
  edt.Sorted := True;
  edt.CaseSensitive := True;
  edt.Duplicates := dupIgnore;
  try
    for i := 0 to files.Count - 1 do ScanInto(edt, files[i]);
    if files.Count > 0 then WriteLn(edt.Count, ' external files.');
    WriteLn;

    DirSizes(dt);
    DirSizes(edt);

    SetLength(used, dt.Count);
    for i := 0 to edt.Count - 1 do
    begin
      e := TEdtEntry(edt.Objects[i]);
      idx := dt.IndexOf(Rename(edt[i]));
      if (idx >= 0) and ((all > 0) or (TDT(dt.Objects[idx]).date <> 0)) then
      begin
        dtr := TDT(dt.Objects[idx]);
        used[idx] := True;
        AddEntry(dt[idx], dtr.date, dtr.size, dtr.attr, '-', idx);
      end;
      AddEntry(edt[i], e.date, e.size, e.attr, '+', -1);
    end;
    for idx := 0 to dt.Count - 1 do
    begin
      dtr := TDT(dt.Objects[idx]);
      if used[idx] or ((all = 0) and (dtr.date = 0)) then Continue;
      AddEntry(dt[idx], dtr.date, dtr.size, dtr.attr, '-', idx);
    end;

    { Pair each internal entry with the following external one. }
    n := Length(names);
    i := 0;
    while (summary <= 0) and (i < n) do
    begin
      if (lstat[i] = '-') and (i + 1 < n) and (lstat[i + 1] = '+') then
      begin
        if force then
        begin
          { upstream's equal(): a directory only has to exist }
          if (names[i] <> '') and (names[i][Length(names[i])] = '/') then
            same := FileExistsUTF8(names[i + 1])
          else
            same := Equal(TDT(dt.Objects[lref[i]]), names[i + 1]);
        end
        else
          same := (ldate[i] = ldate[i + 1]) and (lsize[i] = lsize[i + 1]) and
                  ((lattr[i] = 0) or (lattr[i + 1] = 0) or
                   (lattr[i] = lattr[i + 1]));
        if same then
        begin
          lstat[i] := '=';
          lskip[i + 1] := True;
        end
        else
          lstat[i] := '#';
      end;
      Inc(i);
    end;

    SetLength(order, 0);
    for i := 0 to n - 1 do
    begin
      SetLength(order, Length(order) + 1);
      order[High(order)] := i;
    end;
    if summary > 0 then
    begin
      { insertion sort with upstream's compareFragmentList }
      for i := 1 to High(order) do
      begin
        idx := order[i];
        j := i - 1;
        while (j >= 0) and LessEntry(idx, order[j]) do
        begin
          order[j + 1] := order[j];
          Dec(j);
        end;
        order[j + 1] := idx;
      end;
      if Length(order) > summary then SetLength(order, summary);
      { Mark duplicates: same fragment list as the previous entry. }
      for j := 1 to High(order) do
      begin
        i := order[j];
        if (ldate[i] <> 0) and (names[i] <> '') and
           (names[i][Length(names[i])] <> '/') and (lref[i] >= 0) and
           (Length(TDT(dt.Objects[lref[i]]).ptr) > 0) and
           (PtrCmp(RefPtr(order[j - 1]), RefPtr(i)) = 0) then
          lstat[i] := '^';
      end;
    end;

    usize := 0;
    for ii := 0 to High(order) do
    begin
      i := order[ii];
      if lskip[i] then Continue;
      st := lstat[i];
      case st of
        '=': Inc(matches);
        '#': Inc(mismatches);
        '-': Inc(internal);
        '+': Inc(external);
        '^': Inc(dups);
      end;
      if Pos(st, nottype) > 0 then Continue;
      if (names[i] <> '') and (names[i][Length(names[i])] <> '/') then
        Inc(usize, lsize[i]);
      Write(st, ' ', DateToString(ldate[i]), ' ',
            Format('%12.0f', [lsize[i] + 0.0]), ' ');
      if not noattributes then Write(AttrToString(lattr[i]), ' ');
      Write(names[i]);
      if (all > 0) and (Length(names[i]) = all + 1) and
         (names[i][Length(names[i])] = '/') then
      begin
        idx := StrToIntDef(Copy(names[i], 1, all), 0);
        if (idx > 0) and (idx < Length(ver)) then
        begin
          if idx + 1 < Length(ver) then j := ver[idx + 1].offset
          else j := lastOffset;
          Write(' +', ver[idx].updates, ' -', ver[idx].deletes, ' -> ',
                Format('%1.0f', [Int64(j) - ver[idx].offset + 0.0]));
        end;
      end;
      if (summary < 0) and (lref[i] >= 0) then
      begin
        dtr := TDT(dt.Objects[lref[i]]);
        hyphen := False;
        for j := 0 to High(dtr.ptr) do
        begin
          if (j = 0) or (j = High(dtr.ptr)) or
             (dtr.ptr[j] <> dtr.ptr[j - 1] + 1) or
             (j + 1 > High(dtr.ptr)) or
             (dtr.ptr[j] <> dtr.ptr[j + 1] - 1) then
          begin
            if not hyphen then Write(' ');
            hyphen := False;
            Write(dtr.ptr[j]);
          end
          else
          begin
            if not hyphen then Write('-');
            hyphen := True;
          end;
        end;
      end;
      WriteLn;
    end;

    { Statistics: dedupe counts over every file in the archive. }
    allsize := 0;
    ddsize := 0;
    nfiles := 0;
    refs := 0;
    nfrags := 0;
    unknown := 0;
    total := Cardinal(Length(ht)) - 1;
    SetLength(refsSeen, Length(ht));
    for i := 0 to dt.Count - 1 do
    begin
      dtr := TDT(dt.Objects[i]);
      if dtr.date = 0 then Continue;
      Inc(nfiles);
      for j := 0 to High(dtr.ptr) do
      begin
        idx := dtr.ptr[j];
        if (idx > 0) and (idx < Length(ht)) then
        begin
          Inc(refs);
          if ht[idx].usize >= 0 then Inc(allsize, ht[idx].usize);
          if not refsSeen[idx] then
          begin
            refsSeen[idx] := True;
            Inc(nfrags);
            if ht[idx].usize >= 0 then Inc(ddsize, ht[idx].usize)
            else Inc(unknown);
          end;
        end;
      end;
    end;
    WriteLn;
    WriteLn(Format('%1.6f MB of %1.6f MB (%d files) shown',
                   [usize / 1000000.0, allsize / 1000000.0, nfiles]));
    WriteLn(Format('  -> %1.6f MB (%d refs to %d of %d frags) after dedupe',
                   [ddsize / 1000000.0, refs, nfrags, total]));
    WriteLn(Format('  -> %1.6f MB compressed.',
                   [(lastOffset + dhsize - dcsize) / 1000000.0]));
    if unknown > 0 then
      WriteLn(unknown, ' fragments have unknown size');
    if files.Count > 0 then
      WriteLn(Format('%d =same, %d #different, %d +external, %d -internal',
                     [matches, mismatches, external, internal]));
    if summary > 0 then
      WriteLn(dups, ' of largest ', summary, ' files are ^duplicates');
    if dhsize <> dcsize then
      WriteLn(Format('Note: %1.0f of %1.0f compressed bytes are in archive',
                     [dcsize + 0.0, dhsize + 0.0]));
  finally
    edt.Free;
  end;
  Result := 0;
end;

procedure TruncateFile(const fn: string; size: Int64);
var
  fs: TFileStream;
begin
  fs := TFileStream.Create(fn, fmOpenReadWrite);
  try
    fs.Size := size;
  finally
    fs.Free;
  end;
end;

function SamePtr(const a: array of Cardinal; const b: array of Cardinal): Boolean;
var
  i: Integer;
begin
  Result := False;
  if Length(a) <> Length(b) then Exit;
  for i := 0 to High(a) do
    if a[i] <> b[i] then Exit;
  Result := True;
end;

function TJidac.Add(const addFiles: TStringList; const method: string;
                    date: Int64; force: Boolean; out errors: Integer): Integer;
var
  header_pos, header_end, archive_end, archive_size, cdatasize: Int64;
  outArc: TOutputArchive;
  w: TZqWriter;
  m, m2, fn: string;
  logbs, v: Int64;
  i, j, fi, fj, idx: Integer;
  blocksize, maxFrag, minFrag: Int64;
  edt, vf: TStringList;
  e: TEdtEntry;
  dtp: TDT;
  htinv: THTIndex;
  sb, isb: TZqBuf;
  fragbuf: PAnsiChar;
  total_size, total_done, fsize, dedupesize, esize, newsize: Int64;
  dst: TStream;
  { 64 KiB: the C scanner walks it in one call.  The read size does not affect
    fragmentation (only the bytes do), so this is purely a throughput knob. }
  buf: array[0..65535] of Byte;
  bufptr, buflen: Integer;
  hits: Cardinal;
  fscan: TZqFragscan;
  got: Int64;
  cut: Integer;
  eof: Boolean;
  sz, fsz: Int64;
  htptr: Cardinal;
  sha1result: array[0..19] of AnsiChar;
  sha: TZqSha1;
  o1, o1prev: array[0..1023] of Byte;
  o1ct: array[0..255] of Byte;
  text1, exe1: Integer;
  h1: Int64;
  h2: Cardinal;
  newblock, changed: Boolean;
  frags, redundancy, text, exe, ct: Cardinal;
  blocklist, csize: array of Int64;
  csizeOne, hp: Int64;
  dtcount, added, removed: Integer;
  htsize: Cardinal;
  nfilesC: Cardinal;
  jdcComment: AnsiString;
  outName, part0, partname, readSrc, newname: string;
  ntAdd: Integer;
  idxArc: TOutputArchive;
  wIdx: TZqWriter;
  idxInit: Boolean;
  pool: TCompPool;
  nthreads: Integer;
  pair: TWriterPair;
  wpw: TZqWriter;
  idxSalt: array[0..31] of AnsiChar;
  parts: Integer;
  partOffset: Int64;
  fs: TFileStream;
  salt: array[0..31] of AnsiChar;
  sp: Integer;
  cc: Int64;
  ch8: Int64;
  s15: Int64;
  ok: Boolean;
const
  ON = 4;
begin
  errors := 0;
  Result := 0;
  added := 0;
  removed := 0;
  dtcount := 0;
  SetLength(blocklist, 0);
  SetLength(csize, 0);
  jdcComment := 'jDC' + #1;
  m := method;

  { Read the existing archive (or its index when -index is used) }
  header_pos := 0;
  readSrc := archive;
  if indexName <> '' then readSrc := indexName;
  if FileExistsUTF8(SubPart(readSrc, 1)) then
    header_pos := ReadArchive(errors, nfilesC, readSrc);

  if indexName <> '' then
  begin
    { Indexed archive: each update is a new part named after the version
      count, and indexName holds the metadata of all of them. }
    if dcsize > 0 then
      raise Exception.Create('index is a regular archive');
    if version <> DEFAULT_VERSION then
      raise Exception.Create('cannot truncate with an index');
    partOffset := header_pos + dhsize;
    header_pos := 32 * Ord((password <> '') and (partOffset = 0));
    outName := SubPart(archive, Length(ver));
    if FileExistsUTF8(outName) then
      raise Exception.Create('archive exists');
  end
  else
  begin
  { Multi-part: "*" or "????" in the name.  Reading concatenates the parts;
    each add appends a new part, like upstream. }
  outName := archive;
  partOffset := 0;
  parts := 0;
  part0 := SubPart(archive, 0);
  if part0 <> archive then
  begin
    i := 1;
    while True do
    begin
      partname := SubPart(archive, i);
      if partname = part0 then
        raise Exception.Create('too many archive parts');
      if not FileExistsUTF8(partname) then Break;
      Inc(parts);
      fs := TFileStream.Create(partname, fmOpenRead or fmShareDenyNone);
      try
        Inc(partOffset, fs.Size);
      finally
        fs.Free;
      end;
      Inc(i);
    end;
    header_pos := 32 * Ord((password <> '') and (parts = 0));
    outName := SubPart(archive, parts + 1);
    if FileExistsUTF8(outName) then
      raise Exception.Create('part exists');
  end;
  end;

  { Encryption salt: reuse the one in the first part, or make a new one. }
  FillChar(salt, SizeOf(salt), 0);
  if (password <> '') and (indexName <> '') then
  begin
    { The archive salt is derived from the index salt; a new index gets a
      fresh random salt for the archive. }
    if FileExistsUTF8(SubPart(indexName, 1)) then
    begin
      fs := TFileStream.Create(SubPart(indexName, 1), fmOpenRead or fmShareDenyNone);
      try
        if fs.Read(salt, 32) <> 32 then
          raise Exception.Create('cannot read salt from index');
      finally
        fs.Free;
      end;
      salt[0] := AnsiChar(Ord(salt[0]) xor (Ord('7') xor Ord('z')));
    end
    else
      zq_random(@salt[0], 32);
  end
  else if password <> '' then
  begin
    if FileExistsUTF8(SubPart(archive, 1)) then
    begin
      fs := TFileStream.Create(SubPart(archive, 1), fmOpenRead or fmShareDenyNone);
      try
        if fs.Read(salt, 32) <> 32 then
          raise Exception.Create('cannot read salt');
      finally
        fs.Free;
      end;
    end
    else
    begin
      if header_pos > 32 then
        raise Exception.Create('archive first part not found');
      header_pos := 32;
      zq_random(@salt[0], 32);
    end;
  end;

  if FileExistsUTF8(outName) then Write('Updating ')
  else Write('Creating ');
  Write(outName, ' at offset ', Format('%1.0f', [header_pos + 0.0]), ' + ',
        Format('%1.0f', [partOffset + 0.0]));
  WriteLn;

  { Method defaults }
  if m = '' then m := '1';
  if Length(m) = 1 then
  begin
    if (m[1] >= '2') and (m[1] <= '9') then m := m + '6' else m := m + '4';
  end;
  if Pos(m[1], '0123456789xs') = 0 then
    raise Exception.Create('-method must begin with 0..5, x, s');
  if m[1] = 's' then
    raise Exception.Create('cannot update streaming archive in journaling format');

  v := 0;
  i := 2;
  while (i <= Length(m)) and (m[i] >= '0') and (m[i] <= '9') do
  begin
    v := v * 10 + Ord(m[i]) - Ord('0');
    Inc(i);
  end;
  logbs := 20 + v;
  if (logbs < 20) or (logbs > 31) then
    raise Exception.Create('blocksize must be 0..11');
  blocksize := (Int64(1) shl logbs) - 4096;
  { fragment size: -fragment N, 2^N KiB average (default 6) }
  if (fragment < 0) then fragment := 0;
  if (fragment > 19) or ((Int64(8128) shl fragment) > blocksize - 12) then
    maxFrag := blocksize - 12
  else
    maxFrag := Int64(8128) shl fragment;
  if (fragment > 25) or ((Int64(64) shl fragment) > maxFrag) then
    minFrag := maxFrag
  else
    minFrag := Int64(64) shl fragment;

  if indexName <> '' then
  begin
    if dcsize > 0 then
      raise Exception.Create('index is a regular archive');
    if version <> DEFAULT_VERSION then
      raise Exception.Create('cannot truncate with an index');
  end;
  for i := 0 to High(blocks) do
    if blocks[i].usize < 0 then
      raise Exception.Create('cannot update streaming archive in journaling format');

  { Scan files to add }
  edt := TStringList.Create;
  edt.Sorted := True;
  edt.CaseSensitive := True;
  edt.Duplicates := dupIgnore;
  for i := 0 to addFiles.Count - 1 do ScanInto(edt, addFiles[i]);

  { Select files, mark keepers, compute sort keys }
  vf := TStringList.Create;
  vf.Sorted := False;
  total_size := 0;
  for i := 0 to edt.Count - 1 do
  begin
    e := TEdtEntry(edt.Objects[i]);
    idx := dt.IndexOf(Rename(edt[i]));
    if idx >= 0 then TDT(dt.Objects[idx]).data := 1;
    if (e.date <> 0) and (edt[i] <> '') and (edt[i][Length(edt[i])] <> '/') and
       (force or (idx < 0) or (e.date <> TDT(dt.Objects[idx]).date) or
        (e.size <> TDT(dt.Objects[idx]).size)) then
    begin
      Inc(total_size, e.size);
      sp := 0;
      cc := 0;
      for j := 1 to Length(edt[i]) do
      begin
        ch8 := Ord(edt[i][j]);
        if (ch8 >= Ord('A')) and (ch8 <= Ord('Z')) then Inc(ch8, Ord('a') - Ord('A'));
        if ch8 = Ord('/') then
        begin
          sp := 0;
          cc := 0;
        end
        else if ch8 = Ord('.') then
        begin
          sp := 8;
          cc := 0;
        end
        else if sp > 3 then
        begin
          Dec(sp);
          Inc(cc, ch8 shl (sp * 8));
        end;
      end;
      s15 := e.size shr 14;
      if s15 >= (Int64(1) shl 24) then s15 := (Int64(1) shl 24) - 1;
      Inc(cc, (Int64(1) shl 24) - s15 - 1);
      e.sortkey := cc;
      e.data := 1;
      vf.AddObject(edt[i], e);
    end;
  end;

  if noattributes then
    for i := 0 to edt.Count - 1 do
      TEdtEntry(edt.Objects[i]).attr := 0;

  { Sort by sortkey, then by full path }
  i := 1;
  while i < vf.Count do
  begin
    j := i;
    while (j > 0) and
          ((TEdtEntry(vf.Objects[j - 1]).sortkey >
            TEdtEntry(vf.Objects[j]).sortkey) or
           ((TEdtEntry(vf.Objects[j - 1]).sortkey =
             TEdtEntry(vf.Objects[j]).sortkey) and (vf[j - 1] > vf[j]))) do
    begin
      vf.Exchange(j - 1, j);
      Dec(j);
    end;
    Inc(i);
  end;

  { Resolved thread count, as printed by upstream }
  ntAdd := threads;
  if ntAdd < 1 then ntAdd := CPUCount;
  WriteLn('Adding ', Format('%1.6f', [total_size / 1000000.0]), ' MB in ',
          vf.Count, ' files -method ', m, ' -threads ', ntAdd, ' at ',
          DateToString(date), '.');

  { Adjust date to keep versions sequential }
  if (Length(ver) > 0) and (ver[High(ver)].lastdate >= date) then
  begin
    j := Int64(UnixTime(ver[High(ver)].lastdate) + 1);
    WriteLn(StdErr, 'Warning: adjusting date from ', DateToString(date),
            ' to ', DateToString(DecimalTime(j)));
    date := DecimalTime(j);
  end;

  if Length(ht) = 0 then
  begin
    SetLength(ht, 1);
    FillChar(ht[0], SizeOf(THT), 0);
  end;
  htinv := THTIndex.Create(Length(ht) + (total_size shr 10) + vf.Count);
  htinv.Update(ht);   { index the fragments already in the archive }
  htsize := Cardinal(Length(ht));

  if password <> '' then
    outArc := TOutputArchive.Create(outName, password, @salt[0], partOffset)
  else
    outArc := TOutputArchive.Create(outName, password, nil, partOffset);
  idxArc := nil;
  pair := nil;
  idxInit := False;
  pool := nil;
  sb := zq_buf_new(SizeUInt(blocksize) + 4096 - 128);
  isb := zq_buf_new(64);
  GetMem(fragbuf, maxFrag);
  sha := zq_sha1_new;
  try
    w := OutputWriter(outArc);
    outArc.Seek(header_pos, 0);

    { Parallel compression: block boundaries and the method string are decided
      here, so the archive is identical for any thread count. }
    if threads <> 1 then
    begin
      nthreads := threads;
      if nthreads < 1 then nthreads := CPUCount;
      if nthreads > 1 then
      begin
        pool := TCompPool.Create(nthreads);
        pool.Start;
      end;
    end;

    { Placeholder transaction header }
    PutI(isb, -1, 8);
    fn := 'jDC' + Itos(date, 14) + 'c' + Itos(htsize, 10);
    if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(fn)),
                         PAnsiChar(jdcComment), 1) < 0 then
      raise Exception.Create(string(zq_last_error));
    zq_buf_resize(isb, 0);
    header_end := outArc.Tell();

    FillChar(o1prev, SizeOf(o1prev), 0);
    frags := 0;
    redundancy := 0;
    text := 0;
    exe := 0;
    total_done := 0;
    dedupesize := 0;

    for fi := 0 to vf.Count do
    begin
      dst := nil;
      bufptr := 0;
      buflen := 0;
      if fi < vf.Count then
      begin
        e := TEdtEntry(vf.Objects[fi]);
        dst := OpenIn(vf[fi]);
        if dst = nil then
        begin
          { upstream: skip the file, count the error and shrink the total }
          e.date := 0;
          Inc(errors);
          Dec(total_size, e.size);
          Continue;
        end;
      end;
      fsize := 0;
      fj := 0;
      while True do
      begin
        sz := 0;
        hits := 0;
        { Upstream starts this pass with c=EOF, so the synthetic pass at
          fi = vf.Count (which only flushes the last block) runs once. }
        eof := fi >= vf.Count;
        htptr := 0;
        FillChar(o1, 256, 0);
        if fi < vf.Count then
        begin
          { The cut loop lives in the C core: same state machine, same cuts,
            same hits and o1 table, but a call per buffer instead of a call (or
            a branch) per byte. }
          zq_fragscan_init(@fscan);
          zq_sha1_init(sha);
          cut := 0;
          repeat
            if bufptr >= buflen then
            begin
              bufptr := 0;
              buflen := dst.Read(buf, SizeOf(buf));
              if buflen < 0 then buflen := 0;
            end;
            if bufptr >= buflen then
            begin
              eof := True;
              Break;
            end;
            got := zq_frag_scan(PAnsiChar(@buf[bufptr]), buflen - bufptr,
                                @fscan, maxFrag, minFrag, fragment, @cut);
            if got > 0 then
            begin
              Move(buf[bufptr], fragbuf[fscan.sz - got], got);
              Inc(bufptr, got);
            end;
          until cut <> 0;
          sz := fscan.sz;
          hits := fscan.hits;
          Move(fscan.o1[0], o1[0], 256);
          Inc(total_done, sz);
          { One call per fragment instead of one per byte. }
          if sz > 0 then zq_sha1_write(sha, fragbuf, sz);
          Move(zq_sha1_result(sha)^, sha1result[0], 20);
          htptr := htinv.Find(ht, @sha1result[0]);
        end;

        if htptr = 0 then
        begin
          { Analyze the fragment: text, exe, redundancy }
          text1 := 0;
          exe1 := 0;
          h1 := sz;
          FillChar(o1ct, SizeOf(o1ct), 0);
          for i := 0 to 255 do
          begin
            if o1ct[o1[i]] < 255 then
            begin
              Dec(h1, (sz * ZQ_RED_DT[o1ct[o1[i]]]) shr 15);
              Inc(o1ct[o1[i]]);
            end;
            if (o1[i] = Ord(' ')) and
               (((i >= Ord('0')) and (i <= Ord('9'))) or
                ((i >= Ord('A')) and (i <= Ord('Z'))) or
                ((i >= Ord('a')) and (i <= Ord('z'))) or
                (i = Ord('.')) or (i = Ord(','))) then Inc(text1);
            if (o1[i] <> 0) and ((i < 9) or (i = 11) or (i = 12) or
               ((i >= 14) and (i <= 31)) or (i >= 240)) then Dec(text1);
            if (i >= 192) and (i < 240) and (o1[i] <> 0) and
               ((o1[i] < 128) or (o1[i] >= 192)) then Dec(text1);
            if o1[i] = 139 then Inc(exe1);
          end;
          if text1 >= 3 then text1 := 1 else text1 := 0;
          if exe1 >= 5 then exe1 := 1 else exe1 := 0;
          if sz > 0 then h1 := h1 * h1 div sz;
          h2 := Cardinal(h1);
          if h2 > hits then hits := h2;
          h2 := o1ct[0] * Cardinal(sz) div 256;
          if h2 > hits then hits := h2;
          h2 := 0;
          for i := 0 to 256 * ON - 1 do
            if o1prev[i] = o1[i and 255] then Inc(h2);
          h2 := h2 * Cardinal(sz) div (256 * ON);
          if h2 > hits then hits := h2;
          if hits > sz then hits := Cardinal(sz);

          { Should a new block start here? }
          newblock := False;
          if (frags > 0) and (fj = 0) and (fi < vf.Count) then
          begin
            esize := TEdtEntry(vf.Objects[fi]).size;
            newsize := Int64(zq_buf_size(sb)) + esize + (esize shr 14) + 4096 +
                       Int64(frags) * 4;
            if (newsize > blocksize div 4) and
               (Int64(redundancy) < Int64(zq_buf_size(sb)) div 128) then
              newblock := True;
            if newblock then
            begin
              ct := 0;
              for i := 0 to 256 * ON - 1 do
                if (o1prev[i] <> 0) and (o1prev[i] = o1[i and 255]) then Inc(ct);
              if ct > ON * 2 then newblock := False;
            end;
            if newsize >= blocksize then newblock := True;
          end;
          if Int64(zq_buf_size(sb)) + sz + 80 + Int64(frags) * 4 >= blocksize then
            newblock := True;
          if fi = vf.Count then newblock := True;
          if frags < 1 then newblock := False;

          if newblock then
          begin
            for i := Length(ht) - Int64(frags) to Length(ht) - 1 do
              PutI(sb, ht[i].usize, 4);
            PutI(sb, 0, 4);
            PutI(sb, frags, 4);
            m2 := m;
            if (m[1] >= '0') and (m[1] <= '9') then
              m2 := m2 + ',' +
                    Itos(Int64(redundancy) div (Int64(zq_buf_size(sb)) div 256 + 1)) +
                    ',' + Itos(Ord(exe > frags) * 2 + Ord(text > frags));
            PrintProgress(total_size, total_done, summary);
            if summary <= 0 then
              WriteLn('[', Length(ht) - Int64(frags), '..',
                      Int64(Length(ht)) - 1, '] ', zq_buf_size(sb),
                      ' -method ', m2);
            fn := 'jDC' + Itos(date, 14) + 'd' +
                  Itos(Length(ht) - Int64(frags), 10);
            SetLength(blocklist, Length(blocklist) + 1);
            blocklist[High(blocklist)] := Length(ht) - Int64(frags);
            if pool <> nil then
            begin
              { Keep the ring full; writing happens in submission order. }
              if pool.Queued >= pool.Slots then
              begin
                csizeOne := pool.FlushOne(w);
                SetLength(csize, Length(csize) + 1);
                csize[High(csize)] := csizeOne;
              end;
              pool.Submit(sb, m2, fn, jdcComment);
            end
            else
            begin
              hp := outArc.Tell();
              if zq_compress_block(sb, w, PAnsiChar(AnsiString(m2)),
                                   PAnsiChar(AnsiString(fn)),
                                   PAnsiChar(jdcComment), 1) < 0 then
                raise Exception.Create(string(zq_last_error));
              csizeOne := outArc.Tell() - hp;
              SetLength(csize, Length(csize) + 1);
              csize[High(csize)] := csizeOne;
              zq_buf_resize(sb, 0);
            end;
            frags := 0;
            redundancy := 0;
            text := 0;
            exe := 0;
            FillChar(o1prev, SizeOf(o1prev), 0);
          end;

          if sz > 0 then zq_buf_write(sb, fragbuf, LongInt(sz));
          Inc(frags);
          Inc(redundancy, hits);
          Inc(exe, Cardinal(exe1) * 4);
          Inc(text, Cardinal(text1) * 2);
          if sz >= minFrag then
          begin
            Move(o1prev[256], o1prev[0], 256 * (ON - 1));
            Move(o1[0], o1prev[256 * (ON - 1)], 256);
          end;
        end;

        if fi < vf.Count then
        begin
          if htptr = 0 then
          begin
            htptr := Cardinal(Length(ht));
            SetLength(ht, Length(ht) + 1);
            Move(sha1result[0], ht[High(ht)].sha1[0], 20);
            ht[High(ht)].usize := sz;
            ht[High(ht)].csize := 0;
            htinv.Update(ht);
            Inc(fsize, sz);
          end;
          SetLength(e.ptr, Length(e.ptr) + 1);
          e.ptr[High(e.ptr)] := htptr;
        end;
        if eof then Break;
        Inc(fj);
      end;
      if fi < vf.Count then
      begin
        Inc(dedupesize, fsize);
        PrintProgress(total_size, total_done, summary);
        if summary <= 0 then
        begin
          newname := Rename(vf[fi]);
          idx := dt.IndexOf(newname);
          if (idx < 0) or (TDT(dt.Objects[idx]).date = 0) then Write('+ ')
          else Write('# ');
          Write(vf[fi]);
          if newname <> vf[fi] then Write(' -> ', newname);
          Write(Format(' %1.0f', [TEdtEntry(vf.Objects[fi]).size + 0.0]));
          if fsize <> TEdtEntry(vf.Objects[fi]).size then
            Write(Format(' -> %1.0f', [fsize + 0.0]));
          WriteLn;
        end;
        if dst <> nil then FreeAndNil(dst);
      end;
    end;

    { Wait for the remaining blocks and write them in order. }
    if pool <> nil then
      while pool.Queued > 0 do
      begin
        csizeOne := pool.FlushOne(w);
        SetLength(csize, Length(csize) + 1);
        csize[High(csize)] := csizeOne;
      end;

    cdatasize := outArc.Tell() - header_end;

    { With -index the metadata blocks are mirrored into the index file; its
      transaction header records a data size of 0. }
    wpw := w;
    if indexName <> '' then
    begin
      Move(salt, idxSalt, 32);
      idxSalt[0] := AnsiChar(Ord(idxSalt[0]) xor (Ord('7') xor Ord('z')));
      if password <> '' then
        idxArc := TOutputArchive.Create(indexName, password, @idxSalt[0], 0)
      else
        idxArc := TOutputArchive.Create(indexName, password, nil, 0);
      wIdx := OutputWriter(idxArc);
      zq_buf_resize(isb, 0);
      PutI(isb, 0, 8);
      fn := 'jDC' + Itos(date, 14) + 'c' + Itos(htsize, 10);
      if zq_compress_block(isb, wIdx, '0', PAnsiChar(AnsiString(fn)),
                           PAnsiChar(jdcComment), 1) < 0 then
        raise Exception.Create(string(zq_last_error));
      zq_buf_resize(isb, 0);
      pair := TWriterPair.Create;
      pair.a := outArc;
      pair.b := idxArc;
      wpw := PairWriter(pair);
    end;

    { Fragment tables (h blocks) }
    SetLength(blocklist, Length(blocklist) + 1);
    blocklist[High(blocklist)] := Length(ht);
    zq_buf_resize(isb, 0);
    for i := 0 to High(csize) do
    begin
      if blocklist[i] < blocklist[i + 1] then
      begin
        PutI(isb, csize[i], 4);
        for j := blocklist[i] to blocklist[i + 1] - 1 do
        begin
          zq_buf_write(isb, @ht[j].sha1[0], 20);
          PutI(isb, ht[j].usize, 4);
        end;
        fn := 'jDC' + Itos(date, 14) + 'h' + Itos(blocklist[i], 10);
        if zq_compress_block(isb, wpw, '0', PAnsiChar(AnsiString(fn)),
                             PAnsiChar(jdcComment), 1) < 0 then
          raise Exception.Create(string(zq_last_error));
        zq_buf_resize(isb, 0);
      end;
    end;

    { Deletions }
    for i := 0 to dt.Count - 1 do
    begin
      dtp := TDT(dt.Objects[i]);
      if (dtp.date <> 0) and (dtp.data = 0) then
      begin
        PutI(isb, 0, 8);
        zq_buf_write(isb, PAnsiChar(AnsiString(dt[i])), Length(dt[i]));
        zq_buf_write(isb, #0, 1);
        if summary <= 0 then WriteLn('- ', dt[i]);
        Inc(removed);
        if zq_buf_size(isb) > 16000 then
        begin
          Inc(dtcount);
          fn := 'jDC' + Itos(date) + 'i' + Itos(dtcount, 10);
          if zq_compress_block(isb, wpw, '1', PAnsiChar(AnsiString(fn)),
                               PAnsiChar(jdcComment), 1) < 0 then
            raise Exception.Create(string(zq_last_error));
          zq_buf_resize(isb, 0);
        end;
      end;
    end;

    { Additions }
    i := 0;
    while True do
    begin
      if i < edt.Count then
      begin
        e := TEdtEntry(edt.Objects[i]);
        newname := Rename(edt[i]);
        idx := dt.IndexOf(newname);
        if idx >= 0 then dtp := TDT(dt.Objects[idx]) else dtp := nil;
        changed := (e.date <> 0) and
          ((dtp = nil) or (dtp.date <> e.date) or
           ((dtp.attr <> 0) and (dtp.attr <> e.attr)) or
           (dtp.size <> e.size) or
           ((e.sortkey <> 0) and (not SamePtr(dtp.ptr, e.ptr))));
        if changed then
        begin
          if (summary <= 0) and (e.data = 0) then
          begin
            newname := Rename(edt[i]);
            if (dtp = nil) or (dtp.date = 0) then Write('+ ')
            else Write('# ');
            Write(edt[i]);
            if newname <> edt[i] then Write(' -> ', newname);
            WriteLn;
          end;
          PutI(isb, e.date, 8);
          zq_buf_write(isb, PAnsiChar(AnsiString(newname)), Length(newname));
          zq_buf_write(isb, #0, 1);
          if (e.attr and 255) = Ord('u') then
          begin
            PutI(isb, 3, 4);
            PutI(isb, e.attr, 3);
          end
          else if (e.attr and 255) = Ord('w') then
          begin
            PutI(isb, 5, 4);
            PutI(isb, e.attr, 5);
          end
          else
            PutI(isb, 0, 4);
          if (dtp = nil) or (e.sortkey <> 0) then
          begin
            PutI(isb, Length(e.ptr), 4);
            for j := 0 to High(e.ptr) do PutI(isb, e.ptr[j], 4);
          end
          else
          begin
            PutI(isb, Length(dtp.ptr), 4);
            for j := 0 to High(dtp.ptr) do PutI(isb, dtp.ptr[j], 4);
          end;
          Inc(added);
        end;
      end;
      if (zq_buf_size(isb) > 16000) or
         ((zq_buf_size(isb) > 0) and (i >= edt.Count)) then
      begin
        Inc(dtcount);
        fn := 'jDC' + Itos(date) + 'i' + Itos(dtcount, 10);
        if zq_compress_block(isb, wpw, '1', PAnsiChar(AnsiString(fn)),
                             PAnsiChar(jdcComment), 1) < 0 then
          raise Exception.Create(string(zq_last_error));
        zq_buf_resize(isb, 0);
      end;
      if i >= edt.Count then Break;
      Inc(i);
    end;
    WriteLn(added, ' +added, ', removed, ' -removed.');

    { Rewrite the transaction header with the data size }
    archive_end := outArc.Tell();
    outArc.Seek(header_pos, 0);
    zq_buf_resize(isb, 0);
    PutI(isb, cdatasize, 8);
    fn := 'jDC' + Itos(date, 14) + 'c' + Itos(htsize, 10);
    if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(fn)),
                         PAnsiChar(jdcComment), 1) < 0 then
      raise Exception.Create(string(zq_last_error));
    outArc.Seek(0, 2);
    archive_size := outArc.Tell();

    { Close before truncating: our file layer keeps a share lock. }
    outArc.CloseArchive;

    if (indexName = '') and (added + removed = 0) and
       (archive_end - header_pos = 104) then
      archive_end := header_pos;
    if (indexName = '') and (archive_end < archive_size) then
    begin
      if archive_end > 0 then
      begin
        WriteLn('truncating archive from ', Format('%1.0f', [archive_size + 0.0]),
                ' to ', Format('%1.0f', [archive_end + 0.0]));
        TruncateFile(outName, archive_end);
      end
      else if archive_end = 0 then
      begin
        if DeleteFile(outName) then
          WriteLn('deleted ', outName);
      end;
    end;
    WriteLn(StdErr);
    WriteLn(StdErr, Format('%1.6f + (%1.6f -> %1.6f -> %1.6f) = %1.6f MB',
                           [header_pos / 1000000.0, total_size / 1000000.0,
                            dedupesize / 1000000.0,
                            (archive_end - header_pos) / 1000000.0,
                            archive_end / 1000000.0]));
  finally
    if pool <> nil then pool.Free;
    if idxArc <> nil then idxArc.Free;
    if pair <> nil then pair.Free;
    outArc.Free;
    zq_buf_free(sb);
    zq_buf_free(isb);
    FreeMem(fragbuf);
    zq_sha1_free(sha);
    htinv.Free;
    edt.Free;
    vf.Free;
  end;
  Result := Ord(errors > 0);
end;

end.
