#include "bang/platform/Window.hpp"

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bang::platform {

struct Window::State {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_surface* surface = nullptr;
    xdg_wm_base* shell = nullptr;
    xdg_surface* shellSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;

    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;

    wl_data_device_manager* dataDeviceManager = nullptr;
    wl_data_device* dataDevice = nullptr;
    wl_data_offer* pendingOffer = nullptr;
    std::vector<std::string> pendingMimeTypes;
    wl_data_offer* clipboardOffer = nullptr;
    std::vector<std::string> clipboardMimeTypes;

    xkb_context* keymapContext = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* keyState = nullptr;

    ui::PointerState pointerState;
    ui::KeyboardState keyboardState;
    bool leftButtonDown = false;
    bool quitRequested = false;
    bool configured = false;

    WindowEvents events;
    Window* owner = nullptr;

    static const wl_registry_listener registryListener;
    static const wl_seat_listener seatListener;
    static const xdg_wm_base_listener shellListener;
    static const wl_data_device_listener dataDeviceListener;
    static const wl_data_offer_listener dataOfferListener;

    static void registryHandle(void* data, wl_registry* registry,
        std::uint32_t name, const char* interface, std::uint32_t version);
    static void registryRemove(void*, wl_registry*, std::uint32_t) { }

    static void seatCapabilities(void* data, wl_seat* seat,
        std::uint32_t capabilities);
    static void seatName(void*, wl_seat*, const char*) { }

    static void keyboardKeymap(void* data, wl_keyboard*, std::uint32_t format,
        int fd, std::uint32_t size);
    static void keyboardEnter(void*, wl_keyboard*, std::uint32_t, wl_surface*,
        wl_array*) { }
    static void keyboardLeave(
        void* data, wl_keyboard* keyboard, std::uint32_t serial,
        wl_surface* surface);
    static void keyboardKey(void* data, wl_keyboard*, std::uint32_t,
        std::uint32_t time, std::uint32_t key, std::uint32_t state);
    static void keyboardModifiers(void* data, wl_keyboard*, std::uint32_t,
        std::uint32_t modsDepressed, std::uint32_t modsLatched,
        std::uint32_t modsLocked, std::uint32_t group);
    static void keyboardRepeat(void*, wl_keyboard*, int32_t, int32_t) { }

    static void pointerEnter(void* data, wl_pointer*, std::uint32_t,
        wl_surface*, wl_fixed_t x, wl_fixed_t y);
    static void pointerLeave(void* data, wl_pointer*, std::uint32_t,
        wl_surface*);
    static void pointerMotion(void* data, wl_pointer*, std::uint32_t,
        wl_fixed_t x, wl_fixed_t y);
    static void pointerButton(void* data, wl_pointer*, std::uint32_t,
        std::uint32_t, std::uint32_t button, std::uint32_t state);
    static void pointerAxis(void* data, wl_pointer*, std::uint32_t,
        std::uint32_t axis, wl_fixed_t value);
    static void pointerFrame(void*, wl_pointer*) { }
    static void pointerAxisSource(void*, wl_pointer*, std::uint32_t) { }
    static void pointerAxisStop(
        void*, wl_pointer*, std::uint32_t, std::uint32_t) { }
    static void pointerAxisDiscrete(
        void*, wl_pointer*, std::uint32_t, std::int32_t) { }
    static void pointerAxisValue120(
        void*, wl_pointer*, std::uint32_t, std::int32_t) { }

    static void dataDeviceDataOffer(void* data, wl_data_device*,
        wl_data_offer* offer);
    static void dataDeviceSelection(void* data, wl_data_device*,
        wl_data_offer* offer);
    static void dataDeviceEnter(void* data, wl_data_device*, std::uint32_t serial,
        wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer);
    static void dataDeviceLeave(void*, wl_data_device*) { }
    static void dataDeviceMotion(
        void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) { }
    static void dataDeviceDrop(void*, wl_data_device*) { }

    static void dataOfferOffer(
        void* data, wl_data_offer*, const char* mimeType);
    static void dataOfferSourceActions(void*, wl_data_offer*, std::uint32_t) { }
    static void dataOfferAction(void*, wl_data_offer*, std::uint32_t) { }

    static void requestPaste(State* self);
    static void ensureDataDevice(State* self);

    static void shellPing(void*, xdg_wm_base*, std::uint32_t serial);
    static void surfaceConfigure(void* data, xdg_surface* surface,
        std::uint32_t serial);
    static void toplevelConfigure(void* data, xdg_toplevel*, int32_t width,
        int32_t height, wl_array* states);
    static void toplevelClose(void* data, xdg_toplevel*);
};

void Window::State::registryHandle(void* data, wl_registry* registry,
    std::uint32_t name, const char* interface, std::uint32_t version)
{
    auto* self = static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry,
            name, &wl_compositor_interface, std::min(version, 5u)));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        self->shell =
            static_cast<xdg_wm_base*>(wl_registry_bind(registry, name,
                &xdg_wm_base_interface, std::min(version, 3u)));
        xdg_wm_base_add_listener(self->shell, &shellListener, self);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        self->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name,
            &wl_seat_interface, std::min(version, 7u)));
        wl_seat_add_listener(self->seat, &seatListener, self);
        ensureDataDevice(self);
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name)
        == 0) {
        self->dataDeviceManager = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name,
                &wl_data_device_manager_interface, std::min(version, 3u)));
        ensureDataDevice(self);
    }
}

