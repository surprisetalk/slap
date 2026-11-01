-- title: shapes
-- script: lua
-- frames: 2
-- many slopes, fractions, clipped and flat shapes, for the reference diff
n = 0
function TIC()
  n = n + 1
  cls(n)
  for i = 0, 23 do
    line(120, 68, 120 + (i * 37 % 300) - 150, (i * 53 % 200) - 30, i % 15 + 1)
    line(i * 10 + 0.5, 3.7, 239 - i * 3.3, 133.2, 1 + i % 4)
  end
  line(-50, 20, 300, 30, 12)
  line(30, -40, 20, 400, 13)
  line(5, 5, 5, 5, 14)
  line(0, 0, 239, 135, 15)
  for i = 0, 11 do
    circ(i * 22 - 8, i * 13 - 5, i * 3 - 2, i + 1)
    circb(240 - i * 20, 20 + i * 9, i * 2, 15 - i)
  end
  circ(0, 0, 40, 6)
  circb(239, 135, 30, 7)
  circ(120, 68, -5, 8)
  for i = 0, 7 do
    tri(30 * i, 10 * i, 30 * i + 25, 8 * i + 40, 30 * i - 10, 50 + i * 6, i + 2)
    tri(100.5 + i, 100.25, 90, 120.75 - i, 130 + i * 4, 135.5, 9 + i % 6)
  end
  tri(-20, -20, 260, 10, 100, 200, 3)
  tri(10, 10, 12, 10.2, 11, 10.1, 4)
  rect(-5, -5, 30, 12, 5)
  rectb(230, 125, 30, 30, 6)
  rect(10, 120, 0, 5, 7)
  rectb(100, 60, 1, 1, 8)
  rectb(110, 60, 2, 2, 9)
  rect(3.9, 4.9, 6.9, 2.2, 10)
  pix(-1, 0, 5) pix(239, 135, 5) pix(240, 135, 6) pix(0, 136, 6)
  pix(10, 10.9, 31)
end
