# YUP ENGINE REVERSAL — PROGRESS LOG

## STEP 0 RESULT: PAK EXTRACTION
- **Found**: 39 PAKs (10 main + 3 overflow + 26 EXTRA)
- **Extracted to**: `PAKS_extracted/Pinball FX Paks/EXTRACTED/`
- **AES Key**: `0x5669BFFC68574E494139A87152C54F2D8A93A5FC05498E4CBD44FCE023E05326`
- **Errors**: Zero
- **Binary in PAKs?**: NO — libUnreal.so is in APK, indexed in BDMP
- **Cleanup**: Original .pak files NOT yet deleted

## STEP 1 RESULT: BINARY IDENTIFIED
- **Binary**: `libUnreal.so` (ARM64, stripped, no .symtab)
- **BDMP database**: `Pinball FX VR Patches/bindump.bdmp` (7.28 GB, 263K functions)
- **Exports**: ~1156 `.dynsym` symbols (63 JNI exports) — everything else stripped
- **BINJA base offset**: 0x400000

## STEP 2 RESULT: YUP STRING DUMP — COMPLETE ✅

### Architecture Discovery

The YUP engine uses a **string-keyed property binding system**:
- Components are named classes (e.g., `"PendulumBallConstraint"`, `"BallInformator"`)
- Properties are registered with string names + type tags + callbacks
- Actions (RPC-style) are registered with string names + return type + handler functions
- Central function `YUP_MakePropertyName` (0x4B8CA44) creates property name objects from strings

### Core Framework Functions

| Address | Alias | Purpose |
|---------|-------|---------|
| 0x4B8CA44 | `YUP_MakePropertyName` | Create property name from string key |
| 0x4B8C80C | `YUP_ResetComponent` | Reset component property bindings |
| 0x4B8D29C | `YUP_AddPropertyToComponent` | Add a property binding to component |
| 0x4B8D6D0 | `YUP_FinalizeComponent` | Finalize component after all bindings |
| 0x4B911B0 | `YUP_AddActionToComponent` | Add an action (callable) to component |
| 0x4B9F6F0 | `YUP_RegisterPropertyBinding` | Register property with callback + type |
| 0x4B9F84C | `YUP_RegisterActionBinding` | Register action with callback + return type |

### PendulumBallConstraint Component (0x4C9F160)

Physics constraint binding that controls ball attachment to pendulums/captured states.

| Property/Action | Type | Handler | Purpose |
|----------------|------|---------|---------|
| `"Attached"` | property | 0x4BC1510 | Is ball attached? |
| `"geometry"` | property | 0x4C8FC30 | Constraint geometry |
| `"ballCenter"` | property | 0x2301CF4 | Ball center position in constraint |
| `"minLength"` | property | 0x4BB6678 | Min constraint length |
| `"maxLength"` | property | 0x4C96078 | Max constraint length |
| `"attach"` | action (Px::s64) | 0x4C9FADC | **ATTACH ball to constraint** |
| `"detach"` | action (Px::s64) | 0x4C9FBD4 | **DETACH ball from constraint** |
| `"reset"` | action (void) | 0x4BC1528 | Reset constraint |

### BallInformator Component (0x4CB651C)

Provides ball speed information (read-only query interface).

| Property/Action | Type | Handler | Purpose |
|----------------|------|---------|---------|
| `"getSpeed"` | action (Px::s64) | 0x4CB67B0 | Get current ball speed |
| `"ballSpeed"` | float property | — | Ball speed value (float) |

### Ball-Related Strings in Binary (filtered, non-enum)

