{ zpaqII - journaling archive (JIDAC) reader, lister and extractor.
  Port of zpaq 7.15 (Unlicense, Matt Mahoney), unix side. }

unit zqjidac;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes, BaseUnix, zqcore, zqfile, zqarchive;

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
    function IsSelected(const fn: string): Boolean;
    function LoadBlock(const arcName, password: string; nb: Integer;
                       osb: TZqBuf; out bad: Boolean): Boolean;
  public
    archive: string;
    password: string;
    version: Int64;
    cmdDate: Int64;   { update date for streaming entries }
    ht: array of THT;
    blocks: array of TBlockInfo;
    ver: array of TVer;
    dt: TStringList;
    files: TStringList;   { selection prefixes, may be empty }
    isJidac: Boolean;
    constructor Create(const arc, pass: string; ver_: Int64);
    destructor Destroy; override;
    function ReadArchive(out errors: Integer; out nfiles: Cardinal): Int64;
    procedure List;
    function Extract(const destDir: string; force: Boolean): Integer;
    function Add(const addFiles: TStringList; const method: string; date: Int64;
                 force: Boolean; out errors: Integer): Integer;
    function FindBlockFor(frag: Cardinal): Integer;
  end;

const
  DEFAULT_VERSION = Int64(99999999999999);

implementation

type
  TNameSink = class
    s: AnsiString;
  end;

  TOutSink = class
    buf: TZqBuf;
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
  isJidac := False;
end;

destructor TJidac.Destroy;
var
  i: Integer;
begin
  for i := 0 to dt.Count - 1 do dt.Objects[i].Free;
  dt.Free;
  files.Free;
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

function TJidac.IsSelected(const fn: string): Boolean;
var
  i: Integer;
  p: AnsiString;
begin
  if files.Count = 0 then Exit(True);
  p := AnsiString(fn);
  for i := 0 to files.Count - 1 do
    if IsPath(PAnsiChar(AnsiString(files[i])), PAnsiChar(p)) then Exit(True);
  Result := False;
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
{ read_archive                                                        }
{ ------------------------------------------------------------------ }

function TJidac.ReadArchive(out errors: Integer; out nfiles: Cardinal): Int64;
var
  inArc: TInputArchive;
  d: TZqDecompresser;
  rd: TZqReader;
  osb: TZqBuf;
  filesSeen: Cardinal;
  lastfile: string;
  block_offset, data_offset: Int64;
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

  lastfile := archive;
  if Copy(lastfile, Length(lastfile) - 4, 5) = '.zpaq' then
    SetLength(lastfile, Length(lastfile) - 5);
  block_offset := 32 * Ord(password <> '');
  data_offset := block_offset;
  found_data := False;
  first := True;

  try
    inArc := TInputArchive.Create(archive, password);
  except
    WriteLn(StdErr, archive, ' not found.');
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
                if IsSelected(fn) then
                begin
                  existing := GetDT(fn);
                  existing.date := dtr.date;
                  existing.size := 0;
                  existing.attr := dtr.attr;
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
            if (segs = 0) or (Length(blocks) = 0) then
            begin
              blk := Default(TBlockInfo);
              blk.start := Cardinal(Length(ht));
              blk.offset := block_offset;
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

procedure TJidac.List;
var
  i: Integer;
  dtr: TDT;
  total: Int64;
begin
  total := 0;
  for i := 0 to dt.Count - 1 do
  begin
    dtr := TDT(dt.Objects[i]);
    if dtr.date = 0 then Continue;
    if (dt[i] <> '') and (dt[i][Length(dt[i])] <> '/') then
      Inc(total, dtr.size);
    Write('-', ' ', DateToString(dtr.date), ' ',
          Format('%12.0f', [dtr.size + 0.0]), ' ',
          AttrToString(dtr.attr), ' ', dt[i]);
    WriteLn;
  end;
end;

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
    rc := zq_d_find_block(d, nil);
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

function TJidac.Extract(const destDir: string; force: Boolean): Integer;
var
  i, nb, k: Integer;
  dtr: TDT;
  frag: Cardinal;
  offsetInBlock, j: Int64;
  outPath: string;
  fo: TFileStream;
  blockData: PAnsiChar;
  curBlock: Integer;
  osb: TZqBuf;
  bad: Boolean;
  errors: Integer;
