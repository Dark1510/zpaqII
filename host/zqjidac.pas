{ zpaqII - journaling archive (JIDAC) reader, lister and extractor.
  Port of zpaq 7.15 (Unlicense, Matt Mahoney), unix side. }

unit zqjidac;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes, zqcore, zqfile, zqarchive;

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
    ptr: array of Cardinal;
  end;

  TJidac = class
  private
    function GetDT(const fn: string): TDT;
    function LoadBlock(const arcName, password: string; nb: Integer;
                       osb: TZqBuf; out bad: Boolean): Boolean;
  public
    archive: string;
    password: string;
    version: Int64;
    ht: array of THT;
    blocks: array of TBlockInfo;
    ver: array of TVer;
    dt: TStringList;
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
  SetLength(ht, 0);
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
  isJidac := False;
end;

destructor TJidac.Destroy;
var
  i: Integer;
begin
  for i := 0 to dt.Count - 1 do dt.Objects[i].Free;
  dt.Free;
  inherited;
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
                  for j := 0 to na - 1 do
                  begin
                    if j < 8 then
                      Inc(dtr.attr, Int64(Ord(s^)) shl (j * 8));
                    Inc(s);
                  end;
                  ni := BToI(s);
                  SetLength(dtr.ptr, ni);
                  for j := 0 to ni - 1 do
                    dtr.ptr[j] := BToI(s);
                end;
                existing := GetDT(fn);
                existing.date := dtr.date;
                existing.size := 0;
                existing.attr := dtr.attr;
                existing.ptr := dtr.ptr;
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
              dtr.date := version;
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
    for j := 0 to Cardinal(Length(dtr.ptr)) - 1 do
    begin
      if (dtr.ptr[j] > 0) and (dtr.ptr[j] < Cardinal(Length(ht))) then
      begin
        if ht[dtr.ptr[j]].usize >= 0 then
          Inc(dtr.size, ht[dtr.ptr[j]].usize)
        else
          dtr.size := -1;
      end;
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
{ add (journaling): not implemented yet                                }
{ ------------------------------------------------------------------ }

function TJidac.Add(const addFiles: TStringList; const method: string; date: Int64;
                    force: Boolean; out errors: Integer): Integer;
begin
  errors := 0;
  raise Exception.Create('journaling add is not implemented yet');
  Result := 1;
end;

end.
