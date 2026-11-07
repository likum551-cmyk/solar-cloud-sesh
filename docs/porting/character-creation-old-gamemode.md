# Character creation in the old gamemode (RedAge / NeptuneEvo on RAGE:MP)

GI 2026-11-07: "go to my old gamemode and find character creation. 1 character min insta creation if not created on server join. can buy
additional 2 char slots. grab it out fully."

This is the old system **as the code has it** (read only; nothing in the old tree was run or changed), so the Solar RP port can keep its
intent and drop its bugs. The port itself is described at the end ("What Solar RP keeps").

Paths are relative to `H:/Users/Administrator/Downloads/OUR OLD GAMEMODE/GAMEMODE`:

- `S/` = `dotnet/resources/NeptuneEvo/` (C# server)
- `C/` = `src_client/` (client JS sources; the shipped `client_packages/main.js` is built from them: it contains
  `client.characters.customization.create`, `-2635.406`, `redagelogo`, `newchar`)
- `U/` = `src_cef/src/` (Svelte CEF sources; the shipped `client_packages/interface/build/bundle.js` contains their strings)
- `database/main.sql` = the MySQL schema

## 1. Flow: connect -> login -> character select -> create -> spawn

1. **Connect.** The server sends `client.init(serverId, serverName, donateMultiplier, donateConvert, isMerger)` and puts the player in a
   private dimension (`S/Players/Connect/Events.cs:34-43`). The client points the auth camera at (-1700.7537, -1145.9944, 29.260117), rot z
   -90.6186, FOV 70 (`C/player/auth.js:42-55`).
2. **Login / registration.** `signin(loginOrEmail, password)` (SHA-256 on the server, `S/Accounts/Autorization/Events.cs:14-27`) or
   `signup(login, pass, email, promo)` (`S/Accounts/Email/Registration/Events.cs:14-27`; client checks: login 3-32 chars of
   `/^[a-zA-Z1-9]*$/`, an email format, matching passwords of 3+ chars, `C/player/auth.js:119-148`, `C/utils/validator.js:1-29`). Both end in
   `LoadCharacter.Repository.Load` (`S/Accounts/Autorization/Repository.cs:36`, `S/Accounts/Registration/Repository.cs:154`).
3. **Slot list.** The server sends `toslots(accountJson, customizationsJson, clothesJson, accessoryJson)`
   (`S/Accounts/LoadCharacter/Repository.cs:183`). `accountJson` carries the login, Redbucks, VIP, `LastSelectCharUUID`, `charsSlot` and
   `chars` (`:41-52`, `:180-181`); each `chars[uuid]` holds `Type` (`char`/`ban`), ban info and the character's summary (UUID, DeleteData,
   names, level, faction, house, money; `:94-170`). A slot whose character no longer exists is reset to -1 (`:78-86`).
4. **Select screen.** `toslots` -> `initCustomizationCharsData` -> `CreatorCamera(false)` (`C/player/hud.js:261-277`,
   `C/player/character.js:179-196`); placeholder peds are spawned and dressed with each saved look (`client.characters.initChars`,
   `C/player/character.js:452-486`, `:531-671`, `:673-695`; `U/store/customization.js:162-190`), then the "Chars" view opens.
5. **Zero characters: nothing is forced.** The list auto-selects the first empty slot in create mode
   (`U/views/player/newauthentication/chars/list/index.svelte:63-67`) and the create panel shows inline (`.../chars/chars.svelte:10-21`). The
   only forced creator is an EXISTING character with no customization row: `Customization.SendToCreator` (`S/Character/Load/Repository.cs:380-387`
   -> `S/Core/Customization.cs:1056-1086`: `CreatorCamera(true)`, dimension `player.Value+1`).
6. **Auto-spawn.** With `LastSelectCharUUID` set, that character spawns by itself after 10 s unless a key or the mouse is pressed
   (`.../chars/list/index.svelte:9-24`, `:36-40`, `:52-59`).