| String | Offset | Relevance |
|--------|--------|-----------|
| `ballCenter` | 0x44A1DF | Ball center position in constraint |
| `ballSpeed` | 0x4F772A | Ball speed (float) |
| `ball_idx` | 0x3F2960 | Ball index (internal ID) |
| `ballIdx` | 0x3F29AC | Ball index (alternate) |
| `Contact2BallIdx` | 0x3F29B4 | Contact pair ball index |
| `BallIndex` | 0x3F1B6F | Ball index lookup |
| `BallIndexAddress` | 0x420CA2 | Ball index memory address key |
| `BallCollisionParameters` | 0x425979 | Collision tuning params |
| `setBallCollisionNudgeLimit` | 0x40E23E | Collision nudge limit setter |
| `ballsContactCalculation` | 0x46C060 | Contact physics calc |
| `AttachedBallPosition` | 0x46512D | Attached ball position |
| `PendulumBallConstraint` | 0x403133 | Pendulum constraint name |
| `BallInformator` | 0x4437F3 | Ball info component name |
| `debugBallReset` | 0x4131FC | Debug ball reset function |
| `ballLaunchedManualy` | 0x3E8F80 | Manual launch flag |
| `ballSaverStatus` | 0x41A606 | Ball saver active status |
| `ballAttractBits` | 0x41EA47 | Ball attract magnet bitmask |
| `attachOrientationToBall_velocity` | 0x3E42EC | Camera track ball velocity |
| `attachOrientationToBall_lookAt` | 0x4194FE | Camera look-at ball |
| `attachOrientationToBall_horizontalLookAt` | 0x419536 | Horizontal cam track |
| `ballHouse` | 0x4AB07E | Ball drain/home position |
| `ballInside` | 0x4D9710 | Ball is inside (something) |
| `ballPrepared` | 0x4F065B | Ball ready for launch |
| `ballCaptured` | 0x4EF340 | Ball captured by mechanism |
| `ballSaved` | 0x4EAA4B | Ball save triggered |
| `ballLaunched` | 0x4F5FD1 | Ball launched event |
| `balloutFinished` | 0x4F593F | Ball drain complete |
| `multiballStart` | 0x3FE53A | Multiball event start |
| `multiballEnd` | 0x4E5030 | Multiball event end |
| `FlippersSticky` | 0x3E9010 | Sticky flippers flag |
| `IsPlungerActive` | 0x4A00E7 | Plunger active state |
| `PlungerLauncher` | 0x44EF09 | Plunger mechanism |
| `RightFlipperPressed` | 0x4EE698 | Right flipper input event |
| `LeftFlipperPressed` | 0x4EE6AC | Left flipper input event |

### Physics Type System

The YUP property system supports typed properties:
- `"float"` — 32-bit float
- `"Px::s64"` — 64-bit signed (for actions/RPCs with ball index args)
- `"void"` — no return (for fire-and-forget actions like reset)

### Source Paths Leaked from SVN

```
C:/svn/YUP_PinballFX_VR/game/Source/YUPCore/Source/CGLide/WPC/Emulator2/  (WPC ROM emulation)
C:/svn/YUP_PinballFX_VR/game/Source/PinballFX_VR/                         (Game-specific code)
C:/svn/YUP_PinballFX_VR/game/Source/UEPinballFramework/Source/             (UE bridge framework)
C:/svn/YUP_PinballFX_VR/game/Source/ZOperation/Source/                     (Operation/process system)
C:/svn/YUP_PinballFX_VR/game/Source/ZGameflow/Public/                      (Gameflow manager)
C:/svn/YUP_engine/UE5_3_2_MetaFork/                                        (Custom UE5 fork)
```

### SDK Classes (UE Reflection — Visual Only)

| Class | Role |
|-------|------|
| `ball_C` (StaticMeshActor) | Visual ball actor — meshes + particles only |
| `ball_mini_C` (StaticMeshActor) | Mini ball variant |
| `PXCollidableComponent` | Native collision shape (EPXCollidableMaterial) |
| `PXSensorComponent` / `PXPolySensorComponent` / `PXSphereSensorComponent` | Physics sensors |
| `PXAnimationComponent` | Physics-relevant animation |
| `PXSplineComponent` | Spline with physics material |
| `YUPTableInfo` | Table config: `ForceNewPhysics`, `NudgeStrength`, `BallIndexAddress` |

### Key Insight: Physics Architecture

The ball physics is **completely native C++** — NOT exposed via UE reflection.

**Visual path (UE side)**: `ball_C` StaticMeshActor with cosmetic meshes/VFX
**Physics path (native side)**: YUP engine property system with `BallInformator`, `PendulumBallConstraint`

The sync between physics → visuals happens in native code. The `ball_C` actor's transform is SET by the engine each tick — the visual actor just follows.

---

## STEP 2B RESULT: HIDDEN DEBUG/CHEAT SYSTEMS — COMPLETE ✅

