-- Expressions in the subset where pico8 and Lua 5.5 agree: integers whose
-- magnitude stays below 32768, and no / or ^, which give floats in Lua.
print(1 + 2 * 3, (1 + 2) * 3, 7 - 10, -7 - -2)
print(17 % 5, -17 % 5, 17 % -5, 0x10 + 0XfF, 1000 * 32)
print(1 < 2, 2 < 1, 2 <= 2, 3 >= 4, 5 > 4, 1 == 1, 1 ~= 1, "a" == "a", "a" ~= "b")
print("a" < "b", "ab" < "a", "" < "a", "abc" <= "abc", "b" > "abc")
print(nil == false, nil, true, false, not nil, not 0, not not "x")
print(1 and 2, nil and 2, false or "x", nil or false, 1 or error, false and nil)
print(1 .. 2, "x" .. 3 .. "y", "a" .. "b" .. "c" == "abc", -5 .. "")
print(#"hello", #"", #("a" .. "bc"), -#"abc")
print(12 & 10, 12 | 10, 12 ~ 10, 1 << 4, 256 >> 4, ~0 & 255)
print('single', "tab\tquote\"", 'nl\\n', "\65\066\x43", [[long
string]], [[
skips the first newline]])
print(2 + 3 .. "", 1 + 2 == 3 and "yes" or "no", 3 > 2 == true)
print()
print(((((((((((1)))))))))))
print("multi", "args", 3, nil, false)
print('a\
b', 'c\z
     d')
