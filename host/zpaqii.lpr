{ zpaqII - command line archiver (Pascal host).

  Stage E5: streaming archives (method s...) with add/extract/list.  The
  journaling (JIDAC) commands are added on top of the same archive layer. }

program zpaqii;
{$mode objfpc}{$H+}

uses SysUtils, DateUtils, Classes, zqcore, zqfile, zqarchive, zqjidac;

const
  VERSION = 'zpaqII 0.1';

var
  GName: AnsiString;
  GStart: Int64;

{ End the program: print the elapsed time like upstream (stderr) and exit. }
procedure Finish(code: Integer);
var
  secs: Double;
  state: string;
begin
  secs := (MTime - GStart) / 1000.0;
  if code > 1 then state := '(with errors)'
  else if code > 0 then state := '(with warnings)'
  else state := '(all OK)';
  WriteLn(StdErr, Format('%1.3f seconds %s', [secs, state]));
  Halt(code);
end;

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
  WriteLn('Options: -method/-m METHOD   compress level 0..5 or explicit method');
  WriteLn('         -to DIR...          rename files... to dir... (or all to DIR)');
  WriteLn('         -only files...      include only these prefixes');
  WriteLn('         -not files...       exclude these prefixes');
  WriteLn('         -key PASSWORD       create or access an encrypted archive');
  WriteLn('         -fragment N         average fragment size 2^N KiB (default 6)');
  WriteLn('         -index F            extract: write an index F for the archive');
  WriteLn('                             add: append a part and update index F');
  WriteLn('         -force/-f           add: re-add changed contents');
  WriteLn('         -until N            roll back to the N-th update');
  WriteLn('         Use * or ???? in the archive name for multi-part archives.');
end;

{ -key X: upstream uses SHA256(X) as the stretchKey password. }
function KeyDigest(const text: string): string;
var
  s: TZqSha256;
  p: PAnsiChar;
  i: Integer;
begin
  s := zq_sha256_new;
  try
    for i := 1 to Length(text) do
      zq_sha256_put(s, Ord(text[i]));
    p := zq_sha256_result(s);
    SetLength(Result, 32);
    Move(p^, Result[1], 32);
  finally
    zq_sha256_free(s);
  end;
end;

procedure Fail(const msg: string);
begin
  WriteLn(StdErr, 'zpaqii: ' + msg);
  Finish(1);
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

function AddStreaming(const archive, method, key: string;
                      files: TStringList): Integer;
