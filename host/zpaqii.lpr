{ zpaqII - command line archiver (Pascal host).

  Stage E5: streaming archives (method s...) with add/extract/list.  The
  journaling (JIDAC) commands are added on top of the same archive layer. }

program zpaqii;
{$mode objfpc}{$H+}

uses {$IFDEF UNIX}cthreads,{$ENDIF} SysUtils, DateUtils, Classes, zqcore,
  zqfile, zqarchive, zqjidac;

const
  VERSION = 'zpaqII 0.1';

var
  GName: AnsiString;
  GStart: Int64;
  GThreads: Integer = 0;   { -tN: 0 = auto-detect, like upstream's threads }

{ End the program: print the elapsed time like upstream (stderr) and exit.
  quiet is used for the usage paths, which upstream exits before printing it. }
procedure Finish(code: Integer; quiet: Boolean = False);
var
  secs: Double;
  state: string;
begin
  if not quiet then
  begin
    secs := (MTime - GStart) / 1000.0;
    if code > 1 then state := '(with errors)'
    else if code > 0 then state := '(with warnings)'
    else state := '(all OK)';
    WriteLn(StdErr, Format('%1.3f seconds %s', [secs, state]));
  end;
  Halt(code);
end;

{ Print the usage text and exit like upstream's usage() (no timing line). }
procedure Usage; forward;

procedure UsageExit;
begin
  Usage;
  Finish(1, True);
end;

type
  TFileData = class
  public
    Stream: TFileStream;
    destructor Destroy; override;
    procedure Put(c: LongInt);
    procedure WriteBuf(buf: PAnsiChar; len: LongInt);
  end;

destructor TFileData.Destroy;
begin
  Stream.Free;   { close (and unlock) the previous segment's output }
  inherited;
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

{ Current UTC date as YYYYMMDDHHMMSS, honouring the ZPAQII_NOW test hook. }
function CurrentDate: Int64;
var
  v: string;
begin
  v := GetEnvironmentVariable('ZPAQII_NOW');
  if v <> '' then
    Result := DecimalTime(StrToInt64(v))
  else
    Result := DecimalTime(DateTimeToUnix(Now, False));
end;

{ Upstream's usage() text, verbatim. }
procedure Usage;
var
  nt: Integer;
begin
  WriteLn('Usage: zpaq command archive[.zpaq] files... -options...');
  WriteLn('Files... may be directory trees. Default is the whole archive.');
  WriteLn('Use * or ???? in archive name for multi-part or "" for empty.');
  WriteLn('Commands:');
  WriteLn('   a  add         Append files to archive if dates have changed.');
  WriteLn('   x  extract     Extract most recent versions of files.');
  WriteLn('   l  list        List or compare external files to archive by dates.');
  WriteLn('Options:');
  WriteLn('  -all [N]        Extract/list versions in N [4] digit directories.');
  WriteLn('  -f -force       Add: append files if contents have changed.');
  WriteLn('                  Extract: overwrite existing output files.');
  WriteLn('                  List: compare file contents instead of dates.');
  WriteLn('  -index F        Extract: create index F for archive.');
  WriteLn('                  Add: create suffix for archive indexed by F, update F.');
  WriteLn('  -key X          Create or access encrypted archive with password X.');
  WriteLn('  -mN  -method N  Compress level N (0..5 = faster..better, default 1).');
  WriteLn('  -noattributes   Ignore/don''t save file attributes or permissions.');
  WriteLn('  -not files...   Exclude. * and ? match any string or char.');
  WriteLn('       =[+-#^?]   List: exclude by comparison result.');
  WriteLn('  -only files...  Include only matches (default: *).');
  WriteLn('  -repack F [X]   Extract to new archive F with key X (default: none).');
  WriteLn('  -sN -summary N  List: show top N sorted by size. -1: show frag IDs.');
  WriteLn('                  Add/Extract: if N > 0 show brief progress.');
  WriteLn('  -test           Extract: verify but do not write files.');
  WriteLn('  -tN -threads N  Use N threads (default: 0 = ', GThreads, ' cores).');
  WriteLn('  -to out...      Rename files... to out... or all to out/all.');
  WriteLn('  -until N        Roll back archive to N''th update or -N from end.');
  WriteLn('  -until ', DateToString(CurrentDate),
          '  Set date, roll back (UT, default time: 235959).');
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

{ Sort key used by add (upstream): last path component, first 5 extension
  bytes case insensitive, then descending size rounded to 16 KiB. }
function AddSortKey(const name: string; size: Int64): Int64;
var
  sp, i: Integer;
  cc, ch: Int64;
begin
  sp := 0;
  cc := 0;
  for i := 1 to Length(name) do
  begin
    ch := Ord(name[i]);
    if (ch >= Ord('A')) and (ch <= Ord('Z')) then Inc(ch, Ord('a') - Ord('A'));
    if ch = Ord('/') then
    begin
      sp := 0;
      cc := 0;
    end
    else if ch = Ord('.') then
    begin
      sp := 8;
      cc := 0;
    end
    else if sp > 3 then
    begin
      Dec(sp);
      Inc(cc, ch shl (sp * 8));
    end;
  end;
  i := 0;
  if size shr 14 >= (Int64(1) shl 24) then i := (1 shl 24) - 1
  else i := Integer(size shr 14);
  Result := cc + (Int64(1) shl 24) - i - 1;
end;

function AddStreaming(const archive, method, key: string;
                      files: TStringList; trDate: Int64;
                      nthreads, sum: Integer; const indexName: string): Integer;
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
  headerPos, totalSize, totalDone: Int64;
  nfiles: Cardinal;
  errs, nt: Integer;
  reader: TJidac;
  order: array of Integer;
  keys: array of Int64;
  tmpIdx, si, sj: Integer;
  mixed: Boolean;
begin
  headerPos := 0;
  mixed := False;
  totalSize := 0;
  totalDone := 0;
  nt := nthreads;
  if nt < 1 then nt := CPUCount;
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
  if FileExistsUTF8(archive) then
  begin
    { Read it first: the banner and the append offset come from the scan. }
    reader := TJidac.Create(archive, key, DEFAULT_VERSION);
    try
      for i := 0 to files.Count - 1 do reader.files.Add(files[i]);
      headerPos := reader.ReadArchive(errs, nfiles);
      { a journaling archive cannot be updated in streaming format }
      for i := 0 to High(reader.blocks) do
        if reader.blocks[i].usize >= 0 then mixed := True;
    finally
      reader.Free;
    end;
    Write('Updating ');
  end
  else
    Write('Creating ');
  Write(archive, ' at offset ', Format('%1.0f', [headerPos + 0.0]), ' + 0');
  WriteLn;
  if indexName <> '' then
    raise Exception.Create('cannot index in streaming mode');
  if mixed then
    raise Exception.Create('cannot update journaling archive in streaming format');

  for i := 0 to files.Count - 1 do
  begin
    FileInfo(files[i], date, size, attr, isDir);
    if date <> 0 then Inc(totalSize, size);
  end;
  WriteLn('Adding ', Format('%1.6f', [totalSize / 1000000.0]), ' MB in ',
          files.Count, ' files -method ', method, ' -threads ', nt, ' at ',
          DateToString(trDate), '.');

  { Upstream adds the files in sortkey order (extension, then size). }
  SetLength(order, files.Count);
  SetLength(keys, files.Count);
  for i := 0 to files.Count - 1 do
  begin
    order[i] := i;
    FileInfo(files[i], date, size, attr, isDir);
    keys[i] := AddSortKey(files[i], size);
  end;
  for si := 1 to High(order) do
  begin
    tmpIdx := order[si];
    sj := si - 1;
    while (sj >= 0) and ((keys[order[sj]] > keys[tmpIdx]) or
          ((keys[order[sj]] = keys[tmpIdx]) and
           (files[order[sj]] > files[tmpIdx]))) do
    begin
      order[sj + 1] := order[sj];
      Dec(sj);
    end;
    order[sj + 1] := tmpIdx;
  end;

  out := TOutputArchive.Create(archive, key, saltPtr, 0);
  bs := BlockSizeOf(method);
  try
    w := OutputWriter(out);
    b := zq_buf_new(bs + 4096 - 128);
    try
      for si := 0 to High(order) do
      begin
        i := order[si];
        FileInfo(files[i], date, size, attr, isDir);
        if date = 0 then
        begin
          WriteLn(StdErr, 'zpaqii: cannot read ', files[i]);
          Exit(1);
        end;
        PrintProgress(totalSize, totalDone, sum);
        if sum <= 0 then
          WriteLn('+ ', files[i], ' ', size);
        zq_buf_resize(b, 0);
        fs := TFileStream.Create(files[i], fmOpenRead or fmShareDenyNone);
        try
          first := 1;
          repeat
            nr := fs.Read(tmp, 4096);
            if nr > 0 then
            begin
              zq_buf_write(b, @tmp[0], nr);
              Inc(totalDone, nr);
            end;
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
    WriteLn(Format('%1.0f + (%1.0f -> %1.0f) = %1.0f',
                   [headerPos + 0.0, totalSize + 0.0,
                    out.Tell() - headerPos + 0.0, out.Tell() + 0.0]));
  finally
    out.Free;
  end;
  Result := 0;
end;

procedure NullPut(ctx: Pointer; c: LongInt); cdecl;
begin
end;

procedure NullWrite(ctx: Pointer; buf: PAnsiChar; n: LongInt); cdecl;
begin
end;

function NullWriter: TZqWriter;
begin
  Result.put := @NullPut;
  Result.write := @NullWrite;
  Result.ctx := nil;
end;

function ExtractStreaming(j: TJidac; listOnly, force: Boolean;
                          sum: Integer): Integer;
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
  rc, idx, segments, totalFiles, nt: Integer;
  totalSize: Int64;
  dtr: TDT;
begin
  inArc := TInputArchive.Create(j.archive, j.password);
  d := zq_d_new;
  fd := nil;
  total := 0;
  haveFile := False;
  segments := 0;
  if not listOnly then
  begin
    j.PrepareExtract(force, False, totalFiles, totalSize);
    nt := j.threads;
    if nt < 1 then nt := CPUCount;
    WriteLn('Extracting ', Format('%1.6f', [totalSize / 1000000.0]),
            ' MB in ', totalFiles, ' files -threads ', nt);
  end;
  try
    r := InputReader(inArc);
    zq_d_set_input(d, r);
    nw := NameWriter;
    while True do
    begin
      rc := zq_d_find_block(d, nil);
      if rc < 0 then Fail(string(zq_last_error));
      if rc = 0 then Break;
      { Only extract what read_archive could scan: after a truncated block the
        decompresser can resynchronise on garbage and invent segments that
        upstream never sees (it walks the scanned block list). }
      if (inArc.Tell() - zq_d_buffered(d)) >= j.lastOffset then Break;
      try
      while True do
      begin
        GName := '';
        rc := zq_d_find_filename(d, @nw);
        if rc < 0 then Fail(string(zq_last_error));
        if rc = 0 then Break;
        if zq_d_read_comment(d, nil) < 0 then Fail(string(zq_last_error));

        outPath := string(GName);
        if not listOnly then
        begin
          Inc(segments);
          { A segment with an empty name continues the previous file. }
          idx := -1;
          if outPath <> '' then idx := j.dt.IndexOf(outPath);
          if (outPath = '') and (segments > 1) then
            idx := -1
          else if (outPath <> '') and (idx >= 0) and
             (TDT(j.dt.Objects[idx]).data = 0) then
          begin
            if sum <= 0 then WriteLn('> ', j.Rename(outPath));
            outPath := j.Rename(outPath);
            MakePath(outPath);
            if fd <> nil then FreeAndNil(fd);
            fd := TFileData.Create;
            fd.Stream := TFileStream.Create(outPath, fmCreate);
            haveFile := True;
            dw := MakeWriter(fd);
            zq_d_set_output(d, dw);
          end
          else if (outPath <> '') or (segments = 1) then
          begin
            if fd <> nil then FreeAndNil(fd);
            dw := NullWriter;
            zq_d_set_output(d, dw);
          end;
        end;

        if zq_d_decompress(d, -1) < 0 then
          raise Exception.Create(string(zq_last_error));

        if zq_d_read_segment_end(d, nil) < 0 then
          raise Exception.Create(string(zq_last_error));
        if listOnly and j.IsSelected(string(GName), False) then
          WriteLn(GName);
      end;
      except
        on e: Exception do
        begin
          { upstream catches per block and keeps going }
          WriteLn('Skipping block: ', e.Message);
          if fd <> nil then FreeAndNil(fd);
        end;
      end;
    end;
  finally
    if fd <> nil then fd.Free;
    zq_d_free(d);
    inArc.Free;
  end;
  if segments > 0 then
    WriteLn(segments, ' streaming segments extracted');
  Result := 0;
end;

function RunExtract(j: TJidac; listOnly, force: Boolean): Integer;
var
  errors: Integer;
  nfiles: Cardinal;
  sz: Int64;
begin
  { -repack -all copies the whole archive before reading it (upstream). }
  if (not listOnly) and (j.repack <> '') and (j.all > 0) then
  begin
    Result := j.RepackWhole(force);
    Exit;
  end;
  sz := j.ReadArchive(errors, nfiles);
  { extract() checks the scan result before doing anything else; list() does
    not, so a truncated archive still lists (upstream). }
  if (not listOnly) and (sz < 1) then
    raise Exception.Create('archive not found');
  { Read errors (skipped blocks) only affect the exit code of add: extract()
    and the -index path ignore them, and list() always returns 0. }
  if (j.indexName <> '') and (not listOnly) then
    Result := j.WriteIndex(j.indexName, force)
  else if listOnly then
  begin
    j.List(force);        { tolerant: works for streaming and journaling }
    Result := 0;          { upstream list() always returns 0 }
  end
  else if j.isJidac then
  begin
    if j.repack <> '' then
      Result := j.RepackSelected(force)
    else
      Result := Ord(j.Extract(force, j.dotest) > 0);
  end
  else if j.repack <> '' then
    Result := j.RepackSelected(force)   { errors out, like upstream }
  else
    Result := ExtractStreaming(j, False, force, j.summary);
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
  repackName, newKey: string;
  methodSet, optsStarted, archiveSet: Boolean;
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
  summary: Integer;
  dotest, noattrs: Boolean;
  allver: Integer;
  tmp: TJidac;
  errs2: Integer;
  nfiles2: Cardinal;

begin
  GStart := MTime;
  ZqStartTime := GStart;
  DefaultFormatSettings.DecimalSeparator := '.';
  WriteLn('zpaq v7.15 journaling archiver, compiled ', {$I %DATE%});
  if ParamCount < 1 then
  begin
    GThreads := CPUCount;   { upstream resolves the thread count first }
    UsageExit;
  end;
  cmd := ParamStr(1);
  if (cmd <> '') and (cmd[1] = '-') then
  begin
    { upstream parses argv[1] as an option, warns and prints the usage }
    WriteLn('Unknown option ignored: ', cmd);
    UsageExit;
  end;

  archive := '';
  method := '1';
  methodSet := False;
  optsStarted := False;
  archiveSet := False;
  repackName := '';
  newKey := '';
  key := '';
  indexName := '';
  untilVer := DEFAULT_VERSION;
  dateSet := False;
  dateVal := 0;
  force := False;
  frag := 6;
  GThreads := 0;
  summary := 0;
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
      { upstream reads files only between the archive name and the first
        option; any later bare argument is a usage error }
      if (a <> '') and (a[1] = '-') then optsStarted := True;
      if (a = '-m') or (a = '-method') then
      begin
        Inc(i);
        method := ParamStr(i);
        methodSet := True;
      end
      else if (Copy(a, 1, 2) = '-m') and (Length(a) > 2) then
      begin
        method := Copy(a, 3, MaxInt);
        methodSet := True;
      end
      else if (Copy(a, 1, 2) = '-t') and (Length(a) > 2) and
              (a[3] >= '0') and (a[3] <= '9') then
        GThreads := StrToIntDef(Copy(a, 3, MaxInt), 0)
      else if (a = '-threads') then
      begin
        Inc(i);
        GThreads := StrToIntDef(ParamStr(i), 0);
      end
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
      else if (a = '-all') then
      begin
        allver := 4;
        if (i < ParamCount) and (ParamStr(i + 1) <> '') and
           (ParamStr(i + 1)[1] >= '0') and (ParamStr(i + 1)[1] <= '9') then
        begin
          Inc(i);
          allver := StrToIntDef(ParamStr(i), 4);
        end;
      end
      else if (a = '-noattributes') then
        noattrs := True
      else if (a = '-test') then
        dotest := True
      else if (a = '-repack') and (i < ParamCount) then
      begin
        Inc(i);
        repackName := ParamStr(i);
        if (i < ParamCount) and
           ((ParamStr(i + 1) = '') or (ParamStr(i + 1)[1] <> '-')) then
        begin
          Inc(i);
          newKey := KeyDigest(ParamStr(i));
        end;
      end
      else if (a = '-summary') then
      begin
        Inc(i);
        summary := StrToIntDef(ParamStr(i), 0);
      end
      else if (Copy(a, 1, 2) = '-s') and (Length(a) > 2) and
              (((a[3] >= '0') and (a[3] <= '9')) or (a[3] = '-')) then
        summary := StrToIntDef(Copy(a, 3, MaxInt), 0)
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
      else if (a <> '') and (a[1] = '-') and (Length(a) > 1) then
      begin
        { upstream warns and then prints the usage, exiting with 1 }
        WriteLn('Unknown option ignored: ', a);
        UsageExit;
      end
      else if (a = '-') then
      begin
        UsageExit;
      end
      else if optsStarted then
      begin
        UsageExit;
      end
      else if not archiveSet then
      begin
        archive := a;
        archiveSet := True;
      end
      else
        files.Add(a);
      Inc(i);
    end;

    if not archiveSet then
    begin
      UsageExit;
    end;
    { An unknown command is rejected during parsing, before the thread count is
      resolved (upstream calls usage() from the argument loop). }
    if (cmd <> 'add') and (cmd <> 'a') and (cmd <> 'extract') and
       (cmd <> 'x') and (cmd <> 'list') and (cmd <> 'l') and
       (cmd <> 'convert') and (cmd <> 'c') then
    begin
      UsageExit;
    end;
    { upstream resolves the thread count right after parsing }
    if GThreads < 1 then GThreads := CPUCount;
    if not dateSet then
    begin
      a := GetEnvironmentVariable('ZPAQII_NOW');
      if a <> '' then dateVal := DecimalTime(StrToInt64(a))
      else dateVal := DecimalTime(DateTimeToUnix(Now, False));
    end;
    if (archive <> '') and (ExtractFileExt(archive) = '') then
      archive := archive + '.zpaq';

    if (cmd = 'add') or (cmd = 'a') then
    begin
      if files.Count = 0 then Fail('no files to add');
      { upstream scans into a map keyed by name: byte order, case sensitive }
      files.CaseSensitive := True;
      files.Sort;
      if (method <> '') and (method[1] = 's') then
        Finish(AddStreaming(archive, method, key, files, dateVal, GThreads,
                            summary, indexName));
      if not dateSet then
      begin
        { testing hook: ZPAQII_NOW freezes the clock (unix seconds) }
        a := GetEnvironmentVariable('ZPAQII_NOW');
        if a <> '' then
          dateVal := DecimalTime(StrToInt64(a))
        else
          dateVal := DecimalTime(DateTimeToUnix(Now, False));
      end;
      begin
        jadd := TJidac.Create(archive, key, untilVer);
        jadd.fragment := frag;
        jadd.noattributes := noattrs;
        jadd.summary := summary;
        jadd.threads := GThreads;
        jadd.indexName := indexName;
        for i := 0 to files.Count - 1 do jadd.files.Add(files[i]);
        for i := 0 to tofiles.Count - 1 do jadd.tofiles.Add(tofiles[i]);
        for i := 0 to onlyfiles.Count - 1 do jadd.onlyfiles.Add(onlyfiles[i]);
        for i := 0 to notfiles.Count - 1 do jadd.notfiles.Add(notfiles[i]);
        jadd.nottype := nottype;
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
        jadd.threads := GThreads;
        jadd.summary := summary;
        jadd.dotest := dotest;
        jadd.noattributes := noattrs;
        jadd.all := allver;
        jadd.repack := repackName;
        jadd.newpassword := newKey;
        jadd.methodSet := methodSet;
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
      UsageExit;
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