7. **Create** (client -> server): the UI calls `client.characters.customization.create(slot, first, last, gender)`
   (`U/views/player/newauthentication/chars/create/index.svelte:55`), the client sends remote `CreateCharacter(slot, firstName, lastName, gender,
   fatherid, motherid, shapemix, skintone, featuresJSON, appearanceJSON, hairColorsJSON, clothesJSON)` (`C/player/character.js:951-963`), handled
   at `S/Core/Customization.cs:1151-1154`. (A second path, `client:OnCreateCharacterv2` -> remote `newchar`, `C/player/auth.js:186-205`, has no
   server handler: dead code.)
8. **Create** (server -> client): errors as `client.characters.create.error(text)` (`S/Character/Create/Repository.cs:35,40`,
   `S/Core/Customization.cs:1176`); on success `client.charStore.Gender`, `client.charcreate.close`, `client:OnOpenHelpMenu` (new characters
   only) (`S/Core/Customization.cs:1289`, `:1320`, `:1329`), then `Main.ClientEvent_Spawn(player, 0)` sends `ready` (`S/Main.cs:1303`).
9. **Select:** `client:OnSelectCharacterv2(uuid, spawnid)` -> remote `selectchar(uuid, spawnid)` (`C/player/auth.js:165-178`); the server checks
   the uuid belongs to the account and stores `LastSelectCharUUID` (`S/Character/Load/Events.cs:14-29`), refuses a banned or delete-pending
   character (`S/Character/Load/Repository.cs:52-71`), then spawns (`:377-387`). Spawn ids: 0 saved position, 1 faction / organisation, 2 house
   or hotel (`S/Main.cs:1335-1373`; UI `.../chars/spawn/spawn.svelte:23-28`).

## 2. Slots

- **3 slots, 2 free.** Client and UI hard-code `maxSlots = 3` (`C/player/character.js:71`, `U/store/customization.js:19`). Registration writes
  `character1=-1, character2=-1, character3=-2, characters="[-2,-2,-2,-2,-2,-2]"` (-1 empty, -2 not bought, else a character uuid;
  `S/Accounts/Registration/Repository.cs:90-103`, `:138-141`). Six more slots in the JSON `characters` column exist only for a server merge
  (`IsMerger`, `S/Accounts/Autorization/Repository.cs:155-178`), which is off (`settings/serverSettings.json:12`). Saves write `Chars[0..2]` to
  `character1..3` (`S/Accounts/Save/Repository.cs:40-64`).
- **Buying the third slot** (`server.buySlots(slotId)`, `S/Chars/Donate.cs:1480-1529`): needs `Chars[slotId] == -2` (else "Already3Slot"),
  1000 RedBucks (the donate currency, `S/Accounts/Models/AccountData.cs:33-36`), deducts them, gives VIP (level 3 for 30 days when the VIP level
  is 3 or lower, else +7 days), sets the slot to -1 and answers `unlockSlot(slotId)` (client `buyNewSlot` -> `server.buySlots`,
  `C/player/auth.js:213-221`, `:260-262`). `slotId` is not bounds-checked. The UI shows the unlock button on slot index 2 only
  (`.../chars/list/characterMain.svelte:10-17`).
- **Tables** (`database/main.sql`): `accounts.character1..3 int`, `accounts.characters varchar(100)`, `redbucks`, `viplvl`, `vipdate`,
  `LastSelectCharUUID` (`:19-51`); `characters` (uuid PK, `IsDelete`, `DeleteData`, `firstname`/`lastname varchar(50)` with a case-insensitive
  collation and **no unique index**, `gender`, `pos` JSON, money, bank, level, ...; `:546-604`); `customization` (uuid PK, `gender`, `parents`,
  `features`, `appearance`, `hair` as JSON strings, `tattoos`, `eyec`, `iscreated`; `:634-645`).
- **Delete** (`S/Character/Delete/Repository.cs`, behind the `DeleteCharacter` feature switch, `S/Character/Delete/Events.cs:20`): asking again
  cancels a pending delete (`:54-55`, `:115-132`); refused with an admin-jail sentence, warns (`:59-63`) or an active ban (`:90-97`); otherwise
  `IsDelete=1`, `DeleteData = now + 72 h - 1 min` (`:99-107`). A 1-minute timer (`S/Main.cs:856`, `:3602`) purges expired characters with their
  businesses, items, bank, vehicles and SIM and frees the slot (`:140-267`). No cost (a level rule is commented out, `:84-88`).
