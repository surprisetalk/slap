-- title: draw
-- script: lua
-- frames: 3
-- draws every primitive lua.slap has, for the reference diff
t = 0
function TIC()
  t = t + 1
  cls(1)
  rect(2, 2, 40, 20, 8)
  rectb(44, 2, 36, 18, 9)
  rect(90, 30, -4, -4, 3)
  rect(230, 130, 20, 20, 4)
  circ(20, 50, 10, 12)
  circb(50, 50, 12, 7)
  circ(70, 50, 0, 10)
  circb(90, 50, 1, 10)
  line(0, 135, 239, 60, 11)
  line(100, 10, 105, 50, 14)
  line(120, 5, 90, 8, 15)
  line(-10, -10, 20, 70, 6)
  for i = 0, 15 do pix(i, 90, i) end
  pix(300, 300, 5)
  tri(150, 100, 190, 110, 170, 130, 13)
  tri(200, 20, 230, 30, 210, 5, 2)
  tri(150, 20, 150, 40, 150, 30, 5)
  rect(100, 100, 3, 3, peek(0x3FC0) == 0x1a and 5 or 6)
  rect(104, 100, 3, 3, peek(0x3FC0 + 47) == 0x57 and 5 or 6)
  pix(0, 100, pix(1, 90))
  pix(2, 100, time() % 16)
  pix(3, 100, t)
end
