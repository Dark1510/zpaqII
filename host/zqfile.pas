{ zpaqII - file helpers, dates and attributes.

  Port of zpaq 7.15 (Unlicense, Matt Mahoney) for both sides: Unix stores
  attributes as 'u' + (mode << 8) and Windows as 'w' + (attributes << 8), which
  is exactly what upstream does on each platform. }

unit zqfile;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes
{$IFDEF WINDOWS}
  , Windows
{$ELSE}
  , BaseUnix, Unix
{$ENDIF};

{ 64-bit decimal YYYYMMDDHHMMSS from a unix time_t }
function DecimalTime(tt: Int64): Int64;
{ inverse of DecimalTime; -1 if date <= 0 }
function UnixTime(date: Int64): Int64;
{ current time in milliseconds (monotonic: only differences are used) }
function MTime: Int64;
{ "YYYY-MM-DD HH:MM:SS" or blanks }
function DateToString(date: Int64): string;
{ attribute string as shown by list (unix mode bits or Windows attributes) }
function AttrToString(attrib: Int64): string;

{ mtime (decimal), size, attributes and type of a file.  Symlinks are not
  followed: isReg and isDir are both False for them (and for devices, fifos
  and sockets), so the scanner can skip them silently. }
procedure FileInfo(const fn: string; out date, size, attr: Int64;
                   out isDir, isReg: Boolean);

function FileExistsUTF8(const fn: string): Boolean;
function DeleteFileUTF8(const fn: string): Boolean;
{ set mtime and attributes if date/attr are non-zero }
procedure CloseFileAttrs(const fn: string; date, attr: Int64);
{ create parent directories of path }
procedure MakePath(const path: string; date: Int64 = 0; attr: Int64 = 0);
{ non-negative decimal with at least n digits }
function Itos(x: Int64; n: Integer = 1): string;
{ replace ? and * in fn with digits of part }
function SubPart(const fn: string; part: Integer): string;
{ number of online CPUs (1 if it cannot be determined) }
function CPUCount: Integer;
{ last error of the system call that failed (errno or GetLastError) }
function FileGetErrno: Integer;
{ the text upstream prints for a file that does not exist }
function MissingFileMessage: string;