void Window::State::ensureDataDevice(State* self)
{
    if (self->dataDevice != nullptr || self->seat == nullptr
        || self->dataDeviceManager == nullptr) {
        return;
    }
    self->dataDevice = wl_data_device_manager_get_data_device(
        self->dataDeviceManager, self->seat);
    wl_data_device_add_listener(self->dataDevice, &dataDeviceListener, self);
}

const wl_registry_listener Window::State::registryListener = {
    .global = registryHandle,
    .global_remove = registryRemove,
};

const wl_seat_listener Window::State::seatListener = {
    .capabilities = seatCapabilities,
    .name = seatName,
};

const xdg_wm_base_listener Window::State::shellListener = {
    .ping = shellPing,
};

const wl_data_device_listener Window::State::dataDeviceListener = {
    .data_offer = dataDeviceDataOffer,
    .enter = dataDeviceEnter,
    .leave = dataDeviceLeave,
    .motion = dataDeviceMotion,
    .drop = dataDeviceDrop,
    .selection = dataDeviceSelection,
};

const wl_data_offer_listener Window::State::dataOfferListener = {
    .offer = dataOfferOffer,
    .source_actions = dataOfferSourceActions,
    .action = dataOfferAction,
};

void Window::State::dataOfferOffer(
    void* data, wl_data_offer*, const char* mimeType)
{
    auto* self = static_cast<State*>(data);
    self->pendingMimeTypes.emplace_back(mimeType);
}

void Window::State::dataDeviceDataOffer(void* data, wl_data_device*,
    wl_data_offer* offer)
{
    auto* self = static_cast<State*>(data);
    self->pendingOffer = offer;
    self->pendingMimeTypes.clear();
    wl_data_offer_add_listener(offer, &dataOfferListener, self);
}

void Window::State::dataDeviceSelection(void* data, wl_data_device*,
    wl_data_offer* offer)
{
    auto* self = static_cast<State*>(data);
    if (self->clipboardOffer != nullptr && self->clipboardOffer != offer) {
        wl_data_offer_destroy(self->clipboardOffer);
    }
    self->clipboardOffer = offer;
    self->clipboardMimeTypes = self->pendingOffer == offer
        ? self->pendingMimeTypes
        : std::vector<std::string> {};
    self->pendingOffer = nullptr;
    self->pendingMimeTypes.clear();
}

void Window::State::dataDeviceEnter(void* data, wl_data_device*,
    std::uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t,
    wl_data_offer* offer)
{
    // Drag-and-drop isn't supported; decline the offer immediately so the
    // compositor doesn't wait on us and so we don't leak the offer object.
    auto* self = static_cast<State*>(data);
    if (offer != nullptr) {
        wl_data_offer_accept(offer, serial, nullptr);
        if (self->pendingOffer == offer) {
            self->pendingOffer = nullptr;
            self->pendingMimeTypes.clear();
        }
        wl_data_offer_destroy(offer);
    }
}

