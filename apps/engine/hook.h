// GAGE-MP in-game engine: game hook primitives.
//
// The game (GTA5.exe) is the module we are injected into, so GetModuleHandleW
// (nullptr) is the game image. Two mechanisms drive the multiplayer session:
//
//   1) D3D11 Present hook (build-independent): a dummy D3D11 device yields the
//      Present function address in dxgi.dll; an inline E9 detour runs onTick
//      every frame on the game's main/render thread.
//
//   2) RAGE native invocation: a single byte-signature locates the 256-bucket
//      XOR-obfuscated native registration table. Every game primitive (ped
//      spawn, position, heading, model loading, entity lifecycle) is called by
//      its 64-bit native hash through scrNativeCallContext. No per-primitive
//      signatures are needed.
//
// Each mechanism degrades independently: if D3D11 Present is not found the tick
// hook is not installed; if the native table is not found the entity API returns
// zero/false. Nothing aborts the engine.
#pragma once
#include "common/common.h"
#include "ambient.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace gage::game {

    // -----------------------------------------------------------------------
    // FloatArg: encode a float as a uint64_t for native argument passing.
    // The game's native handlers read floats from the low 4 bytes of each
    // 64-bit argument slot.
    // -----------------------------------------------------------------------
    inline uint64_t FloatArg(float f) {
        uint64_t v = 0;
        memcpy(&v, &f, 4);
        return v;
    }

    // -----------------------------------------------------------------------
    // GTA V native hashes: the public NativeDB values. Build 1.0.3889 registers
    // most natives under re-hashed values; CallNative maps each of these to the
    // 3889 hash (kCrossmap3889 in hook.cpp). Only the natives the engine uses.
    // -----------------------------------------------------------------------
    namespace native {
        // ped & player
        constexpr uint64_t PLAYER_PED_ID                       = 0xD80958FC74E988A6;
        constexpr uint64_t PLAYER_ID                           = 0x4F8644AF03D0E0D6;
        constexpr uint64_t SET_PLAYER_MODEL                    = 0x00A1CADD00108836;
        constexpr uint64_t SET_MAX_WANTED_LEVEL                = 0xAA5F02DB48D704B9;
        constexpr uint64_t CLEAR_PLAYER_WANTED_LEVEL           = 0xB302540597885499;
        constexpr uint64_t CREATE_PED                          = 0xD49F9B0955C367DE;
        constexpr uint64_t SET_PED_DEFAULT_COMPONENT_VARIATION = 0x45EEE61580806D63;
        constexpr uint64_t IS_PED_DEAD_OR_DYING                = 0x3317DEDB88C95038;

        // a freemode ped's look (ApplyPedOutfit): validate then set each component, and the head blend
        // (face + skin tone). Public hashes from Mr-X-GTA/YimMenu scripts/natives.json; the 3889 values
        // are in kCrossmap3889 (hook.cpp).
        constexpr uint64_t IS_PED_COMPONENT_VARIATION_VALID    = 0xE825F6B6CEA7671D;
        constexpr uint64_t SET_PED_COMPONENT_VARIATION         = 0x262B14F48D29DE80;
        constexpr uint64_t SET_PED_HEAD_BLEND_DATA             = 0x9414E18B9434C2FE;

        // entity coords & heading
        constexpr uint64_t GET_ENTITY_COORDS                   = 0x3FEF770D40960D5A;
        constexpr uint64_t SET_ENTITY_COORDS                   = 0x06843DA7060A026B;
        constexpr uint64_t GET_ENTITY_HEADING                  = 0xE83D4F9BA2A38914;
        constexpr uint64_t SET_ENTITY_HEADING                  = 0x8E2530AA8ADA980E;

        // entity lifecycle & flags
        constexpr uint64_t DELETE_ENTITY                       = 0xAE3CBE5BF394C9C9;
        constexpr uint64_t SET_ENTITY_VISIBLE                  = 0xEA1C610A04DB6BBB;
        constexpr uint64_t SET_ENTITY_INVINCIBLE               = 0x3882114BDE571AD4;
        constexpr uint64_t FREEZE_ENTITY_POSITION              = 0x428CA6DBD1094446;

        // model streaming
        constexpr uint64_t REQUEST_MODEL                       = 0x963D27A58DF860AC;
        constexpr uint64_t HAS_MODEL_LOADED                    = 0x98A4EB5D89A0C952;
        constexpr uint64_t SET_MODEL_AS_NO_LONGER_NEEDED       = 0xE532F5D78798DAAB;

        // singleplayer system suppression
        constexpr uint64_t SET_RANDOM_EVENT_FLAG               = 0x971927086CFD2158;
        constexpr uint64_t SET_RANDOM_TRAINS                   = 0x80D9F74197EA47D9;
        constexpr uint64_t SET_GARBAGE_TRUCKS                  = 0x2AFD795EEAC8D30D;
        constexpr uint64_t SET_CREATE_RANDOM_COPS              = 0x102E68B2024D536D;

        // story-mode evasion: dispatch/cops, character wheel, HUD, and autosave blocking.
        // (SET_MISSION_FLAG(true) stops the game saving the multiplayer character into the
        // player's singleplayer savegame.)
        constexpr uint64_t SET_MISSION_FLAG                        = 0xC4301E5121A0ED73;
        constexpr uint64_t CLEAR_CODE_REQUESTED_AUTOSAVE          = 0x06462A961E94B67C;
        constexpr uint64_t TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME   = 0x9DC711BC69C548DF;
        constexpr uint64_t ENABLE_DISPATCH_SERVICE               = 0xDC0F817884CDD856;
        constexpr uint64_t SET_CREATE_RANDOM_COPS_NOT_ON_SCENARIOS = 0x8A4986851C4EF6E7;
        constexpr uint64_t SET_CREATE_RANDOM_COPS_ON_SCENARIOS    = 0x444CB7D7DBE6973D;
        constexpr uint64_t SET_DISPATCH_COPS_FOR_PLAYER          = 0xDB172424876553F4;
        constexpr uint64_t SET_POLICE_IGNORE_PLAYER              = 0x32C62AA929C2DA6A;
        constexpr uint64_t SET_WANTED_LEVEL_MULTIPLIER           = 0x020E5F00CDA207BA;
        constexpr uint64_t SET_PLAYER_CAN_BE_HASSLED_BY_GANGS    = 0xD5E460AD7020A246;
        constexpr uint64_t SET_POLICE_RADAR_BLIPS                = 0x43286D561B72B8BF;
        constexpr uint64_t DISABLE_CONTROL_ACTION                = 0xFE99B66D079CF6BC;
        constexpr uint64_t DISPLAY_RADAR                         = 0xA0EBB943C300E693;
        constexpr uint64_t DISPLAY_HUD                           = 0xA6294919E56FF02A;
        constexpr uint64_t GET_PLAYER_WANTED_LEVEL               = 0xE28E54788CE8F12D;
        constexpr uint64_t DISABLE_PLAYER_VEHICLE_REWARDS        = 0xC142BE3BB9CE125F;

        // self-driven respawn (0.1.53): the stock respawn_controller's hospital scene bails
        // for a freemode ped and never re-enables the post-death fade-in, so the screen stays
        // black after death. The engine drives its own MP-style respawn with these instead.
        constexpr uint64_t PAUSE_DEATH_ARREST_RESTART         = 0x2C2B3493FBF51C71;
        constexpr uint64_t IGNORE_NEXT_RESTART                = 0x21FFB63D8C615361;
        constexpr uint64_t SET_FADE_OUT_AFTER_DEATH           = 0x4A18E01DF2C87B86;
        constexpr uint64_t SET_FADE_OUT_AFTER_ARREST          = 0x1E0B4DC0D990A4E7;
        constexpr uint64_t SET_FADE_IN_AFTER_DEATH_ARREST     = 0xDA66D2796BA33F12;
        constexpr uint64_t FORCE_GAME_STATE_PLAYING           = 0xC0AA53F866B3134D;
        constexpr uint64_t RESET_PLAYER_ARREST_STATE          = 0x2D03E13C460760D6;
        constexpr uint64_t RESURRECT_PED                      = 0x71BC8E838B9C6035;
        constexpr uint64_t SET_ENTITY_HEALTH                  = 0x6B76DC1F3AE6E6A3;
        constexpr uint64_t GET_ENTITY_MAX_HEALTH              = 0x15D757606D170C3C;
        constexpr uint64_t CLEAR_PED_TASKS_IMMEDIATELY        = 0xAAA34F8A7CB32098;
        constexpr uint64_t CLEAR_PED_BLOOD_DAMAGE             = 0x8FE22675A5A45817;
        constexpr uint64_t RESET_PED_VISIBLE_DAMAGE           = 0x3AC1F7B898F30C05;
        constexpr uint64_t IS_SCREEN_FADED_OUT                = 0xB16FCE9DDC7BA182;
        constexpr uint64_t DO_SCREEN_FADE_IN                  = 0xD4E8E24955024033;
        constexpr uint64_t DO_SCREEN_FADE_OUT                 = 0x891B5B39AC6302AF;
        constexpr uint64_t SET_TIME_SCALE                     = 0x1D408577D440E81E;
        constexpr uint64_t ANIMPOSTFX_STOP_ALL                = 0xB4EDDC19532BFB85;
        constexpr uint64_t SET_CAM_DEATH_FAIL_EFFECT_STATE    = 0x80C8B1846639BB19;
        constexpr uint64_t STOP_AUDIO_SCENE                   = 0xDFE8422B3B94E688;
        constexpr uint64_t REQUEST_COLLISION_AT_COORD         = 0x07503F7948F491A7;
        constexpr uint64_t HAS_COLLISION_LOADED_AROUND_ENTITY = 0xE9676F61BC0B3321;

        // clean no-Prologue boot: with the SP mission flow killed, GTA never dismisses its own
        // loading screen or hands control to the player, so the engine does both itself at the reveal.
        constexpr uint64_t SHUTDOWN_LOADING_SCREEN            = 0x078EBE9809CCD637;
        constexpr uint64_t SET_PLAYER_CONTROL                 = 0x8D32347D6D4C40A2;

        // HUD bars (0.1.54): a 2D rect in screen space, and the ped's current health.
        constexpr uint64_t DRAW_RECT                          = 0x3A618A217E5154F0;
        constexpr uint64_t GET_ENTITY_HEALTH                  = 0xEEF059FAD016D209;

        // client-side commands (0.1.54): /car /gun /fix spawn vehicles, give weapons,
        // repair, by hashing a model/weapon name and streaming the model.
        constexpr uint64_t GET_HASH_KEY                       = 0xD24D37CC275948CC;
        constexpr uint64_t IS_MODEL_IN_CDIMAGE                = 0x35B9E0803292B641;
        constexpr uint64_t IS_MODEL_A_VEHICLE                 = 0x19AAC8F07BFEC53E;
        constexpr uint64_t CREATE_VEHICLE                     = 0xAF35D0D2583051B0;
        constexpr uint64_t SET_PED_INTO_VEHICLE               = 0xF75B0D629E1C063D;
        constexpr uint64_t SET_VEHICLE_ON_GROUND_PROPERLY     = 0x49733E92263139D1;
        constexpr uint64_t SET_VEHICLE_ENGINE_ON              = 0x2497C4717C8B881E;
        constexpr uint64_t GET_VEHICLE_PED_IS_IN              = 0x9A9112A0FE9A4713;
        constexpr uint64_t SET_VEHICLE_FIXED                  = 0x115722B1B9C14C1C;
        constexpr uint64_t SET_VEHICLE_DEFORMATION_FIXED      = 0x953DA1E1B12C0491;
        constexpr uint64_t GIVE_WEAPON_TO_PED                 = 0xBF0FD6E56C964FCB;

        // speedometer (0.1.55): shown only while in a vehicle (see the in-game overlay).
        constexpr uint64_t IS_PED_IN_ANY_VEHICLE              = 0x997ABD671D25CA0B;
        constexpr uint64_t GET_ENTITY_SPEED                   = 0xD5037BA82E12416F;

        // notifications (0.2.0): hide GTA's phone/SMS/notification toasts (the feed).
        constexpr uint64_t THEFEED_HIDE_THIS_FRAME            = 0x25F87B30C382FCA7;
        constexpr uint64_t THEFEED_FLUSH_QUEUE                = 0xA8FDB297A8D25FBA;

        // world clean-up: ambient peds, traffic, parked cars and boats, and clearing an area of
        // them. The *_THIS_FRAME multipliers last one frame (call them every script tick).
        constexpr uint64_t SET_PED_DENSITY_MULTIPLIER_THIS_FRAME            = 0x95E3D6257B166CF2;
        constexpr uint64_t SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME   = 0x7A556143A1C03898;
        constexpr uint64_t SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME        = 0x245A6883D966D537;
        constexpr uint64_t SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME = 0xB3B3359379FE77D3;
        constexpr uint64_t SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME = 0xEAE6DCC7EEE3DB1D;
        constexpr uint64_t SET_NUMBER_OF_PARKED_VEHICLES                    = 0xCAA15F13EBD417FF;
        constexpr uint64_t SET_RANDOM_BOATS                                 = 0x84436EC293B1415F;
        constexpr uint64_t CLEAR_AREA_OF_PEDS                               = 0xBE31FD6CE464AC59;
        constexpr uint64_t CLEAR_AREA_OF_COPS                               = 0x04F8FC8FCF58F88D;
        constexpr uint64_t REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA          = 0x46A1E1A299EC4BBA;
        // T5 ambient sweep natives, resolved for 0.4.7 (public hash = alloc8or native DB; 3889 hash = kCrossmap3889 row, found by
        // the lookup tool and confirmed by two crossmaps). The wrappers still treat a 0 constant as UNRESOLVED (inert, logged once),
        // so a native that ever has to be pulled again is switched off by setting it back to 0. Never fill one in from memory.
        //   SET_PED_POPULATION_BUDGET(int budget)                             0 = no ambient peds, persistent
        //   SET_VEHICLE_POPULATION_BUDGET(int budget)                         0 = no ambient vehicles, persistent
        //   SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA(float x1, y1, z1, x2, y2, z2, BOOL toggle, BOOL p7)
        //   DELETE_ALL_TRAINS()                                               trains that already exist
        //   GET_PED_NEARBY_PEDS(Ped ped, int* sizeAndPeds, int ignore)        returns the count; slot 0 = capacity in
        //   GET_PED_NEARBY_VEHICLES(Ped ped, int* sizeAndVehs)                returns the count; slot 0 = capacity in
        constexpr uint64_t SET_PED_POPULATION_BUDGET                        = 0x8C95333CFC3340F3;
        constexpr uint64_t SET_VEHICLE_POPULATION_BUDGET                    = 0xCB9E1EB3BE2AF4E9;
        constexpr uint64_t SET_ALL_VEHICLE_GENERATORS_ACTIVE_IN_AREA        = 0xC12321827687FE4D;
        constexpr uint64_t DELETE_ALL_TRAINS                                = 0x736A718577F39C7D;
        constexpr uint64_t GET_PED_NEARBY_PEDS                              = 0x23F8F5FC7E8C4A6B;
        constexpr uint64_t GET_PED_NEARBY_VEHICLES                          = 0xCFF869CBFA210D82;

        // join readiness gate (loading screen, fade, player switch, control) and model verify
        constexpr uint64_t GET_IS_LOADING_SCREEN_ACTIVE                     = 0x10D0A8F259E93EC9;
        constexpr uint64_t IS_SCREEN_FADED_IN                               = 0x5A859503B0C08678;
        constexpr uint64_t IS_PLAYER_SWITCH_IN_PROGRESS                     = 0xD9D2CFFF49FAB35F;
        constexpr uint64_t IS_PLAYER_CONTROL_ON                             = 0x49C32D60007AFA47;
        constexpr uint64_t GET_ENTITY_MODEL                                 = 0x9F47B058362C84B5;

        // blips: strip the default singleplayer shop/property blips off the map.
        constexpr uint64_t GET_FIRST_BLIP_INFO_ID                           = 0x1BEDE233E6CD2A1F;
        constexpr uint64_t GET_NEXT_BLIP_INFO_ID                            = 0x14F96AA50D6FBEA7;
        constexpr uint64_t GET_BLIP_SPRITE                                  = 0x1FC877464A04FC4F;
        constexpr uint64_t REMOVE_BLIP                                      = 0x86A652570E5F25DD;
        constexpr uint64_t DOES_BLIP_EXIST                                  = 0xA6DB27D19ECBB7DA;

        // remote-player ped driving (GTA Network / RAGECOOP style: a locomotion task, a per-frame
        // velocity steer and a rare hard snap), the local-state readers the sender needs, and the
        // appearance read/apply. Each also has a kCrossmap3889 row (hook.cpp).
        constexpr uint64_t SET_ENTITY_COORDS_NO_OFFSET            = 0x239A3351AC1DA385;
        constexpr uint64_t SET_ENTITY_VELOCITY                    = 0x1C99BB7B6E96D16F;
        constexpr uint64_t GET_ENTITY_VELOCITY                    = 0x4805D2B1D8CF94A9;
        constexpr uint64_t SET_ENTITY_COLLISION                   = 0x1A9205C1B9EE827F;
        constexpr uint64_t GET_ENTITY_HEIGHT_ABOVE_GROUND         = 0x1DD55701034110E5;
        constexpr uint64_t GET_GROUND_Z_FOR_3D_COORD              = 0xC906A7DAB05C8D2B;
        constexpr uint64_t TASK_GO_STRAIGHT_TO_COORD              = 0xD76B57B44F1E6F8B;
        constexpr uint64_t TASK_STAND_STILL                       = 0x919BE13EED931959;
        constexpr uint64_t CLEAR_PED_TASKS                        = 0xE1EF3C1216AFF2CD;
        constexpr uint64_t SET_PED_DESIRED_MOVE_BLEND_RATIO       = 0x1E982AC8716912C5;
        constexpr uint64_t SET_PED_DESIRED_HEADING                = 0xAA5A7ECE2AA8FE70;
        constexpr uint64_t IS_PED_WALKING                         = 0xDE4C184B2B9B071A;
        constexpr uint64_t IS_PED_RUNNING                         = 0xC5286FFC176F28A2;
        constexpr uint64_t IS_PED_SPRINTING                       = 0x57E457CD2C0FC168;
        constexpr uint64_t IS_PED_STILL                           = 0xAC29253EEF8F0180;
        constexpr uint64_t IS_PED_JUMPING                         = 0xCEDABC5900A0BF97;
        constexpr uint64_t TASK_JUMP                              = 0x0AE4086104E067B1;
        constexpr uint64_t IS_PED_RAGDOLL                         = 0x47E4E977581C5B55;
        constexpr uint64_t SET_PED_TO_RAGDOLL                     = 0xAE99FB955581844A;
        constexpr uint64_t SET_PED_CAN_RAGDOLL                    = 0xB128377056A54E2A;
        constexpr uint64_t SET_PED_CAN_EVASIVE_DIVE               = 0x6B7A646C242A7059;
        constexpr uint64_t SET_BLOCKING_OF_NON_TEMPORARY_EVENTS   = 0x9F8AA94D6D97DBF4;
        constexpr uint64_t SET_PED_CONFIG_FLAG                    = 0x1913FE4CBF41C463;
        constexpr uint64_t SET_PED_KEEP_TASK                      = 0x971D38760FBC02EF;
        constexpr uint64_t SET_PED_CAN_PLAY_AMBIENT_ANIMS         = 0x6373D1349925A70E;
        constexpr uint64_t IS_PED_FALLING                         = 0xFB92A102F1C4DFA3;
        constexpr uint64_t IS_PED_CLIMBING                        = 0x53E8CB4F48BFE623;
        constexpr uint64_t IS_PED_IN_PARACHUTE_FREE_FALL          = 0x7DCE8BDA0F1C1200;
        constexpr uint64_t IS_PED_SHOOTING                        = 0x34616828CD07F1A1;
        constexpr uint64_t IS_PLAYER_FREE_AIMING                  = 0x2E397FD2ECD37C87;
        constexpr uint64_t GET_SELECTED_PED_WEAPON                = 0x0A6DB4965674D243;
        constexpr uint64_t IS_WEAPON_VALID                        = 0x937C71165CF334B3;
        constexpr uint64_t SET_CURRENT_PED_WEAPON                 = 0xADF692B254977C0C;
        constexpr uint64_t TASK_AIM_GUN_AT_COORD                  = 0x6671F3EEC681BDA1;
        constexpr uint64_t TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD = 0x11315AB3385B8AC0;
        constexpr uint64_t GET_PED_DRAWABLE_VARIATION             = 0x67F3780DD425D4FC;
        constexpr uint64_t GET_PED_TEXTURE_VARIATION              = 0x04A355E041E004E6;
        constexpr uint64_t GET_PED_PALETTE_VARIATION              = 0xE3DD5F2A84B42281;
        constexpr uint64_t GET_PED_PROP_INDEX                     = 0x898CC20EA75BACD8;
        constexpr uint64_t GET_PED_PROP_TEXTURE_INDEX             = 0xE131A28626F81AB2;
        constexpr uint64_t SET_PED_PROP_INDEX                     = 0x93376B65A266EB5F;
        constexpr uint64_t CLEAR_PED_PROP                         = 0x0943E5B8E078E76E;
        constexpr uint64_t GET_PED_HEAD_BLEND_DATA                = 0x2746BD9D88C5C5D0;

        // vehicle sync: orientation, seats, enter/leave tasks, health, siren/horn/colours/locks,
        // ownership. Public hashes from alloc8or natives.json; each kCrossmap3889 row AGREES with
        // YimMenuV2 `enhanced` by name (scratchpad peddrive_tool/lookup.mjs).
        constexpr uint64_t GET_ENTITY_QUATERNION                  = 0x7B3703D2D32DFA18;
        constexpr uint64_t SET_ENTITY_QUATERNION                  = 0x77B21BE7AC540F07;
        constexpr uint64_t GET_ENTITY_ROTATION                    = 0xAFBD61CC738D9EB9;
        constexpr uint64_t SET_ENTITY_ROTATION                    = 0x8524A8B0171D5E07;
        constexpr uint64_t GET_ENTITY_ROTATION_VELOCITY           = 0x213B91045D09B983;
        constexpr uint64_t SET_ENTITY_ANGULAR_VELOCITY            = 0x8339643499D1222E;
        constexpr uint64_t GET_PED_IN_VEHICLE_SEAT                = 0xBB40DD2270B65366;
        constexpr uint64_t GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS   = 0xA7C4F2C6E744A550;
        constexpr uint64_t IS_VEHICLE_SEAT_FREE                   = 0x22AC59A870E6A669;
        constexpr uint64_t GET_SEAT_PED_IS_TRYING_TO_ENTER        = 0x6F4C85ACD641BCD2;
        constexpr uint64_t GET_VEHICLE_PED_IS_TRYING_TO_ENTER     = 0x814FA8BE5449445D;
        constexpr uint64_t GET_VEHICLE_PED_IS_USING               = 0x6094AD011A2EA87D;
        constexpr uint64_t TASK_ENTER_VEHICLE                     = 0xC20E50AA46D09CA8;
        constexpr uint64_t TASK_LEAVE_VEHICLE                     = 0xD3DBCE61A490BE02;
        constexpr uint64_t TASK_WARP_PED_INTO_VEHICLE             = 0x9A7D091411C5F684;
        constexpr uint64_t SET_VEHICLE_STEER_BIAS                 = 0x42A8EC77D5150CBE;
        constexpr uint64_t GET_VEHICLE_ENGINE_HEALTH              = 0xC45D23BAF168AAB8;
        constexpr uint64_t SET_VEHICLE_ENGINE_HEALTH              = 0x45F6D8EEF34ABEF1;
        constexpr uint64_t GET_VEHICLE_BODY_HEALTH                = 0xF271147EB7B40F12;
        constexpr uint64_t SET_VEHICLE_BODY_HEALTH                = 0xB77D05AC8C78AADB;
        constexpr uint64_t GET_IS_VEHICLE_ENGINE_RUNNING          = 0xAE31E7DF9B5B132E;
        constexpr uint64_t SET_VEHICLE_SIREN                      = 0xF4924635A19EB37D;
        constexpr uint64_t IS_VEHICLE_SIREN_ON                    = 0x4C9BF537BE2634B2;
        constexpr uint64_t START_VEHICLE_HORN                     = 0x9C8C6504B5B63D2C;
        constexpr uint64_t GET_VEHICLE_COLOURS                    = 0xA19435F193E081AC;
        constexpr uint64_t SET_VEHICLE_COLOURS                    = 0x4F1D4BE3A7F24601;
        constexpr uint64_t SET_VEHICLE_DOORS_LOCKED               = 0xB664292EAECF7FA6;
        constexpr uint64_t SET_VEHICLE_LIGHTS                     = 0x34E710FF01247C5A;
        constexpr uint64_t SET_VEHICLE_FORWARD_SPEED              = 0xAB54A438726D25D5;
        constexpr uint64_t SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER   = 0x2B5F9D2AF1F1722D;
        constexpr uint64_t SET_VEHICLE_IS_STOLEN                  = 0x67B2C79AA7FF5738;
        constexpr uint64_t SET_ENTITY_AS_MISSION_ENTITY           = 0xAD738C3085FE7E11;
        constexpr uint64_t SET_VEHICLE_DOOR_SHUT                  = 0x93D9BD300D7789E5;

        // weapons, shots and damage (same sourcing as above).
        constexpr uint64_t SHOOT_SINGLE_BULLET_BETWEEN_COORDS     = 0x867654CBC7606F2C;
        constexpr uint64_t SET_PED_SHOOTS_AT_COORD                = 0x96A05E4FB321B1BA;
        constexpr uint64_t TASK_SHOOT_AT_COORD                    = 0x46A6CC01E0826106;
        constexpr uint64_t GET_PED_LAST_WEAPON_IMPACT_COORD       = 0x6C4D0409BA1A2BC2;
        constexpr uint64_t HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY      = 0xC86D67D52A707CF8;
        constexpr uint64_t HAS_ENTITY_BEEN_DAMAGED_BY_ANY_PED     = 0x605F5A140F202491;
        constexpr uint64_t CLEAR_ENTITY_LAST_DAMAGE_ENTITY        = 0xA72CD9CA74A5ECBA;
        constexpr uint64_t GET_PED_LAST_DAMAGE_BONE               = 0xD75960F6BD9EA49C;
        constexpr uint64_t CLEAR_PED_LAST_DAMAGE_BONE             = 0x8EF6B7AC68E2F01B;
        constexpr uint64_t GET_PED_ARMOUR                         = 0x9483AF821605B1D8;
        constexpr uint64_t SET_PED_ARMOUR                         = 0xCEA04D83135264CC;
        constexpr uint64_t GET_WEAPON_DAMAGE                      = 0x3133B907D8B32053;
        constexpr uint64_t GET_PED_SOURCE_OF_DEATH                = 0x93C8B64DEB84728C;
        constexpr uint64_t IS_ENTITY_DEAD                         = 0x5F9532F3B5CC2551;
        constexpr uint64_t SET_ENTITY_CAN_BE_DAMAGED              = 0x1760FFA8AB074D66;
        constexpr uint64_t SET_ENTITY_PROOFS                      = 0xFAEE099C6F890BB8;
        constexpr uint64_t SET_PED_SUFFERS_CRITICAL_HITS          = 0xEBD76F2359F190AC;
        constexpr uint64_t APPLY_DAMAGE_TO_PED                    = 0x697157CED63F18D4;
        constexpr uint64_t GET_CURRENT_PED_WEAPON_ENTITY_INDEX    = 0x3B390A939AF0B5FC;
        constexpr uint64_t GET_ENTITY_BONE_INDEX_BY_NAME          = 0xFB71170B7E76ACBA;
        constexpr uint64_t GET_WORLD_POSITION_OF_ENTITY_BONE      = 0x44A8FCB8ED227738;
        constexpr uint64_t GET_AMMO_IN_PED_WEAPON                 = 0x015A522136D7F951;
        constexpr uint64_t SET_PED_INFINITE_AMMO_CLIP             = 0x183DADC6AA953186;

        // camera / aim and the synchronous line-of-sight probe (same sourcing as above).
        constexpr uint64_t GET_GAMEPLAY_CAM_COORD                 = 0x14D6F5678D8F1B37;
        constexpr uint64_t GET_GAMEPLAY_CAM_ROT                   = 0x837765A25378F0BB;
        constexpr uint64_t START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE = 0x377906D8A31E5586;
        constexpr uint64_t GET_SHAPE_TEST_RESULT                  = 0x3D87450E15D98694;
        constexpr uint64_t IS_AIM_CAM_ACTIVE                      = 0x68EDDA28A5976D07;
        constexpr uint64_t GET_ENTITY_PLAYER_IS_FREE_AIMING_AT    = 0x2975C866E6713290;

        // network-state baseline (net_probe=1, G0): read-only getters sampled after the entry.
        // Public hashes from alloc8or natives.json; each kCrossmap3889 row AGREES with YimMenuV2
        // `enhanced` by name (scratchpad peddrive_tool/lookup.mjs).
        constexpr uint64_t NETWORK_IS_SIGNED_IN                   = 0x054354A99211EB96;
        constexpr uint64_t NETWORK_IS_SIGNED_ONLINE               = 0x1077788E268557C2;
        constexpr uint64_t NETWORK_HAVE_ONLINE_PRIVILEGES         = 0x25CB5A9F37BFD063;
        constexpr uint64_t NETWORK_IS_CLOUD_AVAILABLE             = 0x9A4CF4F48AD77302;
        constexpr uint64_t NETWORK_IS_SESSION_STARTED             = 0x9DE624D2FC4B603F;
        constexpr uint64_t NETWORK_IS_IN_SESSION                  = 0xCA97246103B63917;

        // protocol v4 (emotes, server-owned health, vehicle state). Public hashes from alloc8or
        // natives.json; each kCrossmap3889 row AGREES with YimMenuV2 `enhanced` by name (scratchpad
        // peddrive_tool/lookup.mjs, checked with hookagent_verify/verify_rows.mjs).
        constexpr uint64_t REQUEST_ANIM_DICT                      = 0xD3BD40951412FEF6;
        constexpr uint64_t HAS_ANIM_DICT_LOADED                   = 0xD031A9162D01088C;
        constexpr uint64_t REMOVE_ANIM_DICT                       = 0xF66A602F829E2A06;
        constexpr uint64_t DOES_ANIM_DICT_EXIST                   = 0x2DA49C3B79856961;
        constexpr uint64_t TASK_PLAY_ANIM                         = 0xEA47FE3719165B94;
        constexpr uint64_t TASK_START_SCENARIO_IN_PLACE           = 0x142A02425FF02BD9;
        constexpr uint64_t IS_ENTITY_PLAYING_ANIM                 = 0x1F0B79228E461EC9;
        constexpr uint64_t IS_PED_USING_ANY_SCENARIO              = 0x57AB4A3080F85143;
        constexpr uint64_t SET_ENTITY_MAX_HEALTH                  = 0x166E7CF68597D8B5;
        constexpr uint64_t DOES_ENTITY_EXIST                      = 0x7239B21A38F536BA;
        constexpr uint64_t IS_HORN_ACTIVE                         = 0x9D6BFC12B05C6121;
        constexpr uint64_t SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER  = 0x5DB660B38DD98A31;

        // the no-pause pause menu, the game timer for its proof line, and the weapon group (a thrown
        // weapon is never played as a bullet). Public hashes from alloc8or natives.json; every
        // kCrossmap3889 row AGREES with YimMenuV2 `enhanced` by name (peddrive_tool/lookup.mjs,
        // checked with hookagent_verify/verify_rows.mjs).
        constexpr uint64_t ACTIVATE_FRONTEND_MENU                 = 0xEF01D36B9C9D0C7B;
        constexpr uint64_t IS_PAUSE_MENU_ACTIVE                   = 0xB0034A223497FFCB;
        constexpr uint64_t IS_DISABLED_CONTROL_JUST_PRESSED       = 0x91AEF906BCA88877;
        constexpr uint64_t DISABLE_FRONTEND_THIS_FRAME            = 0x6D3465A73092F0E6;
        constexpr uint64_t SET_PAUSE_MENU_ACTIVE                  = 0xDF47FC56C71569CF;
        constexpr uint64_t GET_GAME_TIMER                         = 0x9CD27B0045628463;
        constexpr uint64_t GET_WEAPONTYPE_GROUP                   = 0xC3287EE3050FB74C;

        // sync plan W0 (docs/SYNC-PLAN.md "Natives"): on-foot reload/stealth/swim/cover/melee/move
        // rate, vehicle lights/doors/roof, drive-by tasks, particle fx. Public hashes from alloc8or
        // natives.json; every kCrossmap3889 row is the YimMenu crossmap entry at the natives.hpp
        // index and AGREES with YimMenuV2 `enhanced` by name. Not live-checked: logNewNativeResolution
        // reports them.
        constexpr uint64_t IS_PED_RELOADING                            = 0x24B100C68C645951;
        constexpr uint64_t TASK_RELOAD_WEAPON                          = 0x62D2916F56B9CD2D;
        constexpr uint64_t GET_PED_STEALTH_MOVEMENT                    = 0x7C2AC9CA66575FBF;
        constexpr uint64_t SET_PED_STEALTH_MOVEMENT                    = 0x88CBB5CEB96B7BD2;
        constexpr uint64_t IS_PED_SWIMMING                             = 0x9DE327631295B4C2;
        constexpr uint64_t IS_PED_IN_COVER                             = 0x60DFD0691A170B88;
        constexpr uint64_t TASK_PUT_PED_DIRECTLY_INTO_COVER            = 0x4172393E6BE1FECE;
        constexpr uint64_t IS_PED_PERFORMING_MELEE_ACTION              = 0xDCCA191DF9980FD7;
        constexpr uint64_t SET_PED_MOVE_RATE_OVERRIDE                  = 0x085BF80FA50A39D1;
        // 0.4.3 fixes (W8). Public hash here; the 3889 hash is in kCrossmap3889 (hook.cpp). Sourcing: the lookup tool's
        // alloc8or (public) and yim3889 + v2enh (3889, agreeing).
        //   IS_PED_VAULTING                      public 0x117C70D1F5730B5E  3889 0x75B105C651D87D0E  BOOL (Ped)
        //   TASK_CLIMB                           public 0x89D9FCC2435112F1  3889 0x345C12E1D643634F  void (Ped, BOOL usePlayerLaunchForce)
        //   SET_AI_WEAPON_DAMAGE_MODIFIER        public 0x1B1E2A40A65B8521  3889 0x36978A351E9CB5D8  void (float)
        //   SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER  public 0x66460DEDDD417254  3889 0x767EC66D239AED1D  void (float)
        //   RESET_AI_WEAPON_DAMAGE_MODIFIER      public 0xEA16670E7BA4743C  3889 0xDD84F9C956E69AC8  void ()
        //   RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER public 0x46E56A7CD1D63C3F 3889 0x01B722A2B2026558  void ()
        //   WAS_PED_KILLED_BY_TAKEDOWN           public 0x7F08E26039C7347C  3889 0xD9F6166CE6292D26  BOOL (Ped)   (diagnostic only)
        constexpr uint64_t IS_PED_VAULTING                             = 0x117C70D1F5730B5E;
        constexpr uint64_t TASK_CLIMB                                  = 0x89D9FCC2435112F1;
        constexpr uint64_t SET_AI_WEAPON_DAMAGE_MODIFIER               = 0x1B1E2A40A65B8521;
        constexpr uint64_t SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER         = 0x66460DEDDD417254;
        constexpr uint64_t RESET_AI_WEAPON_DAMAGE_MODIFIER             = 0xEA16670E7BA4743C;
        constexpr uint64_t RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER       = 0x46E56A7CD1D63C3F;
        constexpr uint64_t WAS_PED_KILLED_BY_TAKEDOWN                  = 0x7F08E26039C7347C;
        // Hyper tune (GI 2026-11-02, libs/sync/hypertune.h); lookup tool, yim3889 and v2enh agreeing:
        //   MODIFY_VEHICLE_TOP_SPEED             public 0x93A3996368C94158  3889 0xAC89BB42FE09CC80  void (Vehicle, float)
        //   SET_VEHICLE_CHEAT_POWER_INCREASE     public 0xB59E4BD37AE292DB  3889 0xF1C985BBEC6B6321  void (Vehicle, float)
        //   SET_VEHICLE_MAX_SPEED                public 0xBAA045B4E42F3C06  3889 0x8E9B5F00B41A9F8C  void (Vehicle, float)
        //   GET_VEHICLE_ESTIMATED_MAX_SPEED      public 0x53AF99BAA671CA47  3889 0x66C1D6E6DBF7AC9B  float (Vehicle)
        constexpr uint64_t MODIFY_VEHICLE_TOP_SPEED                    = 0x93A3996368C94158;
        constexpr uint64_t SET_VEHICLE_CHEAT_POWER_INCREASE            = 0xB59E4BD37AE292DB;
        constexpr uint64_t SET_VEHICLE_MAX_SPEED                       = 0xBAA045B4E42F3C06;
        constexpr uint64_t GET_VEHICLE_ESTIMATED_MAX_SPEED             = 0x53AF99BAA671CA47;
        // 0.4.4 (W11, anchor steering). Public hash here; 3889 hash in kCrossmap3889. Looked up on the PC, both sources agree:
        //   TASK_GO_TO_ENTITY  public 0x6A071245EB0D1882  3889 0xAB3658A740EED98E
        //                      void (Entity entity, Entity target, int duration, float distance, float moveBlendRatio,
        //                            float slowDownDistance, int flags)
        constexpr uint64_t TASK_GO_TO_ENTITY                           = 0x6A071245EB0D1882;
        constexpr uint64_t GET_VEHICLE_LIGHTS_STATE                    = 0xB91B4C20085BD12F;
        constexpr uint64_t SET_VEHICLE_FULLBEAM                        = 0x8B7FD87F0DDB421E;
        constexpr uint64_t SET_VEHICLE_INTERIORLIGHT                   = 0xBC2042F090AF6AD3;
        constexpr uint64_t SET_VEHICLE_INDICATOR_LIGHTS                = 0xB5D45264751B7DF0;
        constexpr uint64_t GET_VEHICLE_DOOR_ANGLE_RATIO                = 0xFE3F9C29F7B32BD5;
        constexpr uint64_t SET_VEHICLE_DOOR_OPEN                       = 0x7C65DAC73C35C862;
        constexpr uint64_t GET_CONVERTIBLE_ROOF_STATE                  = 0xF8C397922FC03F41;
        constexpr uint64_t RAISE_CONVERTIBLE_ROOF                      = 0x8F5FB35D7E88FC70;
        constexpr uint64_t LOWER_CONVERTIBLE_ROOF                      = 0xDED51F703D0FA83D;
        constexpr uint64_t TASK_DRIVE_BY                               = 0x2F8AF0E82773A171;
        constexpr uint64_t SET_DRIVEBY_TASK_TARGET                     = 0xE5B302114D8162EE;
        constexpr uint64_t TASK_VEHICLE_AIM_AT_COORD                   = 0x447C1E9EF844BC0F;
        constexpr uint64_t CLEAR_DRIVEBY_TASK_UNDERNEATH_DRIVING_TASK  = 0xC35B5CDB2824CF69;
        constexpr uint64_t GET_IS_TASK_ACTIVE                          = 0xB0760331C7AA4155;
        constexpr uint64_t START_PARTICLE_FX_NON_LOOPED_ON_ENTITY      = 0x0D53A3B8DA0809D2;
        constexpr uint64_t USE_PARTICLE_FX_ASSET                       = 0x6C38AF3693A69A91;
        constexpr uint64_t REQUEST_NAMED_PTFX_ASSET                    = 0xB80D8756B4668AB6;
        constexpr uint64_t HAS_NAMED_PTFX_ASSET_LOADED                 = 0x8702416E512EC454;
        // 0.4.4 (W9, vehicles and collision). Public hash here; 3889 hash in kCrossmap3889. Looked up on the PC, both sources agree:
        //   HAS_ENTITY_COLLIDED_WITH_ANYTHING  public 0x8BAD02F0368D9E14  3889 0x14E25ED5E75102C8  BOOL (Entity)
        //   GET_VEHICLE_CLASS                  public 0x29439776AAA00A62  3889 0x5506AFBD6056C05F  int (Vehicle); 8 = motorcycles, 13 = cycles
        constexpr uint64_t HAS_ENTITY_COLLIDED_WITH_ANYTHING           = 0x8BAD02F0368D9E14;
        constexpr uint64_t GET_VEHICLE_CLASS                           = 0x29439776AAA00A62;
        // 0.4.4 (W10, player name tags). Same convention; looked up on the PC, both sources agree:
        //   GET_SCREEN_COORD_FROM_WORLD_COORD  public 0x34E82F05DF2974F5  3889 0x2F8A75C70AF54208  BOOL (float x, y, z, float* sx, float* sy)
        //   GET_FINAL_RENDERED_CAM_COORD       public 0xA200EB1EE790F448  3889 0x02AF3EA0F67D2329  Vector3 ()
        //   HAS_ENTITY_CLEAR_LOS_TO_ENTITY     public 0xFCDFF7B72D23A1AC  3889 0xAD915B5E38F323E5  BOOL (Entity, Entity, int traceType)
        //   GET_PED_BONE_COORDS                public 0x17C07FC640E86B4E  3889 0x83FDC027F0BEA202  Vector3 (Ped, int bone, float ox, oy, oz)
        constexpr uint64_t GET_SCREEN_COORD_FROM_WORLD_COORD           = 0x34E82F05DF2974F5;
        constexpr uint64_t GET_FINAL_RENDERED_CAM_COORD                = 0xA200EB1EE790F448;
        constexpr uint64_t HAS_ENTITY_CLEAR_LOS_TO_ENTITY              = 0xFCDFF7B72D23A1AC;
        constexpr uint64_t GET_PED_BONE_COORDS                         = 0x17C07FC640E86B4E;
        // Desync overhaul round 2 / T3 (vehicle visual state, protocol v6). Resolved for 0.4.7: public hash = the constant here,
        // 3889 hash = the kCrossmap3889 row in hook.cpp. A World:: wrapper whose constant is 0 still returns false without
        // calling anything (and logs once), and the sync code around it (libs/sync/vehicle_visual.h) treats that as "no
        // information / could not do it", so setting one back to 0 switches that native off. Never guess a hash.
        //   IS_VEHICLE_TYRE_BURST       VEHICLE  BOOL (Vehicle, int wheelId, BOOL completely)
        //   SET_VEHICLE_TYRE_BURST      VEHICLE  void (Vehicle, int wheelId, BOOL onRim, float damage)
        //   SET_VEHICLE_TYRE_FIXED      VEHICLE  void (Vehicle, int wheelId)
        //   IS_VEHICLE_WINDOW_INTACT    VEHICLE  BOOL (Vehicle, int windowId)
        //   SMASH_VEHICLE_WINDOW        VEHICLE  void (Vehicle, int windowId)
        //   FIX_VEHICLE_WINDOW          VEHICLE  void (Vehicle, int windowId)
        //   GET_CONTROL_NORMAL          PAD      float (int inputGroup, int control)
        //   SET_VEHICLE_HANDBRAKE       VEHICLE  void (Vehicle, BOOL toggle)
        //   SET_VEHICLE_BRAKE_LIGHTS    VEHICLE  void (Vehicle, BOOL toggle)
        constexpr uint64_t IS_VEHICLE_TYRE_BURST                       = 0xBA291848A0815CA9;
        constexpr uint64_t SET_VEHICLE_TYRE_BURST                      = 0xEC6A202EE4960385;
        constexpr uint64_t SET_VEHICLE_TYRE_FIXED                      = 0x6E13FC662B882D1D;
        constexpr uint64_t IS_VEHICLE_WINDOW_INTACT                    = 0x46E571A0E20D01F1;
        constexpr uint64_t SMASH_VEHICLE_WINDOW                        = 0x9E5B5E4D2CCD2259;
        constexpr uint64_t FIX_VEHICLE_WINDOW                          = 0x772282EBEB95E682;
        constexpr uint64_t GET_CONTROL_NORMAL                          = 0xEC3C9B8D5327B563;
        constexpr uint64_t SET_VEHICLE_HANDBRAKE                       = 0x684785568EF26A22;
        constexpr uint64_t SET_VEHICLE_BRAKE_LIGHTS                    = 0x92B35082E0B42F66;
        // Desync round 3 (0.4.8 work): natives confirmed by two independent crossmaps (public hash here, 3889 hash in kCrossmap3889).
        //   SET_PED_MAX_HEALTH  PED void(Ped, int)
        //   SET_PLAYER_MAX_ARMOUR  PLAYER void(Player, int)
        //   GET_VEHICLE_PETROL_TANK_HEALTH  VEHICLE float(Vehicle)
        //   SET_VEHICLE_PETROL_TANK_HEALTH  VEHICLE void(Vehicle, float)
        //   SET_VEHICLE_DAMAGE  VEHICLE void(Vehicle, float x, float y, float z, float damage, float radius, BOOL focusOnModel)
        //   GET_VEHICLE_DEFORMATION_AT_POS  VEHICLE Vector3(Vehicle, float x, float y, float z)
        //   IS_VEHICLE_DOOR_DAMAGED  VEHICLE BOOL(Vehicle, int door)
        //   SET_VEHICLE_DOOR_BROKEN  VEHICLE void(Vehicle, int door, BOOL deleteDoor)
        //   SET_VEHICLE_DOOR_CONTROL  VEHICLE void(Vehicle, int door, int speed, float angle)
        //   IS_VEHICLE_DOOR_FULLY_OPEN  VEHICLE BOOL(Vehicle, int door)
        //   GET_IS_DOOR_VALID  VEHICLE BOOL(Vehicle, int door)
        //   GET_IS_LEFT_VEHICLE_HEADLIGHT_DAMAGED  VEHICLE BOOL(Vehicle)
        //   GET_IS_RIGHT_VEHICLE_HEADLIGHT_DAMAGED  VEHICLE BOOL(Vehicle)
        //   SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED  VEHICLE void(Vehicle, BOOL)
        //   SET_VEHICLE_ENGINE_CAN_DEGRADE  VEHICLE void(Vehicle, BOOL)
        //   SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE  VEHICLE void(Vehicle, BOOL)
        //   IS_PED_GOING_INTO_COVER  PED BOOL(Ped)
        //   TASK_STAY_IN_COVER  TASK void(Ped)
        //   IS_PED_AIMING_FROM_COVER  PED BOOL(Ped)
        //   TASK_COMBAT_PED  TASK void(Ped, Ped target, int combatFlags, int threatResponseFlags)
        //   IS_PED_IN_MELEE_COMBAT  PED BOOL(Ped)
        //   TASK_PUT_PED_DIRECTLY_INTO_MELEE  TASK void(Ped, Ped target, float blendIn, float timeInMelee, float strafePhaseSync, int aiCombatFlags)
        //   SET_PED_COMBAT_ATTRIBUTES  PED void(Ped, int attributeId, BOOL enabled)
        constexpr uint64_t SET_PED_MAX_HEALTH                           = 0xF5F6378C4F3419D3;
        constexpr uint64_t SET_PLAYER_MAX_ARMOUR                        = 0x77DFCCF5948B8C71;
        constexpr uint64_t GET_VEHICLE_PETROL_TANK_HEALTH               = 0x7D5DABE888D2D074;
        constexpr uint64_t SET_VEHICLE_PETROL_TANK_HEALTH               = 0x70DB57649FA8D0D8;
        constexpr uint64_t SET_VEHICLE_DAMAGE                           = 0xA1DD317EA8FD4F29;
        constexpr uint64_t GET_VEHICLE_DEFORMATION_AT_POS               = 0x4EC6CFBC7B2E9536;
        constexpr uint64_t IS_VEHICLE_DOOR_DAMAGED                      = 0xB8E181E559464527;
        constexpr uint64_t SET_VEHICLE_DOOR_BROKEN                      = 0xD4D4F6A4AB575A33;
        constexpr uint64_t SET_VEHICLE_DOOR_CONTROL                     = 0xF2BFA0430F0A0FCB;
        constexpr uint64_t IS_VEHICLE_DOOR_FULLY_OPEN                   = 0x3E933CFF7B111C22;
        constexpr uint64_t GET_IS_DOOR_VALID                            = 0x645F4B6E8499F632;
        constexpr uint64_t GET_IS_LEFT_VEHICLE_HEADLIGHT_DAMAGED        = 0x5EF77C9ADD3B11A3;
        constexpr uint64_t GET_IS_RIGHT_VEHICLE_HEADLIGHT_DAMAGED       = 0xA7ECB73355EB2F20;
        constexpr uint64_t SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED           = 0x4C7028F78FFD3681;
        constexpr uint64_t SET_VEHICLE_ENGINE_CAN_DEGRADE               = 0x983765856F2564F9;
        constexpr uint64_t SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE       = 0x37C8252A7C92D017;
        constexpr uint64_t IS_PED_GOING_INTO_COVER                      = 0x9F65DBC537E59AD5;
        constexpr uint64_t TASK_STAY_IN_COVER                           = 0xE5DA8615A6180789;
        constexpr uint64_t IS_PED_AIMING_FROM_COVER                     = 0x3998B1276A3300E5;
        constexpr uint64_t TASK_COMBAT_PED                              = 0xF166E48407BAC484;
        constexpr uint64_t IS_PED_IN_MELEE_COMBAT                       = 0x4E209B2C1EAD5159;
        constexpr uint64_t TASK_PUT_PED_DIRECTLY_INTO_MELEE             = 0x1C6CD14A876FFE39;
        constexpr uint64_t SET_PED_COMBAT_ATTRIBUTES                    = 0x9F7794730795E019;
    }

    // One slot of a ped's look: SET_PED_COMPONENT_VARIATION(ped, component, drawable, texture, palette).
    // `component` is the ePedVarComp index: 0 head, 1 mask, 2 hair, 3 torso/arms, 4 legs, 5 bags,
    // 6 shoes, 7 neck accessories, 8 undershirt, 9 armor, 10 decals, 11 tops. `what` names the
    // item for the log.
    struct PedComponent {
        int         component;
        int         drawable;
        int         texture;
        const char* what;
    };

    // The SET_PED_HEAD_BLEND_DATA arguments of a freemode ped: the face shape and skin tone IDs
    // (0-20 male, 21-41 female, then the DLC faces) of up to three parents, and how much each
    // contributes (mix 0.0 = all first parent).
    struct PedHeadBlend {
        int   shapeFirst, shapeSecond, shapeThird;
        int   skinFirst, skinSecond, skinThird;
        float shapeMix, skinMix, thirdMix;
    };

    // A ped's whole visible look, read from one ped (ReadPedLook) and put on another (ApplyPedLook):
    // the model, the 12 ePedVarComp components (see PedComponent), the 8 prop anchors (0 head,
    // 1 eyes, 2 ears, 3 mouth, 4/5 left/right hand, 6/7 left/right wrist; drawable -1 = no prop) and, for a
    // freemode ped, the head blend (hasHead false when the ped has none or it could not be read).
    struct PedLook {
        uint32_t     model;
        int16_t      drawable[12];
        uint8_t      texture[12];
        uint8_t      palette[12];
        int16_t      propDrawable[8];   // -1 none
        uint8_t      propTexture[8];
        bool         hasHead;
        PedHeadBlend head;
    };

    // A resolved game world. Populate once with Init(); individual primitives
    // that cannot be resolved degrade (null/false + a one-shot log) rather than
    // aborting the whole engine.
    struct World
    {
        bool      ready = false;      // true once the scan finished (see err)
        std::string err;              // first unresolved primitive ("" when all found)

        HMODULE   module = nullptr;   // the game image base
        uint32_t  imageSize = 0;      // SizeOfImage, for log/scan bounds
        // Set before Init: no dummy D3D11 device and no Present detour, so the
        // engine patches nothing in the game (HKCU\Software\GAGE-MP hooks=0).
        bool      passive = false;
        // Set before Init: resolve Present (dummy device) but never detour it
        // (hooks=2), to tell the probe apart from the detour.
        bool      noDetour = false;
        // Set before Init: how HookTick takes the Present hook (HKCU\Software\GAGE-MP present_mode).
        // -1 unset = replace the swap chain vtable's Present slot, with the inline hook as the watchdog's fallback;
        // 0 = the inline hook only (the pre-0.4.7 behaviour: it loses to RTSS-style re-asserting overlays);
        // 1 = the vtable hook only, no fallback. See present_hook.h.
        int       presentMode = -1;
        // Set before Init: natives that create or change game state (spawning
        // peds, the server entry, suppressing SP systems), HKCU\Software\GAGE-MP
        // spawn: -1 unset = on inside GTA's script tick and off on the render-thread
        // fallback, 0 = always off, 1 = always on. Reading the local player is
        // always on.
        int       spawnMode = -1;
        // Set before Init: patch the landing-page check so the game boots
        // straight into Story Mode (HKCU\Software\GAGE-MP skip_landing=1).
        // Off by default — a code patch not yet verified live on build 3889.
        bool      skipLanding = false;
        // Set before Init: write the DECRYPTED game image to disk for offline
        // signature derivation on an Arxan-encrypted build (HKCU\Software\GAGE-MP
        // enh_dump). -1 unset = automatic (dump only on an unknown/unported build,
        // where the engine is inert anyway), 0 = never, 1 = always (re-dump a
        // known build after a patch). See the dump mode in hook.cpp.
        int       enhDump = -1;
        // Set before Init: run the network-session recon pass (HKCU\Software\GAGE-MP
        // net_session=1). A background thread scans the decrypted image for the
        // FiveM/YimMenu session-entry signatures and logs which resolve uniquely on
        // this build, before any session hook is wired. Recon only -- no hooks, no
        // game call. 0 = off (default), 1 = on.
        int       netSession = 0;
        // Set before Init: install the FiveM-style local session-entry port
        // (HKCU\Software\GAGE-MP net_host=1). Once the network code decrypts, the
        // host stubs are applied from the poller (currently: the reversible max-slot
        // raise; the host-call sites are resolved and held for live bring-up) and
        // undone on Unhook. Independent of netSession (recon). 0 = off (default), 1 = on.
        int       netHost = 0;
        // Set before Init: the read-only network baseline (HKCU\Software\GAGE-MP net_probe;
        // 1 unless net_probe=0). Logs every distinct hostname / destination the game resolves or
        // connects to (IAT hooks on its ws2_32/wininet imports, restored on Unhook; Legacy 3889
        // only) and, after the entry, samples the NETWORK_IS_* getters every 10 s for 60 s.
        // With net_host=1 the same hooks run as the ROS guard instead (net_guard.h).
        int       netProbe = 1;
        // kill_sp_hijack: after the entry, terminate the story-character scripts every tick
        // (SuppressPedHijackers). 1 = on (default), 0 = off.
        int       killSpHijack = 1;
        // no_sp_scripts: FiveM JustNoScript -- GtaThread::Tick becomes a no-op so no stock
        // (singleplayer) script ever executes. 1 = on (default), 0 = off.
        int       noSpScripts = 1;

        // -- per-frame hook --------------------------------------------------
        // Hook D3D11 Present so `onTick` runs every frame on the main/render
        // thread (presentMode: the swap chain vtable slot by default, see
        // present_hook.h). Replaces any previously installed hook.
        // onTick must not block.
        bool     HookTick(void (*onTick)());
        void     Unhook();
        bool     Hooked() const;
        // True once GtaThread::Tick is a no-op (no_sp_scripts=1 installed): no stock script runs,
        // so per-tick story-script suppression has nothing to do.
        bool     StockScriptsOff() const;

        // -- script tick -----------------------------------------------------
        // Run `onScriptTick` once per game frame at the top of GTA's script
        // update (run_script_threads), on the game thread, as if it were the
        // game's own main_persistent script: the context natives that create or
        // move entities need. Installed by the native poller once the code is
        // decrypted (after natives validate); ScriptTickActive() says when.
        void     HookScriptTick(void (*onScriptTick)());
        bool     ScriptTickActive() const;

        // Which mechanisms resolved (bitmask):
        //   bit0: D3D11 Present (tick hook target)
        //   bit1: native registration table (native invocation)
        uint32_t Resolved() const;

        // -- native dispatch -------------------------------------------------
        // CallNative invokes a game native by its 64-bit hash. args are
        // packed as uint64_t (use FloatArg for floats). result receives up
        // to 4 uint64_t return values (a Vector3 return lands as 3 float slots,
        // each in the low 4 bytes of an 8-byte slot). A Vector3* argument must
        // point at 3 such 8-byte slots (a script scrVector); CallNative copies the
        // native's result into it afterwards (SetVectorResults). Returns false
        // when the handler is not found.
        bool     HaveNatives() const;
        bool     CallNative(uint64_t hash, const uint64_t* args, int argCount,
                            uint64_t* result);

        // -- local player ----------------------------------------------------
        // The local player ped's script handle. Returns 0 before the game has
        // spawned the player; may change on respawn — call each frame.
        int32_t  LocalPlayer();

        // -- entity geometry (via natives) -----------------------------------
        bool     GetEntityCoords(int32_t handle, float& x, float& y, float& z);
        bool     SetEntityCoords(int32_t handle, float x, float y, float z);
        float    GetEntityHeading(int32_t handle);
        bool     SetEntityHeading(int32_t handle, float heading);

        // -- local player changes (script tick only) -------------------------
        // Switch the local player to `modelHash` with its default components.
        // Requests the model and returns false while it streams in; call again
        // next frame until true. True only when the swap verifiably took
        // (GET_ENTITY_MODEL of the player ped == modelHash); a swap that did not take
        // returns false so the caller retries. If GET_ENTITY_MODEL itself cannot be
        // called the swap cannot be checked: that logs once and returns true.
        bool     SetPlayerModel(uint32_t modelHash);
        // Dress a freemode ped (script tick only; call it right after a SetPlayerModel that took).
        // Every slot is checked with IS_PED_COMPONENT_VARIATION_VALID first and nothing is set
        // unless that native can be called. The ped is then reset to its default components, each
        // valid slot is set (an invalid slot keeps its default, logged once per component) and
        // `head` is applied last. Returns false (ped untouched) when ped is 0, there are no slots,
        // or the validator cannot be called.
        bool     ApplyPedOutfit(int32_t ped, const PedHeadBlend& head,
                                const PedComponent* slots, size_t count);
        // Wanted level: cap it (0 = police never chase) and clear the current one.
        void     DisableWanted();

        // -- entity lifecycle (via natives) ----------------------------------
        int32_t  SpawnPed(int modelHash, float x, float y, float z);
        bool     DeleteEntity(int32_t& handle);  // zeroes the handle

        // -- entity flags (via natives) --------------------------------------
        bool     FreezeEntityPosition(int32_t handle, bool freeze);
        bool     SetEntityVisible(int32_t handle, bool visible);
        bool     SetEntityInvincible(int32_t handle, bool invincible);
        bool     IsPedDeadOrDying(int32_t handle);

        // -- singleplayer suppression ----------------------------------------
        // Once-and-periodic (call at entry and re-assert every few seconds): blocks autosave
        // (SET_MISSION_FLAG(true) so the game never writes the multiplayer character into the
        // player's singleplayer savegame; also terminates the autosave_controller script), the phone
        // and the story/activity scripts. (T5: the ambient half -- random events, trains, garbage trucks,
        // boats, parked cars, random cops, dispatch, gang hassling, wanted multiplier, radar blips --
        // moved to SuppressAmbientSystems, which runs whatever the connection state.)
        void     SuppressSPSystems();
        // T5 ambient: the persistent ambient switches that used to sit at the top of SuppressSPSystems
        // (random events, trains, garbage trucks, boats, parked cars, random cops, every dispatch service,
        // police ignore / dispatch cops / gang hassling, wanted multiplier, cop radar blips), plus the
        // sentinel ones (population budgets 0, vehicle generators off in a box around (x, y), delete the
        // existing trains). Re-assert every few seconds, independent of the connection. Counted in
        // ambientTally. SuppressSPSystems no longer calls it: the engine's ambient tick does.
        void     SuppressAmbientSystems(float x, float y, float z, bool haveCoords);
        // Every-frame (call each script tick while on the server): holds the character-wheel, weapon
        // wheel, phone and MP character-select controls disabled, hides GTA's minimap and the
        // health/armour arcs around it (we draw our own HUD), hides the notification/SMS feed, blocks
        // vehicle rewards, pre-empts a code-requested autosave, and clears any wanted level the instant
        // it appears. (The pause menu is left alone here; the engine's HoldMenuControls disables Esc
        // (control 200) so it cannot pause the game; P (199) still opens the pausing menu.)
        void     SuppressSPFrame();
        // Every-frame (the engine's TickAmbient calls it on every tick, whatever the connection or
        // entry state, and on the render-thread fallback): sets the ped, scenario-ped, vehicle,
        // random-vehicle and parked-vehicle density multipliers to 0 for this frame, so the streets
        // stay empty of ambient peds and traffic. Each call is counted in ambientTally.
        void     SuppressAmbientFrame();
        // T5 ambient: every call of SuppressAmbientFrame / SuppressAmbientSystems / ClearAmbientAround /
        // the sweep, counted by native (ok / failed / inert), and the first call of each logged once. The
        // engine's `engine: ambient` line reads and diffs it. Game thread (and the render-thread fallback,
        // which only runs SuppressAmbientFrame; the two never run in the same frame).
        gage::ambient::Tally ambientTally;
        // True when the enumeration natives have hashes (the sweep can run at all).
        bool     AmbientSweepAvailable() const;
        // GET_PED_NEARBY_PEDS / GET_PED_NEARBY_VEHICLES around `around` into out[0..cap): the count read,
        // or -1 when the native is inert (hash 0), could not be called or returned an implausible array.
        int      NearbyPeds(int32_t around, int32_t* out, int cap);
        int      NearbyVehicles(int32_t around, int32_t* out, int cap);
        // SET_ENTITY_AS_MISSION_ENTITY(h, true, true) then DELETE_ENTITY: the way a script deletes an
        // entity it does not own. ONLY for a handle gage::ambient::ExemptSet::MayDelete() accepted.
        bool     DeleteAmbientEntity(int32_t handle);
        // Entry-time (call each script tick through the entry window): terminate the SP mission-flow
        // scripts (flow_controller, which relaunches the Prologue, plus prologue1 / prologuescreen /
        // the character wheel) and force the playing game-state, so a fresh Story Mode boot's
        // Prologue (Ludendorff / North Yankton) never runs instead of launching, forcing a story
        // character and teleporting away. Shares the terminate list with SuppressSPSystems; does not
        // touch the ped or the world position.
        void     SuppressEntryBoot();
        // After the entry (call each script tick while kill_sp_hijack=1): terminate the story-
        // character scripts main relaunches (player_controller, player_timetable_scene), which
        // otherwise swap the player to Michael/Franklin/Trevor against the ped guard.
        void     SuppressPedHijackers();
        // Clear ambient peds and cops within `radius` of a point and drop generator-spawned
        // vehicles from a box of +/- radius around it. Deliberately not CLEAR_AREA_OF_VEHICLES
        // (it can delete the player's own car). The engine calls it through AmbientClearGuarded (entry,
        // respawn, and every 3 s): the exemption list and the breaker watch it. Counted in ambientTally.
        void     ClearAmbientAround(float x, float y, float z, float radius);
        // Strip the default singleplayer map blips (shops, properties, activities) and keep the
        // player's waypoint. A logged no-op while the blip natives do not resolve; it walks
        // about a thousand sprite ids, so call it on a cadence (seconds), not every frame.
        void     ClearSpBlips();
        // Diagnostic: log every live GTA script thread (id + script-name hash, resolved to a
        // readable name when known, else hex) to gage.log. `when` tags the line (e.g. "pristine"
        // before suppression, "after-suppress" a few seconds in). Read-only; walks the same
        // thread array as the script tick. Use it to identify which singleplayer story/mission
        // scripts are driving campaign voice and on-screen text so they can be terminated.
        void     DumpLiveScripts(const char* when);

        // -- self-driven respawn (script tick only) --------------------------
        // The stock respawn_controller leaves a freemode ped on a black screen after death, so
        // the engine runs its own MP-style respawn (TickLife in engine.cpp). HoldDeathRestart
        // disables the game's own death fade and auto-restart so they never fight us;
        // ReviveLocalPlayer brings the local ped back at a point (full health, cleared
        // damage/tasks, arrest state reset, frozen until its collision streams in); the rest
        // drive the screen fade, the post-death clean-up, and the collision wait.
        bool     IsScreenFadedOut();
        void     DoScreenFadeIn(int ms);
        void     DoScreenFadeOut(int ms);
        void     HoldDeathRestart();      // disable the game's death fade + auto-restart
        void     RestoreDeathDefaults();  // re-enable them after our respawn
        void     ClearDeathEffects();     // stop the death post-fx, camera effect, audio scene, slow-mo
        bool     ReviveLocalPlayer(float x, float y, float z, float heading);
        void     RequestCollisionAt(float x, float y, float z);
        bool     HasCollisionLoadedAround(int32_t handle);
        // Clean no-Prologue boot reveal (script tick): dismiss GTA's own loading screen, and hand
        // control to the player. The engine does both because killing the SP mission flow means GTA
        // never does. Both are safe no-ops when there is no loading screen / control is already on
        // (so the shared respawn reveal can call them too).
        void     ShutdownLoadingScreen();
        void     SetPlayerControl(bool on);

        // -- HUD (script tick only) ------------------------------------------
        // A filled rectangle in normalised screen space (cx, cy are the rect's
        // CENTRE, 0..1; colour 0..255). Call every frame it should show.
        void     DrawRect(float cx, float cy, float w, float h, int r, int g, int b, int a);
        int      GetEntityHealth(int32_t handle);     // current health
        int      GetEntityMaxHealth(int32_t handle);  // max health (for the bar fraction)

        // -- client-side commands (script tick only) -------------------------
        // Spawn vehicles, give weapons, heal and repair for /car /gun /heal /kill
        // /fix, driven from the chat input (see engine.cpp). Model names are hashed
        // and streamed the same way as the player model swap.
        uint32_t HashKey(const char* name);               // GET_HASH_KEY
        bool     IsVehicleModel(uint32_t modelHash);      // valid, and a vehicle
        void     RequestModel(uint32_t modelHash);        // begin streaming
        bool     HasModelLoaded(uint32_t modelHash);      // streamed in yet?
        void     ModelNoLongerNeeded(uint32_t modelHash); // release after use
        int32_t  CreateVehicle(uint32_t modelHash, float x, float y, float z, float heading);
        bool     SetPedIntoVehicle(int32_t ped, int32_t veh, int seat);
        void     PlaceVehicleOnGround(int32_t veh);
        void     SetVehicleEngineOn(int32_t veh, bool on);
        int32_t  GetVehiclePedIsIn(int32_t ped);
        void     FixVehicle(int32_t veh);
        void     GiveWeapon(int32_t ped, uint32_t weaponHash, int ammo);
        bool     SetEntityHealth(int32_t handle, int health);  // e.g. /kill sets 0
        void     HealLocal();  // full health + clear blood/visible damage

        // -- speedometer (read on the script tick, pushed to the overlay) -----
        bool     IsPedInVehicle(int32_t ped);     // IS_PED_IN_ANY_VEHICLE
        float    GetEntitySpeed(int32_t handle);  // m/s (x3.6 for km/h)

        // -- join readiness gate and model verify (read on the script tick) ----
        // Each returns false (GetEntityModel: 0) when the native cannot be called, so a gate
        // built on them needs a timeout; hook.cpp logs once which of them resolved.
        uint32_t GetEntityModel(int32_t handle);   // GET_ENTITY_MODEL: the model hash
        bool     IsLoadingScreenActive();          // GET_IS_LOADING_SCREEN_ACTIVE
        bool     IsScreenFadedIn();                // IS_SCREEN_FADED_IN
        bool     IsPlayerSwitchInProgress();       // IS_PLAYER_SWITCH_IN_PROGRESS
        bool     IsPlayerControlOn();              // IS_PLAYER_CONTROL_ON(PLAYER_ID())

        // -- remote-player peds (script tick only: tasks, spawns and moves) ---
        // Drive a remote player's ped the GTA Network / RAGECOOP way: a locomotion task toward the
        // target, a per-frame velocity steer, and a rare hard snap when it drifts too far. Every
        // bool is "the native could be called" (false for handle 0 or an unresolved native).
        // Like SpawnPed (requests the model, returns 0 until it has streamed in; call again next
        // frame) but for a networked player: NOT frozen, collision on, `heading` set, non-temporary
        // events blocked (it ignores threats and shoves), no ragdoll, no evasive dive, no ambient
        // anims, keeps its task, config flag 188 (DisableHurt) on, invincible. Default components
        // until ApplyPedLook. SpawnPed stays the companion's.
        // R3 T2 `recipe` (the repairs of libs/sync LocoRepair): 0 = all of the above; 1 = LEAN (invincible, heading, non-temporary
        // events blocked and nothing else: no ragdoll block, no keep-task, no ambient / dive flags, no config flag 188, no freeze or
        // collision call); 2 = ALT (lean, a CIVMALE ped type and no script-host flag). A puppet that never gets a gait from any task
        // is created again with the next recipe; gage.log says which one it is ("engine: spawned remote peer ... recipe").
        int32_t  SpawnRemotePed(int modelHash, float x, float y, float z, float heading, int recipe = 0);
        bool     IsPedStill(int32_t ped);                            // IS_PED_STILL (R3 T2 diagnostic)
        bool     SetEntityCoordsNoOffset(int32_t handle, float x, float y, float z);  // hard snap, no axis offset
        bool     SetEntityVelocity(int32_t handle, float vx, float vy, float vz);    // m/s, world axes
        bool     GetEntityVelocity(int32_t handle, float& vx, float& vy, float& vz); // m/s, world axes
        // TASK_GO_STRAIGHT_TO_COORD: `speed` is the move blend ratio (1 walk, 2 run, 3 sprint),
        // timeoutMs -1 = none, targetHeading the final heading (GTAN, RAGECOOP and SHVDN all pass
        // 0), slideDist the distance at which the ped slides into the final heading.
        bool     TaskGoStraightToCoord(int32_t ped, float x, float y, float z, float speed,
                                       int timeoutMs, float targetHeading, float slideDist);
        bool     TaskStandStill(int32_t ped, int ms);                // -1 = until another task
        bool     ClearPedTasks(int32_t ped);                         // blends out (not IMMEDIATELY)
        bool     SetPedDesiredMoveBlendRatio(int32_t ped, float mbr); // 0 still .. 3 sprint
        bool     SetPedDesiredHeading(int32_t ped, float heading);    // degrees, turned toward
        // Gait for the wire: 0 still/none, 1 walking, 2 running, 3 sprinting (checked sprint first,
        // then run, then walk). 0 also when the natives cannot be called.
        int      PedGait(int32_t ped);
        bool     TaskJump(int32_t ped);                              // a normal jump
        bool     IsPedJumping(int32_t ped);
        bool     IsPedRagdoll(int32_t ped);
        bool     SetPedToRagdoll(int32_t ped, int time1Ms, int time2Ms);  // type 0; the native's own BOOL
        bool     SetPedCanRagdoll(int32_t ped, bool can);
        bool     SetPedConfigFlag(int32_t ped, int flag, bool value);    // ePedConfigFlags id
        bool     SetEntityCollision(int32_t handle, bool on);            // keepPhysics = true
        // GET_GROUND_Z_FOR_3D_COORD(ignoreWater false): ground height under (x, y) below z. False
        // when there is none (not streamed in, beyond render distance) or it cannot be called.
        bool     GetGroundZ(float x, float y, float z, float& groundZ);
        float    GetEntityHeightAboveGround(int32_t handle);  // metres; 0 when it cannot be called

        // -- local state readers for the sender (script tick) ----------------
        bool     IsPedFalling(int32_t ped);
        bool     IsPedClimbing(int32_t ped);
        bool     IsPedInParachuteFreeFall(int32_t ped);
        bool     IsPedShooting(int32_t ped);                 // fired this frame
        bool     IsLocalPlayerFreeAiming();                  // IS_PLAYER_FREE_AIMING(PLAYER_ID())
        uint32_t GetSelectedPedWeapon(int32_t ped);          // weapon hash (WEAPON_UNARMED when none); 0 = cannot call
        bool     IsWeaponValid(uint32_t weaponHash);         // IS_WEAPON_VALID; false also when it cannot be called
        bool     SetCurrentPedWeapon(int32_t ped, uint32_t weaponHash, bool inHand);
        // Instant blend to aim, no intro anim; ms -1 = until replaced.
        bool     TaskAimGunAtCoord(int32_t ped, float x, float y, float z, int ms);
        // Walk/run toward (x, y, z) at move blend ratio `mbr` while aiming at (ax, ay, az), firing
        // when `shoot` (target radius 0.5, slow distance 0, no navmesh, default firing pattern).
        bool     TaskGoToCoordWhileAimingAtCoord(int32_t ped, float x, float y, float z,
                                                 float ax, float ay, float az, float mbr, bool shoot);

        // -- appearance (script tick) -----------------------------------------
        // Read every component, prop and (freemode) head blend of `ped`. False (look untouched)
        // when ped is 0 or the component getters cannot be called.
        bool     ReadPedLook(int32_t ped, PedLook& out);
        // Put `look` on `ped` (its model must already be look.model; a different model returns
        // false). Same component path as ApplyPedOutfit: every slot is checked with
        // IS_PED_COMPONENT_VARIATION_VALID, the ped is reset to its defaults, valid slots are set
        // with look's palettes and an invalid one keeps the model default; props are set or
        // cleared (-1); the head blend is applied when look.hasHead. False when ped is 0 or the
        // validator cannot be called (nothing is changed then).
        bool     ApplyPedLook(int32_t ped, const PedLook& look);

        // -- vehicle sync (script tick) ----------------------------------------
        // Every bool is "the native could be called" (false for handle 0 or an unresolved
        // native) unless it says otherwise; a getter with an out parameter leaves it untouched
        // on false. Rotations are degrees in rotation order 2 (ZXY, R*'s script default; get and
        // set must use the same order): pitch = x, roll = y, yaw = z.
        bool     GetEntityQuaternion(int32_t handle, float& x, float& y, float& z, float& w);
        bool     SetEntityQuaternion(int32_t handle, float x, float y, float z, float w);
        bool     GetEntityRotation(int32_t handle, float& pitch, float& roll, float& yaw);
        bool     SetEntityRotation(int32_t handle, float pitch, float roll, float yaw);  // p5 true
        // GET_ENTITY_ROTATION_VELOCITY / SET_ENTITY_ANGULAR_VELOCITY (there is no
        // GET_ENTITY_ANGULAR_VELOCITY). NativeDB documents neither the unit nor the axis frame:
        // check live that get -> set round-trips before relying on it.
        bool     GetEntityRotationVelocity(int32_t handle, float& x, float& y, float& z);
        bool     SetEntityAngularVelocity(int32_t handle, float x, float y, float z);
        // Seat -1 = driver, 0 = front passenger, 1.. = rear. 0 = empty seat or cannot call. Can
        // CREATE a random occupant in an ambient (non-mission) vehicle: MarkVehicleOwned first.
        int32_t  GetPedInVehicleSeat(int32_t veh, int seat);
        int      GetVehicleMaxPassengers(int32_t veh);  // excludes the driver; -1 = cannot call
        // countEntering true: a seat someone is entering (TASK_ENTER_VEHICLE) already counts taken.
        bool     IsVehicleSeatFree(int32_t veh, int seat, bool countEntering = false);
        // The raw seat index; only meaningful while GetVehiclePedIsTryingToEnter is non-zero.
        bool     GetSeatPedIsTryingToEnter(int32_t ped, int& seat);
        int32_t  GetVehiclePedIsTryingToEnter(int32_t ped);  // 0 = none
        int32_t  GetVehiclePedIsUsing(int32_t ped);          // in, or entering; 0 = none
        // speed 1 walk / 2 run; flags 1 normal, 3 teleport to the door, 16 warp straight in
        // (NativeDB); no clipset override; timeoutMs -1 = none.
        bool     TaskEnterVehicle(int32_t ped, int32_t veh, int timeoutMs, int seat, float speed,
                                  int flags = 1);
        // flags 0 normal exit + close door, 16 teleport out, 256 leave the door open, 4160 bail
        // out (NativeDB list).
        bool     TaskLeaveVehicle(int32_t ped, int32_t veh, int flags);
        bool     TaskWarpPedIntoVehicle(int32_t ped, int32_t veh, int seat);
        bool     SetVehicleSteerBias(int32_t veh, float bias);  // -1 full right .. 1 full left; EVERY frame
        bool     GetVehicleEngineHealth(int32_t veh, float& health);  // 1000 new .. 0 broken .. -4000 fire
        bool     SetVehicleEngineHealth(int32_t veh, float health);
        bool     GetVehicleBodyHealth(int32_t veh, float& health);
        bool     SetVehicleBodyHealth(int32_t veh, float health);
        bool     IsVehicleEngineRunning(int32_t veh);  // false also when it cannot be called
        // W9: HAS_ENTITY_COLLIDED_WITH_ANYTHING (true when the entity touched anything since it was last asked; whether the
        // ground counts is INFERRED: the drive controller opens a grace on a rising contact only and has a cooldown) and
        // GET_VEHICLE_CLASS (-1 when it cannot be called; 8 motorcycles, 13 cycles).
        bool     HasEntityCollidedWithAnything(int32_t entity);
        int      GetVehicleClass(int32_t veh);
        // W10 (player name tags). GetPedBoneCoords: GET_PED_BONE_COORDS(ped, bone, offX, offY, offZ), world position (SKEL_Head is
        // 31086). GetScreenCoordFromWorldCoord: GET_SCREEN_COORD_FROM_WORLD_COORD, sx / sy in 0..1 of the screen; returns the native's
        // BOOL (false: behind the camera or off screen; also when it cannot be called). GetFinalRenderedCamCoord: the camera the frame
        // is drawn with. HasEntityClearLosToEntity: 1 = clear line of sight, 0 = blocked, -1 = could not be called
        // (traceType 17 is what R*'s scripts pass; the caller owns the meaning).
        bool     GetPedBoneCoords(int32_t ped, int bone, float ox, float oy, float oz, float& x, float& y, float& z);
        bool     GetScreenCoordFromWorldCoord(float x, float y, float z, float& sx, float& sy);
        bool     GetFinalRenderedCamCoord(float& x, float& y, float& z);
        int      HasEntityClearLosToEntity(int32_t from, int32_t to, int traceType);
        bool     SetVehicleSiren(int32_t veh, bool on);
        bool     IsVehicleSirenOn(int32_t veh);       // false also when it cannot be called
        bool     StartVehicleHorn(int32_t veh, int durationMs);  // mode 0 (default), not forever
        bool     GetVehicleColours(int32_t veh, int& primary, int& secondary);
        bool     SetVehicleColours(int32_t veh, int primary, int secondary);
        bool     SetVehicleDoorsLocked(int32_t veh, int lockState);  // eVehicleLockState: 1 unlocked, 2 locked
        bool     SetVehicleLights(int32_t veh, int state);  // 0 normal, 1 always off, 2 always on
        bool     SetVehicleForwardSpeed(int32_t veh, float mps);
        bool     SetVehicleDoorShut(int32_t veh, int door, bool instantly);
        // Keep a synced vehicle ours: has-been-owned-by-player true, stolen false, mission entity
        // (persistent, never cleaned up by population). True only when all three could be called.
        bool     MarkVehicleOwned(int32_t veh);

        // -- weapons, shots and damage (script tick) ---------------------------
        // A bullet from -> to (p7 true). speed -1 = the weapon's default. ownerPed 0 = no owner.
        bool     ShootBulletBetween(float fx, float fy, float fz, float tx, float ty, float tz,
                                    int damage, uint32_t weaponHash, int32_t ownerPed, bool audible,
                                    bool invisible, float speed);
        bool     PedShootAtCoord(int32_t ped, float x, float y, float z);  // SET_PED_SHOOTS_AT_COORD, toggle true
        // durationMs -1 = until replaced; firingPattern 0 = FIRING_PATTERN_FULL_AUTO.
        bool     TaskShootAtCoord(int32_t ped, float x, float y, float z, int durationMs,
                                  uint32_t firingPattern = 0);
        // Where the ped's last shot hit. False when it has none or it cannot be called.
        bool     LastWeaponImpact(int32_t ped, float& x, float& y, float& z);
        bool     DamagedBy(int32_t entity, int32_t byEntity);  // victim, attacker; false also when uncallable
        bool     DamagedByAnyPed(int32_t entity);
        bool     ClearLastDamage(int32_t entity);              // CLEAR_ENTITY_LAST_DAMAGE_ENTITY
        // The bone id (eAnimBoneTag, e.g. 31086 head) of the last hit. False when none / uncallable.
        bool     LastDamageBone(int32_t ped, int& bone);
        bool     ClearLastDamageBone(int32_t ped);
        int      GetPedArmour(int32_t ped);                    // -1 = cannot call
        bool     SetPedArmour(int32_t ped, int amount);
        // Base damage of a weapon (no component). -1 = cannot call; melee/explosive groups read 0.
        float    WeaponDamage(uint32_t weaponHash);
        int32_t  GetPedSourceOfDeath(int32_t ped);             // killer entity; 0 = none / uncallable
        bool     IsEntityDead(int32_t entity);                 // false also when it cannot be called
        bool     SetEntityCanBeDamaged(int32_t entity, bool can);
        // drown is the native's waterProof; p7 its dontResetOnCleanup.
        bool     SetEntityProofs(int32_t entity, bool bullet, bool fire, bool explosion, bool collision,
                                 bool melee, bool steam, bool p7, bool drown);
        bool     SetPedSuffersCriticalHits(int32_t ped, bool on);  // false = no head-shot multiplier
        bool     ApplyDamageToPed(int32_t ped, int damage, bool armorFirst);  // p3 0, weaponType 0
        int32_t  GetCurrentPedWeaponEntity(int32_t ped);       // the weapon object; 0 = none / uncallable
        int      GetEntityBoneIndexByName(int32_t entity, const char* bone);  // -1 = no such bone / uncallable
        bool     GetWorldPositionOfEntityBone(int32_t entity, int boneIndex, float& x, float& y, float& z);
        // The muzzle ("gun_muzzle" bone of the current weapon object). False when the ped holds
        // no weapon object (unarmed, melee) or the weapon has no such bone.
        bool     GetWeaponMuzzlePos(int32_t ped, float& x, float& y, float& z);
        int      GetAmmoInPedWeapon(int32_t ped, uint32_t weaponHash);  // total ammo; -1 = cannot call
        bool     SetPedInfiniteAmmoClip(int32_t ped, bool on);

        // -- camera and aim (script tick) --------------------------------------
        bool     GetGameplayCamCoord(float& x, float& y, float& z);
        bool     GetGameplayCamRot(float& pitch, float& roll, float& yaw);  // degrees, order 2
        bool     IsAimCamActive();                              // false also when uncallable
        // The entity the local player is free-aiming at. False when none / uncallable.
        bool     GetEntityLocalPlayerIsFreeAimingAt(int32_t& entity);
        // One synchronous line-of-sight probe from the gameplay camera along its view direction,
        // maxDist metres, against everything (flags -1), ignoring `ignoreEntity` (pass the local
        // ped: the third-person camera sits behind it) and options 7 (ignore glass, see-through
        // and no-collision; R*/YimMenu default). Returns true when the probe ran and its result
        // was read; then `hit` says whether it hit, and x, y, z / hitEntity are the hit point and
        // the entity hit (0 for the world) — or the probe end point and 0 when nothing was hit.
        bool     RaycastFromCamera(float maxDist, int32_t ignoreEntity, bool& hit,
                                   float& x, float& y, float& z, int32_t& hitEntity);

        // -- protocol v4: emotes, health, entity checks (script tick) ----------
        // Tri-state readers return 1 yes, 0 no, -1 the native cannot be called.
        bool     RequestAnimDict(const char* dict);
        bool     HasAnimDictLoaded(const char* dict);        // false also when uncallable
        bool     RemoveAnimDict(const char* dict);
        int      AnimDictExists(const char* dict);           // DOES_ANIM_DICT_EXIST
        // TASK_PLAY_ANIM: blend speeds as GTA's (8 / -8 normal), durationMs -1 = the clip's length (or
        // looped with flag 1), flags eAnimationFlags, rate 1.0 normal; no axis locks.
        bool     TaskPlayAnim(int32_t ped, const char* dict, const char* name, float blendIn, float blendOut,
                              int durationMs, int flags, float playbackRate);
        // TASK_START_SCENARIO_IN_PLACE(ped, name, 0, playEnterAnim true).
        bool     TaskStartScenarioInPlace(int32_t ped, const char* scenario);
        int      IsEntityPlayingAnim(int32_t entity, const char* dict, const char* name);  // taskFlag 3
        int      IsPedUsingAnyScenario(int32_t ped);
        bool     SetEntityMaxHealth(int32_t entity, int value);
        int      EntityExists(int32_t entity);              // DOES_ENTITY_EXIST
        bool     IsHornActive(int32_t veh);                  // false also when uncallable
        // SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(PLAYER_ID(), m): 0 stops GTA's own regeneration.
        bool     SetLocalHealthRecharge(float multiplier);

        // -- the no-pause pause menu (script tick) ----------------------------
        // ACTIVATE_FRONTEND_MENU(menuHash, togglePause, component); togglePause false opens the menu
        // without pausing the world. False when it cannot be called.
        bool     ActivateFrontendMenu(uint32_t menuHash, bool togglePause, int component);
        bool     IsPauseMenuActive();                        // false also when uncallable
        // IS_DISABLED_CONTROL_JUST_PRESSED(pad, control): still reads a control that was disabled this frame.
        bool     IsDisabledControlJustPressed(int pad, int control);
        // False when the native cannot be called (the caller falls back to SetPauseMenuActive(false)).
        bool     DisableFrontendThisFrame();
        bool     SetPauseMenuActive(bool on);
        int      GetGameTimer();                             // ms; -1 = cannot call
        uint32_t GetWeaponTypeGroup(uint32_t weaponHash);    // GET_WEAPONTYPE_GROUP; 0 = cannot call

        // -- sync plan W0: on-foot, vehicle body, drive-by, particles (script tick) ----------
        // Same conventions as above: a setter or task returns "the native could be called" (false
        // for handle 0 or an unresolved native); a BOOL reader is false also when it cannot be
        // called; a getter with an out parameter leaves it untouched on false. Signatures are the
        // ones in docs/SYNC-PLAN.md "Natives"; hook.cpp logs which of the 27 resolved (3889 hashes
        // not live-checked yet).
        bool     IsPedReloading(int32_t ped);                        // IS_PED_RELOADING
        bool     TaskReloadWeapon(int32_t ped, bool drawWeapon);     // alloc8or: drawWeapon is not implemented
        bool     GetPedStealthMovement(int32_t ped);                 // GET_PED_STEALTH_MOVEMENT
        // p1 is the native's unnamed BOOL (alloc8or: usually 0 in the scripts); action is null or
        // "DEFAULT_ACTION" (alloc8or).
        bool     SetPedStealthMovement(int32_t ped, bool p1, const char* action);
        bool     IsPedSwimming(int32_t ped);                         // IS_PED_SWIMMING
        bool     IsPedInCover(int32_t ped, bool exceptUseWeapon);    // IS_PED_IN_COVER
        // TASK_PUT_PED_DIRECTLY_INTO_COVER with the native's own parameters, in its order.
        bool     TaskPutPedDirectlyIntoCover(int32_t ped, float x, float y, float z, int time,
                                             bool allowPeekingAndFiring, float blendInDuration,
                                             bool forceInitialFacingDirection, bool forceFaceLeft,
                                             int identifier, bool doEntry);
        bool     IsPedPerformingMeleeAction(int32_t ped);            // IS_PED_PERFORMING_MELEE_ACTION
        // 0..10 (alloc8or); the native has to be called again every frame to hold.
        bool     SetPedMoveRateOverride(int32_t ped, float value);
        // 0.4.3 fixes (W8). IS_PED_VAULTING: a vault or slide over an obstacle (IS_PED_CLIMBING is the climb).
        bool     IsPedVaulting(int32_t ped);
        // TASK_CLIMB(ped, usePlayerLaunchForce): the ped climbs / vaults whatever is in front of it. Once per climb.
        bool     TaskClimb(int32_t ped, bool usePlayerLaunchForce);
        // W11: TASK_GO_TO_ENTITY(ped, target, duration, distance, moveBlendRatio, slowDownDistance, flags): the ped walks / runs to
        // an entity and follows it. duration -1 = until another task; the remote puppets use it on an invisible anchor entity.
        bool     TaskGoToEntity(int32_t ped, int32_t target, int durationMs, float distance, float moveBlendRatio,
                                float slowDownDistance, int flags);
        // The AI damage modifiers are global (every AI ped's weapon / melee damage, which includes the remote
        // players' puppets): 0.0 at Welcome so a puppet's real shot or punch hurts nobody locally (damage is the
        // server's), RESET at the end of the session. True = could be called.
        bool     SetAiWeaponDamageModifier(float value);
        bool     SetAiMeleeWeaponDamageModifier(float value);
        bool     ResetAiWeaponDamageModifier();
        bool     ResetAiMeleeWeaponDamageModifier();
        // Diagnostic only (logged when a puppet dies locally): the game killed it with a takedown.
        bool     WasPedKilledByTakedown(int32_t ped);
        // Hyper tune (libs/sync/hypertune.h). True = could be called; GetVehicleEstimatedMaxSpeed: m/s, 0 when not.
        bool     ModifyVehicleTopSpeed(int32_t veh, float percent);
        bool     SetVehicleCheatPowerIncrease(int32_t veh, float value);   // every frame
        bool     SetVehicleMaxSpeed(int32_t veh, float metresPerSecond);
        float    GetVehicleEstimatedMaxSpeed(int32_t veh);
        // Both outs are BOOL* slots. The native's own BOOL return has no documented meaning and is
        // not used: true here is "could be called", and then lightsOn / highbeamsOn are valid.
        bool     GetVehicleLightsState(int32_t veh, bool& lightsOn, bool& highbeamsOn);
        bool     SetVehicleFullbeam(int32_t veh, bool on);           // true = high beams
        bool     SetVehicleInteriorLight(int32_t veh, bool on);
        bool     SetVehicleIndicatorLights(int32_t veh, int turnSignal, bool on);  // turnSignal 1 left, 0 right
        // `door` ids as SET_VEHICLE_DOOR_SHUT (SetVehicleDoorShut above).
        bool     GetVehicleDoorAngleRatio(int32_t veh, int door, float& ratio);
        bool     SetVehicleDoorOpen(int32_t veh, int door, bool loose, bool instantly);
        bool     GetConvertibleRoofState(int32_t veh, int& state);   // 0 up, 1 lowering, 2 down, 3 raising
        bool     RaiseConvertibleRoof(int32_t veh, bool instantly);
        bool     LowerConvertibleRoof(int32_t veh, bool instantly);
        // TASK_DRIVE_BY: pass 0 for a target ped / vehicle that is not used; firingPattern is a hash,
        // 0 = FIRING_PATTERN_FULL_AUTO (like TaskShootAtCoord).
        bool     TaskDriveBy(int32_t driverPed, int32_t targetPed, int32_t targetVehicle,
                             float x, float y, float z, float distanceToShoot, int pedAccuracy,
                             bool pushUnderneathDrivingTaskIfDriving, uint32_t firingPattern);
        bool     SetDrivebyTaskTarget(int32_t shootingPed, int32_t targetPed, int32_t targetVehicle,
                                      float x, float y, float z);
        bool     TaskVehicleAimAtCoord(int32_t ped, float x, float y, float z);
        bool     ClearDrivebyTaskUnderneathDrivingTask(int32_t ped);
        bool     GetIsTaskActive(int32_t ped, int taskType);         // taskType is an eTaskType id
        // ---- T3 vehicle visual state (protocol v6). Each returns false WITHOUT calling when its hash constant is still 0
        // (UNRESOLVED: lookup needed, logged once) or the native cannot be called: the caller treats false as "no information"
        // (a read) or "not done" (a write). `wheel` / `window` are the game's slots 0..7. ----
        bool     IsVehicleTyreBurst(int32_t veh, int wheel, bool completely, bool& burst);   // completely = off the rim
        bool     SetVehicleTyreBurst(int32_t veh, int wheel, bool onRim, float damage);      // damage 1000 pops it at once
        bool     SetVehicleTyreFixed(int32_t veh, int wheel);
        bool     IsVehicleWindowIntact(int32_t veh, int window, bool& intact);
        bool     SmashVehicleWindow(int32_t veh, int window);
        bool     FixVehicleWindow(int32_t veh, int window);
        bool     GetControlNormal(int inputGroup, int control, float& value);                // -1..1 axis / 0..1 button
        bool     SetVehicleHandbrake(int32_t veh, bool on);
        bool     SetVehicleBrakeLights(int32_t veh, bool on);
        // Particle fx: request the asset, wait for HasNamedPtfxAssetLoaded, UseParticleFxAsset, then
        // start (the alloc8or example order). Null / empty names return false without calling.
        bool     RequestNamedPtfxAsset(const char* fxName);
        bool     HasNamedPtfxAssetLoaded(const char* fxName);
        bool     UseParticleFxAsset(const char* name);
        // Offsets and rotation are relative to the entity. True only when the effect started (the
        // native's own BOOL).
        bool     StartParticleFxNonLoopedOnEntity(const char* effectName, int32_t entity,
                                                  float offX, float offY, float offZ,
                                                  float rotX, float rotY, float rotZ, float scale,
                                                  bool axisX, bool axisY, bool axisZ);
    };

    // Resolve the game module and every primitive. Cheap enough to run once at
    // attach. Does not install the tick hook (see HookTick).
    void Init(World& w);

    // Retry native table resolution if Init ran before the game populated it.
    // Call from Frame() — stops after ~30 retries. No-op once the table is found.
    void retryNativeTableOnce();

    // The IDXGISwapChain* captured from the Present hook. Valid after
    // HookTick is installed. Use for D3D11 overlay rendering in Frame().
    void* GetSwapChain();

}
