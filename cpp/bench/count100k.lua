-- Her thread kendi payını sayar; -t4 ile 4x25000 = 100000 istek sonra durur.
local n = 0
function response(status, headers, body)
  n = n + 1
  if n >= 25000 then wrk.thread:stop() end
end
