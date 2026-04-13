#pragma once
// modloader/include/modloader/ue_types.h
// ═══════════════════════════════════════════════════════════════════════════
// Complete Unreal Engine type definitions for ARM64 Android (Quest 3)
// Covers UE4.25 through UE5.6 — all types derived from source analysis of
// https://github.com/oculus-vr/UnrealEngine across all supported branches.
//
// These are OPAQUE structures accessed via offset-based reads — we never
// instantiate them. The struct definitions serve as documentation and
// provide field offset constants for the reflection/memory access layer.
//
// For field access, always use ue::read_field<T>(ptr, offset) from types.h.
// Never dereference struct members directly (offsets are runtime-configurable).
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>

namespace ue
{

    // ═══════════════════════════════════════════════════════════════════════════
    // Core Container Types (binary-compatible definitions)
    // ═══════════════════════════════════════════════════════════════════════════

    // TArray<T> — UE's dynamic array, binary layout is always the same:
    //   +0x00: T*    Data      (heap pointer)
    //   +0x08: int32 Num       (element count)
    //   +0x0C: int32 Max       (allocated capacity)
    // Total: 16 bytes (0x10) on ARM64
    struct FTArray
    {
        void *Data;
        int32_t Num;
        int32_t Max;
    };
    static_assert(sizeof(FTArray) == 0x10, "FTArray must be 16 bytes");

    namespace tarray
    {
        constexpr uint32_t DATA = 0x00;
        constexpr uint32_t NUM = 0x08;
        constexpr uint32_t MAX = 0x0C;
        constexpr uint32_t SIZE = 0x10;
    }

    // FString — TArray<TCHAR> where TCHAR = char16_t on Android ARM64
    //   Same layout as TArray: {char16_t* Data, int32 Num, int32 Max}
    //   Num includes null terminator. Empty string: Num=0, Data=nullptr.
    struct FString
    {
        char16_t *Data;
        int32_t Num;
        int32_t Max;
    };
    static_assert(sizeof(FString) == 0x10, "FString must be 16 bytes");

    namespace fstring
    {
        constexpr uint32_t DATA = 0x00;
        constexpr uint32_t NUM = 0x08;
        constexpr uint32_t MAX = 0x0C;
        constexpr uint32_t SIZE = 0x10;
    }

    // FText — opaque text container
    // UE4: FTextData* (shared pointer internals), 24-40 bytes depending on config
    // UE5: similar but with extra flags
    // We treat FText as opaque and use FText_ToString / FText_FromString functions.
    // Size varies: 0x18 (UE4 shipping) to 0x28 (UE5 with editor data)
    // For property access, the reflection system knows the actual size per-property.
    namespace ftext
    {
        // Minimum sizes by version (shipping ARM64)
        constexpr uint32_t UE4_SIZE = 0x18; // 24 bytes (FTextData shared ref + flags)
        constexpr uint32_t UE5_SIZE = 0x18; // 24 bytes (same in shipping builds)
    }

    // TSortedMap / TMap — hash map
    //   +0x00: FHashBucket* HashBuckets (pointer)
    //   +0x08: int32 HashSize
    //   +0x0C: (pad)
    //   +0x10: TArray<TPair<K,V>> Pairs
    //   +0x20: int32 KeyHash
    // Actual layout is complex (TSparseArray + TBitArray), but for TArray-based
    // TMap in UE4/UE5 shipping, the effective structure is:
    //   TSet<TPair<K,V>> with inline allocator
    // We access TMap through reflection which knows the exact layout.

    // FName — 8 bytes (always)
    // Already defined in types.h — reference only
    // struct FName { int32 ComparisonIndex; int32 Number; };

    // ═══════════════════════════════════════════════════════════════════════════
    // Math Types — used in UE properties and function parameters
    // ═══════════════════════════════════════════════════════════════════════════

    struct FVector
    {
        float X, Y, Z;
    };
    static_assert(sizeof(FVector) == 12, "FVector must be 12 bytes");

    struct FVector2D
    {
        float X, Y;
    };
    static_assert(sizeof(FVector2D) == 8, "FVector2D must be 8 bytes");

    struct FVector4
    {
        float X, Y, Z, W;
    };
    static_assert(sizeof(FVector4) == 16, "FVector4 must be 16 bytes");

    struct FIntPoint
    {
        int32_t X, Y;
    };
    static_assert(sizeof(FIntPoint) == 8, "FIntPoint must be 8 bytes");

