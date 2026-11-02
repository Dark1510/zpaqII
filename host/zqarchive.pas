{ zpaqII - archive file layer (multi-part, optional AES-256-CTR).
  Port of zpaq 7.15 (Unlicense, Matt Mahoney), unix side. }

unit zqarchive;
{$mode objfpc}{$H+}

interface

uses SysUtils, Classes, zqcore, zqfile;

type
  TInputArchive = class
  private
    fAes: TZqAes;
    fParts: array of Int64;
    fName: string;
    fOff: Int64;
    fStream: TFileStream;
  public
    constructor Create(const fn, password: string);
    destructor Destroy; override;
    function Read(buf: PAnsiChar; len: LongInt): LongInt;
    procedure Seek(p: Int64; whence: Integer);
    function Tell: Int64;
    function IsOpen: Boolean;
  end;

  TOutputArchive = class
  private
    fAes: TZqAes;
    fOff: Int64;
    fPtr: Integer;
    fBuf: array[0..65535] of AnsiChar;
    fStream: TFileStream;
  public
    constructor Create(const fn, password: string; const salt: PAnsiChar;
                       off: Int64);
    destructor Destroy; override;
    procedure Flush;
    procedure Put(c: LongInt);
    procedure WriteBuf(buf: PAnsiChar; len: LongInt);
    procedure Seek(p: Int64; whence: Integer);
    function Tell: Int64;
    procedure CloseArchive;
    function IsOpen: Boolean;
  end;

{ Reader/Writer records bound to an archive object (valid while it lives) }
function InputReader(a: TInputArchive): TZqReader;
function OutputWriter(a: TOutputArchive): TZqWriter;

implementation

function ZqInputGet(ctx: Pointer): LongInt; cdecl;
begin
  Result := -1;
end;

function ZqInputRead(ctx: Pointer; buf: PAnsiChar; n: LongInt): LongInt; cdecl;
begin
  Result := TInputArchive(ctx).Read(buf, n);
end;

procedure ZqOutputPut(ctx: Pointer; c: LongInt); cdecl;
begin
  TOutputArchive(ctx).Put(c);
end;

procedure ZqOutputWrite(ctx: Pointer; buf: PAnsiChar; n: LongInt); cdecl;
begin
  TOutputArchive(ctx).WriteBuf(buf, n);
end;

function InputReader(a: TInputArchive): TZqReader;
begin
  Result.get := @ZqInputGet;
  Result.read := @ZqInputRead;
  Result.ctx := Pointer(a);
end;

function OutputWriter(a: TOutputArchive): TZqWriter;
begin
  Result.put := @ZqOutputPut;
  Result.write := @ZqOutputWrite;
  Result.ctx := Pointer(a);
end;

{ ------------------------------------------------------------------ }
{ TInputArchive                                                     }
{ ------------------------------------------------------------------ }

constructor TInputArchive.Create(const fn, password: string);
var
  part0, parti: string;
  i: Integer;
  salt, key: array[0..31] of AnsiChar;
  n: LongInt;
begin
  inherited Create;
  fAes := nil;
  fStream := nil;
  fOff := 0;
  fName := fn;
  SetLength(fParts, 0);

  { sizes of all parts }
  part0 := SubPart(fn, 0);
  i := 1;
  while True do
  begin
    parti := SubPart(fn, i);
    if (i > 1) and (parti = part0) then Break;
    try
      fStream := TFileStream.Create(parti, fmOpenRead or fmShareDenyWrite);
    except
      fStream := nil;
      Break;
    end;
    SetLength(fParts, Length(fParts) + 1);
    fParts[High(fParts)] := fStream.Size;
    FreeAndNil(fStream);
    Inc(i);
  end;

  parti := SubPart(fn, 1);
  fStream := TFileStream.Create(parti, fmOpenRead or fmShareDenyWrite);

  if password <> '' then
  begin
    if fStream.Read(salt, 32) <> 32 then
      raise Exception.Create('cannot read salt');
    zq_stretch_key(@key, PAnsiChar(AnsiString(password)), @salt);
    fAes := zq_aes_new(@key, 32, @salt);
    fOff := 32;
  end;
end;

destructor TInputArchive.Destroy;
begin
  if fAes <> nil then zq_aes_free(fAes);
  FreeAndNil(fStream);
  inherited;
end;

function TInputArchive.IsOpen: Boolean;
begin
  Result := fStream <> nil;
end;

function TInputArchive.Tell: Int64;
begin
  Result := fOff;
end;

function TInputArchive.Read(buf: PAnsiChar; len: LongInt): LongInt;
var
  nr: LongInt;
begin
  if not IsOpen then Exit(0);
  nr := fStream.Read(buf^, len);
  if nr = 0 then
  begin
    Seek(0, 1);
    nr := fStream.Read(buf^, len);
  end;
  if nr = 0 then Exit(0);
  if fAes <> nil then zq_aes_encrypt(fAes, buf, nr, fOff);
  Inc(fOff, nr);
  Result := nr;