### 🔥 TableDebugInterface — Zen Studios' Built-In Cheat Console

**Function**: `YUP_TableDebugInterface_Init` at **0x492CAD0** (IDA) / **0x4D2CAD0** (BINJA)
**Component Name**: `"TableDebugInterface"`

This is a YUP component that registers **ALL** of these actions via `YUP_RegisterActionBinding`:

| Action Name | Type | Handler Address | Purpose |
|-------------|------|-----------------|---------|
| `"dump"` | void | 0x4D2D4BC | Dump table debug info |
| `"setPause"` | bool | 0x4D2D574 | **Pause/unpause physics** |
| `"setGodMode"` | bool | 0x4D2D584 | **Enable/disable god mode** |
| `"pauseToggle"` | void | 0x4D2D594 | Toggle pause state |
| `"toggleGodMode"` | void | 0x4D2D5B8 | Toggle god mode |
| `"setSpeed"` | float | 0x4D2D5CC | **Set physics speed multiplier** |
| `"debugSave"` | void | 0x4D2D5F0 | Save debug state |
| `"debugLoad"` | void | 0x4D2D5F4 | Load debug state |
| `"reloadGL"` | void | (JNI reuse) | Reload game logic |
| `"reloadGL_and_LoadLastSave"` | void | (JNI reuse) | Reload + load save |
| `"GOD_SaveTheBall"` | Px::s64 | (property) | Ball save handler (god mode) |
| `"GOD_NotSaveTheBall"` | Px::s64 | (property) | Ball NOT saved (god mode off) |

**Architecture**: The init function stores global IDs at `data_783c5a8` (GOD_SaveTheBall) and `data_783c5ac` (GOD_NotSaveTheBall) — these are property IDs used to intercept ball drain.

### God Mode Deep Dive

Additional god mode strings found:
| String | Address | Purpose |
|--------|---------|---------|
| `"DEBUG_godMode_scale"` | 0x4CDCC7 | God mode visual scale factor |
| `"godModeSaveChance"` | 0x4DE64D | Probability of ball save in god mode |
| `"setGodMode"` | 0x4D8F0A | YUP action: enable god mode (bool) |
| `"toggleGodMode"` | 0x4D8F15 | YUP action: toggle god mode |
| `"GOD_SaveTheBall"` | 0x47D698 | God mode ball save property |
| `"GOD_NotSaveTheBall"` | 0x47D685 | God mode ball NOT saved property |

### PFXDebug — Blueprint Debug Functions (UE Reflection Layer)

These are UFunction names registered in UE reflection — **callable via our modloader's Call() API**:

#### Table & Content Unlocking
| Function Name | Purpose |
|---------------|---------|
| `PFXDebug_UserInventory_UnLockAllTables` | **Unlock ALL pinball tables** |
| `PFXDebug_UserInventory_ReLockAllTables` | Re-lock all tables |
| `PFXDebug_TableAwardUnlockAll` | Unlock all table awards |
| `PFXDebug_TableAwardLockAll` | Lock all table awards |
| `PFXDebug_TableAwardUnlock` | Unlock specific table award |
| `PFXDebug_TableAwardUnlockTable` | Unlock awards for specific table |
| `PFXDebug_TableAwardLock` | Lock specific table award |
| `PFXDebug_TableAwardLockTable` | Lock awards for specific table |
| `IsContentSubmissionBuild_UnlockAllTables` | Content submission build flag |
| `IsContentSubmissionBuild_UnlockAllCustomization` | Content submission build flag |
| `bDebugUnlockAllTables` | Boolean: unlock all tables |

#### Collectibles System
| Function Name | Purpose |
|---------------|---------|
| `PFXDebug_Collectibles_UnlockAll` | **Unlock ALL collectibles** |
| `PFXDebug_Collectibles_LockAll` | Lock all collectibles |
| `PFXDebug_Collectibles_UnlockEntry` | Unlock specific collectible |
| `PFXDebug_Collectibles_LockEntry` | Lock specific collectible |
| `PFXDebug_Collectibles_UnlockAllEntriesInBundle` | Unlock all in bundle |
| `PFXDebug_Collectibles_LockAllEntriesInBundle` | Lock all in bundle |