    struct FIntVector
    {
        int32_t X, Y, Z;
    };
    static_assert(sizeof(FIntVector) == 12, "FIntVector must be 12 bytes");

    struct FRotator
    {
        float Pitch, Yaw, Roll;
    };
    static_assert(sizeof(FRotator) == 12, "FRotator must be 12 bytes");

    struct FQuat
    {
        float X, Y, Z, W;
    };
    static_assert(sizeof(FQuat) == 16, "FQuat must be 16 bytes");

    struct FTransform
    {
        FQuat Rotation;      // +0x00 (16)
        FVector Translation; // +0x10 (12)
        float _pad0;         // +0x1C (4) — alignment padding
        FVector Scale3D;     // +0x20 (12)
        float _pad1;         // +0x2C (4) — alignment padding
    };
    static_assert(sizeof(FTransform) == 0x30, "FTransform must be 48 bytes");

    struct FLinearColor
    {
        float R, G, B, A;
    };
    static_assert(sizeof(FLinearColor) == 16, "FLinearColor must be 16 bytes");

    struct FColor
    {
        uint8_t B, G, R, A; // Note: BGRA order in memory
    };
    static_assert(sizeof(FColor) == 4, "FColor must be 4 bytes");

    struct FBox
    {
        FVector Min;
        FVector Max;
        uint8_t IsValid;
    };

    struct FBox2D
    {
        FVector2D Min;
        FVector2D Max;
        uint8_t IsValid;
    };

    struct FMatrix
    {
        float M[4][4];
    };
    static_assert(sizeof(FMatrix) == 64, "FMatrix must be 64 bytes");

    struct FPlane
    {
        float X, Y, Z, W;
    };
    static_assert(sizeof(FPlane) == 16, "FPlane must be 16 bytes");

    // ═══════════════════════════════════════════════════════════════════════════
    // Gameplay Framework Types — Offset-based access only
    // ═══════════════════════════════════════════════════════════════════════════
    // These are all UObject-derived. Their internal layout is complex and
    // version-dependent. We access their PROPERTIES through UE reflection
    // (Get/Set/Call) and their UOBJECT FIELDS through the offsets in types.h.
    //
    // The structures below document the class hierarchy for reference.
    // We never instantiate these — they exist only as opaque pointers.

    // Forward declarations (opaque — used as pointer types only)
    struct UObject;   // defined in types.h as forward decl
    struct UClass;    // defined in types.h as forward decl
    struct UStruct;   // defined in types.h as forward decl
    struct UFunction; // defined in types.h as forward decl
    struct UField;    // defined in types.h as forward decl
    struct UEnum;     // defined in types.h as forward decl
    struct FField;    // defined in types.h as forward decl
    struct FProperty; // defined in types.h as forward decl

    // ═══ UObject Hierarchy ═════════════════════════════════════════════════
    // All accessed via reflection (Get/Set/Call), never via direct offsets.
    // Listed here for documentation and pointer typing only.

    // UObjectBase → UObjectBaseUtility → UObject → UField → UStruct → UClass
    //                                            → UField → UEnum
    //                                            → UField → UStruct → UFunction
    //                                            → UField → UStruct → UScriptStruct
    // AActor → derives from UObject
    // APawn → derives from AActor
    // ACharacter → derives from APawn
    // AController → derives from AActor
    // APlayerController → derives from AController
    // AGameModeBase → derives from AActor (UE5) / AInfo (UE4)
    // AGameMode → derives from AGameModeBase
    // AGameStateBase → derives from AInfo
    // APlayerState → derives from AInfo

    struct AActor;
    struct APawn;
    struct ACharacter;
    struct AController;
    struct APlayerController;
    struct AGameModeBase;
    struct AGameMode;
    struct AGameStateBase;
    struct APlayerState;
    struct AInfo;
    struct AHUD;
    struct APlayerCameraManager;

    // Component hierarchy (all UObject-derived)
    struct UActorComponent;
    struct USceneComponent;
    struct UPrimitiveComponent;
    struct UMeshComponent;
    struct UStaticMeshComponent;
    struct USkeletalMeshComponent;
    struct UCameraComponent;
    struct UWidgetComponent;

    // Engine types
    struct UEngine;
    struct UGameEngine;
    struct UEditorEngine;
    struct UWorld;
    struct ULevel;
    struct UGameInstance;
    struct UGameViewportClient;
    struct ULocalPlayer;
    struct UPlayer;

