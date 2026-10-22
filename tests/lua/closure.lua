-- Closures: shared cells, a fresh cell per loop pass, three levels deep.
local function counter()
  local n = 0
  return function() n = n + 1 return n end
end
local c1, c2 = counter(), counter()
print(c1(), c1(), c2(), c1())

local function pair()
  local v = 0
  local function get() return v end
  local function set(x) v = x end
  return get, set
end
local get, set = pair()
set(7) print(get()) set(9) print(get())

local f1, f2, f3
for i = 1, 3 do
  local function f() return i * 10 end
  if i == 1 then f1 = f elseif i == 2 then f2 = f else f3 = f end
end
print(f1(), f2(), f3())

local g1, g2
local k = 1
while k <= 2 do
  local captured = k
  if k == 1 then g1 = function() return captured end else g2 = function() return captured end end
  k = k + 1
end
print(g1(), g2())

local function outer(a)
  return function(b)
    return function(c) return a + b + c end
  end
end
print(outer(1)(20)(300))

local function adder(x) return function(y) x = x + y return x end end
local add = adder(100)
print(add(1), add(2))

local function deep()
  local x = 1
  local function mid()
    local function inner() x = x * 2 return x end
    return inner
  end
  local i = mid()
  i() i()
  return x
end
print(deep())

local outside = 5
local function reads() return outside end
outside = 6
print(reads())
