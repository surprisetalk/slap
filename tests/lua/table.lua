-- Tables, in the subset where pico8 and Lua 5.5 agree. pairs runs in the
-- order keys were first set, so the tests print through arrays or sums.
local t = {10, 20, 30, x = 1, ["y z"] = 2, [5] = 50}
print(t[1], t[2], t[3], t.x, t["y z"], t[5], t[4], #"abc")
t[4] = 40
print(#t, t[4])
t.x = t.x + 1
t["x"] = t["x"] * 10
print(t.x)

local a = {}
for i = 1, 10 do a[#a + 1] = i * i end
print(#a, a[1], a[10])
a[10] = nil
print(#a)

local sum = 0
for i, v in ipairs(a) do sum = sum + i * v end
print(sum)

local keys = 0
local vals = 0
for k, v in pairs({a = 1, b = 2, c = 3, 4, 5}) do keys = keys + 1 vals = vals + v end
print(keys, vals)

local seen = 0
local u = {p = 1, q = 2, r = 3}
for k, v in pairs(u) do if k == "q" then u[k] = nil end seen = seen + 1 end
print(seen, u.p, u.q, u.r)
print(next({}), type(next({7})), next({7}))

local nested = {inner = {deep = {value = 42}}}
print(nested.inner.deep.value)
nested.inner.deep.value = 43
print(nested["inner"]["deep"].value)

local obj = {n = 0}
function obj.inc(self, d) self.n = self.n + d return self end
function obj:dec(d) self.n = self.n - d return self end
obj:inc(5):inc(2):dec(1)
print(obj.n)

local Point = {}
function Point.new(x, y) return {x = x, y = y, len2 = function(p) return p.x * p.x + p.y * p.y end} end
local p = Point.new(3, 4)
print(p:len2(), p.len2(p))

local function three() return 1, 2, 3 end
local packed = {three()}
local mid = {three(), three()}
local wrapped = {(three())}
print(#packed, #mid, #wrapped, mid[1], mid[2], mid[4])

local x, y = {}, {}
x.a, y.b, x.c = 1, 2, 3
print(x.a, y.b, x.c)
local m = {}
m[1], m[2] = m[2], 7
print(m[1], m[2])
print(type(t), type(print), type(nil), type(2), type("s"), type(true), type(function() end))
print(tostring(12), tostring(nil), tostring(false) .. "!")
local s = {}
s[s] = "self"
print(s[s], t == t, t == {}, {} == {})
local counts = {}
for _, w in ipairs({"a", "b", "a", "c", "a"}) do counts[w] = (counts[w] or 0) + 1 end
print(counts.a, counts.b, counts.c, counts.d)
