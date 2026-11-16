-- Numpad + and - change how fast player one runs, in steps of a quarter; Numpad * is the game's own pace.
-- The extra distance is added after the game has moved her, without its wall checks, so a big factor can
-- slip through thin walls. Scores stay off the leaderboards while any mod is on.

local function show()
  rex.screen_text(string.format("Run speed x%.2f", kameo.player.run_speed()), 2, 0xFFD070, 0, "speed")
end

rex.on("frame", function()
  local faster = rex.input.pressed("NumpadPlus")
  local slower = rex.input.pressed("NumpadMinus")
  local reset = rex.input.pressed("NumpadStar")
  if not rex.input.accepts() then return end
  local speed = kameo.player.run_speed()
  if faster then speed = math.min(speed + 0.25, 4) end
  if slower then speed = math.max(speed - 0.25, 0.25) end
  if reset then speed = 1 end
  if faster or slower or reset then
    kameo.player.set_run_speed(speed)
    show()
  end
end)
