-- A Lua workload for the Lua interpreter benchmark: recursion, tables,
-- string building and closures.
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local t = {}
for i = 1, 200000 do t[i] = (i * 7919) % 1000 end
table.sort(t)
local parts = {}
for i = 1, 20000 do parts[#parts + 1] = tostring(t[i * 10]) end
local s = table.concat(parts, ",")
local acc = 0
local function adder(k) return function(x) return x + k end end
for i = 1, 300000 do acc = adder(i % 13)(acc) end
print(fib(30), #s, acc)
