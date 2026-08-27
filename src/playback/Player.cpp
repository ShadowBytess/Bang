#include "bang/Player.hpp"

#include <gst/gst.h>

#include <stdexcept>

namespace bang {

namespace {

std::string fileUri(const std::filesystem::path& file)
{
    std::string uri = "file://";
    for (const char character : file.string()) {
        if (static_cast<unsigned char>(character) < 0x80
            && character != ' ' && character != '"' && character != '#'
            && character != '%' && character != '?' && character != '&') {
            uri.push_back(character);
        } else {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "%%%02X",
                static_cast<unsigned>(static_cast<unsigned char>(character)));
            uri += escaped;
        }
    }
    return uri;
}

} // namespace

Player::Player()
{
    static bool gstInitialized = false;
    if (!gstInitialized) {
        if (!gst_init_check(nullptr, nullptr, nullptr)) {
            throw std::runtime_error("cannot initialize GStreamer");
        }
        gstInitialized = true;
    }

    playbin_ = gst_element_factory_make("playbin", "bang-playbin");
    if (playbin_ == nullptr) {
        throw std::runtime_error(
            "GStreamer playbin element unavailable; install gst-plugins-base");
    }
    bus_ = gst_element_get_bus(playbin_);

    GObject* object = G_OBJECT(playbin_);
    g_object_set(object, "volume", volume_, nullptr);
}

Player::~Player()
{
    stop();
    if (bus_ != nullptr) {
        gst_object_unref(bus_);
    }
    if (playbin_ != nullptr) {
        gst_object_unref(playbin_);
    }
}

void Player::load(const std::filesystem::path& file)
{
    stop();
    const std::string uri = fileUri(file);
    g_object_set(G_OBJECT(playbin_), "uri", uri.c_str(), nullptr);
    play();
}

void Player::play()
{
    if (state_ == State::Playing) {
        return;
    }
    gst_element_set_state(playbin_, GST_STATE_PLAYING);
    state_ = State::Playing;
}

void Player::pause()
{
    if (state_ != State::Playing) {
        return;
    }
    gst_element_set_state(playbin_, GST_STATE_PAUSED);
    state_ = State::Paused;
}

void Player::toggle()
{
    switch (state_) {
    case State::Playing:
        pause();
        break;
    case State::Paused:
    case State::Stopped:
        play();
        break;
    }
}

void Player::stop()
{
    if (playbin_ == nullptr) {
        return;
    }
    gst_element_set_state(playbin_, GST_STATE_NULL);
    state_ = State::Stopped;
}

void Player::seek(std::int64_t positionMs)
{
    const gint64 position = static_cast<gint64>(positionMs) * GST_MSECOND;
    gst_element_seek_simple(playbin_, GST_FORMAT_TIME,
        GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), position);
}

void Player::setVolume(double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    volume_ = volume;
    g_object_set(G_OBJECT(playbin_), "volume", volume_, nullptr);
}

std::int64_t Player::positionMs()
{
    gint64 position = 0;
    if (gst_element_query_position(playbin_, GST_FORMAT_TIME, &position)) {
        return static_cast<std::int64_t>(position / GST_MSECOND);
    }
    return 0;
}

std::int64_t Player::durationMs() const
{
    gint64 duration = 0;
    if (gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration)) {
        return static_cast<std::int64_t>(duration / GST_MSECOND);
    }
    return 0;
}

void Player::setOnFinished(FinishedCallback callback)
{
    onFinished_ = std::move(callback);
}

bool Player::poll()
{
    bool finished = false;
    while (true) {
        GstMessage* message =
            gst_bus_pop_filtered(bus_,
                GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR
                    | GST_MESSAGE_STATE_CHANGED));
        if (message == nullptr) {
            break;
        }
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_EOS:
            stop();
            finished = true;
            break;
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            g_warning("playback error: %s (%s)", error != nullptr ? error->message : "?",
                debug != nullptr ? debug : "");
            if (error != nullptr) {
                g_error_free(error);
            }
            g_free(debug);
            stop();
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT_CAST(playbin_)) {
                GstState newState = GST_STATE_VOID_PENDING;
                gst_message_parse_state_changed(message, nullptr, &newState,
                    nullptr);
                switch (newState) {
                case GST_STATE_PLAYING:
                    state_ = State::Playing;
                    break;
                case GST_STATE_PAUSED:
                    state_ = state_ == State::Stopped ? State::Stopped
                                                      : State::Paused;
                    break;
                case GST_STATE_READY:
                case GST_STATE_NULL:
                    state_ = State::Stopped;
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
        gst_message_unref(message);
    }

    if (finished && onFinished_) {
        onFinished_();
    }
    return finished;
}

} // namespace bang
