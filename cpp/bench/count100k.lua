-- Each thread counts its own share; with -t4, 4x25000 = 100000 requests, then it stops.
local n = 0
function response(status, headers, body)
  n = n + 1
  if n >= 25000 then wrk.thread:stop() end
end