void Window::State::requestPaste(State* self)
{
    if (self->clipboardOffer == nullptr) {
        return;
    }
    const char* mimeType = nullptr;
    for (const auto& type : self->clipboardMimeTypes) {
        if (type == "text/plain;charset=utf-8") {
            mimeType = "text/plain;charset=utf-8";
            break;
        }
    }
    if (mimeType == nullptr) {
        for (const auto& type : self->clipboardMimeTypes) {
            if (type == "UTF8_STRING" || type == "text/plain") {
                mimeType = type.c_str();
                break;
            }
        }
    }
    if (mimeType == nullptr) {
        return; // Clipboard holds something other than text (e.g. an image).
    }

    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
        return;
    }
    wl_data_offer_receive(self->clipboardOffer, mimeType, pipeFds[1]);
    ::close(pipeFds[1]);

    // The receive request has to actually reach the compositor/source
    // before anything will be written to the pipe.
    wl_display_flush(self->display);

    std::string pasted;
    char chunk[512];
    ssize_t bytesRead = 0;
    while ((bytesRead = ::read(pipeFds[0], chunk, sizeof(chunk))) > 0) {
        pasted.append(chunk, static_cast<std::size_t>(bytesRead));
    }
    ::close(pipeFds[0]);

    if (!pasted.empty() && pasted.back() == '\n') {
        pasted.pop_back();
    }
    self->keyboardState.text += pasted;
}

void Window::State::seatCapabilities(void* data, wl_seat* seat,
    std::uint32_t capabilities)
{
    auto* self = static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0
        && self->keyboard == nullptr) {
        self->keyboard = wl_seat_get_keyboard(seat);
        static const wl_keyboard_listener listener = {
            .keymap = keyboardKeymap,
            .enter = keyboardEnter,
            .leave = keyboardLeave,
            .key = keyboardKey,
            .modifiers = keyboardModifiers,
            .repeat_info = keyboardRepeat,
        };
        wl_keyboard_add_listener(self->keyboard, &listener, self);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0
        && self->pointer == nullptr) {
        self->pointer = wl_seat_get_pointer(seat);
        static const wl_pointer_listener listener = {
            .enter = pointerEnter,
            .leave = pointerLeave,
            .motion = pointerMotion,
            .button = pointerButton,
            .axis = pointerAxis,
            .frame = pointerFrame,
            .axis_source = pointerAxisSource,
            .axis_stop = pointerAxisStop,
            .axis_discrete = pointerAxisDiscrete,
            .axis_value120 = pointerAxisValue120,
        };
        wl_pointer_add_listener(self->pointer, &listener, self);
    }
}