    // Widget types (UMG)
    struct UWidget;
    struct UUserWidget;
    struct UPanelWidget;
    struct UContentWidget;
    struct UVerticalBox;
    struct UHorizontalBox;
    struct UCanvasPanel;
    struct UOverlay;
    struct UTextBlock;
    struct UImage;
    struct UButton;
    struct UBorder;
    struct USizeBox;
    struct UWidgetTree;
    struct UScrollBox;

    // Asset types
    struct UPackage;
    struct UBlueprint;
    struct UBlueprintGeneratedClass;
    struct UDataAsset;
    struct UTexture;
    struct UTexture2D;
    struct UMaterial;
    struct UMaterialInterface;
    struct UMaterialInstance;
    struct UStaticMesh;
    struct USkeletalMesh;
    struct USoundBase;
    struct USoundWave;
    struct UAnimSequence;
    struct UAnimBlueprint;

    // Subsystems
    struct USubsystem;
    struct UWorldSubsystem;
    struct UGameInstanceSubsystem;
    struct ULocalPlayerSubsystem;

    // ═══════════════════════════════════════════════════════════════════════════
    // UWorld Layout — Key offsets for the world context
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Source: Engine/Source/Runtime/Engine/Classes/Engine/World.h
    //
    // UWorld is a massive class. We only need a few key fields:
    //   - PersistentLevel (ULevel*)
    //   - AuthorityGameMode (AGameModeBase*)
    //   - GameState (AGameStateBase*)
    //   - OwningGameInstance (UGameInstance*)
    //   - Levels (TArray<ULevel*>)
    //
    // These are accessed via reflection (Get/Set), not direct offsets.
    // The offsets below are for emergency fallback only.
    //
    // UWorld inherits: UObject → UObjectBaseUtility → UObjectBase
    // UWorld does NOT inherit from UField/UStruct — it's a leaf UObject.
    //
    namespace uworld
    {
        // These are approximate and version-specific. Use reflection instead.
        // Listed for documentation only.
        // UE4.27: PersistentLevel ~0x30, Levels ~0x158, GameState ~0x270
        // UE5.4:  PersistentLevel ~0x30, Levels ~0x168, GameState ~0x280
        // The exact offsets vary by game due to engine modifications.
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // UGameEngine Layout — Singleton engine object
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Source: Engine/Source/Runtime/Engine/Classes/Engine/GameEngine.h
    //
    // class UGameEngine : public UEngine
    //
    // UEngine key fields:
    //   - GameViewport (UGameViewportClient*)
    //   - GameInstance (UGameInstance*)  [actually in UGameEngine, not UEngine]
    //
    // Access pattern: GEngine → UGameEngine* → Get("GameViewport") etc.
    // Always use reflection for property access.
    //
    namespace ugameengine
    {
        // UEngine properties (via reflection):
        //   "GameViewport" → UGameViewportClient*
        //   "bSmoothFrameRate" → bool
        //   "SmoothedFrameRateRange" → FFloatRange
        //
        // UGameEngine properties:
        //   "GameInstance" → UGameInstance*
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // AActor Key Virtual Functions (vtable reference)
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // AActor::GetWorld() — important for world context
    // AActor::GetActorLocation() — returns FVector
    // AActor::K2_SetActorLocation() — Blueprint-callable, takes FVector
    // AActor::K2_DestroyActor() — Blueprint destroy
    //
    // All called via ProcessEvent (reflection), not vtable dispatch.
    //

    // ═══════════════════════════════════════════════════════════════════════════
    // FWeakObjectPtr — Weak reference to UObject
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // struct FWeakObjectPtr {
    //     int32 ObjectIndex;     // index into GUObjectArray
    //     int32 ObjectSerialNumber; // serial number for staleness check
    // };
    //
    struct FWeakObjectPtr
    {
        int32_t ObjectIndex;
        int32_t ObjectSerialNumber;
    };
    static_assert(sizeof(FWeakObjectPtr) == 8, "FWeakObjectPtr must be 8 bytes");

    // ═══════════════════════════════════════════════════════════════════════════
    // FScriptDelegate / FMulticastScriptDelegate
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // FScriptDelegate:
    //   +0x00: FWeakObjectPtr Object (8 bytes)
    //   +0x08: FName FunctionName (8 bytes)
    //   Total: 16 bytes
    //
    // FMulticastScriptDelegate:
    //   +0x00: TArray<FScriptDelegate> InvocationList (16 bytes)
    //   Total: 16 bytes
    //
    struct FScriptDelegate
    {
        FWeakObjectPtr Object;
        uint8_t FunctionName[8]; // FName
    };
    static_assert(sizeof(FScriptDelegate) == 16, "FScriptDelegate must be 16 bytes");