#### Achievements & Progression
| Function Name | Purpose |
|---------------|---------|
| `PFXDebug_Achievement_UnlockAll` | **Unlock ALL achievements** |
| `PFXDebug_Achievement_Unlock` | Unlock specific achievement |
| `PFXDebug_Achievement_SetProgress` | Set achievement progress |
| `PFXDebug_Achievement_AddProgress` | Add to achievement progress |
| `PFXDebug_TableMasteryMaxAll` | Max all table mastery |
| `PFXDebug_TableMasteryMaxTable` | Max specific table mastery |
| `PFXDebug_TableMasteryLevelSet` | Set mastery level |
| `PFXDebug_TableMasteryResetAll` | Reset all mastery |
| `PFXDebug_TableMasteryResetTable` | Reset specific table mastery |
| `PFXDebug_TablePerkMaxAll` | Max all table perks |
| `PFXDebug_TablePerkMaxTable` | Max specific table perks |
| `PFXDebug_TablePerkLevelSet` | Set perk level |
| `PFXDebug_TablePerkResetAll` | Reset all perks |
| `PFXDebug_TablePerkResetTable` | Reset specific table perks |

#### Championship System
| Function Name | Purpose |
|---------------|---------|
| `PFXDebug_Championship_Enable` | Enable championship mode |
| `PFXDebug_Championship_MaxAll` | Max all championship |
| `PFXDebug_Championship_MaxLeague` | Max league level |
| `PFXDebug_Championship_MasteryMaxAll` | Max championship mastery |
| `PFXDebug_Championship_MasteryLevelSet` | Set championship mastery level |
| `PFXDebug_Championship_MasteryResetAll` | Reset championship mastery |
| `PFXDebug_Championship_SetDebugMatch` | Set debug match |
| `PFXDebug_Championship_BenefitGrant` | Grant championship benefit |
| `PFXDebug_Championship_BenefitEffectCalculator` | Debug benefit calculator |
| `PFXDebug_Championship_MissionReward` | Debug mission reward |

#### Miscellaneous
| Function Name | Purpose |
|---------------|---------|
| `PFXDebug_RewardWheel_AddSpin` | Add free reward wheel spins |
| `PFXDebug_Onboarding_Reset` | Reset onboarding/tutorial |
| `PFXDebug_Onboarding_ShowHintPanelByIndex` | Show hint panel |
| `PFXDebug_Onboarding_ShowHintPanelByName` | Show hint panel by name |
| `PFXDebug_PlayerProfile_Reset` | **Reset entire player profile** |
| `PFXDebug_Audio_TestTone` | Play audio test tone |
| `PFXDebug_Audio_SetMusicCutoffFrequency` | Set music cutoff freq |
| `PFXDebug_Performance_SetTargetDisplayFrequency` | Set display frequency |
| `PFXDebug_Performance_ActivateNiagaraEffects` | Toggle Niagara VFX |
| `PFXDebug_Performance_TableIsolation` | Isolate table for perf testing |
| `PFXDebug_Sequence_SetPlaybackPosition` | Set sequence playback position |
| `PFXDebug_Pawn_ToggleCapsuleVisibility` | Show/hide pawn capsule |
| `PFXDebug_Pawn_SetCameraFade` | Set camera fade |

### Debug Key System

| String | Address | Purpose |
|--------|---------|---------|
| `EPFXDebugKeyListenerMode` | 0x4D70CC | Enum for debug key mode |
| `EPFXDebugKeyListenerMode::DontTriggerInDebugMode` | 0x4D8130 | Skip in debug mode |
| `EPFXDebugKeyListenerMode::TriggerInDebugMode` | 0x4D8161 | Only trigger in debug mode |
| `overideOtherDebugKeys` | 0x419918 | Override other debug key handlers |
| `InputDebugKeyDelegateBindings` | 0x431433 | Debug key input bindings array |
| `bDisableDebugSwitches` | 0x43888F | Master switch to disable debug |
| `FlipperSwitches` | 0x43887F | Flipper switch bindings |

### UE Cheat Manager (Standard UE)

