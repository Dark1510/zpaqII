{ zpaqII - file helpers and date/attribute conversion.
  Port of zpaq 7.15 (Unlicense, Matt Mahoney), unix side. }

unit zqfile;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes, BaseUnix, Unix;

{ 64-bit decimal YYYYMMDDHHMMSS from a unix time_t }
function DecimalTime(tt: Int64): Int64;
{ inverse of DecimalTime; -1 if date <= 0 }
function UnixTime(date: Int64): Int64;
{ current time in milliseconds }
function MTime: Int64;
{ "YYYY-MM-DD HH:MM:SS" or blanks }
function DateToString(date: Int64): string;
{ attribute string as shown by list (unix mode bits) }
function AttrToString(attrib: Int64): string;

{ mtime (decimal), size and unix attributes ('u'+mode<<8) of a file }
procedure FileInfo(const fn: string; out date, size, attr: Int64;
                   out isDir: Boolean);

function FileExistsUTF8(const fn: string): Boolean;
function DeleteFileUTF8(const fn: string): Boolean;
{ set mtime and permissions if date/attr are non-zero }
procedure CloseFileAttrs(const fn: string; date, attr: Int64);
{ create parent directories of path }
procedure MakePath(const path: string; date: Int64 = 0; attr: Int64 = 0);
{ non-negative decimal with at least n digits }
function Itos(x: Int64; n: Integer = 1): string;
{ replace ? and * in fn with digits of part }
function SubPart(const fn: string; part: Integer): string;
{ number of online CPUs (1 if it cannot be determined) }
function CPUCount: Integer;

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

function MTime: Int64;
var
  tv: TTimeVal;
begin
  fpGetTimeOfDay(@tv, nil);
  Result := Int64(tv.tv_sec) * 1000 + tv.tv_usec div 1000;
end;

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
begin
  Result := '     ';
  if (attrib and 255) = Ord('u') then
  begin
    Result[1] := types[((attrib shr 20) and 15) + 1];
    Result[5] := Chr(Ord('0') + (attrib shr (8 + 0)) mod 8);
    Result[4] := Chr(Ord('0') + (attrib shr (8 + 3)) mod 8);
    Result[3] := Chr(Ord('0') + (attrib shr (8 + 6)) mod 8);
    Result[2] := Chr(Ord('0') + (attrib shr (8 + 9)) mod 8);
  end;
end;

{ mtime (decimal), size and unix attributes ('u'+mode<<8) of a file }
procedure FileInfo(const fn: string; out date, size, attr: Int64;
  out isDir: Boolean);
var
  st: Stat;
begin
  date := 0;
  size := 0;
  attr := 0;
  isDir := False;
  if fpStat(fn, st) <> 0 then Exit;
  date := DecimalTime(st.st_mtime);
  size := st.st_size;
  attr := Ord('u') + (Int64(st.st_mode) shl 8);
  isDir := fpS_ISDIR(st.st_mode);
end;

function FileExistsUTF8(const fn: string): Boolean;
begin
  Result := FileExists(fn) or DirectoryExists(fn);
end;

function DeleteFileUTF8(const fn: string): Boolean;
begin
  Result := SysUtils.DeleteFile(fn);
end;

procedure CloseFileAttrs(const fn: string; date, attr: Int64);
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
      if p <> '' then fpMkdir(PChar(p), &777);
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

end.