void Window::State::keyboardKeymap(void* data, wl_keyboard*, std::uint32_t format,
    int fd, std::uint32_t size)
{
    auto* self = static_cast<State*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    auto* map = static_cast<char*>(
        mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    if (self->keymapContext == nullptr) {
        self->keymapContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    self->keymap = xkb_keymap_new_from_buffer(self->keymapContext, map, size - 1,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (self->keymap != nullptr && self->keyState == nullptr) {
        self->keyState = xkb_state_new(self->keymap);
    }
}

void Window::State::keyboardKey(void* data, wl_keyboard*, std::uint32_t,
    std::uint32_t, std::uint32_t key, std::uint32_t state)
{
    auto* self = static_cast<State*>(data);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || self->keyState == nullptr) {
        return;
    }
    const std::uint32_t keyCode = key + 8;
    const xkb_keysym_t symbol = xkb_state_key_get_one_sym(self->keyState, keyCode);

    switch (symbol) {
    case XKB_KEY_BackSpace:
        self->keyboardState.backspace = true;
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        self->keyboardState.enter = true;
        return;
    case XKB_KEY_Escape:
        self->keyboardState.escape = true;
        return;
    default:
        break;
    }

    const bool ctrlHeld = xkb_state_mod_name_is_active(self->keyState,
        XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    if (ctrlHeld) {
        if (symbol == XKB_KEY_v || symbol == XKB_KEY_V) {
            requestPaste(self);
        }
        // xkb still produces a raw control byte for every other Ctrl+<key>
        // combo (e.g. Ctrl+C -> 0x03). That byte has no glyph in the font
        // atlas, so letting it fall through to the text buffer below is
        // what was showing up as an "unavailable character" box. Swallow
        // it here instead.
        return;
    }

    char buffer[8];
    const int length =
        xkb_state_key_get_utf8(self->keyState, keyCode, buffer, sizeof(buffer));
    for (int index = 0; index < length; ++index) {
        self->keyboardState.text.push_back(buffer[index]);
    }
}

void Window::State::keyboardModifiers(void* data, wl_keyboard*, std::uint32_t,
    std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
    std::uint32_t group)
{
    auto* self = static_cast<State*>(data);
    if (self->keyState != nullptr) {
        xkb_state_update_mask(self->keyState, depressed, latched, locked, 0, 0,
            group);
    }
}

void Window::State::keyboardLeave(void*, wl_keyboard*, std::uint32_t,
    wl_surface*)
{
}

void Window::State::pointerEnter(void* data, wl_pointer*, std::uint32_t,
    wl_surface*, wl_fixed_t x, wl_fixed_t y)
{
    auto* self = static_cast<State*>(data);
    self->pointerState.x = wl_fixed_to_double(x);
    self->pointerState.y = wl_fixed_to_double(y);
}

void Window::State::pointerLeave(void* data, wl_pointer*, std::uint32_t,
    wl_surface*)
{
    auto* self = static_cast<State*>(data);
    self->pointerState.x = -1000.0f;
    self->pointerState.y = -1000.0f;
    self->leftButtonDown = false;
}

void Window::State::pointerMotion(void* data, wl_pointer*, std::uint32_t,
    wl_fixed_t x, wl_fixed_t y)
{
    auto* self = static_cast<State*>(data);
    self->pointerState.x = wl_fixed_to_double(x);
    self->pointerState.y = wl_fixed_to_double(y);
}

void Window::State::pointerButton(void* data, wl_pointer*, std::uint32_t,
    std::uint32_t, std::uint32_t button, std::uint32_t state)
{
    auto* self = static_cast<State*>(data);
    if (button != BTN_LEFT) {
        return;
    }
    self->leftButtonDown = state == WL_POINTER_BUTTON_STATE_PRESSED;
    self->pointerState.down = self->leftButtonDown;
    if (self->leftButtonDown) {
        self->pointerState.pressed = true;
    } else {
        self->pointerState.released = true;
    }
}

void Window::State::pointerAxis(void* data, wl_pointer*, std::uint32_t,
    std::uint32_t axis, wl_fixed_t value)
{
    auto* self = static_cast<State*>(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    const double delta = wl_fixed_to_double(value);
    self->pointerState.wheelDelta += static_cast<float>(delta / 10.0);
}

void Window::State::shellPing(void*, xdg_wm_base* shell, std::uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

void Window::State::surfaceConfigure(void* data, xdg_surface* surface,
    std::uint32_t serial)
{
    auto* self = static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (!self->configured) {
        self->configured = true;
        if (self->events.onConfigure) {
            self->events.onConfigure();
        }
    }
}

void Window::State::toplevelConfigure(void* data, xdg_toplevel*, int32_t width,
    int32_t height, wl_array*)
{
    auto* self = static_cast<State*>(data);
    if (width > 0 && height > 0 && self->owner != nullptr
        && (static_cast<std::uint32_t>(width) != self->owner->width_
            || static_cast<std::uint32_t>(height) != self->owner->height_)) {
        self->owner->width_ = static_cast<std::uint32_t>(width);
        self->owner->height_ = static_cast<std::uint32_t>(height);
        if (self->events.onResize) {
            self->events.onResize(static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height));
        }
    }
}

void Window::State::toplevelClose(void* data, xdg_toplevel*)
{
    static_cast<State*>(data)->quitRequested = true;
}

Window::Window(const std::string& title, std::uint32_t width,
    std::uint32_t height)
    : width_(width)
    , height_(height)
{
    state_ = new State;
    state_->owner = this;

    state_->display = wl_display_connect(nullptr);
    if (state_->display == nullptr) {
        throw std::runtime_error("cannot connect to a Wayland compositor");
    }

    state_->registry = wl_display_get_registry(state_->display);
    wl_registry_add_listener(state_->registry, &State::registryListener, state_);
    wl_display_roundtrip(state_->display);

    if (state_->compositor == nullptr || state_->shell == nullptr) {
        throw std::runtime_error("compositor lacks required Wayland globals");
    }

    state_->surface = wl_compositor_create_surface(state_->compositor);
    state_->shellSurface =
        xdg_wm_base_get_xdg_surface(state_->shell, state_->surface);
    static const xdg_surface_listener surfaceListener = {
        .configure = State::surfaceConfigure,
    };
    xdg_surface_add_listener(state_->shellSurface, &surfaceListener, state_);

    state_->toplevel = xdg_surface_get_toplevel(state_->shellSurface);
    static const xdg_toplevel_listener toplevelListener = {
        .configure = State::toplevelConfigure,
        .close = State::toplevelClose,
    };
    xdg_toplevel_add_listener(state_->toplevel, &toplevelListener, state_);
    xdg_toplevel_set_title(state_->toplevel, title.c_str());
    xdg_toplevel_set_app_id(state_->toplevel, "bang");
    wl_surface_commit(state_->surface);
    wl_display_roundtrip(state_->display);
}

Window::~Window()
{
    if (state_ == nullptr) {
        return;
    }
    if (state_->clipboardOffer != nullptr) {
        wl_data_offer_destroy(state_->clipboardOffer);
    }
    if (state_->dataDevice != nullptr) {
        wl_data_device_release(state_->dataDevice);
    }
    if (state_->dataDeviceManager != nullptr) {
        wl_data_device_manager_destroy(state_->dataDeviceManager);
    }
    if (state_->keyState != nullptr) {
        xkb_state_unref(state_->keyState);
    }
    if (state_->keymap != nullptr) {
        xkb_keymap_unref(state_->keymap);
    }
    if (state_->keymapContext != nullptr) {
        xkb_context_unref(state_->keymapContext);
    }
    if (state_->pointer != nullptr) {
        wl_pointer_release(state_->pointer);
    }
    if (state_->keyboard != nullptr) {
        wl_keyboard_release(state_->keyboard);
    }
    if (state_->toplevel != nullptr) {
        xdg_toplevel_destroy(state_->toplevel);
    }
    if (state_->shellSurface != nullptr) {
        xdg_surface_destroy(state_->shellSurface);
    }
    if (state_->surface != nullptr) {
        wl_surface_destroy(state_->surface);
    }
    if (state_->shell != nullptr) {
        xdg_wm_base_destroy(state_->shell);
    }
    if (state_->seat != nullptr) {
        wl_seat_release(state_->seat);
    }
    if (state_->compositor != nullptr) {
        wl_compositor_destroy(state_->compositor);
    }
    if (state_->registry != nullptr) {
        wl_registry_destroy(state_->registry);
    }
    if (state_->display != nullptr) {
        wl_display_disconnect(state_->display);
    }
    delete state_;
}

void Window::setEvents(WindowEvents events)
{
    state_->events = std::move(events);
}

wl_display* Window::display() const
{
    return state_->display;
}

wl_surface* Window::surface() const
{
    return state_->surface;
}

ui::PointerState Window::takePointer()
{
    ui::PointerState snapshot = state_->pointerState;
    state_->pointerState.pressed = false;
    state_->pointerState.released = false;
    state_->pointerState.wheelDelta = 0.0f;
    return snapshot;
}

ui::KeyboardState Window::takeKeyboard()
{
    ui::KeyboardState snapshot = std::move(state_->keyboardState);
    state_->keyboardState = ui::KeyboardState {};
    return snapshot;
}

bool Window::poll()
{
    wl_display_dispatch_pending(state_->display);
    while (wl_display_prepare_read(state_->display) != 0) {
        wl_display_dispatch_pending(state_->display);
    }
    wl_display_flush(state_->display);

    struct pollfd descriptor { wl_display_get_fd(state_->display), POLLIN, 0 };
    const int ready = ::poll(&descriptor, 1, 16);
    if (ready < 0 && errno != EINTR) {
        wl_display_cancel_read(state_->display);
        return false;
    }

    if ((descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(state_->display) < 0) {
            return false;
        }
    } else {
        wl_display_cancel_read(state_->display);
        if ((descriptor.revents & (POLLERR | POLLHUP)) != 0) {
            return false;
        }
    }
    wl_display_dispatch_pending(state_->display);
    return !state_->quitRequested;
}

} // namespace bang::platform