begin
  Result := 0;
  errors := 0;
  curBlock := -1;
  osb := zq_buf_new(1 shl 16);
  try
    for i := 0 to dt.Count - 1 do
    begin
      dtr := TDT(dt.Objects[i]);
      if dtr.date = 0 then Continue;
      if (dt[i] <> '') and (dt[i][Length(dt[i])] = '/') then
      begin
        if destDir <> '' then
          MakePath(IncludeTrailingPathDelimiter(destDir) + dt[i])
        else
          MakePath(dt[i]);
        Continue;
      end;
      outPath := dt[i];
      if destDir <> '' then
        outPath := IncludeTrailingPathDelimiter(destDir) + outPath;
      MakePath(outPath);
      fo := TFileStream.Create(outPath, fmCreate);
      try
        for k := 0 to High(dtr.ptr) do
        begin
          frag := dtr.ptr[k];
          if (frag = 0) or (frag >= Cardinal(Length(ht))) then Continue;
          nb := FindBlockFor(frag);
          if nb < 0 then
          begin
            WriteLn(StdErr, 'missing block for fragment ', frag);
            Inc(errors);
            Continue;
          end;
          if nb <> curBlock then
          begin
            if not LoadBlock(archive, password, nb, osb, bad) then
            begin
              WriteLn(StdErr, 'cannot decompress block ', nb);
              Inc(errors);
              curBlock := -1;
              Continue;
            end;
            if bad then
            begin
              WriteLn(StdErr, 'bad checksum in block ', nb);
              Inc(errors);
            end;
            curBlock := nb;
          end;
          offsetInBlock := 0;
          for j := Int64(blocks[nb].start) to Int64(frag) - 1 do
            Inc(offsetInBlock, ht[j].usize);
          blockData := zq_buf_data(osb);
          if ht[frag].usize > 0 then
            fo.Write((blockData + offsetInBlock)^, ht[frag].usize);
        end;
      finally
        fo.Free;
      end;
      CloseFileAttrs(outPath, dtr.date, dtr.attr);
    end;
  finally
    zq_buf_free(osb);
  end;
  Result := errors;
end;


{ ------------------------------------------------------------------ }
{ add (journaling)                                                    }
{ ------------------------------------------------------------------ }

type
  TEdtEntry = class
    date, size, attr, sortkey: Int64;
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

procedure ScanInto(edt: TStringList; const name: string);
var
  date, size, attr: Int64;
  isDir: Boolean;
  idx: Integer;
  e: TEdtEntry;
  sr: TSearchRec;
  fn, sub, nm: string;