| String | Address | Purpose |
|--------|---------|---------|
| `EnableCheats` | 0x4202D0 | Enable UE cheat system |
| `CheatScript` | 0x3FEEBC | Execute cheat script |
| `CheatClass` | 0x422B0C | Cheat manager class |
| `CheatManagerExtensions` | 0x429AB4 | Cheat manager extensions |
| `ClientCheatFly` | 0x3E85D4 | Client-side fly mode |
| `ClientCheatGhost` | 0x3FAA39 | Client-side ghost/noclip |
| `ClientCheatWalk` | 0x485DF1 | Client-side walk (disable fly) |
| `bCheatFlying` | 0x49195A | Is cheat flying active? |

### Ball Control YUP Actions (Game-Specific)

| String | Address | Purpose |
|--------|---------|---------|
| `debugBallReset` | 0x4131FC | Reset ball to start position |
| `setBallsNumberOfBallout` | 0x3F9434 | Set ball-out count |
| `setBallCollisionNudgeLimit` | 0x40E23E | Set collision nudge limit |
| `setBallAttractBits` | 0x41EA57 | Set ball attract magnet bits |
| `setBallsContactCalculation` | 0x46C078 | Set contact physics calc mode |
| `setBallPerGame` | 0x4C7295 | Set balls per game |
| `extraBall` | 0x47D6D2 | Award extra ball |
| `BuyExtraball` | 0x47D454 | Buy extra ball |
| `ManualMultiball` | 0x47D407 | Trigger manual multiball |
| `ManualMultiballControl` | 0x47B78E | Manual multiball control |
| `startBallSaver` | 0x4492D6 | Start ball saver |
| `startBallSaverOnce` | 0x4DE6F5 | Start ball saver (once) |
| `startBallSaverOnceAtTime` | 0x4C149E | Start ball saver at time |
| `startBallSaverAfterLaunch` | 0x48E516 | Ball saver after launch |
| `FlippersSticky` | 0x3E9010 | Sticky flippers (always up) |
| `disableTiltCounting` | 0x49237F | Disable tilt detection |
| `enableTiltCounting` | 0x492393 | Enable tilt detection |
| `tiltAutoDisable` | 0x4CBF63 | Auto-disable tilt |
| `tiltAutoEnable` | 0x4CC383 | Auto-enable tilt |
| `setTiltLimit` | 0x40DEBB | Set tilt limit threshold |
| `TimeScale` | 0x4CEEB7 | Physics time scale |

### Kensho Backend System

Zen Studios uses "Kensho" as their backend service:
- `KenshoIdentity` — player identity
- `cKenshoAuthDeviceRequest` — device auth
- `cKenshoGameStateRequest` — game state sync
- `/inventory/v1/inventory/unlock` — REST API unlock endpoint
- `KenshoInventoryUnlockItemResult` / `KenshoInventoryUnlockItemConfig` — inventory unlock system

---

## STEP 3-7: PENDING

### Next Steps
1. **Step 3**: Find the ball sync function — the native code that writes `ball_C` actor location each tick
2. **Step 4**: Find the kinematic toggle — how to freeze/unfreeze ball physics
3. **Step 5**: Analyze `YUP_PendulumBall_Attach`/`Detach` — could be the mechanism to "capture" the ball
4. **Step 6**: Scan uassets for ball blueprints
5. **Step 7**: Continue renaming stripped functions

### Working Theory for Ball Dragging

The `PendulumBallConstraint` system is the most promising lead:
- **attach(ball_idx)** — attaches ball to a constraint (captures it)
- **detach(ball_idx)** — releases ball from constraint
- **ballCenter** — the constraint's center position (writable?)
- If we can: (1) attach ball to a fake constraint, (2) set ballCenter to hand position, (3) physics follows

Alternative: Find and hook the native function that writes `K2_SetActorLocation` on `ball_C` each tick, replace with our position.

### Compat Layer Plan

For the Lua mod compat layer, we need to expose these native YUP actions:
1. **TableDebugInterface** actions (setPause, setGodMode, setSpeed, etc.) — hook the handler functions directly
2. **PFXDebug_*** functions — these are already UFunction, callable via `obj:Call("PFXDebug_*")`
3. **Ball control actions** (debugBallReset, setBallPerGame, ManualMultiball, etc.)
4. **God mode properties** (GOD_SaveTheBall, GOD_NotSaveTheBall) — global property IDs at 0x783c5a8/0x783c5ac