    struct FMulticastScriptDelegate
    {
        FTArray InvocationList; // TArray<FScriptDelegate>
    };
    static_assert(sizeof(FMulticastScriptDelegate) == 16, "FMulticastScriptDelegate must be 16 bytes");

    // ═══════════════════════════════════════════════════════════════════════════
    // FSoftObjectPath / FSoftObjectPtr
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // FSoftObjectPath:
    //   +0x00: FName AssetPathName (8 bytes)  [UE4]
    //     OR   FTopLevelAssetPath (16 bytes)  [UE5.1+]
    //   +0x08/0x10: FString SubPathString (16 bytes)
    //   Total: 24 bytes (UE4), 32 bytes (UE5.1+)
    //
    // We access these via reflection, so exact layout is handled by the engine.
    //

    // ═══════════════════════════════════════════════════════════════════════════
    // FPakFile / FPakPlatformFile — PAK system types
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // FPakFile — represents a single .pak archive
    // Source: Engine/Source/Runtime/PakFile/Public/IPlatformFilePak.h
    //
    // Key fields (approximate, version-dependent):
    //   - PakFilename (FString)
    //   - MountPoint (FString)
    //   - NumEntries (int32)
    //   - bEncrypted (bool)
    //   - bSigned (bool)
    //   - Info (FPakInfo) — header info struct
    //
    // FPakInfo — PAK file header
    //   - Magic (uint32, 0x5A6F12E1)
    //   - Version (int32)
    //   - IndexOffset (int64)
    //   - IndexSize (int64)
    //   - IndexHash (FSHAHash, 20 bytes)
    //   - bEncryptedIndex (bool)
    //   - EncryptionKeyGuid (FGuid, 16 bytes)
    //
    // FPakPlatformFile — IPlatformFile implementation for PAK archives
    //   - PakFiles (TArray<TRefCountPtr<FPakFile>>) — all mounted PAKs
    //   - LowerLevel (IPlatformFile*) — fallback file system
    //
    namespace fpakfile
    {
        // FPakInfo magic number — same across all UE versions
        constexpr uint32_t PAK_MAGIC = 0x5A6F12E1;

        // FPakInfo version constants
        constexpr int32_t PAK_VERSION_INITIAL = 1;
        constexpr int32_t PAK_VERSION_NO_TIMESTAMPS = 2;
        constexpr int32_t PAK_VERSION_COMPRESSION_ENCRYPTION = 3;
        constexpr int32_t PAK_VERSION_INDEX_ENCRYPTION = 4;
        constexpr int32_t PAK_VERSION_RELATIVE_CHUNK_OFFSETS = 5;
        constexpr int32_t PAK_VERSION_DELETE_RECORDS = 6;
        constexpr int32_t PAK_VERSION_ENCRYPTION_KEY_GUID = 7;
        constexpr int32_t PAK_VERSION_FNAME_BASED_COMPRESSION = 8;
        constexpr int32_t PAK_VERSION_FROZEN_INDEX = 9;
        constexpr int32_t PAK_VERSION_PATH_HASH_INDEX = 10;
        constexpr int32_t PAK_VERSION_FN_NAME_MAP = 11;

        // PAK file footer (at end of file):
        //   EncryptionKeyGuid: FGuid (16 bytes)
        //   bEncryptedIndex:   bool (1 byte) + padding
        //   Magic:             uint32 (0x5A6F12E1)
        //   Version:           int32
        //   IndexOffset:       int64
        //   IndexSize:         int64
        //   IndexHash:         uint8[20] (SHA1)
        //   FrozenIndex:       bool (1 byte, v9+)
        //   CompressionMethods: FString[] (v8+)

        // FPakInfo footer sizes by version
        constexpr uint32_t PAK_INFO_SIZE_V7 = 61;  // up to version 7
        constexpr uint32_t PAK_INFO_SIZE_V8 = 221; // version 8 (added compression method names)
        constexpr uint32_t PAK_INFO_SIZE_V9 = 222; // version 9 (added frozen index flag)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // FAES — AES encryption types
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Source: Engine/Source/Runtime/Core/Public/Misc/AES.h
    //
    // struct FAES::FAESKey {
    //     uint8 Key[32]; // AES-256 key (256 bits = 32 bytes)
    // };
    //
    // static void FAES::DecryptData(uint8* Contents, uint64 NumBytes, const FAESKey& Key);
    // static void FAES::EncryptData(uint8* Contents, uint64 NumBytes, const FAESKey& Key);
    //
    // The PAK system uses FAES to decrypt the index and optionally individual files.
    // Key is typically set via FCoreDelegates::GetPakEncryptionKeyDelegate().
    //
    namespace faes
    {
        constexpr uint32_t KEY_SIZE = 32; // AES-256 = 32 bytes

