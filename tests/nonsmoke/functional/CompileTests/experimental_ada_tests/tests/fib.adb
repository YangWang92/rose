function Fib(x: in Integer) return Integer is
   Result : Integer;
begin
  if x = 0 then
    Result := 0;
  elsif x = 1 then
    Result := 1;
  else
    Result := Fib(x-2) + Fib(x-1);
  end if;

  return Result;
end Fib;
