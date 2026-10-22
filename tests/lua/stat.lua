-- Statements and functions, in the subset where pico8 and Lua 5.5 agree.
local a, b, c = 1, 2
print(a, b, c)
a, b = b, a
print(a, b)
local x = 10 do local x = 20 print(x) end print(x)
g = 5 g = g + 1 print(g)

local function fact(n) if n <= 1 then return 1 end return n * fact(n - 1) end
print(fact(7))
function fib(n) if n < 2 then return n else return fib(n - 1) + fib(n - 2) end end
print(fib(15))

local function two() return 1, 2 end
local function none() end
print(two())
print(two(), 10)
print((two()))
local p, q, r = two()
print(p, q, r)
local s, t = 0, two()
print(s, t)
print(none())
print(none(), 1)

local function sum3(a, b, c) return (a or 0) + (b or 0) + (c or 0) end
print(sum3(1), sum3(1, 2), sum3(1, 2, 3, 4), sum3(two()))

local n = 0
while n < 5 do n = n + 1 end
print(n)
repeat local m = n n = n - 2 until m < 3
print(n)
for i = 1, 3 do print(i) end
for i = 10, 1, -4 do print(i) end
for i = 1, 0 do print("never") end
local total = 0
for i = 1, 100 do if i % 2 == 0 then total = total + i end end
print(total)
for i = 1, 10 do if i > 3 then break end print("b", i) end
local k = 0
while true do k = k + 1 if k == 4 then break end end
print(k)
for i = 1, 3 do for j = 1, 3 do if j == 2 then break end print(i, j) end end

local function classify(v)
  if v < 0 then return "neg" elseif v == 0 then return "zero" elseif v < 10 then return "small" else return "big" end
end
print(classify(-1), classify(0), classify(5), classify(50))

local f = function(y) return y * 2 end
print(f(21))
local function apply(fn, v) return fn(v) end
print(apply(f, 4))
do local t = 1 end
local u
print(u)
;;; print("semicolons")