end;

procedure TInputArchive.Seek(p: Int64; whence: Integer);
var
  sum, newOff: Int64;
  i: Integer;
  nextPart: string;
begin
  if not IsOpen then Exit;
  case whence of
    0: newOff := p;            { soBeginning }
    1: newOff := fOff + p;     { soCurrent }
  else
    begin
      newOff := p;
      for i := 0 to High(fParts) do Inc(newOff, fParts[i]);
    end;
  end;
  fOff := newOff;

  if Length(fParts) = 1 then
  begin
    fStream.Seek(newOff, soBeginning);
    Exit;
  end;

  sum := 0;
  i := 0;
  while True do
  begin
    Inc(sum, fParts[i]);
    if (sum > newOff) or (i + 1 >= Length(fParts)) then Break;
    Inc(i);
  end;
  nextPart := SubPart(fName, i + 1);
  FreeAndNil(fStream);
  fStream := TFileStream.Create(nextPart, fmOpenRead or fmShareDenyWrite);
  fStream.Seek(newOff - sum, soEnd);
end;

{ ------------------------------------------------------------------ }
{ TOutputArchive                                                    }
{ ------------------------------------------------------------------ }

constructor TOutputArchive.Create(const fn, password: string;
  const salt: PAnsiChar; off: Int64);
var
  saltBuf: array[0..31] of AnsiChar;
  key: array[0..31] of AnsiChar;
  existed: Boolean;
  n: LongInt;
begin
  inherited Create;
  fAes := nil;
  fStream := nil;
  fPtr := 0;
  fOff := off;
  if fn = '' then Exit;

  FillChar(saltBuf, SizeOf(saltBuf), 0);
  existed := FileExists(fn);
  if existed then
  begin
    if off <> 0 then raise Exception.Create('file exists and off > 0');
    fStream := TFileStream.Create(fn, fmOpenReadWrite or fmShareDenyWrite);
    if password <> '' then
    begin
      if fStream.Read(saltBuf, 32) <> 32 then
        raise Exception.Create('cannot read salt');
      if (salt <> nil) and (not CompareMem(@saltBuf, salt, 32)) then
        raise Exception.Create('salt mismatch');
    end;
    fStream.Seek(0, soEnd);
  end
  else
  begin
    fStream := TFileStream.Create(fn, fmCreate);
    if password <> '' then
    begin
      if salt = nil then raise Exception.Create('salt not specified');
      Move(salt^, saltBuf, 32);
      if (off = 0) and (fStream.Write(saltBuf, 32) <> 32) then
        raise Exception.Create('write error');
    end;
  end;

  if password <> '' then
  begin
    zq_stretch_key(@key, PAnsiChar(AnsiString(password)), @saltBuf);
    fAes := zq_aes_new(@key, 32, @saltBuf);
  end;
end;

destructor TOutputArchive.Destroy;
begin
  CloseArchive;
  if fAes <> nil then zq_aes_free(fAes);
  inherited;
end;

function TOutputArchive.IsOpen: Boolean;
begin
  Result := fStream <> nil;
end;

procedure TOutputArchive.Flush;
begin
  if fStream = nil then Exit;
  if fPtr > 0 then
  begin
    if fAes <> nil then zq_aes_encrypt(fAes, @fBuf[0], fPtr, fStream.Position + fOff);
    fStream.Write(fBuf, fPtr);
    fPtr := 0;
  end;
end;

procedure TOutputArchive.Put(c: LongInt);
begin
  if fStream = nil then
  begin
    Inc(fOff);
    Exit;
  end;
  if fPtr >= SizeOf(fBuf) then Flush;
  fBuf[fPtr] := AnsiChar(c);
  Inc(fPtr);
end;

procedure TOutputArchive.WriteBuf(buf: PAnsiChar; len: LongInt);
var
  i: LongInt;
begin
  if fStream = nil then
  begin
    Inc(fOff, len);
    Exit;
  end;
  for i := 0 to len - 1 do
  begin
    if fPtr >= SizeOf(fBuf) then Flush;
    fBuf[fPtr] := buf[i];
    Inc(fPtr);
  end;
end;

procedure TOutputArchive.Seek(p: Int64; whence: Integer);
begin
  if fStream <> nil then
  begin
    Flush;
    case whence of
      0: fStream.Seek(p, soBeginning);
      1: fStream.Seek(p, soCurrent);
    else
      fStream.Seek(p, soEnd);
    end;
  end
  else if whence = 0 then
    fOff := p
  else
    Inc(fOff, p);
end;

function TOutputArchive.Tell: Int64;
begin
  if fStream <> nil then
    Result := fStream.Position + fPtr
  else
    Result := fOff;
end;

procedure TOutputArchive.CloseArchive;
begin
  if fStream <> nil then
  begin
    Flush;
    FreeAndNil(fStream);
  end;
end;

end.