{ Open an input file like upstream's fopen(RB); nil and a message on stderr on
  failure (upstream's printerr). }
function OpenRead(const path: string): TStream;
{ Open an output file like upstream's fopen; create=True truncates. }
function OpenWrite(const path: string; create: Boolean): TStream;

implementation

function DecimalTime(tt: Int64): Int64;
var
  t, term: Int64;
  second, minute, hour, year, month, day: Int64;
begin
  if tt = -1 then tt := 0;
  t := tt;
  second := t mod 60;
  minute := t div 60 mod 60;
  hour := t div 3600 mod 24;
  t := t div 86400;
  term := t div 1461;
  t := t mod 1461;
  if t >= 59 then Inc(t);
  if t >= 425 then Inc(t);
  if t >= 1157 then Inc(t);
  year := term * 4 + t div 366 + 1970;
  t := t mod 366;
  if t >= 60 then Inc(t, 2);
  if t >= 123 then Inc(t);
  if t >= 185 then Inc(t);
  if t >= 278 then Inc(t);
  if t >= 340 then Inc(t);
  month := t div 31 + 1;
  day := t mod 31 + 1;
  Result := year * 10000000000 + month * 100000000 + day * 1000000 +
            hour * 10000 + minute * 100 + second;
end;

function UnixTime(date: Int64): Int64;
const
  days: array[0..11] of Integer = (0, 31, 59, 90, 120, 151, 181, 212, 243,
                                   273, 304, 334);
var
  year, month, day, hour, minute, sec: Int64;
begin
  if date <= 0 then Exit(-1);
  year := date div 10000000000 mod 10000;
  month := (date div 100000000 mod 100 - 1) mod 12;
  day := date div 1000000 mod 100;
  hour := date div 10000 mod 100;
  minute := date div 100 mod 100;
  sec := date mod 100;
  Result := (day - 1 + days[month] + Ord((year mod 4 = 0) and (month > 1)) +
             ((year - 1970) * 1461 + 1) div 4) * 86400 + hour * 3600 +
            minute * 60 + sec;
end;

{$IFDEF WINDOWS}
{ 100 ns intervals since 1601 -> unix seconds }
function FileTimeToUnix(const ft: TFileTime): Int64;
var
  v: Int64;
begin
  v := (Int64(ft.dwHighDateTime) shl 32) or Int64(ft.dwLowDateTime);
  Result := v div 10000000 - 11644473600;
end;

function UnixToFileTime(t: Int64): TFileTime;
var
  v: Int64;
begin
  v := (t + 11644473600) * 10000000;
  Result.dwLowDateTime := DWORD(v and $FFFFFFFF);
  Result.dwHighDateTime := DWORD((v shr 32) and $FFFFFFFF);
end;

function WidePath(const fn: string): UnicodeString;
begin
  Result := UTF8Decode(fn);
end;
{$ENDIF}

function MTime: Int64;
{$IFDEF WINDOWS}
begin
  Result := Int64(GetTickCount64);
end;
{$ELSE}
var
  tv: TTimeVal;
begin
  fpGetTimeOfDay(@tv, nil);
  Result := Int64(tv.tv_sec) * 1000 + tv.tv_usec div 1000;
end;
{$ENDIF}

function DateToString(date: Int64): string;
const
  t: array[0..13] of Integer = (18, 17, 15, 14, 12, 11, 9, 8, 6, 5, 3, 2, 1, 0);
var
  s: string;
  i: Integer;
  d: Int64;
begin
  if date <= 0 then Exit('                   ');
  s := '0000-00-00 00:00:00';
  d := date;
  for i := 0 to 13 do
  begin
    Inc(s[t[i] + 1], d mod 10);
    d := d div 10;
  end;
  Result := s;
end;

function AttrToString(attrib: Int64): string;
const
  types = '0pc3d5b7 9lBsDEF';
  wtypes = 'ANH SRHA  T';
var
  i: Integer;
begin
  Result := '     ';
  if (attrib and 255) = Ord('u') then
  begin
    Result[1] := types[((attrib shr 20) and 15) + 1];
    Result[5] := Chr(Ord('0') + (attrib shr (8 + 0)) mod 8);
    Result[4] := Chr(Ord('0') + (attrib shr (8 + 3)) mod 8);
    Result[3] := Chr(Ord('0') + (attrib shr (8 + 6)) mod 8);
    Result[2] := Chr(Ord('0') + (attrib shr (8 + 9)) mod 8);
  end
  else if (attrib and 255) = Ord('w') then
  begin
    { upstream's attrToString for 'w': one letter per attribute bit }
    for i := 0 to 3 do
      if ((attrib shr (i + 8)) and 1) <> 0 then Result[i + 1] := wtypes[i + 1];
  end;
end;

procedure FileInfo(const fn: string; out date, size, attr: Int64;
  out isDir, isReg: Boolean);
{$IFDEF WINDOWS}
var
  fad: TWin32FileAttributeData;
begin
  date := 0;
  size := 0;
  attr := 0;
  isDir := False;
  isReg := False;
  if GetFileAttributesExW(PWideChar(WidePath(fn)), GetFileExInfoStandard,
                          @fad) then
  begin
    date := DecimalTime(FileTimeToUnix(fad.ftLastWriteTime));
    attr := Ord('w') + (Int64(fad.dwFileAttributes) shl 8);
    isDir := (fad.dwFileAttributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
    { a reparse point (symlink, junction) is neither: upstream skips it }
    if (fad.dwFileAttributes and FILE_ATTRIBUTE_REPARSE_POINT) = 0 then
    begin
      size := (Int64(fad.nFileSizeHigh) shl 32) or Int64(fad.nFileSizeLow);
      isReg := not isDir;
    end;
  end;
end;
{$ELSE}
var
  st: Stat;
begin
  date := 0;
  size := 0;
  attr := 0;
  isDir := False;
  isReg := False;
  { lstat, not stat: upstream does not follow symbolic links when scanning }
  if fpLStat(fn, st) <> 0 then Exit;
  date := DecimalTime(st.st_mtime);
  size := st.st_size;
  attr := Ord('u') + (Int64(st.st_mode) shl 8);
  isDir := fpS_ISDIR(st.st_mode);
  isReg := fpS_ISREG(st.st_mode);
end;
{$ENDIF}

function FileExistsUTF8(const fn: string): Boolean;
begin
  Result := FileExists(fn) or DirectoryExists(fn);
end;

function DeleteFileUTF8(const fn: string): Boolean;
begin
  Result := SysUtils.DeleteFile(fn);
end;

procedure CloseFileAttrs(const fn: string; date, attr: Int64);
{$IFDEF WINDOWS}
var
  h: THandle;
  ft: TFileTime;
begin
  if fn = '' then Exit;
  if (date > 0) or ((attr and 255) = Ord('w')) then
  begin
    h := CreateFileW(PWideChar(WidePath(fn)),
                     FILE_WRITE_ATTRIBUTES,
                     FILE_SHARE_READ or FILE_SHARE_WRITE or
                     FILE_SHARE_DELETE, nil, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS, 0);
    if h <> INVALID_HANDLE_VALUE then
    begin
      if date > 0 then
      begin
        ft := UnixToFileTime(UnixTime(date));
        SetFileTime(h, nil, nil, @ft);
      end;
      CloseHandle(h);
    end;
  end;
  { the attributes are stored as-are, like upstream's SetFileAttributes }
  if (attr and 255) = Ord('w') then
    SetFileAttributesW(PWideChar(WidePath(fn)), DWORD((attr shr 8) and $FFFF));
end;
{$ELSE}
var
  ut: TUTimBuf;
  tv: TTimeVal;
begin
  if fn = '' then Exit;
  if date > 0 then
  begin
    fpGetTimeOfDay(@tv, nil);
    ut.actime := tv.tv_sec;
    ut.modtime := UnixTime(date);
    fpUtime(PChar(fn), @ut);
  end;
  if (attr and 255) = Ord('u') then
    fpChmod(PChar(fn), attr shr 8);
end;
{$ENDIF}

procedure MakePath(const path: string; date: Int64 = 0; attr: Int64 = 0);
var
  i: Integer;
  p: string;
begin
  p := path;
  for i := 1 to Length(p) do
    if (p[i] = '/') or (p[i] = '\') then
    begin
      p[i] := #0;
      if p <> '' then CreateDir(p);
      p[i] := '/';
    end;
  p := path;
  if (p <> '') and (p[Length(p)] = '/') then SetLength(p, Length(p) - 1);
  CloseFileAttrs(p, date, attr);
end;

function Itos(x: Int64; n: Integer = 1): string;
begin
  Result := IntToStr(x);
  while Length(Result) < n do Result := '0' + Result;
end;

function CPUCount: Integer;
{$IFDEF WINDOWS}
var
  si: TSystemInfo;
begin
  GetSystemInfo(si);
  Result := Integer(si.dwNumberOfProcessors);
  if Result < 1 then Result := 1;
end;
{$ELSE}
var
  fs: TFileStream;
  buf: array[0..8191] of AnsiChar;
  n, i: Integer;
  prev: AnsiChar;
begin
  Result := 0;
  prev := #10;
  if FileExists('/proc/cpuinfo') then
  begin
    fs := TFileStream.Create('/proc/cpuinfo', fmOpenRead or fmShareDenyNone);
    try
      repeat
        n := fs.Read(buf, SizeOf(buf));
        for i := 0 to n - 1 do
        begin
          if (prev = #10) and (i + 8 < n) and
             (StrLComp(@buf[i], 'processor', 9) = 0) then
            Inc(Result);
          prev := buf[i];
        end;
      until n <= 0;
    finally
      fs.Free;
    end;
  end;
  if Result < 1 then Result := 1;
end;
{$ENDIF}

function SubPart(const fn: string; part: Integer): string;
var
  j, p: Integer;
begin
  Result := fn;
  p := part;
  for j := Length(Result) downto 1 do
  begin
    if Result[j] = '?' then
    begin
      Result[j] := Chr(Ord('0') + p mod 10);
      p := p div 10;
    end
    else if Result[j] = '*' then
    begin
      Result := Copy(Result, 1, j - 1) + IntToStr(p) + Copy(Result, j + 1, MaxInt);
      p := 0;
    end;
  end;
end;

function FileGetErrno: Integer;
{$IFDEF WINDOWS}
begin
  Result := Integer(GetLastError);
end;
{$ELSE}
begin
  Result := fpGetErrno;
end;
{$ENDIF}

function MissingFileMessage: string;
{$IFDEF WINDOWS}
begin
  Result := 'file not found';   { upstream's printerr on Windows }
end;
{$ELSE}
begin
  Result := 'No such file or directory';
end;
{$ENDIF}

function OpenRead(const path: string): TStream;
{$IFDEF WINDOWS}
var
  h: THandle;
begin
  Result := nil;
  h := CreateFileW(PWideChar(WidePath(path)), GENERIC_READ,
                   FILE_SHARE_READ or FILE_SHARE_WRITE or FILE_SHARE_DELETE,
                   nil, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if h = INVALID_HANDLE_VALUE then
  begin
    WriteLn(StdErr, path, ': ', SysErrorMessage(GetLastError));
    Exit;
  end;
  Result := THandleStream.Create(h);
end;
{$ELSE}
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
{$ENDIF}

function OpenWrite(const path: string; create: Boolean): TStream;
{$IFDEF WINDOWS}
var
  h: THandle;
  disp: DWORD;
begin
  Result := nil;
  if create then disp := CREATE_ALWAYS else disp := OPEN_EXISTING;
  h := CreateFileW(PWideChar(WidePath(path)), GENERIC_READ or GENERIC_WRITE,
                   FILE_SHARE_READ, nil, disp, FILE_ATTRIBUTE_NORMAL, 0);
  if h = INVALID_HANDLE_VALUE then
  begin
    WriteLn(StdErr, path, ': ', SysErrorMessage(GetLastError));
    Exit;
  end;
  Result := THandleStream.Create(h);
end;
{$ELSE}
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
{$ENDIF}

end.