        struct FAESKey
        {
            uint8_t Key[KEY_SIZE];
        };
        static_assert(sizeof(FAESKey) == 32, "FAESKey must be 32 bytes");

        // FAES::DecryptData signature
        // void DecryptData(uint8* Contents, uint64 NumBytes, const FAESKey& Key)
        using DecryptDataFn = void (*)(uint8_t *contents, uint64_t num_bytes, const FAESKey *key);

        // FAES::EncryptData signature
        using EncryptDataFn = void (*)(uint8_t *contents, uint64_t num_bytes, const FAESKey *key);

        // FCoreDelegates::GetPakEncryptionKeyDelegate — returns the AES key
        // Different signatures in UE4 vs UE5:
        // UE4: void GetPakEncryptionKey(uint8[32]& OutKey)
        // UE5: void GetPakEncryptionKey(uint8[32]& OutKey, const FGuid& EncryptionKeyGuid)
        using GetPakEncryptionKeyFn_UE4 = void (*)(uint8_t *out_key);
        using GetPakEncryptionKeyFn_UE5 = void (*)(uint8_t *out_key, const void *encryption_key_guid);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // FGuid — Globally Unique Identifier
    // ═══════════════════════════════════════════════════════════════════════════
    struct FGuid
    {
        uint32_t A, B, C, D;
    };
    static_assert(sizeof(FGuid) == 16, "FGuid must be 16 bytes");

    // ═══════════════════════════════════════════════════════════════════════════
    // FSHAHash — SHA1 hash (20 bytes)
    // ═══════════════════════════════════════════════════════════════════════════
    struct FSHAHash
    {
        uint8_t Hash[20];
    };
    static_assert(sizeof(FSHAHash) == 20, "FSHAHash must be 20 bytes");

    // ═══════════════════════════════════════════════════════════════════════════
    // FArchive — UE serialization stream (opaque — vtable-based interface)
    // ═══════════════════════════════════════════════════════════════════════════
    // Used by pak loading code. We don't need the full layout, just the type.
    struct FArchive;

    // ═══════════════════════════════════════════════════════════════════════════
    // TSharedPtr / TRefCountPtr — Smart pointer wrappers
    // ═══════════════════════════════════════════════════════════════════════════
    // TSharedPtr<T>: { T* Object; FSharedReferencer* SharedReferenceCount; } = 16 bytes
    // TRefCountPtr<T>: { T* Reference; } = 8 bytes (uses intrusive ref counting)

    namespace smart_ptr
    {
        constexpr uint32_t TSHAREDPTR_SIZE = 16;
        constexpr uint32_t TREFCOUNTPTR_SIZE = 8;
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // FUObjectItem — Element in GUObjectArray
    // ═══════════════════════════════════════════════════════════════════════════
    // Already fully documented in engine_versions.h. Reference definition here.
    //
    // UE4:  { UObject* (8), Flags(4), ClusterRoot(4), Serial(4) } = 20 padded to 24
    // UE5:  { UObject* (8), Flags(4), ClusterRoot(4), Serial(4), RefCount(4) } = 24
    //
    namespace fuobjectitem
    {
        constexpr uint32_t OBJECT_PTR = 0x00;
        constexpr uint32_t FLAGS = 0x08;
        constexpr uint32_t CLUSTER_ROOT = 0x0C;
        constexpr uint32_t SERIAL_NUMBER = 0x10;
        constexpr uint32_t UE5_REF_COUNT = 0x14; // UE5 only
    }

