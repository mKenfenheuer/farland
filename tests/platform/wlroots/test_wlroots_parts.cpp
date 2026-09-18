// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The parts of the wlroots backend that work without a compositor: the
// keymap, input mapping, damage, cursor pixels and the launch setup.

#include <farland/platform/wlroots/compositor.hpp>
#include <farland/platform/wlroots/output_capture.hpp>
#include <farland/platform/wlroots/outputs.hpp>
#include <farland/platform/wlroots/virtual_input.hpp>
#include <farland/platform/wlroots/xkb_keymap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace wlroots = farland::platform::wlroots;
using farland::platform::Rect;

namespace {

/// U+2603, which no layout has.
constexpr std::uint32_t snowman = 0x1002603;

std::unique_ptr<wlroots::XkbKeymap> keymap_or_skip(const char* layout)
{
    auto keymap = wlroots::XkbKeymap::create(layout);
    if (!keymap) {
        SKIP("xkbcommon cannot compile the layout (is xkeyboard-config installed?)");
    }
    return std::move(*keymap);
}

}  // namespace

TEST_CASE("XkbKeymap finds keys and their modifiers", "[wlroots][keymap]")
{
    auto keymap = keymap_or_skip("us");
    const auto a = keymap->find(XKB_KEY_a);
    REQUIRE(a);
    CHECK(a->key == KEY_A);
    CHECK(a->modifiers == 0);
    const auto upper = keymap->find(XKB_KEY_A);
    REQUIRE(upper);
    CHECK(upper->key == KEY_A);
    CHECK(upper->modifiers != 0);
    const auto at = keymap->find(XKB_KEY_at);
    REQUIRE(at);
    CHECK(at->key == KEY_2);
    CHECK_FALSE(keymap->find(snowman));

    CHECK(wlroots::XkbKeymap::keysym_for(U'a') == XKB_KEY_a);
    CHECK(wlroots::XkbKeymap::keysym_for(U'\n') == XKB_KEY_Return);
    CHECK(wlroots::XkbKeymap::keysym_for(U'\t') == XKB_KEY_Tab);
    CHECK(wlroots::XkbKeymap::keysym_for(U'\x01') == XKB_KEY_NoSymbol);
    CHECK(wlroots::XkbKeymap::keysym_for(U'€') == XKB_KEY_EuroSign);
}

TEST_CASE("XkbKeymap follows the modifier keys", "[wlroots][keymap]")
{
    auto keymap = keymap_or_skip("us");
    CHECK(keymap->modifiers() == wlroots::XkbKeymap::Modifiers{});
    CHECK(keymap->update_key(KEY_LEFTSHIFT, true));
    CHECK(keymap->modifiers().depressed != 0);
    CHECK_FALSE(keymap->update_key(KEY_A, true));
    CHECK_FALSE(keymap->update_key(KEY_A, false));
    CHECK(keymap->update_key(KEY_LEFTSHIFT, false));
    CHECK(keymap->modifiers().depressed == 0);
    // Caps Lock locks.
    keymap->update_key(KEY_CAPSLOCK, true);
    keymap->update_key(KEY_CAPSLOCK, false);
    CHECK(keymap->modifiers().locked != 0);
}

TEST_CASE("XkbKeymap takes the layout, with a variant", "[wlroots][keymap]")
{
    auto keymap = keymap_or_skip("de(nodeadkeys)");
    // German keyboards swap Y and Z, and have € on AltGr+E, which X11
    // clients can type, unlike the Euro key above keycode 255.
    const auto z = keymap->find(XKB_KEY_z);
    REQUIRE(z);
    CHECK(z->key == KEY_Y);
    const auto euro = keymap->find(XKB_KEY_EuroSign);
    REQUIRE(euro);
    CHECK(euro->key == KEY_E);
    CHECK(euro->modifiers != 0);
    const auto udiaeresis = keymap->find(XKB_KEY_udiaeresis);
    REQUIRE(udiaeresis);
    CHECK(udiaeresis->key == KEY_LEFTBRACE);
    CHECK(udiaeresis->modifiers == 0);
    CHECK_FALSE(wlroots::XkbKeymap::create("no-such-layout-farland"));
}

