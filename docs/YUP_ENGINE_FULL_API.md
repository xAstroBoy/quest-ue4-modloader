# YUP Engine — Full API Reference (Pinball FX VR)

> Reverse-engineered from `libUnreal.so` (stripped) via bindump + SDK reflection dumps.  
> **Game**: Pinball FX VR (com.zenstudios.PFXVRQuest) — UE5.x on Quest 3

---

## Architecture Overview

Pinball FX VR uses **Zen Studios' YUP engine** — a custom C++ physics/game engine embedded inside Unreal Engine. The YUP engine handles:
- Ball physics simulation
- Table object scripting (ROM commands)
- Score/multiball/mission systems
- Flipper/bumper/ramp mechanics

### Two Control Layers

| Layer | Access | What It Controls |
|---|---|---|
| **UE Reflection** (Lua-accessible) | `FindFirstOf()`, `:Get()`, `:Set()`, `:Call()` | Game flow, input simulation, VR pawn, table state, cheats |
| **YUP ROM** (C++ internal only) | NOT callable from Lua | Ball physics, gravity, magnets, script commands |

The YUP ROM layer is **stripped** — function names removed from the binary. ROM commands are property definitions wired up at table init time through an internal vtable dispatch system. They are **not individually callable** from Lua.

---

## YUP Internal Architecture (C++ — Reference Only)

### ROM Object System
- Objects stored in a lookup table at `*(owner + 0xB8) + 0x60`
- `sub_4c16a78(table, &name, 1)` — lookup single object by name
- `sub_4c0a184(table, &name, &results)` — lookup objects by interface name
- Objects have vtable-dispatched property getters/setters

### ROM Script Command Registration (sub_4b9f84c)
During table init, commands are registered as **property descriptors**:
```c
// Pseudocode from decompilation
sub_4b8ca44("setGravityMultiplier", &name);    // string ref builder {ptr, len, flags=1}
sub_4b9f84c(&desc, id, &name, -1, &name2, &callback, &type);  // property descriptor
sub_4b911b0(&table, &desc);  // store in object's property table
```

### Key Internal Functions
| Address | Purpose |
|---|---|
| `sub_4b8ca44` | String reference builder: `*ret = str; ret[1] = strlen-1; *(ret+0xc) = 1` |
| `sub_4b9f84c` | Property descriptor builder (id, name, callback, type). Special-cases "GLVoid" → void |
| `sub_4b911b0` | Stores descriptor into object's property table |
| `sub_4c16a78` | ROM object lookup by name (single result) |
| `sub_4c0a184` | ROM object lookup by interface name (array result) |
| `sub_4c2d5e4` | String init variant (used for interface names) |

### Known ROM Command Strings (94 from .rodata)

<details><summary>Click to expand full list</summary>

**Ball Control**: setBallAttractBits, setBallSteering, setBallPhysObjBits, setBallSpeedLimit, setBallVisible, setBallLit, setBallScale, setBallLockState, launchBall, ballAiming, addBall, removeBall, destroyBall, exchangeBall, moveToKicker

**Physics**: setGravityMultiplier, setFlipperStrength, setPlungerStrength, setBumperStrength, setSlingshotStrength, setWindX, setWindY, setWindZ, setMagnet, setMagnetAttr

**Table Objects**: setGateState, setDiverterState, setBlockerState, setRampDiverter, setTargetState, setDropTargetState, setDropTargetGroupState, setRolloverState, setSpinnerState, setKickerState, setScoopState, setTrapState

**Scoring**: addScore, setScoreMultiplier, setJackpotValue, setBonusMultiplier, setEndOfBallBonus

**Game Flow**: startMultiball, endMultiball, setBallSaverTime, setBallSaverActive, setExtraBallActive, setShootAgainActive, setTimedMode, setMissionState, setMissionProgress, triggerEvent, setSequenceState

**Display**: setDMDText, setDMDAnimation, setInsertLight, setInsertLightFlash, setInsertLightGroup, setGIState, setFlasherState, playTableAnimation, setTableShake

**Audio**: playSound, stopSound, playMusic, stopMusic, setSoundVolume

**Visual**: setObjectVisible, setObjectMaterial, setObjectColor, setObjectEmissive, setMiniPlayfieldActive
</details>

### Known ROM Object Types (94 from ObjectSelector strings)

<details><summary>Click to expand full list</summary>