    // EInternalObjectFlags — stored in FUObjectItem::Flags
    enum EInternalObjectFlags : int32_t
    {
        None = 0,
        // Internal object flags not exposed to UObject
        ReachableInCluster = 1 << 23,  // External reference to object in cluster exists
        ClusterRoot = 1 << 24,         // Root of a cluster
        Native = 1 << 25,              // Native (UClass only)
        Async = 1 << 26,               // Object exists only on a non-game thread
        AsyncLoading = 1 << 27,        // Object is being asynchronously loaded
        Unreachable = 1 << 28,         // Object is not reachable on the object graph
        PendingKill = 1 << 29,         // Objects that are pending destruction (UE4)
        RootSet = 1 << 30,             // Object is part of the root set
        PendingConstruction = 1 << 31, // Object is pending construction
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // FFieldClass — Runtime class for FField objects
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Source: Engine/Source/Runtime/CoreUObject/Public/UObject/FieldPath.h
    //
    // FFieldClass is a small descriptor:
    //   +0x00: FName Name          (8 bytes)
    //   +0x08: uint64 Id           (unique class id)
    //   +0x10: uint64 CastFlags    (bitfield for fast type checking)
    //   +0x18: EClassFlags ClassFlags
    //   +0x20: FFieldClass* SuperClass
    //
    // The Name field is the one we read to determine property type names like
    // "BoolProperty", "IntProperty", "ObjectProperty", etc.
    //
    namespace ffieldclass
    {
        constexpr uint32_t NAME = 0x00;        // FName (8 bytes)
        constexpr uint32_t ID = 0x08;          // uint64
        constexpr uint32_t CAST_FLAGS = 0x10;  // uint64
        constexpr uint32_t CLASS_FLAGS = 0x18; // uint32
        constexpr uint32_t SUPER_CLASS = 0x20; // FFieldClass*
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // UClass Additional Layout
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // class UClass : public UStruct
    //
    // Important fields after UStruct's:
    //   +0x??: ClassConstructor (InternalConstructor function pointer)
    //   +0x??: ClassDefaultObject (UObject* CDO)
    //   +0x??: ClassFlags (EClassFlags)
    //   +0x??: ClassWithin (UClass*)
    //   +0x??: ClassConfigName (FName)
    //   +0x??: Interfaces (TArray<FImplementedInterface>)
    //
    // The CDO offset varies significantly between UE4 and UE5.
    // It's at ~0x108 in UE4.27 and ~0x110 in UE5 (after added fields).
    // We find it via reflection walking, not hardcoded offsets.
    //
    namespace uclass
    {
        // UE4.27 approximate offsets (from RE4 VR verification)
        namespace ue4
        {
            constexpr uint32_t CLASS_FLAGS = 0xC8;
            constexpr uint32_t CLASS_CAST_FLAGS = 0xD0;
            constexpr uint32_t CLASS_DEFAULT_OBJECT = 0x108;
        }
        // UE5 approximate offsets
        namespace ue5
        {
            constexpr uint32_t CLASS_FLAGS = 0xC8;
            constexpr uint32_t CLASS_CAST_FLAGS = 0xD0;
            constexpr uint32_t CLASS_DEFAULT_OBJECT = 0x110;
        }
    }

    // EClassFlags
    enum EClassFlags : uint32_t
    {
        CLASS_None = 0x00000000,
        CLASS_Abstract = 0x00000001,
        CLASS_DefaultConfig = 0x00000002,
        CLASS_Config = 0x00000004,
        CLASS_Transient = 0x00000008,
        CLASS_Optional = 0x00000010,
        CLASS_MatchedSerializers = 0x00000020,
        CLASS_ProjectUserConfig = 0x00000040,
        CLASS_Native = 0x00000080,
        CLASS_NoExport = 0x00000100,
        CLASS_NotPlaceable = 0x00000200,
        CLASS_PerObjectConfig = 0x00000400,
        CLASS_ReplicationDataIsSetUp = 0x00000800,
        CLASS_EditInlineNew = 0x00001000,
        CLASS_CollapseCategories = 0x00002000,
        CLASS_Interface = 0x00004000,
        CLASS_CustomConstructor = 0x00008000,
        CLASS_Const = 0x00010000,
        CLASS_NeedsDeferredDependencyLoading = 0x00040000,
        CLASS_CompiledFromBlueprint = 0x00080000,
        CLASS_MinimalAPI = 0x00100000,
        CLASS_RequiredAPI = 0x00200000,
        CLASS_DefaultToInstanced = 0x00400000,
        CLASS_TokenStreamAssembled = 0x00800000,
        CLASS_HasInstancedReference = 0x01000000,
        CLASS_Hidden = 0x02000000,
        CLASS_Deprecated = 0x04000000,
        CLASS_HideDropDown = 0x08000000,
        CLASS_GlobalUserConfig = 0x10000000,
        CLASS_Intrinsic = 0x20000000,
        CLASS_Constructed = 0x40000000,
        CLASS_ConfigDoNotCheckDefaults = 0x80000000,
    };