- **Paid changes** (`server.donate.buy.char(id, name)`, `S/Chars/Donate.cs:1214`): 1 = redo the appearance, 1000 RB (refused when cuffed, dying,
  jailed; then `SendToCreator`, `:1241-1283`); 0 = rename, 800 RB, `First_Last`, not taken (`:1285-1371`, `S/Character/Change/Repository.cs:65-127`);
  2 = remove a warn (`:1379-1403`). No cooldowns.

## 3. What the creator edits

| Field | Values in the old code | Source |
| --- | --- | --- |
| Gender / model | male `mp_m_freemode_01` (UI default), female `mp_f_freemode_01` | `C/player/character.js:497`, `S/Character/Repository.cs:104`, `U/store/customization.js:27` |
| Parents | one list of GTA head ids 0-45 for both pickers (0-20 male, 21-41 female, 42-45 Claude / Niko / John / Misty); random default | `U/.../create/elements/info/parents.js:1-97`, `U/store/random/index.js:9-11,21` |
| Shape mix, skin tone | sliders 0..1, step 0.1; their default is undefined in the code (commented out) | `U/.../info/index.svelte:53-75`, `U/store/random/index.js:23-24` |
| Face features | 20 floats about -1..1, all 0 by default; 11 controls (x/y grids or sliders) with 3 presets each; index 19 (neck) never set; the client negates x of 2, 10, 11, 12, 17 and maps 16 to (x+1)/2 | `U/.../characteristics/indexes.js:1-45`, `characteristics.js:1-57`, `C/player/character.js:841-868`, `S/Core/Customization.cs:110,133` |
| Head overlays offered | 0 blemishes 255+0..23, 1 facial hair (men) 255+0..28, 2 eyebrows 0..33, 3 ageing 255+0..15, 6 complexion 255+0..11, 7 sun damage 255+0..10, 8 lipstick (women) 255+0..9, 9 moles / freckles 255+0..17 | `U/.../appearance/appearances.js:79-324` |
| Overlays not offered | 4 makeup, 5 blush, 10 chest hair: always sent as 255 | `C/player/character.js:922-932` |
| Overlay opacity | slider 0..1 step 0.1; default 100 (i.e. full) | `U/.../appearance/index.svelte:197-210`, `U/store/random/index.js:80-87` |
| Overlay colours | hair / brows / beard: palette ids 0-9, 11, 12, 14-20, 23-27; lipstick 0-23; colour type 1 for overlays 1, 2, 10, type 2 for 5, 8 | `U/.../appearance/index.svelte:10-35,105-130`, `C/player/character.js:603-609` |
| Hair | component 2: male 1-22, 24-36; female 1-22, 25-38; highlight = the hair colour | `U/.../appearances.js:2-77`, `C/player/character.js:812,936-937` |
| Eye colour | 0-8 | `U/.../appearances.js:263-273` |
| Clothes at creation | 4 sets (hat prop 0, top 11 with 8 emptied, legs 4, shoes 6), short per-gender lists, given as inventory items | `U/.../clothes/clothes.js:1-84`, `S/Core/Customization.cs:1298-1317` |

Random picks only hair, eyebrows, facial hair, moles and eye colour (`U/store/random/index.js:84-85`). The arrows wrap at `length-1`, so the
last entry of a list is unreachable by arrow (`U/.../appearance/index.svelte:155-156`).

**Storage and validation.** `CreateCharacter` deserialises into `PlayerCustomization` (`S/Core/Customization.cs:101-137`): features `float[20]`,
overlays `{Value, Opacity, Color}`, `hairAndColor int[]` = hair, hair colour, highlight, brow colour, beard colour, eye colour, blush colour
(always 0), lip colour, chest colour (always 0) (`:1205-1279`; client `C/player/character.js:934-943`). Written as JSON strings
(`:974-992`, `:1030-1041`). **The server clamps or validates no appearance value.**

**Bugs in the old code (port the intent, not these):**

1. Colours are written through the `BarberComponent` enum (Hair 0, Beard 1, Body 2, Eyebrows 3, Eyes 4, Lips 5, Makeup 6, Palette 7;
   `S/Chars/ClothesComponents.cs:1677-1687`) instead of overlay ids: the eyebrow colour lands on overlay 3 (ageing), the chest colour (0)
   overwrites the eyebrows' (2), the lipstick colour lands on 5 (blush), only the beard is right.