TEST_CASE("XkbKeymap puts missing keysyms on spare keys", "[wlroots][keymap]")
{
    auto keymap = keymap_or_skip("us");
    const auto& spare = keymap->spare_keycodes();
    REQUIRE(spare.size() >= 2);
    CHECK(spare.front() <= 255);
    const std::string original = keymap->text();

    const auto added = keymap->find_or_add(snowman);
    REQUIRE(added);
    CHECK(added->key == spare.front() - 8);
    CHECK(added->modifiers == 0);
    CHECK(keymap->generation() == 1);
    CHECK(keymap->text() != original);
    CHECK(keymap->text().find("0x1002603") != std::string::npos);
    // Found now, and not added twice.
    CHECK(keymap->find(snowman) == added);
    CHECK(keymap->find_or_add(snowman) == added);
    CHECK(keymap->generation() == 1);
    // Keysyms the layout has need no change.
    CHECK(keymap->find_or_add(XKB_KEY_b));
    CHECK(keymap->generation() == 1);
    // US has € only on a key above 255, which X11 clients miss: a spare key.
    const auto euro = keymap->find_or_add(XKB_KEY_EuroSign);
    REQUIRE(euro);
    CHECK(euro->key + 8 <= 255);
    CHECK(keymap->generation() == 2);

    // More characters than spare keys: the one used longest ago makes room.
    std::vector<std::uint32_t> more;
    for (std::uint32_t cp = 0x4e00; more.size() <= spare.size(); ++cp) {
        const auto keysym = wlroots::XkbKeymap::keysym_for(static_cast<char32_t>(cp));
        REQUIRE(keymap->find_or_add(keysym));
        more.push_back(keysym);
    }
    CHECK(keymap->find(more.back()));
    CHECK_FALSE(keymap->find(snowman));  // the oldest went
    CHECK(keymap->find(XKB_KEY_a));
}

TEST_CASE("Absolute positions map onto the screen under them", "[wlroots][input]")
{
    // Two 1000x500 outputs side by side in the layout, letterboxed at half
    // size on the client, with a 100-pixel gap between them.
    const std::array<wlroots::ScreenMapping, 2> screens{
        wlroots::ScreenMapping{Rect{0, 0, 500, 250}, Rect{0, 0, 1000, 500}},
        wlroots::ScreenMapping{Rect{600, 0, 500, 250}, Rect{1000, 0, 1000, 500}},
    };
    auto mapped = wlroots::map_to_layout(screens, 250, 125);
    REQUIRE(mapped);
    CHECK(mapped->first == 500);
    CHECK(mapped->second == 250);
    mapped = wlroots::map_to_layout(screens, 850, 0);
    REQUIRE(mapped);
    CHECK(mapped->first == 1500);
    CHECK(mapped->second == 0);
    // In the gap, nearer the second screen: its left edge.
    mapped = wlroots::map_to_layout(screens, 590, 100);
    REQUIRE(mapped);
    CHECK(mapped->first == 1000);
    // Beyond the bottom right: the last pixel.
    mapped = wlroots::map_to_layout(screens, 5000, 5000);
    REQUIRE(mapped);
    CHECK(mapped->first == 1999);
    CHECK(mapped->second == 499);
    CHECK_FALSE(wlroots::map_to_layout({}, 1, 1));
}

TEST_CASE("Outputs are ordered and boxed by their layout", "[wlroots][outputs]")
{
    std::vector<wlroots::Output> outputs(3);
    outputs[0].logical = Rect{1920, 0, 1280, 720};
    outputs[1].logical = Rect{0, 0, 1920, 1080};
    outputs[2].logical = Rect{0, 1080, 800, 600};
    outputs[2].enabled = false;
    CHECK(wlroots::screen_order(outputs) == std::vector<std::size_t>{1, 0});
    CHECK(wlroots::layout_box(outputs) == Rect{0, 0, 3200, 1080});
    outputs[2].enabled = true;
    CHECK(wlroots::layout_box(outputs) == Rect{0, 0, 3200, 1680});
}