    // EClassCastFlags — for fast type checking via masking
    enum EClassCastFlags : uint64_t
    {
        CASTCLASS_None = 0x0000000000000000,
        CASTCLASS_UField = 0x0000000000000001,
        CASTCLASS_FInt8Property = 0x0000000000000002,
        CASTCLASS_UEnum = 0x0000000000000004,
        CASTCLASS_UStruct = 0x0000000000000008,
        CASTCLASS_UScriptStruct = 0x0000000000000010,
        CASTCLASS_UClass = 0x0000000000000020,
        CASTCLASS_FByteProperty = 0x0000000000000040,
        CASTCLASS_FIntProperty = 0x0000000000000080,
        CASTCLASS_FFloatProperty = 0x0000000000000100,
        CASTCLASS_FUInt64Property = 0x0000000000000200,
        CASTCLASS_FClassProperty = 0x0000000000000400,
        CASTCLASS_FUInt32Property = 0x0000000000000800,
        CASTCLASS_FInterfaceProperty = 0x0000000000001000,
        CASTCLASS_FNameProperty = 0x0000000000002000,
        CASTCLASS_FStrProperty = 0x0000000000004000,
        CASTCLASS_FProperty = 0x0000000000008000,
        CASTCLASS_FObjectProperty = 0x0000000000010000,
        CASTCLASS_FBoolProperty = 0x0000000000020000,
        CASTCLASS_FUInt16Property = 0x0000000000040000,
        CASTCLASS_UFunction = 0x0000000000080000,
        CASTCLASS_FStructProperty = 0x0000000000100000,
        CASTCLASS_FArrayProperty = 0x0000000000200000,
        CASTCLASS_FInt64Property = 0x0000000000400000,
        CASTCLASS_FDelegateProperty = 0x0000000000800000,
        CASTCLASS_FNumericProperty = 0x0000000001000000,
        CASTCLASS_FMulticastDelegateProperty = 0x0000000002000000,
        CASTCLASS_FObjectPropertyBase = 0x0000000004000000,
        CASTCLASS_FWeakObjectProperty = 0x0000000008000000,
        CASTCLASS_FLazyObjectProperty = 0x0000000010000000,
        CASTCLASS_FSoftObjectProperty = 0x0000000020000000,
        CASTCLASS_FTextProperty = 0x0000000040000000,
        CASTCLASS_FInt16Property = 0x0000000080000000,
        CASTCLASS_FDoubleProperty = 0x0000000100000000,
        CASTCLASS_FSoftClassProperty = 0x0000000200000000,
        CASTCLASS_UPackage = 0x0000000400000000,
        CASTCLASS_ULevel = 0x0000000800000000,
        CASTCLASS_AActor = 0x0000001000000000,
        CASTCLASS_APlayerController = 0x0000002000000000,
        CASTCLASS_APawn = 0x0000004000000000,
        CASTCLASS_USceneComponent = 0x0000008000000000,
        CASTCLASS_UPrimitiveComponent = 0x0000010000000000,
        CASTCLASS_USkinnedMeshComponent = 0x0000020000000000,
        CASTCLASS_USkeletalMeshComponent = 0x0000040000000000,
        CASTCLASS_UBlueprint = 0x0000080000000000,
        CASTCLASS_UDelegateFunction = 0x0000100000000000,
        CASTCLASS_UStaticMeshComponent = 0x0000200000000000,
        CASTCLASS_FMapProperty = 0x0000400000000000,
        CASTCLASS_FSetProperty = 0x0000800000000000,
        CASTCLASS_FEnumProperty = 0x0001000000000000,
        CASTCLASS_USparseDelegateFunction = 0x0002000000000000,
        CASTCLASS_FMulticastInlineDelegateProperty = 0x0004000000000000,
        CASTCLASS_FMulticastSparseDelegateProperty = 0x0008000000000000,
        CASTCLASS_FFieldPathProperty = 0x0010000000000000,
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // UScriptStruct — ScriptStruct (used for blueprinted struct types)
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // class UScriptStruct : public UStruct
    // Adds:
    //   StructFlags (EStructFlags)
    //   bPrepareCppStructOps (bool)
    //   CppStructOps (UScriptStruct::ICppStructOps*)
    //
    // Not needed for offset access — we walk properties via UStruct's
    // ChildProperties chain. Documented for completeness.
    //