begin
  FileInfo(name, date, size, attr, isDir);
  if date = 0 then Exit;
  nm := name;
  while (Length(nm) > 1) and (nm[Length(nm)] = '/') do
    SetLength(nm, Length(nm) - 1);
  if isDir then
  begin
    fn := nm + '/';
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
  else
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
  end;
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
  dst: TFileStream;
  buf: array[0..4095] of Byte;
  bufptr, buflen: Integer;
  c, c1: Integer;
  h, hits: Cardinal;
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

  { Read the existing archive }
  header_pos := 0;
  if FileExists(SubPart(archive, 1)) then
    header_pos := ReadArchive(errors, nfilesC);

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
  { fragment size: default -fragment 6 }
  if (6 > 19) or ((Int64(8128) shl 6) > blocksize - 12) then
    maxFrag := blocksize - 12
  else
    maxFrag := Int64(8128) shl 6;
  if (6 > 25) or ((Int64(64) shl 6) > maxFrag) then
    minFrag := maxFrag
  else
    minFrag := Int64(64) shl 6;

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
    idx := dt.IndexOf(edt[i]);
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
      vf.AddObject(edt[i], e);
    end;
  end;

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

  { Adjust date to keep versions sequential }
  if (Length(ver) > 0) and (ver[High(ver)].lastdate >= date) then
  begin
    date := DecimalTime(UnixTime(ver[High(ver)].lastdate) + 1);
    WriteLn(StdErr, 'Warning: adjusting date to ', DateToString(date));
  end;

  if Length(ht) = 0 then
  begin
    SetLength(ht, 1);
    FillChar(ht[0], SizeOf(THT), 0);
  end;
  htinv := THTIndex.Create(Length(ht) + (total_size shr 10) + vf.Count);
  htsize := Cardinal(Length(ht));

  outArc := TOutputArchive.Create(archive, password, nil, 0);
  sb := zq_buf_new(SizeUInt(blocksize) + 4096 - 128);
  isb := zq_buf_new(64);
  GetMem(fragbuf, maxFrag);
  sha := zq_sha1_new;
  try
    w := OutputWriter(outArc);
    outArc.Seek(header_pos, 0);

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
        dst := TFileStream.Create(vf[fi], fmOpenRead or fmShareDenyNone);
      end;
      fsize := 0;
      fj := 0;
      while True do
      begin
        sz := 0;
        hits := 0;
        c := -1;
        htptr := 0;
        FillChar(o1, 256, 0);
        if fi < vf.Count then
        begin
          c1 := 0;
          h := 0;
          zq_sha1_init(sha);
          while True do
          begin
            if bufptr >= buflen then
            begin
              bufptr := 0;
              buflen := dst.Read(buf, 4096);
            end;
            if bufptr >= buflen then
              c := -1
            else
            begin
              c := buf[bufptr];
              Inc(bufptr);
            end;
            if c >= 0 then
            begin
              if c = o1[c1] then
              begin
                h := (h + Cardinal(c) + 1) * 314159265;
                Inc(hits);
              end
              else
                h := (h + Cardinal(c) + 1) * 271828182;
              o1[c1] := Byte(c);
              c1 := c;
              zq_sha1_put(sha, c);
              if sz < maxFrag then fragbuf[sz] := AnsiChar(c);
              Inc(sz);
            end;
            if (c < 0) or (sz >= maxFrag) or
               ((h < (Cardinal(1) shl (22 - 6))) and (sz >= minFrag)) then
              Break;
          end;
          Inc(total_done, sz);
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
            fn := 'jDC' + Itos(date, 14) + 'd' +
                  Itos(Length(ht) - Int64(frags), 10);
            hp := outArc.Tell();
            if zq_compress_block(sb, w, PAnsiChar(AnsiString(m2)),
                                 PAnsiChar(AnsiString(fn)),
                                 PAnsiChar(jdcComment), 1) < 0 then
              raise Exception.Create(string(zq_last_error));
            csizeOne := outArc.Tell() - hp;
            SetLength(blocklist, Length(blocklist) + 1);
            blocklist[High(blocklist)] := Length(ht) - Int64(frags);
            SetLength(csize, Length(csize) + 1);
            csize[High(csize)] := csizeOne;
            zq_buf_resize(sb, 0);
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
        if c < 0 then Break;
        Inc(fj);
      end;
      if fi < vf.Count then
      begin
        Inc(dedupesize, fsize);
        if dst <> nil then FreeAndNil(dst);
      end;
    end;

    cdatasize := outArc.Tell() - header_end;

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
        if zq_compress_block(isb, w, '0', PAnsiChar(AnsiString(fn)),
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
        Inc(removed);
        if zq_buf_size(isb) > 16000 then
        begin
          Inc(dtcount);
          fn := 'jDC' + Itos(date) + 'i' + Itos(dtcount, 10);
          if zq_compress_block(isb, w, '1', PAnsiChar(AnsiString(fn)),
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
        idx := dt.IndexOf(edt[i]);
        if idx >= 0 then dtp := TDT(dt.Objects[idx]) else dtp := nil;
        changed := (e.date <> 0) and
          ((dtp = nil) or (dtp.date <> e.date) or
           ((dtp.attr <> 0) and (dtp.attr <> e.attr)) or
           (dtp.size <> e.size) or
           ((e.sortkey <> 0) and (not SamePtr(dtp.ptr, e.ptr))));
        if changed then
        begin
          PutI(isb, e.date, 8);
          zq_buf_write(isb, PAnsiChar(AnsiString(edt[i])), Length(edt[i]));
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
        if zq_compress_block(isb, w, '1', PAnsiChar(AnsiString(fn)),
                             PAnsiChar(jdcComment), 1) < 0 then
          raise Exception.Create(string(zq_last_error));
        zq_buf_resize(isb, 0);
      end;
      if i >= edt.Count then Break;
      Inc(i);
    end;

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

    if (added + removed = 0) and (archive_end - header_pos = 104) then
      archive_end := header_pos;
    if archive_end < archive_size then
    begin
      if archive_end > 0 then
        TruncateFile(archive, archive_end)
      else
        DeleteFile(archive);
    end;
  finally
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