TEST_CASE("Damage keeps rectangles up to a limit", "[wlroots][capture]")
{
    wlroots::Damage damage;
    CHECK(damage.empty());
    damage.add(Rect{0, 0, 0, 10});  // empty rectangles are ignored
    CHECK(damage.empty());
    for (std::size_t i = 0; i < wlroots::Damage::max_rects; ++i) {
        damage.add(Rect{static_cast<std::int32_t>(i) * 10, 5, 4, 4});
    }
    CHECK(damage.rects().size() == wlroots::Damage::max_rects);
    damage.add(Rect{1000, 1000, 10, 10});
    REQUIRE(damage.rects().size() == 1);
    CHECK(damage.rects().front() == Rect{0, 5, 1010, 1005});

    wlroots::Damage all;
    all.add_all();
    CHECK(all.for_frame().empty());
    damage.merge(all);
    CHECK(damage.all());
    CHECK(damage.rects().empty());
    damage.clear();
    CHECK(damage.empty());
}

TEST_CASE("Cursor pixels lose their premultiplied alpha", "[wlroots][capture]")
{
    std::array<std::byte, 12> pixels{
        std::byte{0x40}, std::byte{0x20}, std::byte{0x10}, std::byte{0x80},  // half transparent
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // transparent
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0xff},  // opaque
    };
    wlroots::unpremultiply(pixels);
    CHECK(pixels[0] == std::byte{0x80});
    CHECK(pixels[1] == std::byte{0x40});
    CHECK(pixels[2] == std::byte{0x20});
    CHECK(pixels[3] == std::byte{0x80});
    CHECK(pixels[8] == std::byte{0x12});
    CHECK(pixels[10] == std::byte{0x56});
}

TEST_CASE("Compositors start headless with farland's environment", "[wlroots][compositor]")
{
    wlroots::CompositorLaunch launch;
    launch.kind = wlroots::CompositorKind::cage;
    launch.command = {"foot", "-e", "top"};
    launch.outputs = 2;
    CHECK(wlroots::compositor_command(launch) == std::vector<std::string>{"cage", "--", "foot", "-e", "top"});
    launch.kind = wlroots::CompositorKind::sway;
    CHECK(wlroots::compositor_command(launch).front() == "sway");

    const std::vector<std::string> environment{"HOME=/home/u",        "WAYLAND_DISPLAY=wayland-0", "DISPLAY=:0",
                                               "WLR_RENDERER=vulkan", "XDG_RUNTIME_DIR=/old",      "PATH=/usr/bin"};
    auto env = wlroots::compositor_environment(launch, environment, "/run/user/1000");
    const auto has = [&env](const std::string& entry) { return std::ranges::find(env, entry) != env.end(); };
    CHECK(has("HOME=/home/u"));
    CHECK(has("PATH=/usr/bin"));
    CHECK(has("WLR_BACKENDS=headless"));
    CHECK(has("WLR_LIBINPUT_NO_DEVICES=1"));
    CHECK(has("WLR_HEADLESS_OUTPUTS=2"));
    CHECK(has("WLR_RENDERER=pixman"));
    CHECK(has("XDG_RUNTIME_DIR=/run/user/1000"));
    CHECK_FALSE(has("WAYLAND_DISPLAY=wayland-0"));
    CHECK_FALSE(has("DISPLAY=:0"));
    CHECK_FALSE(has("WLR_RENDERER=vulkan"));
    CHECK_FALSE(has("XDG_RUNTIME_DIR=/old"));

    launch.render_node = "/dev/dri/renderD128";
    env = wlroots::compositor_environment(launch, environment, "/tmp/x");
    CHECK(has("WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128"));
    CHECK(std::ranges::none_of(env, [](const std::string& e) { return e.starts_with("WLR_RENDERER="); }));
}

TEST_CASE("The compositor's Wayland socket is found among its sockets", "[wlroots][compositor]")
{
    const std::string proc_net_unix =
        "Num       RefCount Protocol Flags    Type St Inode Path\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 1001 /run/user/1000/wayland-0\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 2001 /run/user/1000/sway-ipc.1000.42.sock\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 2002 /run/user/1000/wayland-1\n"
        "0000000000000000: 00000003 00000000 00000000 0001 03 2003\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 2004 /tmp/.X11-unix/X1\n";
    const std::array<std::uint64_t, 4> inodes{2001, 2002, 2003, 2004};
    CHECK(wlroots::find_wayland_socket(proc_net_unix, inodes, "/run/user/1000") == "/run/user/1000/wayland-1");
    const std::array<std::uint64_t, 1> others{2001};
    CHECK_FALSE(wlroots::find_wayland_socket(proc_net_unix, others, "/run/user/1000"));
    CHECK_FALSE(wlroots::find_wayland_socket(proc_net_unix, inodes, "/run/user/1001"));
}