    // EStructFlags
    enum EStructFlags : uint32_t
    {
        STRUCT_NoFlags = 0x00000000,
        STRUCT_Native = 0x00000001,
        STRUCT_IdenticalNative = 0x00000002,
        STRUCT_HasInstancedReference = 0x00000004,
        STRUCT_NoExport = 0x00000008,
        STRUCT_Atomic = 0x00000010,
        STRUCT_Immutable = 0x00000020,
        STRUCT_AddStructReferencedObjects = 0x00000040,
        STRUCT_RequiredAPI = 0x00000200,
        STRUCT_NetSerializeNative = 0x00000400,
        STRUCT_SerializeNative = 0x00000800,
        STRUCT_CopyNative = 0x00001000,
        STRUCT_IsPlainOldData = 0x00002000,
        STRUCT_NoDestructor = 0x00004000,
        STRUCT_ZeroConstructor = 0x00008000,
        STRUCT_ExportTextItemNative = 0x00010000,
        STRUCT_ImportTextItemNative = 0x00020000,
        STRUCT_PostSerializeNative = 0x00040000,
        STRUCT_SerializeFromMismatchedTag = 0x00080000,
        STRUCT_NetDeltaSerializeNative = 0x00100000,
        STRUCT_PostScriptConstruct = 0x00200000,
        STRUCT_NetSharedSerialization = 0x00400000,
        STRUCT_Trashed = 0x00800000,
        STRUCT_NewerVersionExists = 0x01000000,
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // FName::Init function signature — creates an FName from a string
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Different calling conventions per version:
    //
    // UE4 (4.25/4.27):
    //   void FName::Init(const TCHAR* Name, int32 Number, EFindName FindType, bool bSplitName)
    //   ARM64 ABI: X0=this(FName*), X1=Name(char16_t*), X2=Number, X3=FindType, X4=bSplitName
    //
    // UE5 (5.0+):
    //   FName::FName(const TCHAR* Name, EFindName FindType = FNAME_Add)
    //   ARM64 ABI: X0=this(FName*), X1=Name(char16_t*), X2=FindType
    //   (Simpler interface — Number is handled differently)
    //
    // Both write to the FName at X0. We use the UE4 signature as it's a superset.
    //
    using FNameInitFn = void (*)(void *out_name, const char16_t *name, int32_t number,
                                 int32_t find_type, bool split_name);

    // ═══════════════════════════════════════════════════════════════════════════
    // EFindName — for FName construction
    // ═══════════════════════════════════════════════════════════════════════════
    enum EFindName : int32_t
    {
        FNAME_Find = 0,                           // Find only, don't create
        FNAME_Add = 1,                            // Find or add (create if not found)
        FNAME_Replace_Not_Safe_For_Threading = 2, // Legacy
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // FOutputDevice — logging output device (for Exec functions)
    // ═══════════════════════════════════════════════════════════════════════════
    struct FOutputDevice; // opaque — used as parameter in Exec() calls

    // ═══════════════════════════════════════════════════════════════════════════
    // Tick / Timer types
    // ═══════════════════════════════════════════════════════════════════════════
    struct FTimerHandle
    {
        uint64_t Handle;
    };

    struct FTimerDelegate
    {
        FWeakObjectPtr Object;
        uint8_t FunctionName[8]; // FName
    };

    // ═══════════════════════════════════════════════════════════════════════════
    // FHitResult — Physics hit result (used in traces/sweeps)
    // ═══════════════════════════════════════════════════════════════════════════
    // Very large struct (~200+ bytes). Accessed via reflection properties.
    // Listed for documentation only — never instantiated by us.
    namespace fhitresult
    {
        // Key properties (accessed via reflection):
        // - bBlockingHit (bool)
        // - bStartPenetrating (bool)
        // - Time (float, 0.0-1.0)
        // - Distance (float)
        // - Location (FVector)
        // - ImpactPoint (FVector)
        // - Normal (FVector)
        // - ImpactNormal (FVector)
        // - PhysMaterial (FWeakObjectPtr)
        // - Actor (FWeakObjectPtr)
        // - Component (FWeakObjectPtr)
        // - BoneName (FName)
        // - FaceIndex (int32)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Slate/Widget Layout Types
    // ═══════════════════════════════════════════════════════════════════════════

    // FMargin — padding for widgets
    struct FMargin
    {
        float Left, Top, Right, Bottom;
    };
    static_assert(sizeof(FMargin) == 16, "FMargin must be 16 bytes");

    // FAnchors — widget anchoring
    struct FAnchors
    {
        FVector2D Minimum;
        FVector2D Maximum;
    };
    static_assert(sizeof(FAnchors) == 16, "FAnchors must be 16 bytes");

    // FSlateColor — color wrapper for Slate
    struct FSlateColor
    {
        FLinearColor SpecifiedColor;
        // + rule enum, etc. — accessed via reflection
    };

} // namespace ue
