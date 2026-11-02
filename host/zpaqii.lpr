{ zpaqII - command line archiver (Pascal host).

  Stage E5: streaming archives (method s...) with add/extract/list.  The
  journaling (JIDAC) commands are added on top of the same archive layer. }

program zpaqii;
{$mode objfpc}{$H+}

uses SysUtils, Classes, zqcore, zqfile, zqarchive;

const
  VERSION = 'zpaqII 0.1';

var
  GName: AnsiString;

type
  TFileData = class
  public
    Stream: TFileStream;
    procedure Put(c: LongInt);
    procedure WriteBuf(buf: PAnsiChar; len: LongInt);
  end;

procedure TFileData.Put(c: LongInt);
var
  b: AnsiChar;
begin
  b := AnsiChar(c);
  Stream.Write(b, 1);
end;

procedure TFileData.WriteBuf(buf: PAnsiChar; len: LongInt);
begin
  if len > 0 then Stream.Write(buf^, len);
end;

procedure NamePut(ctx: Pointer; c: LongInt); cdecl;
begin
  GName := GName + AnsiChar(c);
end;

procedure FilePut(ctx: Pointer; c: LongInt); cdecl;
begin
  TFileData(ctx).Put(c);
end;

procedure FileWrite(ctx: Pointer; buf: PAnsiChar; n: LongInt); cdecl;
begin
  TFileData(ctx).WriteBuf(buf, n);
end;

function MakeWriter(d: TFileData): TZqWriter;
begin
  Result.put := @FilePut;
  Result.write := @FileWrite;
  Result.ctx := Pointer(d);
end;

function NameWriter: TZqWriter;
begin
  Result.put := @NamePut;
  Result.write := nil;
  Result.ctx := nil;
end;

procedure Usage;
begin
  WriteLn(VERSION);
  WriteLn('Usage: zpaqii command archive[.zpaq] files... -options...');
  WriteLn('Commands: add, extract, list');
  WriteLn('Options: -method/-m METHOD, -to DIR, -key PASSWORD');
end;

procedure Fail(const msg: string);
begin
  WriteLn(StdErr, 'zpaqii: ' + msg);
  Halt(1);
end;

function ReadWholeFile(const fn: string; b: TZqBuf): Boolean;
var
  fs: TFileStream;
  tmp: array[0..65535] of AnsiChar;
  nr: LongInt;
begin
  Result := False;
  if not FileExists(fn) then Exit;
  fs := TFileStream.Create(fn, fmOpenRead or fmShareDenyWrite);
  try
    repeat
      nr := fs.Read(tmp, SizeOf(tmp));
      if nr > 0 then zq_buf_write(b, @tmp[0], nr);
    until nr <= 0;
  finally
    fs.Free;
  end;
  Result := True;
end;

function BlockSizeOf(const method: string): Int64;
var
  i, n: Integer;
begin
  n := 0;
  i := 2;
  while (i <= Length(method)) and (method[i] >= '0') and (method[i] <= '9') do
  begin
    n := n * 10 + Ord(method[i]) - Ord('0');
    Inc(i);
  end;
  if n > 11 then n := 11;
  Result := (Int64(1) shl (20 + n)) - 4096;
end;

function AddStreaming(const archive, method: string; files: TStringList): Integer;
var
  out: TOutputArchive;
  w: TZqWriter;
  b: TZqBuf;
  i: Integer;
  bs, size, date, attr, n: Int64;
  isDir: Boolean;
  fs: TFileStream;
  tmp: array[0..4095] of AnsiChar;
  nr, first: Integer;
  comment, fn: string;
  cptr: PAnsiChar;
begin
  out := TOutputArchive.Create(archive, '', nil, 0);
  bs := BlockSizeOf(method);
  try
    w := OutputWriter(out);
    b := zq_buf_new(bs + 4096 - 128);
    try
      for i := 0 to files.Count - 1 do
      begin
        FileInfo(files[i], date, size, attr, isDir);
        if date = 0 then
        begin
          WriteLn(StdErr, 'zpaqii: cannot read ', files[i]);
          Exit(1);
        end;
        zq_buf_resize(b, 0);
        fs := TFileStream.Create(files[i], fmOpenRead or fmShareDenyNone);
        try
          first := 1;
          repeat
            nr := fs.Read(tmp, 4096);
            if nr > 0 then zq_buf_write(b, @tmp[0], nr);
            if (nr = 0) or (Int64(zq_buf_size(b)) + 4096 > bs) then
            begin
              fn := '';
              comment := '';
              if first = 1 then
              begin
                fn := files[i];
                comment := Itos(date);
                if (attr and 255) > 0 then
                  comment := comment + ' ' + Chr(attr and 255) + Itos(attr shr 8);
              end;
              cptr := nil;
              if comment <> '' then cptr := PAnsiChar(AnsiString(comment));
              if zq_compress_block(b, w, PAnsiChar(AnsiString(method)),
                                   PAnsiChar(AnsiString(fn)), cptr, 1) < 0 then
                Fail(string(zq_last_error));
              zq_buf_resize(b, 0);
              first := 0;
            end;
          until nr = 0;
        finally
          fs.Free;
        end;
      end;
    finally
      zq_buf_free(b);
    end;
  finally
    out.Free;
  end;
  Result := 0;
