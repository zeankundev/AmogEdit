/*
 * Copyright (c) 2015 Meltytech, LLC
 * Author: Brian Matherly <code@brianmatherly.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "sharedframe.h"
#include <mutex>

void destroyFrame(void* p)
{
    delete static_cast<Mlt::Frame*>(p);
}

class FrameData : public QSharedData
{
public:
    FrameData()
        : f(nullptr)
    {
    }
    explicit FrameData(Mlt::Frame &frame)
        : f(frame)
    {
    }
    ~FrameData() = default;

    Mlt::Frame f;
    std::mutex m;

private:
    Q_DISABLE_COPY(FrameData)
};

SharedFrame::SharedFrame()
    : d(nullptr)
{
}

SharedFrame::SharedFrame(Mlt::Frame &frame)
    : d(new FrameData(frame))
{
}

SharedFrame::SharedFrame(const SharedFrame &other)

    = default;

SharedFrame::~SharedFrame() = default;

SharedFrame &SharedFrame::operator=(const SharedFrame &other) = default;

bool SharedFrame::is_valid() const
{
    return d && d->f.is_valid();
}

Mlt::Frame SharedFrame::clone(bool audio, bool image, bool alpha) const
{
    // TODO: Consider moving this implementation into MLT.
    // It could be added to mlt_frame as an alternative to:
    //     mlt_frame mlt_frame_clone( mlt_frame self, int is_deep );
    // It could also be added to Mlt::Frame as a const function.
    void *data = nullptr;
    void *copy = nullptr;
    int size = 0;
    Mlt::Frame cloneFrame(mlt_frame_init(nullptr));
    cloneFrame.inherit(d->f);
    cloneFrame.set("_producer", d->f.get_data("_producer", size), 0, nullptr, nullptr);
    cloneFrame.set("movit.convert", d->f.get_data("movit.convert", size), 0, nullptr, nullptr);
    cloneFrame.get_frame()->convert_image = d->f.get_frame()->convert_image;
    cloneFrame.get_frame()->convert_audio = d->f.get_frame()->convert_audio;

    data = d->f.get_data("audio", size);
    if (audio && (data != nullptr)) {
        if (size == 0) {
            size = mlt_audio_format_size(get_audio_format(), get_audio_samples(), get_audio_channels());
        }
        copy = mlt_pool_alloc(size);
        memcpy(copy, data, size_t(size));
        cloneFrame.set("audio", copy, size, mlt_pool_release);
    } else {
        cloneFrame.set("audio", 0);
        cloneFrame.set("audio_format", mlt_audio_none);
        cloneFrame.set("audio_channels", 0);
        cloneFrame.set("audio_frequency", 0);
        cloneFrame.set("audio_samples", 0);
    }

    data = d->f.get_data("image", size);
    if (image && (data != nullptr)) {
        if (size == 0) {
            size = mlt_image_format_size(get_image_format(), get_image_width(), get_image_height(), nullptr);
        }
        copy = mlt_pool_alloc(size);
        memcpy(copy, data, size_t(size));
        cloneFrame.set("image", copy, size, mlt_pool_release);
    } else {
        cloneFrame.set("image", 0);
        cloneFrame.set("image_format", mlt_image_none);
        cloneFrame.set("width", 0);
        cloneFrame.set("height", 0);
    }

    data = d->f.get_data("alpha", size);
    if (alpha && (data != nullptr)) {
        if (size == 0) {
            size = get_image_width() * get_image_height();
        }
        copy = mlt_pool_alloc(size);
        memcpy(copy, data, size_t(size));
        cloneFrame.set("alpha", copy, size, mlt_pool_release);
    } else {
        cloneFrame.set("alpha", 0);
    }

    // Release the reference on the initial frame so that the returned frame
    // only has one reference.
    mlt_frame_close(cloneFrame.get_frame());
    return cloneFrame;
}

int SharedFrame::get_int(const char *name) const
{
    return d->f.get_int(name);
}

int64_t SharedFrame::get_int64(const char *name) const
{
    return d->f.get_int64(name);
}

double SharedFrame::get_double(const char *name) const
{
    return d->f.get_double(name);
}

char *SharedFrame::get(const char *name) const
{
    return d->f.get(name);
}

int SharedFrame::get_position() const
{
    return d->f.get_position();
}

mlt_image_format SharedFrame::get_image_format() const
{
    return mlt_image_format(d->f.get_int("format"));
}

int SharedFrame::get_image_width() const
{
    return d->f.get_int("width");
}

int SharedFrame::get_image_height() const
{
    return d->f.get_int("height");
}

const uint8_t *SharedFrame::get_image(mlt_image_format format) const
{
mlt_image_format native_format = get_image_format();
    int width = get_image_width();
    int height = get_image_height();
    uint8_t* image = nullptr;

    if (format == mlt_image_none) {
        format = native_format;
    }

    if (format == native_format) {
        // Native format is requested. Return frame image.
        image = static_cast<uint8_t*>(d->f.get_image(format, width, height, 0));
    } else {
        // Non-native format is requested. Return a cached converted image.
        const char* formatName = mlt_image_format_name( format );
        // Convert to non-const so that the cache can be accessed/modified while
        // under lock.
        auto* nonConstData = const_cast<FrameData*>(d.data());

        nonConstData->m.lock();

        auto* cacheFrame = static_cast<Mlt::Frame*>(nonConstData->f.get_data(formatName));
        if (cacheFrame == nullptr) {
            // A cached image does not exist, create one.
            // Make a non-deep clone of the frame (including convert function)
            mlt_frame cloneFrame = mlt_frame_clone(nonConstData->f.get_frame(), 0);
            cloneFrame->convert_image = nonConstData->f.get_frame()->convert_image;
            // Create a new cache frame
            cacheFrame = new Mlt::Frame(cloneFrame);
            // Release the reference on the clone
            // (now it is owned by the cache frame)
            mlt_frame_close( cloneFrame );
            // Save the cache frame as a property under the name of the image
            // format for later use.
            nonConstData->f.set(formatName, static_cast<void*>(cacheFrame), 0, destroyFrame);
            // Break a circular reference
            cacheFrame->clear("_cloned_frame");
        }

        // Get the image from the cache frame.
        // This will cause a conversion if it was just created.
        image = static_cast<uint8_t*>(cacheFrame->get_image(format, width, height, 0));

        nonConstData->m.unlock();
    }
    return image;
}

mlt_audio_format SharedFrame::get_audio_format() const
{
    return mlt_audio_format(d->f.get_int("audio_format"));
}

int SharedFrame::get_audio_channels() const
{
    return d->f.get_int("audio_channels");
}

int SharedFrame::get_audio_frequency() const
{
    return d->f.get_int("audio_frequency");
}

int SharedFrame::get_audio_samples() const
{
    return d->f.get_int("audio_samples");
}

const int16_t *SharedFrame::get_audio() const
{
    mlt_audio_format format = get_audio_format();
    int frequency = get_audio_frequency();
    int channels = get_audio_channels();
    int samples = get_audio_samples();
    return static_cast<int16_t *>(d->f.get_audio(format, frequency, channels, samples));
}