Ball, BallManager, BallGuideline, Bumper, Cabinet, Camera, Coil, Display, Display7Segment, DisplayAlphanumeric, DisplayDMD, DisplayLCD, DisplaySegment, Diverter, DropTarget, DropTargetBank, Emitter, Flasher, Flipper, Gate, GI, GIString, Guide, Impulse, InsertLight, InsertLightGroup, Kicker, Lane, LaneGuide, Layer, Light, LightGroup, Magnet, MiniPlayfield, Mission, Model, Motor, Nut, ObjectSelector, Overlay, Physics, Plunger, Primitive, Ramp, Relay, Rollover, Rubber, Scoop, Sequencer, Skill, Slingshot, SoundBank, Spinner, StandupTarget, Switch, Table, Target, Timer, Toy, Trap, Trigger, Trough, Tube, Wall, Wire, WireRamp

ROM Interfaces: BallManagerInterface, CameraInterface, DisplayInterface, FlipperInterface, KickerInterface, MissionInterface, PhysicsInterface, PlungerInterface, TableInterface, TroughInterface
</details>

> ⚠️ These are **internal C++ names**. They cannot be called from Lua. Listed for reference when building future C++ hooks.

---

## Lua-Accessible API (via UE Reflection)

Everything below is callable from Lua mods using `FindFirstOf()` + `:Call()`.

---

### PFXPlayTableInputHandler — Direct Table Input

**Access**: `FindFirstOf("PFXPlayTableInputHandler")`

This is the **most powerful** class for gameplay control — it lets you programmatically press flippers, launch balls, and nudge the table through the game's own input pipeline.

| Function | Args | Description |
|---|---|---|
| `LeftFlipperPressed()` | — | Simulate left flipper press |
| `LeftFlipperReleased()` | — | Simulate left flipper release |
| `RightFlipperPressed()` | — | Simulate right flipper press |
| `RightFlipperReleased()` | — | Simulate right flipper release |
| `DigitalLaunchPressed()` | — | Simulate plunger pull (digital) |
| `DigitalLaunchReleased()` | — | Simulate plunger release (digital) |
| `DigitalLaunchSpecialPressed()` | — | Special launch press |
| `DigitalLaunchSpecialReleased()` | — | Special launch release |
| `OnAnalogueLaunchPressure(Pressure)` | float 0.0–1.0 | Analogue plunger pressure |
| `OnAnalogueLaunchReleased()` | — | Analogue plunger release |
| `OnNudgeGesture(AxisX, AxisY)` | float, float | Nudge table (±1.0 range) |
| `OnNudgeAxisXChanged(AxisX)` | float | Nudge X axis only |
| `OnNudgeAxisYChanged(AxisY)` | float | Nudge Y axis only |
| `OnNudgeAxis2DChanged(AxisX, AxisY)` | float, float | Nudge both axes |
| `IsPlungerActive()` | — | Returns bool |
| `OnUsePowerupPressed()` | — | Activate powerup |
| `OnUsePowerupReleased()` | — | Release powerup |
| `OnVisualExtrasPressed()` | — | Visual extras toggle on |
| `OnVisualExtrasReleased()` | — | Visual extras toggle off |

> **KEY INSIGHT for Finger Mode**: `OnNudgeGesture()` tilts the table through real physics. Map hand position deltas to nudge values to create physics-based finger interaction without fighting YUP's ball position control.

---

### PFXGameflowObject_PlayTable (GFO_PlayTable_C) — Game State Control

**Access**: `FindFirstOf("GFO_PlayTable_C")`

| Function | Args | Description |
|---|---|---|
| `PauseGame()` | — | Pause pinball game |
| `ResumeGame()` | — | Resume pinball game |
| `RestartGame()` | — | Restart current game |
| `ExitGame()` | — | Exit to hub |
| `IsPaused()` | — | Returns bool |
| `IsPreparingGameEnd()` | — | Returns bool |
| `SkipPrepareGameEnd()` | — | Skip end animation |
| `SetTableGuideActive(Active)` | bool | Toggle table guide overlay |
| `SetEnvironmentVisibility(bIsVisible, bForce)` | bool, bool | Show/hide environment |
| `OnGameStateChanged(State)` | enum | Force game state change |
| `GetGameInfo()` | — | Returns PFXPinballGameInfo |
| `GetEnvironmentLevelActors()` | — | Returns actor array |

---

### PFXVRPawn (BP_VR_Pawn_C) — VR Body/Hand Control

**Access**: `FindFirstOf("BP_VR_Pawn_C")` or `FindFirstOf("PFXVRPawn")`

49 functions total. Key ones:

| Function | Description |
|---|---|
| `GetHandRight()` / `GetHandLeft()` | Get hand components |
| `GetShoulderRight()` / `GetShoulderLeft()` | Get shoulder components |
| `GetGrabSocketTransform_Right()` / `_Left()` | Grab socket transforms |
| `SetNudgeGestureActive(MC, IsActive)` | Enable/disable nudge on controller |
| `OnNudgeGestureStart(MC)` / `OnNudgeGestureEnd(MC)` | Nudge gesture lifecycle |
| `UpdateGrabMode(IsGrabbing, heldComp, isRight)` | Update grab state |
| `SetFixedGrabTarget(Actor)` / `UnsetFixedGrabTarget(Actor)` | Lock/unlock grab targets |
| `Blink(middleEvent, BlinkTime, holdTime)` | Screen blink (fade out/in) |
| `SetFadeEnabled(Enabled)` / `SetFadeColor(InColor)` | VR fade control |
| `EnableActions()` / `DisableActions()` | Enable/disable all player input |
| `SetControlHintVisibility(IsVisible)` | Show/hide VR control hints |
| `SetLoadingWidgetActive(bIsActive)` | Loading overlay |
| `SetXArcadeDockingModeEnabled(Enabled)` | Arcade cabinet docking mode |
| `SetCabinetAlignmentEnabled(Enabled)` | Cabinet alignment mode |
| `HideAllActors(excludeActors, outHidden)` | Hide all actors |
| `ShowActors(actorsToShow)` | Show specific actors |
| `BP_OnMinigameStart()` / `BP_OnMinigameEnd()` | Minigame lifecycle |
| `BP_OnEnterTableSlot(SlotActor)` / `BP_OnLeaveTableSlot()` | Table slot events |

---

### PFXTableReference — Table State Queries

**Access**: `FindFirstOf("PFXTableReference")`

| Function | Returns | Description |
|---|---|---|
| `IsTableStarted()` | bool | Table loaded and init complete |
| `IsTableLoaded()` | bool | Level loaded |
| `IsTableLoadedAndVisible()` | bool | Loaded + rendered |
| `IsTableGameRunning()` | bool | Active game in progress |
| `IsTableGameOver()` | bool | Game ended |
| `IsPlungerActive()` | bool | Plunger ready |
| `IsTableStartButtonActive()` | bool | Start button available |
| `IsTableIsInActiveGameState()` | bool | Any active state |
| `GetPfxGameHandler()` | obj | Get PFX game handler |
| `GetVRTableInfo()` | obj | Get VR table metadata |
| `FindActorInTableLevel(tagOrName)` | actor | Find actor by tag/space name |
| `FindActorsInTableLevelThatNeedToBeAttached()` | array | Attachment candidates |

---

### PFXGameflowManager — Table Launch & Loading

**Access**: `FindFirstOf("PFXGameflowManager")`

| Function | Description |
|---|---|
| `StartTable_Classic(tableRef)` | Start classic mode |
| `StartTable_Practice(tableRef)` | Start practice mode |
| `StartTable_Trial(tableRef)` | Start trial mode |
| `StartTable_Championship(Match, tableRef)` | Start championship match |
| `StartTable(GameInfo, tableRef)` | Generic start with custom GameInfo |
| `StartGameflowState(State, Environment)` | Direct gameflow state change |
| `IsMixedReality(WorldContext)` | Check MR mode |
| `GetEnvironment()` | Get current environment enum |
| `BP_ShowLoadingScreen()` | Show loading screen |
| `BP_HideLoadingScreen()` | Hide loading screen |
| `BP_IsLoadingScreenActivated()` | Is loading visible |
| `GetVirtualRealityLevels()` | VR level list |
| `GetMixedRealityLevels()` | MR level list |

---

### PFXPinballGameInfo — Game Mode Queries

**Access**: via `GameHandler:Get("m_GameInfo")` or `GFO_PlayTable:GetGameInfo()`

| Function | Returns | Description |
|---|---|---|
| `IsProMode()` | bool | Pro difficulty active |
| `IsChampionshipMode()` | bool | Championship active |
| `IsXPAllowed()` | bool | XP tracking enabled |
| `IsPerkProgressAllowed()` | bool | Perk progress enabled |
| `IsMasteryProgressAllowed()` | bool | Mastery progress enabled |
| `GetGamePlayMode()` | enum | Current play mode |
| `GetGameEndType()` | enum | How game ended |
| `GetGameEndParams()` | struct | End parameters |
| `SetFlag_bp(iFlag)` | — | Set game flag (inherited from YUP) |
| `RemoveFlag_bp(iFlag)` | — | Remove game flag |
| `GetFlag_bp(iFlag)` | bool | Check game flag |

---

### YUP Base Classes (Reflected)

#### YUPGameSubsystem
| Function | Description |
|---|---|
| `IsPlayingOnTable()` | Is a table game active |
| `GetTableInfoHolder()` | Get table info holder |
| `GetTableReference()` | Get active table reference |
| `GetGameflowManager()` | Get gameflow manager |

#### YUPPinballGameInfo
| Function | Description |
|---|---|
| `SetFlag_bp(iFlag)` / `RemoveFlag_bp(iFlag)` / `GetFlag_bp(iFlag)` | Game flags |
| `IsGameSaveAllowed()` / `IsAutoSaveAllowed()` | Save permissions |
| `GetTableCtxID()` | Table context ID |