2. `InsertCustomization` writes eye colour 0 (`S/Core/Customization.cs:1039`); the real value is saved only by the next periodic save
   (`S/Character/Save/Repository.cs:35`).
3. Parents are swapped twice (`U/store/customization.js:80,102` -> `C/player/character.js:120-123` -> `:956-957` -> server
   `S/Core/Customization.cs:929-940`): the rendered face matches the preview but the stored `Father` holds the mother.
4. `customization.gender` is always 0 (never set at create; the select screen reads `characters.gender`,
   `S/Accounts/LoadCharacter/Repository.cs:136`).
5. A customization row with `iscreated = 0` is deleted on load (`S/Character/Load/Repository.cs:412-417`).

## 4. Natives and sync

- **Client preview** (local peds, `C/player/character.js`): `setHeadBlendData(shape1, shape2, 0, skin1, skin2, 0, shapeMix, skinMix, 0.0, true)`
  (`:571-585`, `:744-758`), `setFaceFeature(i, v)` (`:589-590`, `:730`, `:862`, `:867`), `setHeadOverlay(id, value, opacity, 1, 1)` (`:601`,
  `:814-832`), `setHeadOverlayColor(id, colorType, color, 0)` (`:603-609`, `:815`, `:818`, `:833`), `setComponentVariation(2, hair, 0, 0)` +
  `setHairColor(color, highlight)` (`:555-556`, `:811-812`), `setEyeColor` (`:594`, `:830`), props and clothes (`:893-902`), tattoos (`:563`).
- **Server apply, synced to every player by RAGE:MP itself:** `SetDefaultSkin` sets the freemode model and calls `ApplyCharacter`
  (`S/Character/Repository.cs:99-107`): hair (component 2), tattoos, `SetPlayerHeadBlend`, `SetPlayerFaceFeature` x20,
  `SetPlayerHeadOverlay(i, {Index, Opacity, Color, SecondaryColor=100})`, `SetPlayerEyeColor` (`S/Core/Customization.cs:879-956`); hair colour
  goes with component 2 (`S/Chars/ClothesComponents.cs:1457-1464`). The client has no appearance sync of its own.

## 5. Names and other fields

- **UI** (`U/api/validation.js:4`, `:45-96`): first and last name 3-25 characters of `/^[a-zA-Z]+$/`, the first letter upper case; at most 1
  capital in the first name, at most 2 in the last ("McCry").
- **Server** at create: only length >= 1 (`S/Character/Create/Repository.cs:33-37`) and `First_Last` not already in `Main.PlayerNames`
  (`:38-42`, case-SENSITIVE, filled from every character at startup, `S/Main.cs:682`). Admin `/setname` wants both parts 3+ letters
  (`S/Core/Commands.cs:2296-2322`).
- **Not asked at all:** age, birth date, nationality, start location, a banned-word list.

## 6. The creator UI

- **Where:** the player is hidden and moved to (-2635.406, 1894.9324, 157.87411); nine ped positions on a hillside, three used
  (`C/player/character.js:200-210`, `:427-434`), a server-logo prop beside them (`:441-444`), idle / gesture clips on the peds
  (`:219-420`, `:462`, `:648-662`), a private dimension.
- **Camera** (`C/camera/create.js:7-29`, `C/camera/other.js:22-35`, `C/camera/manager.js:76-82,151-157`): FOV 50, orbits the head bone
  (12844) at radius -1.5..-0.5, azimuth 80-160; focus presets for hat (head), top (spine 11816), legs (11816, offset 0.5), shoes (foot 65245).
  Left drag rotates (control 237), right drag moves the height (-0.35..0.6, control 238), the wheel zooms in 0.2 steps (241/242); the camera is
  off while the mouse is over a panel (`:119-199`).
- **Steps** (`U/views/player/newauthentication/chars/create/index.svelte:17-23`): 1 Heredity (father, mother, shape mix, skin tone); 2 Face
  (11 controls with presets and a 2D grid or slider, `.../characteristics/index.svelte:14-114`); 3 Features (hair, brows, beard for men,
  blemishes, ageing, complexion, moles, sun damage, eye colour, lipstick for women; opacity slider and colour swatches each); 4 Clothes (hat,
  top, legs, shoes; picking one moves the camera). A "random" button behind a confirmation (`:80-87`, `:122-124`). Gender and the names are on
  the slot card (`.../list/toCreateChar.svelte:47-65`).
