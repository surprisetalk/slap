pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- frames: 3
-- printh, and the words that change state
function _init() srand(1) end
function _update60() end
function _draw()
  cls(0)
  n = (n or 0) + 1 printh(n .. " " .. tostr(n / 3))
  mset(1, 0, 2) sset(3, 3, 9) fset(1, 3, true) fset(2, 1)
  cursor(90, 2) print(sget(3, 3) .. sub("hello", 2, 3) .. chr(65) .. ord("A") .. tonum("12"))
  print(mget(1, 0) .. " " .. fget(1) .. " " .. fget(2), 2, 20, 7)
  pal({[1] = 8, [2] = 9}) rectfill(0, 30, 10, 40, 1) rectfill(12, 30, 20, 40, 2) pal()
  ?"question"
  map(0, 0, 40, 40, 4, 4)
end