#### YUPTableReference
| Function | Description |
|---|---|
| `LoadAndStartTable_BP(GameInfo)` | Load + start table |
| `InitializeTable(GameInfo)` | Initialize without starting |
| `IsLoading()` | Currently loading |
| `UnloadTable()` | Unload current table |

#### YUPTableInfoHolder
| Function | Description |
|---|---|
| `GetTablesNum()` | Total table count |
| `GetTableInfoByIndex(idx)` | Get table info by index |

---

### PFXCheatManager / BP_CheatManager_C — Debug Cheats

See `PFX_NativeCheats` mod for complete list (~90+ actions). Categories:
- **Ball**: AddBall, RemoveBall, MultiballStart/End
- **Score**: AddScore, BigScore, SetMultiplier
- **Speed**: SloMo, TickRate
- **Physics**: GravityMultiplier, FlipperStrength
- **Game Flow**: RestartBall, EndGame, SkipMission
- **Debug**: ShowCollision, ShowPhysics, ToggleWireframe
- **Table-specific**: Per-table cheat functions

---

### Other Significant Classes (50+)

| Class | Functions | Key Capabilities |
|---|---|---|
| `PFXChampionshipManager` | 50 | Tournament management, matchmaking, leaderboards |
| `PFXMasteryManager` | 34 | Table mastery progression, XP, levels |
| `PFXPerkManager` | 28 | Perk equip/unequip/upgrade |
| `PFXRewardWheelManager` | 30 | Reward wheel spins, prizes |
| `PFXCollectiblesManager` | 16 | Collectible unlock/equip/swap |
| `PFXAchievementsManager` | 24 | Achievement tracking |
| `PFXPlayerSettingsManager` | 20 | Player preferences |
| `PFXUIManager` | 22 | UI screen navigation |
| `PFXGameAudioManager` | 16 | Audio/music control |
| `PFXCabinetInterface` | 27 | Virtual cabinet positioning/rotation |
| `PFXGrabComponent` | 14 | VR grab mechanics |
| `PFXGrabPawnInterface` | 13 | Grab pawn integration |
| `PFXInteractableInterface` | 15 | Object interaction |
| `PFXInteractionPawnInterface` | 20 | Pawn interaction system |
| `PFXMRLayoutManager` | 22 | Mixed reality room layout |
| `PFXSaveLoadManager` | ~15 | Save/load profiles |
| `PFXGameHandler` | 8 | UpdateMinigameCam, SetEnvironmentVisible, IsInMinigame |

---

## Finger Mode Strategy

### The Problem
Moving the ball directly via `Set("Position", ...)` doesn't work — YUP's physics loop overwrites ball position every simulation tick.

### The Solution: Physics-Based Nudge
Use `PFXPlayTableInputHandler:OnNudgeGesture(AxisX, AxisY)` to **nudge the table** based on hand tracking position:

```lua
-- Map hand position delta to nudge values
local dx = hand_x - last_hand_x
local dy = hand_y - last_hand_y
local ih = FindFirstOf("PFXPlayTableInputHandler")
ih:Call("OnNudgeGesture", dx * scale, dy * scale)
```

This makes the ball respond to hand movement through **real table physics** — tilting the table causes the ball to roll naturally, including tilt warnings. The YUP engine processes the nudge as a real physical force.

### Alternative: Flipper Automation
```lua
-- Auto-flip when ball is near flipper zone
local ih = FindFirstOf("PFXPlayTableInputHandler")
ih:Call("LeftFlipperPressed")
-- small delay
ih:Call("LeftFlipperReleased")
```

---

## Future Work: C++ Hooks for Direct YUP ROM Access

To unlock the full 94 ROM commands from Lua, the C++ modloader would need to:

1. **Hook `sub_4b9f84c`** (property descriptor builder) during table init to capture callback function pointers
2. **Build a registry** mapping command name → native callback address
3. **Expose `CallROMCommand(name, value)`** in the Lua API that invokes the captured callback directly
4. **OR hook `sub_4c16a78`** (ROM object lookup) to intercept and cache ROM object pointers, then call through their vtable

This would unlock direct control of: gravity, magnets, ball steering, flipper strength, individual table objects, lighting, DMD text, sound, and all 94 ROM script commands.

### Hook Targets (for C++ implementation)
```
sub_4b9f84c  — property descriptor builder (capture name→callback mapping)
sub_4b911b0  — property table insert (capture all descriptors)
sub_4c16a78  — ROM object lookup by name (cache object pointers)
sub_4c0a184  — ROM object lookup by interface (cache interface groups)
```