- **Keys:** up / down rows, left / right values (`.../appearance/listbutton.svelte:16-27`), Enter creates / unlocks / spawns, Q / E switch tabs
  (`.../chars/index.svelte:59-71`).
- **Save:** the name is validated, the button locks until the answer; an error shows a toast and unlocks it (`.../create/index.svelte:36-76`;
  `C/player/character.js:909-912`). Server texts: name length, name taken, slot taken (`dotnet/resources/Localization/Lang/LibraryRu.cs:781-782`,
  `S/Core/Customization.cs:1176`). Re-customising reuses the panel without the name step (`U/views/player/customization/index.svelte:8-15`, `:60`).

## 7. Around it

- A new character got 250,000,000 money (`CreateCharMoney`, `S/Character/Create/Repository.cs:61`, `settings/moneySettings.json:2`), an empty
  bank account (`:55`), licences and achievements all false (`:49`, `:59`), a starting quest (`:114`), and the chosen clothes as inventory items
  (`S/Core/Customization.cs:1298-1317`, `S/Chars/Repository.cs:2332-2349`). An account started with 60,000 RedBucks (`settings/moneySettings.json:3-5`).
- Fresh characters spawn at (-477.23682, -307.19675, 34.98841) or (-479.1031, -307.97488, 34.989967) by turns (`S/Core/Customization.cs:850-874`,
  `:1330`); a re-customised one at (-385.3705, -195.6715, 36.67242) (`:1333`); level 0 characters spawn with damage off
  (`S/Character/Spawn/Repository.cs:54-55`).
- The server's create checks only `Chars[slot] != -1` (`S/Core/Customization.cs:1174-1178`), with no bounds check on `slot`. Admins see new
  characters (`:1327`); `/sendcreator id` sends a player back to the creator (`S/Core/Commands.cs:2139-2183`), `/setname` renames.

## What Solar RP keeps, changes and leaves out

**Kept (the intent):** characters belong to an account and live in numbered slots; a character is a first + last name and a freemode
appearance (gender, the two parents with shape and skin mix, the 20 face features, the head overlays with opacity and colour, hair with its
colour and highlight, eye colour); the server checks names and uniqueness; peers see the appearance; the creator has the same four kinds of
controls (heredity, face, features, then save) and a select screen.

**Changed by GI's rules (2026-11-07):**

- **1 free slot, 2 to buy (3 max)** instead of 2 free + 1 bought. The slot is a platform product (`character_slot` entitlement, fake payments
  are fine per GI's platform decision), not an in-game currency spend, and it no longer gives VIP.
- **No blocking creator.** A player with no character gets one INSTANTLY on join (a default look, a name made from the account name) and plays
  at once; the creator edits it afterwards. The old select screen with its 10 s auto-spawn is not needed: Solar RP spawns the active character.
- **Server-side validation of everything** (the old server checked only empty names and uniqueness): names by the old UI's rules, every
  appearance value clamped to the ranges above, uniqueness case-INSENSITIVE (the old check was case-sensitive while the DB collation was not).
- **None of the five bugs** above (colours go to the right overlay ids, eye colour saved at once, parents stored as named, gender stored).

**Left out, and why:**

- **Delete** (72 h pending, purge timer) and **paid rename / re-customise**: GI asked for creation and slots; a delete touches the per-character
  inventory files and the platform slot count, so it is its own round.
- **Clothes at creation:** Solar RP's clothes are inventory items (0.5.4 `outfit_items`); a new character wears the starter outfit.
- **Starting money, licences, quests, spawn choice (house / faction):** Solar RP has none of these systems yet.
- **The creator location on the hillside, the logo prop, the gesture clips:** the port edits the player's own ped where it stands (no Rockstar
  or RedAge assets, no teleport).
- **Tattoos, makeup, blush, chest hair:** the old creator did not offer them either (tattoos came from shops).
- **Server merge slots (`characters` JSON column):** a RedAge-specific migration aid, switched off there too.
