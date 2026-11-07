// overlay_inventory.h: the 0.6 inventory screen (GI's REV3 Claude Design, HUD_DESIGN_COMPONENTS/REV3/BUILD_SPEC.md), drawn by the overlay's
// Tab menu. Internal to the overlay library (overlay_ui.cpp calls it; the engine talks to it through overlay.h's SetInventory /
// PopInventoryOp, defined here).
//
// Round 1 (0.6.0): the CHARACTER panel (14 equipment slots, the mannequin, the 4 quick slots), the INVENTORY panel (weight pill, search,
// category chips, pockets, backpack, the detail card and the action bar), the NEARBY panel in its "nothing in reach" state (vehicle, ground and
// stash storage are rounds 2-3), the key hint bar, the tooltip, the context menu and the split dialog. Every change the player makes is an
// inv::Op: applied at once to a prediction (libs/sync/inventory.h Apply, the same function the server runs) and sent; the server's next
// snapshot replaces the prediction (ops it has not processed yet are applied again on top).
#pragma once

#include <cstdint>
#include <string>

struct ImFontAtlas;

namespace gage::game::Overlay::detail::inv3 {

    // UiInit: Reserve before the font atlas is built (the REV3 Sofia faces, the icon and mannequin rects), Fill after it is built and before the
    // backend uploads it. scale = the bake scale (display height / 1080).
    void Reserve(ImFontAtlas* atlas, float scale);
    void Fill(ImFontAtlas* atlas, float scale);
    void Reset();   // UiReset: the ImGui context went away (the pushed snapshot survives)

    // The pages of the Tab menu: the inventory, 0.5.9 stage 4 the characters (select screen + creator) while the server sent a roster and
    // char_screens is on, and for admins the old quick actions / cars pages under ADMIN.
    enum class Page { Inventory, Admin, Characters };

    struct FrameIn {
        float W = 0, H = 0;        // display size
        float a = 1.0f;            // the menu's fade (0..1)
        bool justOpened = false;   // first frame after Tab
        bool isAdmin = false;      // show the ADMIN tab
        bool characters = false;   // 0.5.9 stage 4: show the CHARACTERS tab (CharactersAvailable)
        Page page = Page::Inventory;
        std::string playerName;    // the local player's name (the CHARACTER chip when the server has sent no inventory)
    };
    struct FrameOut {
        Page page = Page::Inventory;   // the page after this frame's clicks on the top pill
        bool escConsumed = false;      // Esc closed a menu / dialog / drag / the search box: the Tab menu stays open
        bool typing = false;           // the search box has the keyboard (Tab still closes; the chat keys are not ours anyway)
    };

    // The top pill (INVENTORY | CHARACTERS | ADMIN), drawn when there are two tabs or more, on every page. Returns the page clicked (or in.page).
    Page DrawTopTabs(const FrameIn& in);
    // The whole REV3 screen (dim, hint bar, three panels, overlays). Call when in.page == Inventory or Characters (the CHARACTERS page).
    FrameOut DrawScreen(const FrameIn& in);
    // Esc while the screen is open: closes the innermost thing (drag, split dialog, menu, search focus). false = nothing was open.
    bool HandleEsc();
    // 0.5.9 stage 4: the CHARACTERS page exists (char_screens on and the server sent a roster this session). Any thread.
    bool CharactersAvailable();
    // Esc on the CHARACTERS page: closes the delete dialog, then a name field's focus, then the creator (its draft is dropped). false = nothing.
    bool HandleCharEsc();
    // 0.5.6 weapon_keys: quick slot k (0..3) used while no panel is open (the number keys 1-4 in the world); true = a UseQuick was sent.
    // QuickKeysOn: the switch (Overlay::SetQuickKeys). Render thread.
    bool QuickKey(int k);
    bool QuickKeysOn();

}  // namespace gage::game::Overlay::detail::inv3