var
  out: TOutputArchive;
  salt: array[0..31] of AnsiChar;
  saltPtr: PAnsiChar;
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
  saltPtr := nil;
  if key <> '' then
  begin
    if FileExistsUTF8(archive) then saltPtr := nil  { reuse the file's salt }
    else
    begin
      zq_random(@salt[0], 32);
      saltPtr := @salt[0];
    end;
  end;
  out := TOutputArchive.Create(archive, key, saltPtr, 0);
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

function ExtractStreaming(j: TJidac; listOnly: Boolean): Integer;
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
  inArc := TInputArchive.Create(j.archive, j.password);
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
        if not listOnly and (outPath <> '') and
           j.IsSelected(outPath, False) then
        begin
          outPath := j.Rename(outPath);
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
        if listOnly and j.IsSelected(string(GName), False) then
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

function RunExtract(j: TJidac; listOnly, force: Boolean): Integer;
var
  errors: Integer;
  nfiles: Cardinal;
begin
  j.ReadArchive(errors, nfiles);
  if (j.indexName <> '') and (not listOnly) then
  begin
    Result := j.WriteIndex(j.indexName, force);
    if errors <> 0 then Result := 1;
  end
  else if listOnly then
  begin
    j.List;               { tolerant: works for streaming and journaling }
    Result := 0;          { upstream list() always returns 0 }
  end
  else if j.isJidac then
  begin
    Result := j.Extract(force);
    if errors <> 0 then Result := 1;
  end
  else
    Result := ExtractStreaming(j, False);
end;

{ -until N: N-th update, -N: count from the end, or a date.  Follows
  upstream: single-digit fields are zero padded (2020-1-5), and values in the
  date ranges get the default time appended. }
function ParseUntil(const s: string): Int64;
var
  i, digits: Integer;
  v: Int64;
  neg: Boolean;
begin
  neg := (s <> '') and (s[1] = '-');
  if neg then
  begin
    v := StrToInt64Def(Copy(s, 2, MaxInt), 0);
    Exit(-v);
  end;
  v := 0;
  digits := 0;
  for i := 1 to Length(s) do
  begin
    if (s[i] >= '0') and (s[i] <= '9') then
    begin
      v := v * 10 + Ord(s[i]) - Ord('0');
      Inc(digits);
    end
    else
    begin
      if digits = 1 then v := v div 10 * 100 + v mod 10;
      digits := 0;
    end;
  end;
  if digits = 1 then v := v div 10 * 100 + v mod 10;
  if (v >= 19000000) and (v <= 29991231) then v := v * 100 + 23;
  if (v >= 1900000000) and (v <= 2999123123) then v := v * 100 + 59;
  if (v >= 190000000000) and (v <= 299912312359) then v := v * 100 + 59;
  if v > 9999999 then
  begin
    if (v < 19000101000000) or (v > 29991231235959) then
      raise Exception.Create('date is incorrect, use -until YYYY-MM-DD HH:MM:SS');
  end;
  Result := v;   { small values are version numbers }
end;

var
  cmd, archive, method, key, indexName: string;
  files, tofiles, onlyfiles, notfiles: TStringList;
  nottype: string;
  i: Integer;
  a: string;
  untilVer: Int64;
  dateVal: Int64;
  jadd: TJidac;
  errs, rc: Integer;
  dateSet: Boolean;
  force: Boolean;
  frag: Integer;
  tmp: TJidac;
  errs2: Integer;
  nfiles2: Cardinal;

begin
  GStart := MTime;
  DefaultFormatSettings.DecimalSeparator := '.';
  WriteLn('zpaq v7.15 journaling archiver, compiled ', {$I %DATE%});
  if ParamCount < 1 then
  begin
    Usage;
    Finish(1);
  end;
  cmd := ParamStr(1);
  if (cmd = '-h') or (cmd = '--help') or (cmd = 'help') then
  begin
    Usage;
    Finish(0);
  end;

  archive := '';
  method := '1';
  key := '';
  indexName := '';
  untilVer := DEFAULT_VERSION;
  dateSet := False;
  dateVal := 0;
  force := False;
  frag := 6;
  nottype := '';
  files := TStringList.Create;
  files.CaseSensitive := True;
  tofiles := TStringList.Create;
  tofiles.CaseSensitive := True;
  onlyfiles := TStringList.Create;
  onlyfiles.CaseSensitive := True;
  notfiles := TStringList.Create;
  notfiles.CaseSensitive := True;
  try
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
      else if (Copy(a, 1, 2) = '-m') and (Length(a) > 2) then
        method := Copy(a, 3, MaxInt)
      else if (Copy(a, 1, 2) = '-t') and (Length(a) > 2) and
              (a[3] >= '0') and (a[3] <= '9') then
        { single-threaded: -tN accepted and ignored }
      else if (a = '-threads') then
        Inc(i)
      else if (a = '-to') then
      begin
        while (i < ParamCount) and (ParamStr(i + 1) <> '') and
              (ParamStr(i + 1)[1] <> '-') do
        begin
          Inc(i);
          tofiles.Add(ParamStr(i));
        end;
        if tofiles.Count = 0 then tofiles.Add('');
      end
      else if (a = '-only') then
      begin
        while (i < ParamCount) and (ParamStr(i + 1) <> '') and
              (ParamStr(i + 1)[1] <> '-') do
        begin
          Inc(i);
          onlyfiles.Add(ParamStr(i));
        end;
      end
      else if (a = '-not') then
      begin
        while (i < ParamCount) and (ParamStr(i + 1) <> '') and
              (ParamStr(i + 1)[1] <> '-') do
        begin
          Inc(i);
          if ParamStr(i)[1] = '=' then nottype := ParamStr(i)
          else notfiles.Add(ParamStr(i));
        end;
      end
      else if (a = '-index') then
      begin
        Inc(i);
        indexName := ParamStr(i);
      end
      else if (a = '-fragment') then
      begin
        Inc(i);
        frag := StrToIntDef(ParamStr(i), 6);
      end
      else if (a = '-key') then
      begin
        Inc(i);
        key := KeyDigest(ParamStr(i));
      end
      else if (a = '-force') or (a = '-f') then
        force := True
      else if (a = '-until') then
      begin
        Inc(i);
        untilVer := ParseUntil(ParamStr(i));
      end
      else if (a = '-date') then
      begin
        Inc(i);
        dateVal := ParseUntil(ParamStr(i));
        dateSet := True;
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
    if not dateSet then
    begin
      a := GetEnvironmentVariable('ZPAQII_NOW');
      if a <> '' then dateVal := DecimalTime(StrToInt64(a))
      else dateVal := DecimalTime(DateTimeToUnix(Now));
    end;
    if ExtractFileExt(archive) = '' then archive := archive + '.zpaq';

    if cmd = 'add' then
    begin
      if files.Count = 0 then Fail('no files to add');
      { upstream scans into a map keyed by name: byte order, case sensitive }
      files.CaseSensitive := True;
      files.Sort;
      if (method <> '') and (method[1] = 's') then
        Finish(AddStreaming(archive, method, key, files));
      if not dateSet then
      begin
        { testing hook: ZPAQII_NOW freezes the clock (unix seconds) }
        a := GetEnvironmentVariable('ZPAQII_NOW');
        if a <> '' then
          dateVal := DecimalTime(StrToInt64(a))
        else
          dateVal := DecimalTime(DateTimeToUnix(Now));
      end;
      begin
        jadd := TJidac.Create(archive, key, DEFAULT_VERSION);
        jadd.fragment := frag;
        jadd.indexName := indexName;
        for i := 0 to files.Count - 1 do jadd.files.Add(files[i]);
        try
          rc := jadd.Add(files, method, dateVal, force, errs);
          if errs <> 0 then rc := 1;
        finally
          jadd.Free;
        end;
        Finish(rc);
      end;
    end
    else if (cmd = 'extract') or (cmd = 'x') or
            (cmd = 'list') or (cmd = 'l') then
    begin
      jadd := TJidac.Create(archive, key, untilVer);
      jadd.cmdDate := dateVal;
      try
        for i := 0 to files.Count - 1 do jadd.files.Add(files[i]);
        for i := 0 to tofiles.Count - 1 do jadd.tofiles.Add(tofiles[i]);
        for i := 0 to onlyfiles.Count - 1 do jadd.onlyfiles.Add(onlyfiles[i]);
        for i := 0 to notfiles.Count - 1 do jadd.notfiles.Add(notfiles[i]);
        jadd.nottype := nottype;
        jadd.indexName := indexName;
        if jadd.version < 0 then
        begin
          { -until -N: count from the end, like upstream }
          tmp := TJidac.Create(archive, key, DEFAULT_VERSION);
          try
            tmp.ReadArchive(errs2, nfiles2);
            jadd.version := jadd.version + Int64(Length(tmp.ver) - 1);
            WriteLn('Version ', jadd.version);
          finally
            tmp.Free;
          end;
        end;
        if (cmd = 'list') or (cmd = 'l') then
          Finish(RunExtract(jadd, True, force))
        else
          Finish(RunExtract(jadd, False, force));
      finally
        jadd.Free;
      end;
    end
    else
    begin
      Usage;
      Finish(1);
    end;
   except
    on e: Exception do
    begin
      WriteLn(StdErr, 'zpaq error: ', e.Message);
      Finish(2);
    end;
   end;
  finally
    files.Free;
    tofiles.Free;
    onlyfiles.Free;
    notfiles.Free;
  end;
end.