end;

function ExtractArchive(const archive, toDir: string; listOnly: Boolean): Integer;
var
  inArc: TInputArchive;
  r: TZqReader;
  d: TZqDecompresser;
  nw: TZqWriter;
  outPath: string;
  fd: TFileData;
  dw: TZqWriter;
  haveFile: Boolean;
  total: Int64;
  rc: Integer;
begin
  inArc := TInputArchive.Create(archive, '');
  d := zq_d_new;
  fd := nil;
  total := 0;
  haveFile := False;
  try
    r := InputReader(inArc);
    zq_d_set_input(d, r);
    nw := NameWriter;
    while True do
    begin
      rc := zq_d_find_block(d, nil);
      if rc < 0 then Fail(string(zq_last_error));
      if rc = 0 then Break;
      while True do
      begin
        GName := '';
        rc := zq_d_find_filename(d, @nw);
        if rc < 0 then Fail(string(zq_last_error));
        if rc = 0 then Break;
        if zq_d_read_comment(d, nil) < 0 then Fail(string(zq_last_error));

        outPath := string(GName);
        if not listOnly and (outPath <> '') then
        begin
          outPath := IncludeTrailingPathDelimiter(toDir) + outPath;
          MakePath(outPath);
          if fd <> nil then FreeAndNil(fd);
          fd := TFileData.Create;
          fd.Stream := TFileStream.Create(outPath, fmCreate);
          haveFile := True;
          dw := MakeWriter(fd);
          zq_d_set_output(d, dw);
        end;

        if zq_d_decompress(d, -1) < 0 then Fail(string(zq_last_error));

        if zq_d_read_segment_end(d, nil) < 0 then Fail(string(zq_last_error));
        if listOnly then
          WriteLn(GName);
      end;
    end;
  finally
    if fd <> nil then fd.Free;
    zq_d_free(d);
    inArc.Free;
  end;
  Result := 0;
end;

var
  cmd, archive, method, toDir, key: string;
  files: TStringList;
  i: Integer;
  a: string;

begin
  if ParamCount < 1 then
  begin
    Usage;
    Halt(1);
  end;
  cmd := ParamStr(1);
  if (cmd = '-h') or (cmd = '--help') or (cmd = 'help') then
  begin
    Usage;
    Halt(0);
  end;

  archive := '';
  method := '1';
  toDir := '.';
  key := '';
  files := TStringList.Create;
  try
    i := 2;
    while i <= ParamCount do
    begin
      a := ParamStr(i);
      if (a = '-m') or (a = '-method') then
      begin
        Inc(i);
        method := ParamStr(i);
      end
      else if (a = '-to') then
      begin
        Inc(i);
        toDir := ParamStr(i);
      end
      else if (a = '-key') then
      begin
        Inc(i);
        key := ParamStr(i);
      end
      else if (a <> '') and (a[1] = '-') then
        Fail('unknown option ' + a)
      else if archive = '' then
        archive := a
      else
        files.Add(a);
      Inc(i);
    end;

    if archive = '' then Fail('missing archive name');
    if ExtractFileExt(archive) = '' then archive := archive + '.zpaq';

    if cmd = 'add' then
    begin
      if files.Count = 0 then Fail('no files to add');
      { upstream scans into a map keyed by name: byte order, case sensitive }
      files.CaseSensitive := True;
      files.Sort;
      Halt(AddStreaming(archive, method, files));
    end
    else if (cmd = 'extract') or (cmd = 'x') then
      Halt(ExtractArchive(archive, toDir, False))
    else if (cmd = 'list') or (cmd = 'l') then
      Halt(ExtractArchive(archive, toDir, True))
    else
    begin
      Usage;
      Halt(1);
    end;
  finally
    files.Free;
  end;
end.
