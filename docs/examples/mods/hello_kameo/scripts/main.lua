-- A first mod: copy the hello_kameo folder into the mods folder beside the game, start the game,
-- and press F5 for the mods menu.

-- Runs once, when the mod loads. print goes to the game's log.
print("Hello from " .. kameo.build())

-- Text in the top right corner, so you can see the mod is running as soon as the game draws.
local greeted = false
rex.on("frame", function()
  if greeted then return end
  greeted = true
  rex.screen_text("Hello Kameo is running. Press N in a level to widen the camera.", 10, 0xFFD070)
end)

-- The game tells mods when it changes level.
kameo.on("level_changed", function(name)
  rex.screen_text("Level: " .. name, 6, 0xFFD070)
end)

-- N widens the camera by 30 degrees and N again puts it back, through the game's own setting.
-- The setting applies to the gameplay camera, so the picture only changes inside a level.
local wide = false
rex.on("frame", function()
  local pressed = rex.input.pressed("N")       -- every frame, so no press is missed
  if not (pressed and rex.input.accepts()) then return end
  wide = not wide
  kameo.setting.set("kameo.camera_fov", wide and "30" or "0")
  rex.screen_text(wide and "Wide camera" or "Normal camera", 3)
end)
