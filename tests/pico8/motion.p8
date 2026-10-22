pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- frames: 40
-- _update60, scrolling print, camera, clip and palt edge cases
x, y, dx, dy, n = 10, 20, 3, 2, 0
function _init() srand(12345) end
function _update60()
  x += dx y += dy n += 1
  if x < 4 or x > 123 then dx = -dx end
  if y < 4 or y > 123 then dy = -dy end
end
function _draw()
  if n < 3 then cls() end
  circfill(x, y, 3, 8 + n % 8)
  circ(64, 64, 70, 5)
  line(-20, 30, 200, 31, 6)
  line(64, -10, 66, 140, 7)
  line(n, 0, n, 0, 9)
  print("line " .. n .. " " .. flr(rnd(1000)))
  camera(n, -n) rectfill(60, 60, 70, 70, 12) map(0, 0, 0, 0) camera()
  clip(-5, -5, 20, 20) rectfill(0, 0, 127, 127, 2) clip(120, 120, 50, 50, true) clip()
  palt(0x8000) spr(1, 100, 100) palt()
  sspr(8, 0, 8, 8, 110, 2, -8, 8) sspr(0, 0, 8, 8, 110, 12, 8, -8, true, true)
  rect(127, 127, 120, 120, 11)
  print(sqrt(n) .. " " .. cos(n / 40) .. " " .. atan2(n - 20, 7), 0, 120, 14)
end
__gfx__
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
