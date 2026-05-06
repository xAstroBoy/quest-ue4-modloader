# Pinball FX VR — Table Lock / DLC / Entitlement System Analysis

> Reversed from `libUnreal.so` via bindump binary analysis tool.
> All addresses are IDA/file-relative (subtract 0x400000 from BINJA addresses).

---

## Architecture Overview

The table ownership/locking system has **four layers**:

1. **Meta Quest Entitlement** — `ovr_Entitlement_GetIsViewerEntitled()` checks app ownership
2. **Kensho Backend** — Server-side license/DLC/store system (Zen Studios' backend "Kensho")
3. **UE Reflection Properties** — `IsTableOwned`, `OwnedTables`, `bDebugUnlockAllTables`, `IsLocked`
4. **YUP Physics Layer** — `isLocked` property on YUP components controls physics simulation lock

---

## Layer 1: Meta Quest Entitlement (Oculus Platform SDK)

### Functions (PLT imports from libOVRPlatformLoader.so)
| Function | Call Sites | Purpose |
|---|---|---|
| `ovr_Entitlement_GetIsViewerEntitled()` | 0x4CAEF14, 0x53F0FEC | App-level entitlement check |
| `ovr_IAP_GetViewerPurchases()` | 0x4F7DE48, 0x53F375C | Get user's purchased DLC items |
| `ovr_IAP_GetViewerPurchasesDurableCache()` | 0x4F9152C, 0x53F3940 | Cached durable purchases |
| `ovr_IAP_ConsumePurchase()` | (1 call) | Consume a consumable purchase |
| `ovr_Purchase_GetPurchaseStrID()` | (multiple) | Get purchase SKU string ID |
| `ovr_Purchase_GetSKU()` | (multiple) | Get purchase SKU |
| `ovr_PurchaseArray_GetSize()` | (multiple) | Array size of purchases |
| `ovr_PurchaseArray_GetElement()` | (multiple) | Get purchase at index |
| `ovr_Message_GetPurchase()` | (multiple) | Get purchase from API message |
| `ovr_Message_GetPurchaseArray()` | (multiple) | Get purchase array from message |

### Entitlement Check Flow
1. `sub_4CAEF14` loads `[X0+0x98]` (store/inventory ptr), calls `ovr_Entitlement_GetIsViewerEntitled()`
2. Result stored in X20, then used for purchase array enumeration
3. Purchase array is iterated with `ovr_PurchaseArray_GetSize/GetElement`

---

## Layer 2: Kensho Backend (Zen Studios Server)

### API Endpoints
| Endpoint | Purpose |
|---|---|
| `/cdn/v1/dlc/all/{control}` | List ALL available DLC |
| `/cdn/v1/dlc/download/{idname}` | Download DLC table pak files |
| `/cdn/v1/dlc/info/{dlc_id}` | Get DLC info for a specific table |

### Kensho Data Structures
| Struct | Purpose |
|---|---|
| `KenshoTableInfo` | Table metadata from server |
| `KenshoDLCInfo` | DLC package info |
| `KenshoDLCVersion` | DLC version tracking |
| `KenshoDLCDownloadResult` | Download completion result |
| `KenshoDLCInfoResult` | Single DLC info query result |
| `KenshoDLCInfoAllResult` | All-DLC info query result |
| `KenshoLicenseQueryResult` | License/entitlement query result |
| `KenshoLicenseUpdateResult` | License update result |
| `KenshoStoreItem` / `PFXStoreItem` | Store item data |
| `KenshoStoreFrontItem` | Storefront display item |
| `KenshoStoreInventoryItem` | User's inventory item |
| `KenshoStoreBundle` | Bundle package |

### Kensho Config Structures (request configs)
| Config | Purpose |
|---|---|
| `KenshoTableInfoReadConfig` | Config for reading table info |
| `KenshoDLCInfoConfig` | Config for DLC info request |
| `KenshoDLCInfoAllConfig` | Config for all-DLC info request |
| `KenshoDLCDownloadConfig` | Config for DLC download request |
| `KenshoDLCFileDownloadConfig` | Config for individual file download |
| `KenshoLicenseQueryConfig` | Config for license query |
| `KenshoLicenseUpdateConfig` | Config for license update |
| `KenshoStoreReadConfig` | General store read config |
| `KenshoStoreFrontReadConfig` | Storefront read config |
| `KenshoStoreInfoReadConfig` | Store info read config |

### Store Types (EKenshoStoreType enum)
```
K_OCULUS, K_STEAM, K_EGS, K_MICROSOFT, K_NINTENDO, K_GOOGLE,
K_AMAZON, K_XIAOMI, K_HUAWEI, K_SAMSUNG, K_APPSTORE,
K_XBOXONE, K_XBOXSERIES, K_PS4, K_PS5, K_META, K_NETFLIX, K_UNKNOWN
```

---

## Layer 3: UE Reflection Properties & Debug Commands

### Key UProperties (accessible via reflection API)
| Property | Type | Location | Purpose |
|---|---|---|---|
| `IsTableOwned` | bool | UFunction name (0x4F0E21) | Check if player owns a table |
| `OwnedTables` | TArray? | Property (0x437224) | Array of owned table IDs |
| `bDebugUnlockAllTables` | bool | Property (0x437117) | Debug flag to bypass ownership |
| `IsContentSubmissionBuild_UnlockAllTables` | bool | Property (0x43712D) | Content submission build flag |
| `IsContentSubmissionBuild_UnlockAllCustomization` | bool | Property (0x468D25) | Unlock all customization items |
| `IsLocked` | ? | Property (multiple uses) | Lock state on UObjects |
| `GetTableInfoByIndex` | UFunction | (0x3F0C63) | Get table info by index |
| `GetTableInfos` | UFunction | (0x42786A) | Get all table infos |
| `GetTableDownloadRatio` | UFunction | (0x45E54F) | Download progress 0.0-1.0 |
| `TableDownloadStarted` | Delegate? | (0x4EC0E9) | Fired when download starts |
| `TableDownloadFinishedWithResult` | Delegate? | (0x40B384) | Fired when download completes |

### PFXStoreManager (UObject class)
- Registered at: `/Script/PinballFX_VR.PFXStoreManager`
- Class registration: `sub_1950904("/Script/PinballFX_VR", "PFXStoreManager", ...)`
- Delegates:
  - `PFXStoreManagerOutlink__DelegateSignature` — Store outlink events
  - `PFXStoreManagerPurchase__DelegateSignature` — Purchase events

### Table Info Loading
- `cPFXLoadBultinTableInfosProcess` — Loads built-in table info at startup
  - `OnWork(double)` — Called per-frame during loading (virtual)
  - `OnFinishLoad()` — Called when all table infos are loaded
  - Source path: `C:/svn/YUP_PinballFX_VR/game/Source/Pin...`

### EYUPPinballTableId Enum
~315 table IDs covering all PFX tables. Format: `PREFIX_TableName`

**Prefixes:**
| Prefix | Franchise |
|---|---|
| `ZEN_` | Zen Studios originals |
| `MRV_` | Marvel |
| `STW_` | Star Wars |
| `FOX_` | Fox properties (Family Guy, Bob's Burgers, Alien, Archer, etc.) |
| `WMS_` | Williams (classic real pinball machines) |
| `HSB_` | Hasbro |
| `PNT_` | Peanuts/Snoopy |
| `SPK_` | South Park |
| `BTH_` | Bethesda (Fallout, etc.) |
| `GRB_` | Gearbox (Borderlands, Brothers in Arms) |
| `UNI_` | Universal (Jaws, Xena, etc.) |
| `CRD_` | Crystal Dynamics (Tomb Raider) |
| `CSS_` | Coffee Stain (Goat Simulator) |
| `ASM_` | Asmodee (Terraforming Mars, Exploding Kittens) |
| `DRW_` | DreamWorks (Trolls) |
| `STK_` | Star Trek |

---

## Layer 4: YUP Physics Layer

### isLocked Property
The YUP physics engine uses `isLocked` as a property on physics components:
```c
YUP_MakePropertyName("isLocked", &propertyName);
```
This controls whether the table's physics simulation is locked (demo/trial mode).

---

## PFXDebug Console Commands (ALL KNOWN)

### Table Ownership & Awards
| Command | Purpose |
|---|---|
| `PFXDebug.Table.AwardUnlock` | Unlock specific award |
| `PFXDebug.Table.AwardLock` | Lock specific award |
| `PFXDebug.Table.AwardUnlockTable` | Unlock all awards for a table |
| `PFXDebug.Table.AwardLockTable` | Lock all awards for a table |
| `PFXDebug.Table.AwardUnlockAll` | Unlock ALL awards for ALL tables |
| `PFXDebug.Table.AwardLockAll` | Lock ALL awards for ALL tables |

### Table Mastery
| Command | Purpose |
|---|---|
| `PFXDebug.Table.MasteryLevelSet` | Set mastery level for a table |
| `PFXDebug.Table.MasteryMaxAll` | Max mastery on ALL tables |
| `PFXDebug.Table.MasteryResetAll` | Reset mastery on ALL tables |
| `PFXDebug.Table.MasteryMaxTable` | Max mastery on specific table |
| `PFXDebug.Table.MasteryResetTable` | Reset mastery on specific table |

### Table Perks
| Command | Purpose |
|---|---|
| `PFXDebug.Table.PerkLevelSet` | Set perk level |
| `PFXDebug.Table.PerkMaxTable` | Max all perks on a table |
| `PFXDebug.Table.PerkResetTable` | Reset perks on a table |
| `PFXDebug.Table.PerkMaxAll` | Max ALL perks on ALL tables |
| `PFXDebug.Table.PerkResetAll` | Reset ALL perks |
| `PFXDebug.Table.ForceFavoriteSlot` | Force a favorite slot |

### User Inventory
| Command | Purpose |
|---|---|
| `PFXDebug.UserInventory.UnLockAllTables` | **UNLOCK ALL TABLES (bypasses DLC)** |
| `PFXDebug.UserInventory.ReLockAllTables` | Re-lock all tables |

### Collectibles
| Command | Purpose |
|---|---|
| `PFXDebug.Collectibles.UnlockEntry` | Unlock specific collectible |
| `PFXDebug.Collectibles.LockEntry` | Lock specific collectible |
| `PFXDebug.Collectibles.UnlockAll` | Unlock ALL collectibles |
| `PFXDebug.Collectibles.LockAll` | Lock ALL collectibles |
| `PFXDebug.Collectibles.UnlockAllEntriesInBundle` | Unlock all in a bundle |
| `PFXDebug.Collectibles.LockAllEntriesInBundle` | Lock all in a bundle |

### Achievements
| Command | Purpose |
|---|---|
| `PFXDebug.Achievement.Unlock` | Unlock specific achievement |
| `PFXDebug.Achievement.UnlockAll` | Unlock ALL achievements |
| `PFXDebug.Achievement.SetProgress` | Set achievement progress |
| `PFXDebug.Achievement.AddProgress` | Add to achievement progress |

### Championship
| Command | Purpose |
|---|---|
| `PFXDebug.Championship.Enable` | Enable championship mode |
| `PFXDebug.Championship.MasteryLevelSet` | Set championship mastery |
| `PFXDebug.Championship.MasteryMaxAll` | Max all championship mastery |
| `PFXDebug.Championship.MasteryResetAll` | Reset all championship mastery |
| `PFXDebug.Championship.MaxAll` | Max everything in championship |
| `PFXDebug.Championship.MaxLeague` | Max league level |
| `PFXDebug.Championship.BenefitGrant` | Grant a benefit |
| `PFXDebug.Championship.BenefitEffectCalculator` | Debug benefit effects |
| `PFXDebug.Championship.SetDebugMatch` | Set up a debug match |
| `PFXDebug.Championship.MissionReward` | Grant mission reward |

### Player / Profile
| Command | Purpose |
|---|---|
| `PFXDebug.Onboarding.Reset` | Reset onboarding state |
| `PFXDebug.Onboarding.ShowHintPanelByIndex` | Show hint panel |
| `PFXDebug.Onboarding.ShowHintPanelByName` | Show hint panel by name |
| `PFXDebug.PlayerProfile.Reset` | **RESET ENTIRE PLAYER PROFILE** |

### Performance / Debug
| Command | Purpose |
|---|---|
| `PFXDebug.Performance.SetTargetDisplayFrequency` | Set display Hz |
| `PFXDebug.Performance.ActivateNiagaraEffects` | Toggle Niagara FX |
| `PFXDebug.Performance.TableIsolation` | Isolate table for perf testing |
| `PFXDebug.Pawn.ToggleCapsuleVisibility` | Toggle pawn capsule |
| `PFXDebug.Pawn.SetCameraFade` | Set camera fade |

### Audio
| Command | Purpose |
|---|---|
| `PFXDebug.Audio.TestTone` | Play test tone |
| `PFXDebug.Audio.SetMusicCutoffFrequency` | Set music cutoff |

### Misc
| Command | Purpose |
|---|---|
| `PFXDebug.Sequence.SetPlaybackPosition` | Set sequence position |
| `PFXDebug.RewardWheel.AddSpin` | Add a reward wheel spin |

---

## Debug Command Registration

All PFXDebug commands are registered in a single massive function at **0x4F15CEC** (size: ~11KB, spans to 0x4F18A24).

Registration uses an indirect call through `0x482e3c` (command registrar function):
```c
// Signature: RegisterDebugCommand(registry, commandName, hint, callback, flags)
(*0x482e3c)(&data_12cb06c, u"PFXDebug.Table.AwardUnlockAll", u"todo: hint", callback, 0);
```

The `data_12cb06c` is the debug command registry singleton.

### Key Registration Addresses (BINJA)
| Command | Registration Offset (file) |
|---|---|
| Debug command registrar function | 0x4F15CEC |
| `PFXDebug.Table.MasteryLevelSet` | referenced at 0x4F16CCC |
| `PFXDebug.Table.AwardUnlockTable` | referenced at 0x4F17340 |
| `PFXDebug.UserInventory.UnLockAllTables` | referenced at 0x4F184C4 |

---

## Table Save Data Structures

| Struct | Purpose |
|---|---|
| `PFXTableSaveEntry` | Per-table save data |
| `PFXTableProgressSaveEntry` | Per-table progress save data |
| `PFXTablePerkProgress` | Perk progress per table |
| `PFXTableSavedProgress` | Overall saved progress per table |
| `PFXTableGuideStart` | Table guide state |

---

## EPFXTableXpSource Enum
Source of XP gain:
- `TableAchievement` — XP from table achievements

---

## Modding Implications

### ⚠️ UE Reflection Properties DO NOT WORK for Table Unlocking
Properties like `bDebugUnlockAllTables`, `IsTableOwned`, `OwnedTables` etc. are **read-only indicators** — setting them via reflection has NO effect on the actual unlock state. The real unlock state lives in the **PFXSaveManager**'s native TMap data structures.

### ✅ Method 1: Native Function Calls (WORKING — via YUP Lua API)
Direct calls into the PFXSaveManager's award system. These are the **actual implementations** behind the PFXDebug console commands.

```lua
-- Unlock a single table by its numeric ID (grants all 3 award tiers)
YUP.AwardUnlockTable(42)

-- Lock a single table by its numeric ID
YUP.AwardLockTable(42)
```

### Method 2: YUP Property Manipulation
```lua
-- Via YUP API — set isLocked = false on physics component
YUP.SetProperty(component, "isLocked", false)
```

### Method 3: Hook IsTableOwned
Hook the `IsTableOwned` UFunction via ProcessEvent to always return true.

---

## Native Function Call Chain (Reversed from PFXDebug Callbacks)

### Key Singletons
| Singleton | BINJA Global | Getter Function | Class |
|---|---|---|---|
| PFXSaveManager | `data_7a8a358` | `sub_52D3784` (setter) | `/Script/PinballFX_VR.PFXSaveManager` |
| PFXCheatManager | `data_7a847d0` | `sub_5273F78` (lazy-init) | `/Script/PinballFX_VR.PFXCheatManager` |
| PFXGameSubsystem | `data_7a85c58` | `sub_52917BC` (lazy-init) | `/Script/PinballFX_VR.PFXGameSubsystem` |
| PFXMasteryManager | `data_768A318` | — | `/Script/PinballFX_VR.PFXMasteryManager` |

### Per-Table Award Functions
| Function | BINJA Addr | IDA Addr | Signature | Purpose |
|---|---|---|---|---|
| AwardUnlockTable | `0x533E360` | `0x4F3E360` | `void(void* unused, int32_t tableId)` | Ensures award TArrays at entry+0x18/+0x28 contain {0,1,2} |
| AwardLockTable | `0x533E710` | `0x4F3E710` | `void(void* unused, int32_t tableId)` | Removes award entries for table |
| AwardUnlockAll callback | `0x534400C` | `0x4F4400C` | `void(void* ctx, void* args)` | Iterates all tables via PFXGameSubsystem |
| AwardLockAll callback | `0x53440E0` | `0x4F440E0` | `void(void* ctx, void* args)` | Locks all tables |

### PFXSaveManager Internal Layout
- **Mutex**: offset `+0x58` (locked via `sub_5357BC8`, unlocked via `sub_5357BD4`)
- **Award TMap**: offset `+0x4B8` (entry size 0x48, keyed by table ID)
  - Entry `+0x00`: table ID (int32)
  - Entry `+0x18`: TArray\<int32\> of award tiers (values 0, 1, 2 = Bronze/Silver/Gold)
  - Entry `+0x28`: TArray\<int32\> of award tiers (duplicate/backup)
  - Entry `+0x40`: hash chain next index
- **Hash index**: offsets `+0x4C0`, `+0x4EC`, `+0x4F0`, `+0x4F8`, `+0x500`

### AwardUnlockTable Flow (sub_533E360)
1. Read PFXSaveManager singleton from `data_7a8a358`
2. Lock mutex at `PFXSaveManager + 0x58`
3. Hash lookup in TMap at `+0x4B8` for the given table ID
4. If entry doesn't exist, create via `sub_535375C` (TMap::FindOrAdd)
5. For each of the 3 award tiers (0, 1, 2):
   - Search TArray at entry+0x18 for the value
   - If not found, append it (resizing via `sub_17155C0` if needed)
   - Search TArray at entry+0x28 for the value
   - If not found, append it
6. Unlock mutex

### PFXDebug Callback → Native Function Map
| Debug Command | Callback (BINJA) | Calls Into |
|---|---|---|
| `PFXDebug.Table.AwardUnlockTable <id>` | `sub_5343EA4` | `sub_533E360(unused, tableId)` |
| `PFXDebug.Table.AwardLockTable <id>` | `sub_5343F58` | `sub_533E710(unused, tableId)` |
| `PFXDebug.Table.AwardUnlockAll` | `sub_534400C` | Iterates tables → `sub_533E360` each |
| `PFXDebug.Table.AwardLockAll` | `sub_53440E0` | Iterates tables → `sub_533E710` each |
| `PFXDebug.UserInventory.UnLockAllTables` | `sub_5344618` | PFXCheatManager validation path |
| `PFXDebug.UserInventory.ReLockAllTables` | `sub_5344618` | Same callback (shared) |

---

## Key Offsets Summary

| Symbol | File Offset | BINJA Offset | Purpose |
|---|---|---|---|
| Debug cmd registrar func | 0x4F15CEC | 0x4F55CEC | Registers ALL PFXDebug.* commands |
| AwardUnlockTable | 0x4F3E360 | 0x533E360 | Per-table award unlock (native) |
| AwardLockTable | 0x4F3E710 | 0x533E710 | Per-table award lock (native) |
| AwardUnlockAll callback | 0x4F4400C | 0x534400C | Batch unlock all tables |
| AwardLockAll callback | 0x4F440E0 | 0x53440E0 | Batch lock all tables |
| PFXSaveManager setter | 0x4ED3784 | 0x52D3784 | Stores singleton ptr |
| PFXCheatManager getter | 0x4E73F78 | 0x5273F78 | Lazy-init singleton |
| PFXGameSubsystem getter | 0x4E917BC | 0x52917BC | Lazy-init singleton |
| SaveManager mutex lock | 0x4F57BC8 | 0x5357BC8 | Lock at +0x58 |
| SaveManager mutex unlock | 0x4F57BD4 | 0x5357BD4 | Unlock |
| TMap hash lookup | 0x4F5375C | 0x535375C | FindOrAdd in award TMap |
| `ovr_Entitlement_GetIsViewerEntitled` caller | 0x4CAEF14 | 0x4CEEF14 | Entitlement check wrapper |
| `ovr_IAP_GetViewerPurchases` caller | 0x4F7DE48 | 0x4FBDE48 | Purchase list fetcher |
| `cPFXLoadBultinTableInfosProcess::OnWork` | 0x4F9F980 | 0x4FDF980 | Table info loading loop |
| YUP `isLocked` property user | see L13894384 | — | Physics lock property |
